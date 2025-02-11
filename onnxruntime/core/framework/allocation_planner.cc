// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/allocation_planner.h"
#include <list>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include "core/common/exceptions.h"
#include "core/platform/env.h"
#include "core/framework/data_types.h"
#include "core/framework/kernel_def_builder.h"
#include "core/framework/mldata_type_utils.h"
#include "core/framework/op_kernel.h"
#include "core/framework/session_state.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/utils.h"

using namespace onnxruntime::common;
using namespace ONNX_NAMESPACE;
namespace onnxruntime {

std::ostream& operator<<(std::ostream& out, AllocKind alloc_kind) {
  switch (alloc_kind) {
    case AllocKind::kAllocate:
      out << "Allocate";
      break;
    case AllocKind::kAllocateStatically:
      out << "AllocateStatically";
      break;
    case AllocKind::kPreExisting:
      out << "PreExisting";
      break;
    case AllocKind::kReuse:
      out << "Reuse";
      break;
    case AllocKind::kAllocateOutput:
      out << "AllocateOutput";
      break;
    case AllocKind::kShare:
      out << "Share";
      break;
  }
  return out;
}

// Output details of an execution plan:
std::ostream& operator<<(std::ostream& out, std::pair<const SequentialExecutionPlan*, const SessionState*> planinfo) {
  const SequentialExecutionPlan& plan = *planinfo.first;
  const SessionState& session_state = *planinfo.second;
  auto& graph = *session_state.GetGraphViewer();
  std::unordered_map<int, std::string> index_to_name;

  out << "Allocation Plan:\n";
  out << "(ort_value_idx) output_name : <allocation plan>\n";
  auto plan_size = plan.allocation_plan.size();

  for (auto& name_index : session_state.GetOrtValueNameIdxMap()) {
    auto index = name_index.second;
    index_to_name[index] = name_index.first;
    out << "(" << index << ") " << name_index.first << " : ";
    if (0 <= index && static_cast<size_t>(index) < plan_size) {
      auto& elt_plan = plan.allocation_plan[index];
      out << elt_plan.alloc_kind;
      if (elt_plan.alloc_kind == AllocKind::kReuse) out << " " << elt_plan.reused_buffer;

      auto& loc = elt_plan.location;
      out << ", " << loc.ToString();

      if (elt_plan.create_fence_if_async) out << ", use fence when async";

    } else {
      out << "Index out-of-range!";
    }

    out << std::endl;
  }

  out << "\nExecution Plan:\n";
  for (size_t i = 0; i < plan.execution_plan.size(); ++i) {
    auto& step = plan.execution_plan[i];
    auto node = graph.GetNode(step.node_index);
    ORT_ENFORCE(nullptr != node);
    out << "[" << i << "] ";
    out << node->OpType() << " (" << node->Name() << ")" << std::endl;
    if (step.free_from_index <= step.free_to_index) {
      out << "Free ml-values: ";
      std::string sep;
      for (int j = step.free_from_index; j <= step.free_to_index; ++j) {
        auto freed_value_index = plan.to_be_freed[j];
        auto name_iter = index_to_name.find(freed_value_index);
        auto name = (name_iter == index_to_name.end()) ? "INVALID INDEX" : name_iter->second;
        out << sep << "(" << freed_value_index << ") " << name;
        sep = ", ";
      }
      out << std::endl;
    }
  }

  return out;
}

class PlannerImpl {
 public:
  PlannerImpl(const Node* parent_node, const onnxruntime::GraphViewer& graph_viewer,
              const std::vector<const NodeArg*>& outer_scope_node_args, const ExecutionProviders& providers,
              const KernelRegistryManager& kernel_registry, const OrtValueNameIdxMap& ort_value_name_idx_map,
              const ISequentialPlannerContext& context, SequentialExecutionPlan& plan)
      : context_{context},
        plan_{plan},
        parent_node_{parent_node},
        graph_viewer_{graph_viewer},
        outer_scope_node_args_{outer_scope_node_args},
        execution_providers_{providers},
        kernel_registry_{kernel_registry},
        ort_value_name_idx_map_{ort_value_name_idx_map} {}

