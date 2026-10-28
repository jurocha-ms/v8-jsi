// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include "V8JsiRuntime.h"
#include <cstdint>

namespace v8runtime {

// Process-wide counters. Take deltas in an isolated test process, with no other
// runtime workloads running concurrently. Freed counts follow operator delete.
struct HostProxyLifetimeStats {
  uint64_t proxiesAllocated;
  uint64_t proxiesFreed;
  uint64_t trackersAllocated;
  uint64_t trackersFreed;
};

#if defined(V8JSI_ENABLE_LIFETIME_TESTS)
extern "C" V8JSI_EXPORT void __cdecl v8jsi_get_host_proxy_lifetime_stats(HostProxyLifetimeStats *stats) noexcept;
extern "C" V8JSI_EXPORT facebook::jsi::Runtime *__cdecl v8jsi_create_runtime_for_lifetime_test();
#endif

} // namespace v8runtime
