// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/parallel_executor.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include "core/common/common.h"
#include "core/common/logging/logging.h"
#include "core/framework/allocation_planner.h"
#include "core/framework/execution_frame.h"
#include "core/framework/session_state.h"
#include "core/framework/op_kernel_context_internal.h"
#include "core/framework/utils.h"
#include "core/platform/threadpool.h"

namespace onnxruntime {

ParallelExecutor::ParallelExecutor(const SessionState& session_state, const bool& terminate_flag)
    : out_standings_(0), terminate_flag_{terminate_flag} {
  auto graph_viewer = session_state.GetGraphViewer();
  node_refs_.resize(graph_viewer->MaxNodeIndex());
  for (auto& node : graph_viewer->Nodes()) {
    node_refs_[node.Index()] = node.GetInputEdgesCount();
  }

  executor_pool_ = std::make_unique<onnxruntime::concurrency::ThreadPool>("EXECUTOR", 32);
}

Status ParallelExecutor::Execute(const SessionState& session_state, const std::vector<int>& feed_mlvalue_idxs,
                                 const std::vector<OrtValue>& feeds, const std::vector<int>& fetch_mlvalue_idxs,
                                 std::vector<OrtValue>& fetches,
                                 const std::unordered_map<size_t, CustomAllocator>& fetch_allocators,
                                 const logging::Logger& logger) {
  TimePoint tp;
  const bool is_profiler_enabled = session_state.Profiler().IsEnabled();
  if (is_profiler_enabled) {
    tp = session_state.Profiler().StartTime();
  }

  root_frame_ = std::make_unique<ExecutionFrame>(feed_mlvalue_idxs, feeds, fetch_mlvalue_idxs, fetches,
                                                 fetch_allocators, session_state);
  //std::cout << "start nodes:" << std::endl;
  for (auto node_index : session_state.GetGraphViewer()->GetRootNodes()) {
    auto p_op_kernel = session_state.GetKernel(node_index);
    if (!p_op_kernel)
      continue;

    //std::cout << "\t" << p_op_kernel->Node().Name() << std::endl;
    EnqueueNode(node_index, session_state, logger);
  }

  // Wait for finish.
  {
    std::unique_lock<OrtMutex> lock(complete_mutex_);
    while (out_standings_ > 0) complete_cv_.wait(lock);
  }

  Status status = Status::OK();

  if (!errors_.empty()) {
    if (errors_.size() == 1)
      status = errors_.front();
    else {
      std::stringstream ss;
      ss << "Multiple errors were found.";
      for (const auto& s : errors_) {
        ss << '\n'
           << s;
      }

      status = ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, ss.str());
    }

    LOGS(logger, ERROR) << status;
    return status;
  }

  VLOGS(logger, 1) << "Fetching output.";
  // ExecutionFrame::Finalize will update 'fetches' with the final output
  ORT_RETURN_IF_ERROR(root_frame_->GetOutputs(fetches));
  VLOGS(logger, 1) << "Done execution.";

  if (root_frame_->HasMemoryPatternPlanner()) {
    std::vector<std::reference_wrapper<const TensorShape>> input_shapes;
    bool all_tensors = true;
    for (const auto& feed : feeds) {
      if (!(feed.IsTensor())) {
        all_tensors = false;
        break;
      }
      auto& tensor = feed.Get<Tensor>();
      input_shapes.push_back(std::cref(tensor.Shape()));
    }

    if (all_tensors) {
      auto mem_patterns = std::make_unique<MemoryPatternGroup>();
      ORT_RETURN_IF_ERROR(root_frame_->GeneratePatterns(mem_patterns.get()));
      ORT_RETURN_IF_ERROR(session_state.UpdateMemoryPatternGroupCache(input_shapes, std::move(mem_patterns)));
    }
  }

  if (is_profiler_enabled) {
    session_state.Profiler().EndTimeAndRecordEvent(profiling::SESSION_EVENT, "ParallelExecutor::Execute", tp);
  }

  return Status::OK();
}

