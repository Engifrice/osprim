/*****************************************************************/ /**
 * @file   power_windows.cpp
 * @brief  Windows power and timing.
 *
 * - Timer resolution: timeBeginPeriod/timeEndPeriod (winmm), per process
 *   since Windows 10 2004.
 * - Activity:
 *   - latency_critical: process power throttling, explicitly off for
 *     EXECUTION_SPEED (high QoS) and IGNORE_TIMER_RESOLUTION (timer
 *     resolution honored while occluded). Process-wide, so counted: set by
 *     the first holder, given back to the system by the last.
 *   - keep_system_awake, keep_display_on: a power request object.
 * - Performance sessions, thermal: no public API (UNSUPPORTED).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "power_windows.cpp is for Windows only"
#endif

#include "win32.hpp"

#include <timeapi.h>

#include <atomic>

#if defined(_MSC_VER)
  #pragma comment(lib, "winmm.lib")
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      /*****************************/
      /* LATENCY CRITICAL COUNT    */
      /*****************************/

      std::atomic_flag g_latency_lock; // clear (C++20)
      uint32_t g_latency_holders = 0;  // guarded by g_latency_lock

      struct latency_lock
      {
        latency_lock() noexcept
        {
          while (g_latency_lock.test_and_set(std::memory_order_acquire))
            while (g_latency_lock.test(std::memory_order_relaxed))
              ::SwitchToThread();
        }
        ~latency_lock() { g_latency_lock.clear(std::memory_order_release); }
        latency_lock(const latency_lock&)            = delete;
        latency_lock& operator=(const latency_lock&) = delete;
      };

      /// @brief Sets the process power throttling (ControlMask, StateMask).
      bool set_process_throttling(ULONG control, ULONG state) noexcept
      {
        PROCESS_POWER_THROTTLING_STATE s{};
        s.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        s.ControlMask = control;
        s.StateMask   = state;
        return ::SetProcessInformation(
                   ::GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s))
               != FALSE;
      }

      /// @brief Takes a latency-critical reference.
      /// @return false if the OS refused (pre-1709 Windows 10)
      bool latency_acquire() noexcept
      {
        latency_lock lock;
        if (g_latency_holders == 0)
        {
          // Controlled and off: high QoS, and timer resolution always honored
          if (!set_process_throttling(
                  PROCESS_POWER_THROTTLING_EXECUTION_SPEED
                      | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
                  0))
            return false;
        }
        ++g_latency_holders;
        return true;
      }

      void latency_release() noexcept
      {
        latency_lock lock;
        if (--g_latency_holders == 0)
          set_process_throttling(0, 0); // the system decides again
      }
    } // namespace

    namespace detail
    {
      void platform_power_initialize(power_features* out) noexcept
      {
        // Process power throttling (Windows 10 1709+): reading it changes
        // nothing, and tells whether it exists
        PROCESS_POWER_THROTTLING_STATE s{};
        s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        out->activity_latency_critical =
            ::GetProcessInformation(
                ::GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s))
            != FALSE;
        // Power requests: Windows 7+
        out->activity_keep_system_awake = true;
        out->activity_keep_display_on   = true;
      }

      void platform_power_deinitialize() noexcept
      {
      }

      /*****************************/
      /* TIMER RESOLUTION          */
      /*****************************/

      status<timer_resolution_result> platform_timer_resolution_begin(
          int64_t period_ns, uint32_t* period_ms) noexcept
      {
        int64_t ms = period_ns / 1000000 + (period_ns % 1000000 != 0 ? 1 : 0);
        TIMECAPS caps{};
        if (::timeGetDevCaps(&caps, sizeof(caps)) == MMSYSERR_NOERROR)
        {
          if (ms < static_cast<int64_t>(caps.wPeriodMin))
            ms = caps.wPeriodMin;
          if (ms > static_cast<int64_t>(caps.wPeriodMax))
            ms = caps.wPeriodMax;
        }
        const MMRESULT r = ::timeBeginPeriod(static_cast<UINT>(ms));
        if (r != TIMERR_NOERROR)
          return {
              timer_resolution_result::TIMER_RESOLUTION_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, static_cast<int32_t>(r)};
        *period_ms = static_cast<uint32_t>(ms);
        return {};
      }

      void platform_timer_resolution_end(uint32_t period_ms) noexcept
      {
        // Must match timeBeginPeriod exactly
        ::timeEndPeriod(period_ms);
      }

      /*****************************/
      /* ACTIVITY                  */
      /*****************************/

      status<activity_result> platform_activity_begin(
          const activity_desc& desc, uint64_t* native,
          activity_desc* granted) noexcept
      {
        DWORD error = 0;

        if (desc.keep_system_awake || desc.keep_display_on)
        {
          // The reason is copied by the system
          wchar_t reason[ACTIVITY_REASON_MAX + 1];
          if (::MultiByteToWideChar(
                  CP_UTF8, 0, desc.reason, -1, reason,
                  static_cast<int>(ACTIVITY_REASON_MAX + 1))
              == 0)
            reason[0] = L'\0';
          REASON_CONTEXT ctx{};
          ctx.Version                   = POWER_REQUEST_CONTEXT_VERSION;
          ctx.Flags                     = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
          ctx.Reason.SimpleReasonString = reason;
          const HANDLE request          = ::PowerCreateRequest(&ctx);
          if (request == INVALID_HANDLE_VALUE)
            error = ::GetLastError();
          else
          {
            if (desc.keep_system_awake)
            {
              if (::PowerSetRequest(request, PowerRequestSystemRequired))
                granted->keep_system_awake = true;
              else
                error = ::GetLastError();
            }
            if (desc.keep_display_on)
            {
              if (::PowerSetRequest(request, PowerRequestDisplayRequired))
                granted->keep_display_on = true;
              else
                error = ::GetLastError();
            }
            if (granted->keep_system_awake || granted->keep_display_on)
              *native = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(request));
            else
              ::CloseHandle(request);
          }
        }

        if (desc.latency_critical && g_power_features.activity_latency_critical)
        {
          if (latency_acquire())
            granted->latency_critical = true;
          else
            error = ::GetLastError();
        }

        if (granted->latency_critical || granted->keep_system_awake
            || granted->keep_display_on)
          return {};
        // Not implemented here (stubs, reduced editions): unsupported, not a failure
        if (error == ERROR_CALL_NOT_IMPLEMENTED || error == ERROR_NOT_SUPPORTED
            || error == ERROR_INVALID_FUNCTION)
          error = 0;
        if (error != 0)
          return {
              activity_result::ACTIVITY_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_WIN32, static_cast<int32_t>(error)};
        return {
            activity_result::ACTIVITY_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      void platform_activity_end(
          uint64_t native, const activity_desc& granted) noexcept
      {
        if (native != 0)
        {
          const HANDLE request =
              reinterpret_cast<HANDLE>(static_cast<uintptr_t>(native));
          if (granted.keep_system_awake)
            ::PowerClearRequest(request, PowerRequestSystemRequired);
          if (granted.keep_display_on)
            ::PowerClearRequest(request, PowerRequestDisplayRequired);
          ::CloseHandle(request);
        }
        if (granted.latency_critical)
          latency_release();
      }

      /*****************************/
      /* UNSUPPORTED               */
      /*****************************/

      status<perf_session_result> platform_perf_session_create(
          const uint64_t*, size_t, int64_t, uint64_t*) noexcept
      {
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      status<perf_session_result> platform_perf_session_set_target(
          uint64_t, int64_t) noexcept
      {
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      status<perf_session_result> platform_perf_session_report(
          uint64_t, int64_t) noexcept
      {
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      void platform_perf_session_close(uint64_t) noexcept
      {
      }

      status<thermal_result> platform_thermal_level(thermal_level*) noexcept
      {
        return {
            thermal_result::THERMAL_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      status<thermal_result> platform_thermal_watch_begin(
          std::atomic<wait_word_t>&, uint64_t*, int32_t*) noexcept
      {
        return {
            thermal_result::THERMAL_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      void platform_thermal_watch_end(uint64_t, int32_t) noexcept
      {
      }

      status<thermal_result> platform_thermal_headroom(int64_t, float*) noexcept
      {
        return {
            thermal_result::THERMAL_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
