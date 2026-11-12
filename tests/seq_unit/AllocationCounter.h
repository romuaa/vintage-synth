#pragma once

// Allocation counter for the real-time test (same technique as tests/dsp_unit/EventTests.cpp).
// macOS only: libmalloc calls malloc_logger (if set) for every allocation, whatever the entry
// point (malloc, calloc, realloc, operator new). Only allocations on the thread that starts the
// counter are counted. AddressSanitizer replaces malloc and bypasses the hook, so the test is
// skipped there (and on platforms without the hook).

#include <atomic>
#include <cstdint>

#if defined(__has_feature)
 #if __has_feature(address_sanitizer)
  #define VS_SEQ_TEST_ASAN 1
 #endif
#endif
#if defined(__SANITIZE_ADDRESS__)
 #define VS_SEQ_TEST_ASAN 1
#endif

#if defined(__APPLE__) && ! defined(VS_SEQ_TEST_ASAN)
 #define VS_SEQ_COUNT_ALLOCATIONS 1
 #include <pthread.h>

using SeqMallocLogger = void (std::uint32_t type, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint32_t);
extern "C" SeqMallocLogger* malloc_logger; // libmalloc debug hook (used by MallocStackLogging)

namespace vs::seq::test
{
inline std::atomic<int> gAllocations { 0 };
inline std::atomic<pthread_t> gCountingThread { nullptr };
inline void* volatile gEscape = nullptr; // keeps probe allocations from being optimised away

inline void countAllocation (std::uint32_t type, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint32_t)
{
    constexpr std::uint32_t kAllocate = 2; // MALLOC_LOG_TYPE_ALLOCATE
    if ((type & kAllocate) != 0 && pthread_equal (pthread_self(), gCountingThread.load()) != 0)
        gAllocations.fetch_add (1, std::memory_order_relaxed);
}

struct AllocationCounter
{
    AllocationCounter()
    {
        gAllocations = 0;
        gCountingThread = pthread_self();
        malloc_logger = countAllocation;
    }
    ~AllocationCounter()
    {
        malloc_logger = nullptr;
        gCountingThread = nullptr;
    }
    int count() const { return gAllocations.load(); }
};
} // namespace vs::seq::test
#else
 #define VS_SEQ_COUNT_ALLOCATIONS 0
#endif