  Status CreatePlan();

 private:
  const ISequentialPlannerContext& context_;
  SequentialExecutionPlan& plan_;

  const Node* parent_node_;
  const onnxruntime::GraphViewer& graph_viewer_;
  const std::vector<const NodeArg*>& outer_scope_node_args_;
  const ExecutionProviders& execution_providers_;

  const KernelRegistryManager& kernel_registry_;
  const OrtValueNameIdxMap& ort_value_name_idx_map_;

  // OrtValueInfo: Auxiliary information about an OrtValue used only during plan-generation:
  struct OrtValueInfo {
    const onnxruntime::NodeArg* p_def_site;  // the (unique) NodeArg corresponding to the MLValue
    int usecount = 0;                        // static reference-count
    OrtValueIndex reused_buffer_index;       // index of original buffer to reuse
  };

  // ort_value_info_ is indexed by an OrtValueIndex
  std::vector<OrtValueInfo> ort_value_info_;

  // FreeBufferInfo is used to track information about ml-values whose buffers are
  // free to be reused.
  struct FreeBufferInfo {
    OrtValueIndex ml_value;
    // deallocate_point is an index into the execution-plan; thus, ml_value becomes free after
    // this step in the execution-plan is completed.
    size_t deallocate_point;
    FreeBufferInfo(OrtValueIndex ort_value, size_t dealloc_point)
        : ml_value(ort_value), deallocate_point(dealloc_point) {}
  };
  // freelist_ : a list of ml-values whose buffers are free to be reused, sorted by when
  // they became free (more recently freed earlier in the list).
  std::list<FreeBufferInfo> freelist_;

  OrtValueIndex Index(const OrtValueName& name) {
    OrtValueIndex result;
    auto status = ort_value_name_idx_map_.GetIdx(name, result);
    ORT_ENFORCE(status.IsOK(), status.ErrorMessage());
    return result;
  }

  int& UseCount(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size());
    return ort_value_info_[n].usecount;
  }
  int& UseCount(const OrtValueName& name) { return UseCount(Index(name)); }

