/*****************************************************************/ /**
 * @file   clock_apple.cpp
 * @brief  macOS, iOS, tvOS and visionOS clocks.
 *
 * awake: mach_absolute_time (excludes sleep).
 * continuous: mach_continuous_time (includes sleep).
 * wall: CLOCK_REALTIME.
 * Mach ticks are converted with the timebase, with fast paths for the two
 * timebases in use: 1/1 (Intel) and 125/3 (Apple silicon, 24 MHz).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_APPLE
  #error "clock_apple.cpp is for Apple platforms only"
#endif

#include <errno.h>
#include <mach/mach_time.h>
#include <time.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      using detail::g_clock;

      inline int64_t mach_to_ns(uint64_t t) noexcept
      {
        const uint32_t numer = g_clock.mach_numer;
        const uint32_t denom = g_clock.mach_denom;
        if (numer == denom)
          return static_cast<int64_t>(t);
        if (numer == 125
            && denom == 3) // t * 125 overflows after ~194 years of uptime
          return static_cast<int64_t>(t * 125 / 3);
        // Split to avoid overflowing t * numer
        return static_cast<int64_t>(
            (t / denom) * numer + (t % denom) * numer / denom);
      }
    } // namespace

    awake_instant awake_now() noexcept
    {
      return {mach_to_ns(::mach_absolute_time())};
    }
    continuous_instant continuous_now() noexcept
    {
      return {mach_to_ns(::mach_continuous_time())};
    }
    wall_instant wall_now() noexcept
    {
      return {static_cast<int64_t>(::clock_gettime_nsec_np(CLOCK_REALTIME))};
    }

    namespace detail
    {
      bool awake_ns_to_mach_ceil(int64_t ns, uint64_t* ticks) noexcept
      {
        const uint64_t n     = static_cast<uint64_t>(ns);
        const uint64_t numer = g_clock.mach_numer;
        const uint64_t denom = g_clock.mach_denom;
        // ticks = ceil(n * denom / numer), split to avoid overflowing n * denom
        const uint64_t q = n / numer;
        const uint64_t r = n % numer;
        if (q > (UINT64_MAX - denom) / denom)
          return false;
        *ticks = q * denom + (r * denom + numer - 1) / numer;
        return true;
      }

      status<initialize_result> platform_clock_initialize() noexcept
      {
        mach_timebase_info_data_t tb{};
        const kern_return_t kr = ::mach_timebase_info(&tb);
        if (kr != KERN_SUCCESS)
          return {
              initialize_result::INITIALIZE_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_MACH_KERN, static_cast<int32_t>(kr)};
        if (tb.numer == 0 || tb.denom == 0)
          return {
              initialize_result::INITIALIZE_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        g_clock.mach_numer = tb.numer;
        g_clock.mach_denom = tb.denom;

        // One mach tick, rounded up
        const int64_t mach_res =
            static_cast<int64_t>((tb.numer + tb.denom - 1) / tb.denom);
        g_clock.resolution_ns[static_cast<size_t>(clock_id::CLOCK_ID_AWAKE)] =
            mach_res;
        g_clock.resolution_ns[static_cast<size_t>(clock_id::CLOCK_ID_CONTINUOUS)] =
            mach_res;

        struct timespec ts;
        if (::clock_getres(CLOCK_REALTIME, &ts) != 0)
          return {
              initialize_result::INITIALIZE_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_POSIX_ERRNO,
              static_cast<int32_t>(errno)};
        const int64_t wall_res = static_cast<int64_t>(ts.tv_sec) * 1000000000
                                 + static_cast<int64_t>(ts.tv_nsec);
        g_clock.resolution_ns[static_cast<size_t>(clock_id::CLOCK_ID_WALL)] =
            wall_res > 0 ? wall_res : 1;
        return {};
      }

      void platform_sleep_ns(int64_t ns) noexcept
      {
        struct timespec ts;
        ts.tv_sec  = static_cast<time_t>(ns / 1000000000);
        ts.tv_nsec = static_cast<long>(ns % 1000000000);
        while (::nanosleep(&ts, &ts) != 0 && errno == EINTR)
        {
        }
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
