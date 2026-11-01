/*****************************************************************/ /**
 * @file   clock_linux.cpp
 * @brief  Linux and Android clocks.
 *
 * awake: CLOCK_MONOTONIC, continuous: CLOCK_BOOTTIME, wall: CLOCK_REALTIME.
 * All three are read through the vDSO without a system call.
 *********************************************************************/
#include "internal.hpp"

#if !(OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID)
  #error "clock_linux.cpp is for Linux and Android only"
#endif

#include <errno.h>
#include <time.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      inline int64_t read_ns(clockid_t id) noexcept
      {
        struct timespec ts;
        ::clock_gettime(id, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000000
               + static_cast<int64_t>(ts.tv_nsec);
      }

      /// @return 0 on success, else errno
      int resolution_of(clockid_t id, int64_t* out) noexcept
      {
        struct timespec ts;
        if (::clock_getres(id, &ts) != 0)
          return errno;
        const int64_t ns = static_cast<int64_t>(ts.tv_sec) * 1000000000
                           + static_cast<int64_t>(ts.tv_nsec);
        *out             = ns > 0 ? ns : 1;
        return 0;
      }
    } // namespace

    awake_instant awake_now() noexcept
    {
      return {read_ns(CLOCK_MONOTONIC)};
    }
    continuous_instant continuous_now() noexcept
    {
      return {read_ns(CLOCK_BOOTTIME)};
    }
    wall_instant wall_now() noexcept
    {
      return {read_ns(CLOCK_REALTIME)};
    }

    namespace detail
    {
      status<initialize_result> platform_clock_initialize() noexcept
      {
        constexpr clockid_t IDS[3] = {
            CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_REALTIME};
        for (size_t i = 0; i < 3; ++i)
        {
          // Index i matches CLOCK_ID_AWAKE, CLOCK_ID_CONTINUOUS, CLOCK_ID_WALL
          const int err = resolution_of(IDS[i], &g_clock.resolution_ns[i]);
          if (err != 0)
            return {
                initialize_result::INITIALIZE_SYSTEM_ERROR,
                native_error_kind::NATIVE_ERROR_POSIX_ERRNO,
                static_cast<int32_t>(err)};
        }
        return {};
      }

      void platform_sleep_ns(int64_t ns) noexcept
      {
        struct timespec ts;
        ts.tv_sec  = static_cast<time_t>(ns / 1000000000);
        ts.tv_nsec = static_cast<long>(ns % 1000000000);
        // Relative sleep: on EINTR, ts holds the remaining time
        while (::clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR)
        {
        }
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