  OrtValueIndex& Buffer(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size());
    return ort_value_info_[n].reused_buffer_index;
  }

  AllocPlanPerValue& AllocPlan(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < plan_.allocation_plan.size());
    return plan_.allocation_plan[static_cast<size_t>(n)];
  }

  AllocPlanPerValue& AllocPlan(const OrtValueName& name) { return AllocPlan(Index(name)); }

  // Initialize state for a given ml-value at its definition site:
  void ProcessDef(OrtValueIndex id, const onnxruntime::NodeArg* p_def_site) {
    ORT_ENFORCE(id >= 0 && static_cast<size_t>(id) < ort_value_info_.size());
    OrtValueInfo& info = ort_value_info_[id];
    info.usecount = 0;
    info.reused_buffer_index = id;  // initially, no reuse; the ml-value uses its own buffer
    info.p_def_site = p_def_site;
  }

  // Reuse/Alias/Share between two OrtValue indexes
  void Reuse(OrtValueIndex reused, OrtValueIndex reused_for, AllocKind alloc_kind) {
    ORT_ENFORCE(reused != reused_for);
    // find original buffer underlying ml-value we want to reuse:
    OrtValueIndex original = Buffer(reused);
    // record that the new buffer will reuse that original buffer
    Buffer(reused_for) = original;
    // adjust original buffer's usecount
    UseCount(original) += UseCount(reused_for);

    // update allocation plan (for use at execution-time)
    auto& symplan = AllocPlan(reused_for);
    symplan.alloc_kind = alloc_kind;
    symplan.reused_buffer = original;
  }

  // Find if there exists some input tensor that we can use in-place for output_arg
  bool FindReusableInput(const onnxruntime::Node& node, int output_arg_num, OrtValueIndex* reusable_input) {
    auto p_output_arg = node.OutputDefs()[output_arg_num];
    const KernelCreateInfo* ci;
    Status st = kernel_registry_.SearchKernelRegistry(node, &ci);
    if (!st.IsOK() || ci == nullptr || ci->kernel_def == nullptr) {
      return false;
    }

    const std::vector<std::pair<int, int>>& alias_map = ci->kernel_def->Alias();
    auto input_args = node.InputDefs();
    for (auto pair : alias_map) {
      if (pair.second == output_arg_num) {
        // we _must_ reuse this input to satisfy aliasing requirement: (e.g., for reshape)
        if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
          auto p_input_arg = input_args[pair.first];
          if (p_input_arg->Exists()) {
            *reusable_input = Index(p_input_arg->Name());
            return true;
          }
        }
      }
    }

    const std::vector<std::pair<int, int>>& inplace_map = ci->kernel_def->MayInplace();
    for (auto pair : inplace_map) {
      if (pair.second == output_arg_num) {
        if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
          auto p_input_arg = input_args[pair.first];
          if (p_input_arg->Exists()) {
            auto input_arg_index = Index(p_input_arg->Name());
            auto original = Buffer(input_arg_index);
            if (1 == UseCount(original)) {
              if (SameSize(*p_input_arg, *p_output_arg)) {
                // we can reuse this input since it is its last use and permitted for in-place update
                *reusable_input = input_arg_index;  // or original; both should be okay
                return true;
              }
            }
          }
        }
      }
    }
    return false;
  }

  static bool SameShape(const TensorShapeProto& shape1, const TensorShapeProto& shape2) {
    // TODO: This should probably be defined to be the equality operator on TensorShapeProto.
    namespace on = ONNX_NAMESPACE;
    int rank1 = shape1.dim_size();
    if (shape2.dim_size() != rank1) return false;
    for (int i = 0; i < rank1; i++) {
      const auto& val1 = shape1.dim(i);
      const auto& val2 = shape2.dim(i);
      if (utils::HasDimValue(val1) && utils::HasDimValue(val2) &&
         (val1.dim_value() == val2.dim_value()))
        continue;  // same known dimension
      if (utils::HasDimParam(val1) && utils::HasDimParam(val2)) {
        const auto& val1_param = val1.dim_param();
        if (val1_param == val2.dim_param() && !val1_param.empty())
          continue;  // same unknown dimension
      }
      return false;
    }
    return true;
  }

  /*! \brief Given a tensor-type, return the size of an element of the tensor.
  */
  static size_t GetElementSize(const DataType& tensor_type) {
    const TypeProto& type_proto = ONNX_NAMESPACE::Utils::DataTypeUtils::ToTypeProto(tensor_type);
    MLDataType ml_data_type = DataTypeImpl::TypeFromProto(type_proto);
    const TensorTypeBase* tensor_type_base = ml_data_type->AsTensorType();
    ORT_ENFORCE(nullptr != tensor_type_base);
    MLDataType elt_type = tensor_type_base->GetElementType();
    return elt_type->Size();
  }

  static bool SameSize(const TensorShapeProto& shape1, const DataType& ptype1, const TensorShapeProto& shape2,
                       const DataType& ptype2) {
    return (GetElementSize(ptype1) == GetElementSize(ptype2)) && SameShape(shape1, shape2);

    /* TODO: we can improve this if the concrete shapes are known for both as below.
       Unclear whether this is worthwhile though.
    if (KnownSize(p_shape1) && KnownSize(p_shape2)) {
      // Comparison of statically-known size
      auto size1 = NumElements(p_shape1) * EltSize(ptype1);
      auto size2 = NumElements(p_shape2) * EltSize(ptype2);
      return size1 == size2;
    } else {
      // Comparison of statically-unknown size buffers
      return SameElementSize(ptype1, ptype2) && SameShape(shape1, shape2);
    }
    */
  }

  bool SameSize(const onnxruntime::NodeArg& arg1, const onnxruntime::NodeArg& arg2) {
    if ((!arg1.Exists()) || (!arg2.Exists())) return false;
    auto p_shape1 = context_.GetShape(arg1);
    auto p_shape2 = context_.GetShape(arg2);
    // If the shapes are unknown, we conservatively assume they may be of different size.
    if ((nullptr == p_shape1) || (nullptr == p_shape2)) return false;
    return SameSize(*p_shape1, arg1.Type(), *p_shape2, arg2.Type());
  }

  // Find if freelist contains a buffer of the same size as output_arg
  bool FindReusableTensor(const onnxruntime::NodeArg& output_arg, OrtValueIndex* reusable_tensor) {
    auto p_required_buffer_shape = context_.GetShape(output_arg);
    if (nullptr == p_required_buffer_shape) return false;
    auto required_buffer_type = output_arg.Type();
    auto& required_memory_info = AllocPlan(output_arg.Name()).location;

    for (auto it = freelist_.begin(); it != freelist_.end(); ++it) {
      size_t reusable = static_cast<size_t>(it->ml_value);
      const onnxruntime::NodeArg* p_node_arg = ort_value_info_.at(reusable).p_def_site;
      auto& available_memory_info = AllocPlan(p_node_arg->Name()).location;
      if (!(available_memory_info == required_memory_info)) continue;
      auto p_available_buffer_shape = context_.GetShape(*p_node_arg);
      if (nullptr != p_available_buffer_shape) {
        auto available_buffer_type = p_node_arg->Type();
        if (SameSize(*p_available_buffer_shape, available_buffer_type,
                     *p_required_buffer_shape, required_buffer_type)) {
          *reusable_tensor = it->ml_value;
          freelist_.erase(it);
          return true;
        }
      }
    }
    return false;
  }

  void Initialize(size_t num_graph_nodes, size_t num_ml_values) {
    // All ml-value indices must be in range 0 .. num_ml_values-1
    ort_value_info_.resize(num_ml_values);

    // Initialize execution plan:
    plan_.execution_plan.reserve(num_graph_nodes);

    // Initialize node_has_fence.
    plan_.node_has_fence.resize(graph_viewer_.MaxNodeIndex());

    // Initialize allocation plan:
    plan_.allocation_plan.resize(num_ml_values);
  }

  Status ComputeUseCounts() {
    // Note: for every ml-value, its definition must appear before all its uses in a topological sort of a valid model
    std::unordered_set<std::string> graph_inputs;
    for (auto& graph_input : graph_viewer_.GetInputsIncludingInitializers()) {
      graph_inputs.insert(graph_input->Name());
    }

    for (auto graph_input : graph_viewer_.GetInputs()) {
      OrtValueIndex index = Index(graph_input->Name());
      ProcessDef(index, graph_input);
      UseCount(index)++;  // Models caller's usage post-inference; ensures it will not be reused.
    }

    for (auto node_arg : outer_scope_node_args_) {
      OrtValueIndex index = Index(node_arg->Name());
      ProcessDef(index, node_arg);
      UseCount(index)++;  // ensure will not be re-used as this graph does not own the buffer
    }

    // All initializers should be treated as input
    for (const auto& pair : graph_viewer_.GetAllInitializedTensors()) {
      const auto& initializer_name = pair.first;
      OrtValueIndex index = Index(initializer_name);
      ProcessDef(index, graph_viewer_.GetNodeArg(pair.first));
      UseCount(initializer_name)++;
    }

    for (SequentialExecutionPlan::NodeExecutionPlan& step : plan_.execution_plan) {
      auto pnode = graph_viewer_.GetNode(step.node_index);
      if (pnode == nullptr) return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the node ", step.node_index);

      // Identify where each output of this node should be allocated.
      // This is determined by the opkernel bound to the node.
      const KernelCreateInfo* kernel_create_info = nullptr;
      ORT_RETURN_IF_ERROR(kernel_registry_.SearchKernelRegistry(*pnode, &kernel_create_info));
      auto p_kernelDef = kernel_create_info->kernel_def.get();
      if (nullptr == p_kernelDef) {
        std::ostringstream errormsg;
        errormsg << "No suitable kernel definition found for op " << pnode->OpType();
        if (pnode->Op() != nullptr) {
          errormsg << "(" << pnode->Op()->since_version() << ")";
        }
        if (!pnode->Name().empty()) errormsg << " (node " << pnode->Name() << ")";
        return Status(ONNXRUNTIME, FAIL, errormsg.str());
      }
      auto exec_provider = execution_providers_.Get(*pnode);
      if (exec_provider == nullptr) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the execution provider ",
                               pnode->GetExecutionProviderType());
      }

      // increment UseCount and add location information if applicable for the provided input def
      auto process_input = [&graph_inputs, &exec_provider, &p_kernelDef, this](const NodeArg& input, size_t arg_idx) {
        const auto& name = input.Name();
        UseCount(name)++;

        // If it's a graph input or outer scope node arg, set its plan.
        // NOTE: Copy nodes should have already been added if a graph input is fed as input
        // to nodes assigned to different providers.
        if (graph_inputs.find(name) != graph_inputs.cend() ||
            std::find_if(outer_scope_node_args_.cbegin(), outer_scope_node_args_.cend(),
                         [&name](const NodeArg* value) {
                           return value && value->Name() == name;
                         }) != outer_scope_node_args_.cend()) {
          OrtValueIndex index = Index(name);
          plan_.SetLocation(static_cast<size_t>(index),
                            exec_provider->GetAllocator(0, p_kernelDef->InputMemoryType(arg_idx))->Info());
        }

        return Status::OK();
      };

      ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->InputDefs(), process_input));
      ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->ImplicitInputDefs(), process_input));

      auto outputs = pnode->OutputDefs();
      auto num_outputs = outputs.size();
      for (size_t i = 0; i < num_outputs; ++i) {
        auto* node_output = outputs[i];
        if (!node_output->Exists()) continue;
        OrtValueIndex index = Index(node_output->Name());
        ProcessDef(index, node_output);
        ++UseCount(index);
        plan_.SetLocation(static_cast<size_t>(index), exec_provider->GetAllocator(0, p_kernelDef->OutputMemoryType(i))->Info());
      }
      // if sync is needed, mark allocation plan as create_fence_if_async=true
      // note that the input arg may come from an execution provider (i.e. CPU) that does not support async,
      // in which case create_fence_if_async would be ignored when creating MLValue
      if (p_kernelDef->ExecQueueId() != 0) {
        pnode->ForEachDef([this](const onnxruntime::NodeArg& arg, bool /*is_input*/) {
          OrtValueIndex index = Index(arg.Name());
          AllocPlan(index).create_fence_if_async = true;
        });
      }
    }

    for (auto graph_output : graph_viewer_.GetOutputs()) {
      UseCount(graph_output->Name())++;  // Models caller's usage post-inference; ensures it will not be reused.
    }

    return Status::OK();
  }

  OrtMemoryInfo GetLocationForNodeInput(size_t input_index, const Node& node) {
    auto* p_provider = execution_providers_.Get(node);
    ORT_ENFORCE(p_provider);

    const KernelCreateInfo* kernel_create_info;
    auto st = kernel_registry_.SearchKernelRegistry(node, &kernel_create_info);
    ORT_ENFORCE(st.IsOK(), st.ErrorMessage());
    ORT_ENFORCE(kernel_create_info != nullptr && kernel_create_info->kernel_def != nullptr);
    if (kernel_create_info->kernel_def->IsInputOnCpu(input_index))
      // weights are not output from any node, so it's OK to put its location on CPU provider
      return execution_providers_.GetDefaultCpuMemoryInfo();
    return p_provider->GetAllocator(0, OrtMemTypeDefault)->Info();
  }

  Status GeneratePlanForWeights() {
    auto& weights = graph_viewer_.GetAllInitializedTensors();
    std::vector<std::vector<OrtMemoryInfo>> locations(plan_.allocation_plan.size());
    for (auto& node : graph_viewer_.Nodes()) {
      ORT_RETURN_IF_ERROR(onnxruntime::Node::ForEachWithIndex(
          node.InputDefs(), [this, &locations, &node, &weights](const onnxruntime::NodeArg& def, size_t index) {
            try {
              auto& def_name = def.Name();
              if (!weights.count(def_name)) return Status::OK();
              auto wt_index = Index(def_name);
              locations[wt_index].emplace_back(GetLocationForNodeInput(index, node));
            } catch (std::exception& ex) {
              return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, ex.what());
            }
            return Status::OK();
          }));
    }
    for (size_t i = 0; i != locations.size(); ++i) {
      const std::vector<OrtMemoryInfo>& loc = locations[i];
      if (loc.empty()) continue;
      plan_.allocation_plan[i].alloc_kind = AllocKind::kAllocateStatically;
      plan_.allocation_plan[i].location = loc[0];
      for (size_t j = 0; j != loc.size(); ++j) {
        if (loc[j] != loc[0]) {
          // set the location to CPU
          plan_.allocation_plan[i].location = execution_providers_.GetDefaultCpuMemoryInfo();
          break;
        }
      }
    }
    return Status::OK();
  }

  // Should only be used after ProcessDef()
  Status ComputeReusePlan() {
    std::vector<SequentialExecutionPlan::NodeExecutionPlan>& execution_plan{plan_.execution_plan};

    // Identify allocation/deallocation plan for every ml-value

    auto setup_preexisting = [this](const NodeArg* node_arg) {
      auto input_index = Index(node_arg->Name());
      AllocPlanPerValue& thisplan = AllocPlan(input_index);
      thisplan.alloc_kind = AllocKind::kPreExisting;
      thisplan.value_type = utils::GetMLDataType(*node_arg);
    };

    // inputs of the graph:
    // An input ml-value's data is owned by the caller (of InferenceSession::Run())
    // It must be allocated by the caller, and will not be reused during inference.
    for (auto graph_input : graph_viewer_.GetInputs()) {
      setup_preexisting(graph_input);
    }

    // outer scope node args are treated the same as graph inputs
    for (auto outer_scope_node_arg : outer_scope_node_args_) {
      setup_preexisting(outer_scope_node_arg);
    }

    // set AllocationInfo for each weight
    ORT_RETURN_IF_ERROR(GeneratePlanForWeights());

    for (size_t program_counter = 0; program_counter < execution_plan.size(); ++program_counter) {
      SequentialExecutionPlan::NodeExecutionPlan step = execution_plan[program_counter];
      auto pnode = graph_viewer_.GetNode(step.node_index);
      // graph outputs
      auto& graph_outputs = graph_viewer_.GetOutputs();
      // determine allocation for outputs of pnode
      int output_arg_num = 0;
      for (auto node_output : pnode->OutputDefs()) {
        if (!node_output->Exists()) continue;
        auto current = Index(node_output->Name());
        AllocPlan(current).value_type = utils::GetMLDataType(*node_output);
        OrtValueIndex reused;
        if (std::find(graph_outputs.begin(), graph_outputs.end(), node_output) != graph_outputs.end()) {
          // node_output is graph's output, so we can't reuse intermediate buffer
          AllocPlan(current).alloc_kind = AllocKind::kAllocateOutput;

          // hacky perf optimization to not copy a pre-existing value to an output if this is a Loop subgraph.
          // ideally this is temporary, and a future ONNX change to allow empty variadic inputs means we don't
          // have converted models that unnecessarily add loop state variables. if the value is just being
          // passed through an implicit input should be used instead.
          if (parent_node_ && pnode->OpType() == "Identity" && parent_node_->OpType() == "Loop") {
            const auto& input_name = pnode->InputDefs()[0]->Name();
            const auto input_index = Index(input_name);
            const auto& alloc_plan = AllocPlan(input_index);
            if (alloc_plan.alloc_kind == AllocKind::kPreExisting) {
              Reuse(input_index, current, AllocKind::kShare);
            }
          }
        } else if (IsNonTensor(*node_output)) {
          // we do not try sharing-optimization for non-tensors
          AllocPlan(current).alloc_kind = AllocKind::kAllocate;
        } else if (FindReusableInput(*pnode, output_arg_num, &reused)) {
          // Reuse one of this node's input buffers as the output buffer (for in-place update)
          Reuse(reused, current, AllocKind::kReuse);
        } else if (!context_.IsParallelExecutionEnabled() && FindReusableTensor(*node_output, &reused)) {
          // Reuse an available (dead) buffer for this output, this is only for sequential execution.
          Reuse(reused, current, AllocKind::kReuse);
        } else {
          // otherwise: allocate a new buffer for this output
          AllocPlan(current).alloc_kind = AllocKind::kAllocate;
        }
        output_arg_num++;
      }
      // determine if inputs of *pnode can be freed:
      for (auto node_input : pnode->InputDefs()) {
        if (node_input->Exists()) {
          auto& sym = node_input->Name();
          auto original = Buffer(Index(sym));
          if (0 == --UseCount(original))
            freelist_.push_front(FreeBufferInfo(original, program_counter));
        }
      }

      for (auto node_input : pnode->ImplicitInputDefs()) {
        if (node_input->Exists()) {
          auto& sym = node_input->Name();
          auto original = Buffer(Index(sym));
          if (0 == --UseCount(original))
            freelist_.push_front(FreeBufferInfo(original, program_counter));
        }
      }

      // determine if any outputs of *pnode are unused and can be freed:
      for (auto node_output : pnode->OutputDefs()) {
        if (node_output->Exists()) {
          auto& sym = node_output->Name();
          auto original = Buffer(Index(sym));
          if (0 == --UseCount(original))
            freelist_.push_front(FreeBufferInfo(original, program_counter));
        }
      }
    }
    return Status::OK();
  }

  // Whether a given NodeArg has fence or not.
  // If the buffer is reused, need to check whether original OrtValue has fence or not.
  bool HasFence(const onnxruntime::NodeArg* arg) {
    bool has_fence = false;
    if (arg && arg->Exists()) {
      OrtValueIndex index = Index(arg->Name());
      AllocPlanPerValue& value_plan = AllocPlan(index);

      has_fence = value_plan.create_fence_if_async;
      if (value_plan.alloc_kind == AllocKind::kReuse) {
        // Buffer reused, check original buffer to see if fence is shared.
        has_fence = has_fence || AllocPlan(value_plan.reused_buffer).create_fence_if_async;
      }
    }

    return has_fence;
  }

  // Compute fence check. Set has_fence flag if either one of inputs, implicit inputs or outputs of a given node has fence.
  Status ComputeFenceCheck() {
    for (SequentialExecutionPlan::NodeExecutionPlan& step : plan_.execution_plan) {
      auto pnode = graph_viewer_.GetNode(step.node_index);
      if (pnode == nullptr) return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the node ", step.node_index);

      bool has_fence = false;
      for (auto node_input : pnode->InputDefs()) {
        has_fence = has_fence || HasFence(node_input);
      }

      for (auto node_input : pnode->ImplicitInputDefs()) {
        has_fence = has_fence || HasFence(node_input);
      }

      for (auto node_output : pnode->OutputDefs()) {
        has_fence = has_fence || HasFence(node_output);
      }

      plan_.node_has_fence[step.node_index] = has_fence;
    }

    return Status::OK();
  }

  // Convert information in a freelist (about which ml-value becomes free when) into
  // a deallocation plan in the format required in an ExecutionPlan
  void GenerateDeallocationPlan() {
    // Store (indices of) ml-values to be freed in plan->to_be_freed
    // Set plan->execution_plan[n].free_from_index/free_to_index for every n that must free some ml-value.

    plan_.to_be_freed.reserve(freelist_.size());
    bool has_prev_dealloc_point = false;
    size_t prev_dealloc_point = 0;
    //TODO: should be size_t
    int current = 0;  // current index into the to_be_freed vector

    // Copy all items from freelist to to_be_freed in reverse order
    for (auto it = freelist_.rbegin(), end = freelist_.rend(); it != end; ++it) {
      plan_.to_be_freed.push_back(it->ml_value);
      //
      if (it->deallocate_point != prev_dealloc_point) {
        if (has_prev_dealloc_point)
          plan_.execution_plan[prev_dealloc_point].free_to_index = current - 1;
        prev_dealloc_point = it->deallocate_point;
        has_prev_dealloc_point = true;
        plan_.execution_plan[prev_dealloc_point].free_from_index = current;
      }
      current++;
    }

    if (has_prev_dealloc_point)
      plan_.execution_plan[prev_dealloc_point].free_to_index = current - 1;
  }

  static bool IsNonTensor(const onnxruntime::NodeArg& nodearg) {
    // TODO: unclear why we should go through a string-representation of type
    auto ptype = nodearg.Type();
    auto& type_proto = ONNX_NAMESPACE::Utils::DataTypeUtils::ToTypeProto(ptype);
    return !utils::HasTensorType(type_proto);
  }
};  // namespace onnxruntime