Status ParallelExecutor::RunNodeAsync(size_t p_node_index,
                                      const SessionState& session_state,
                                      const logging::Logger& logger) {
  LOGS(logger, INFO) << "Begin execution";

  Status status = Status::OK();

  size_t node_index = p_node_index;
  bool keep_running = true;
  auto graph_viewer = session_state.GetGraphViewer();
  TimePoint sync_time_begin;
  TimePoint kernel_begin_time;
  const bool f_profiler_enabled = session_state.Profiler().IsEnabled();
  const SequentialExecutionPlan& exec_plan = *session_state.GetExecutionPlan();

  // Avoid context switching if possible.
  while (keep_running) {
    // TODO: Convert RunNodeAsync return Status.
    // to also handle exception propagation
    if (terminate_flag_) {
      LOGS(logger, WARNING) << "Exiting due to terminate flag being set to true.";
      ORT_THROW("Exiting due to terminate flag being set to true.");
    }

    auto p_op_kernel = session_state.GetKernel(node_index);

    // if a kernel has been added in the session state, it better be NON-null.
    if (p_op_kernel == nullptr) {
      ORT_THROW("Got nullptr from GetKernel for node: ",
                graph_viewer->GetNode(node_index)->Name());
    }

    OpKernelContextInternal op_kernel_context(session_state, *root_frame_, *p_op_kernel, logger, terminate_flag_);

    if (f_profiler_enabled) {
      sync_time_begin = session_state.Profiler().StartTime();
    }
    // sync before compute
    int queue_id = p_op_kernel->KernelDef().ExecQueueId();
    if (exec_plan.NodeHasFence(node_index)) {
      for (int input_index = 0; input_index < op_kernel_context.InputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.InputFence(input_index);
        if (fence) {
          auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
          if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
            execution_provider_type = kCpuExecutionProvider;
          }
          fence->BeforeUsingAsInput(execution_provider_type, queue_id);
        }
      }

      for (int input_index = 0; input_index < op_kernel_context.ImplicitInputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.ImplicitInputFence(input_index);
        if (fence) {
          auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
          if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
            execution_provider_type = kCpuExecutionProvider;
          }
          fence->BeforeUsingAsInput(execution_provider_type, queue_id);
        }
      }

      for (int output_index = 0; output_index < op_kernel_context.OutputCount(); ++output_index) {
        Fence_t fence = op_kernel_context.OutputFence(output_index);
        if (fence) {
          fence->BeforeUsingAsOutput(p_op_kernel->Node().GetExecutionProviderType(), queue_id);
        }
      }
    }

    if (f_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     p_op_kernel->Node().Name() + "_fence_before",
                                                     sync_time_begin,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}});

      kernel_begin_time = session_state.Profiler().StartTime();
    }

    // call compute on the kernel
    VLOGS(logger, 1) << "Computing kernel: " << p_op_kernel->Node().Name();

    // Execute the kernel.
    status = p_op_kernel->Compute(&op_kernel_context);
    if (!status.IsOK()) {
      status = ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                               "Compute failed for node: ", graph_viewer->GetNode(node_index)->Name(),
                               "\nError:", status);
      break;
    }

    if (f_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     p_op_kernel->Node().Name() + "_kernel_time",
                                                     kernel_begin_time,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}, {"provider", p_op_kernel->KernelDef().Provider()}});

      sync_time_begin = session_state.Profiler().StartTime();
    }
    // sync after compute for outputs
    if (exec_plan.NodeHasFence(node_index)) {
      for (int input_index = 0; input_index < op_kernel_context.InputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.InputFence(input_index);
        if (fence) {
          fence->AfterUsedAsInput(queue_id);
        }
      }

      for (int input_index = 0; input_index < op_kernel_context.ImplicitInputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.ImplicitInputFence(input_index);
        if (fence) {
          fence->AfterUsedAsInput(queue_id);
        }
      }

      for (int output_index = 0; output_index < op_kernel_context.OutputCount(); ++output_index) {
        Fence_t fence = op_kernel_context.OutputFence(output_index);
        if (fence) {
          fence->AfterUsedAsOutput(queue_id);
        }
      }
    }

    if (f_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     p_op_kernel->Node().Name() + "_fence_after",
                                                     sync_time_begin,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}});
    }

    //std::cout << "Run async node finish: " << p_node_index << std::endl;

    keep_running = false;

    // Checking which output nodes ready for running.
    {
      auto begin = p_op_kernel->Node().OutputEdgesBegin();
      auto end = p_op_kernel->Node().OutputEdgesEnd();

      std::lock_guard<OrtMutex> lock(ref_mutex_);
      for (auto it = begin; it != end; it++) {
        auto idx = (*it).GetNode().Index();
        if ((--node_refs_[idx]) == 0) {
          if (!keep_running) {
            node_index = idx;
            keep_running = true;
          } else {
            EnqueueNode(idx, session_state, logger);
          }
        }

        // std::cout << "handle output, current name: " << p_op_kernel->Node().Name() << ", current index: "
        // << p_node_index << ", output name: " << (*it)->GetNode().Name() << ", output index: "
        // << (*it)->GetNode().Index() << ", after -- output ref: " << node_refs_[idx] << std::endl;
      }
    }
  }

  return status;
}

void ParallelExecutor::EnqueueNode(size_t p_node_index, const SessionState& session_state, const logging::Logger& logger) {
  {
    std::unique_lock<OrtMutex> lock(complete_mutex_);
    // if there are errors there's no point queuing more work
    if (!errors_.empty())
      return;

    out_standings_++;
  }

  executor_pool_->Schedule([this, p_node_index, &session_state, &logger]() {
    auto create_exception_message = [p_node_index, &session_state](const std::exception* ex) {
      const auto* node = session_state.GetGraphViewer()->GetNode(p_node_index);

      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Exception running nodes starting at ", node->OpType(),
                             " node '", node->Name(), "'. ",
                             ex ? ex->what() : "Unknown exception was caught by catch-all handler.");
    };

    Status status;
    try {
      status = ParallelExecutor::RunNodeAsync(p_node_index, std::cref(session_state), std::cref(logger));
    } catch (const std::exception& ex) {
      status = create_exception_message(&ex);
    } catch (...) {
      // catch node processing failure exceptions here to prevent app crash.
      status = create_exception_message(nullptr);
    }

    FinishNodeRun(status);
  });
}
}  // namespace onnxruntime
