#pragma once
// Portable low-level helpers for latency work: core pinning. Pinning the
// benchmark thread keeps it off scheduler migration paths and (on Linux with
// isolcpus/nohz_full) away from OS jitter. Best-effort everywhere: returns
// false when the platform refuses, never aborts.
#include <cstdio>

#if defined(__linux__)
#include <pthread.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

namespace latencylab {

inline bool pin_current_thread_to_core(int core) {
  if (core < 0) return false;
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(__APPLE__)
  // Advisory only on Apple Silicon (E/P cores, no hard affinity), but still
  // reduces migration. Deprecated APIs intentionally avoided.
  thread_affinity_policy_data_t policy = {core};
  kern_return_t r = thread_policy_set(
      mach_thread_self(), THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
  return r == KERN_SUCCESS;
#else
  (void)core;
  return false;
#endif
}

}  // namespace latencylab