Status PlannerImpl::CreatePlan() {
  auto& p_graph_nodes = graph_viewer_.GetNodesInTopologicalOrder();

  int num_ml_values = ort_value_name_idx_map_.MaxIdx() + 1;

  Initialize(p_graph_nodes.size(), static_cast<size_t>(num_ml_values));

  // Determine execution order: we use the default topological sort order for now. We can later
  // explore more efficient orderings (from a memory usage perspective).
  for (auto n : p_graph_nodes) {
    plan_.execution_plan.emplace_back(n);
  }

  // compute use counts for all ml-values
  ORT_RETURN_IF_ERROR(ComputeUseCounts());

  // determine sharing/reuse among ml-values
  ORT_RETURN_IF_ERROR(ComputeReusePlan());

  // Determine nodes that need fence check. This needs to be done after ComputeUseCounts and ComputeReusePlan.
  ORT_RETURN_IF_ERROR(ComputeFenceCheck());

  // convert information in the freelist_ into a deallocation plan in required format
  GenerateDeallocationPlan();

  return Status::OK();
}

Status SequentialPlanner::CreatePlan(const Node* parent_node, const onnxruntime::GraphViewer& graph_viewer,
                                     const std::vector<const NodeArg*>& outer_scope_node_args,
                                     const ExecutionProviders& providers, const KernelRegistryManager& kernel_registry,
                                     const OrtValueNameIdxMap& ort_value_name_idx_map,
                                     const ISequentialPlannerContext& context,
                                     std::unique_ptr<SequentialExecutionPlan>& plan) {
  // allocate/reset here so we know it's clean
  plan = std::make_unique<SequentialExecutionPlan>();

  PlannerImpl planner(parent_node, graph_viewer, outer_scope_node_args, providers, kernel_registry,
                      ort_value_name_idx_map, context, *plan);

  return planner.CreatePlan();
}

}  // namespace onnxruntime
