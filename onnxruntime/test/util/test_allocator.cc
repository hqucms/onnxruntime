// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "test_allocator.h"

MockedOrtAllocator::MockedOrtAllocator() {
  OrtAllocator::version = ORT_API_VERSION;
  OrtAllocator::Alloc = [](OrtAllocator* this_, size_t size) { return static_cast<MockedOrtAllocator*>(this_)->Alloc(size); };
  OrtAllocator::Free = [](OrtAllocator* this_, void* p) { static_cast<MockedOrtAllocator*>(this_)->Free(p); };
  OrtAllocator::Info = [](const OrtAllocator* this_) { return static_cast<const MockedOrtAllocator*>(this_)->Info(); };
  ORT_THROW_ON_ERROR(OrtCreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &cpu_memory_info));
}

MockedOrtAllocator::~MockedOrtAllocator() {
  OrtReleaseMemoryInfo(cpu_memory_info);
}

void* MockedOrtAllocator::Alloc(size_t size) {
  constexpr size_t extra_len = sizeof(size_t);
  memory_inuse.fetch_add(size += extra_len);
  void* p = ::malloc(size);
  *(size_t*)p = size;
  return (char*)p + extra_len;
}

void MockedOrtAllocator::Free(void* p) {
  constexpr size_t extra_len = sizeof(size_t);
  if (!p) return;
  p = (char*)p - extra_len;
  size_t len = *(size_t*)p;
  memory_inuse.fetch_sub(len);
  return ::free(p);
}

const OrtMemoryInfo* MockedOrtAllocator::Info() const {
  return cpu_memory_info;
}

void MockedOrtAllocator::LeakCheck() {
  if (memory_inuse.load())
    throw std::runtime_error("memory leak!!!");
}
