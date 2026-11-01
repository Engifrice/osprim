/*****************************************************************/ /**
 * @file   power_linux.cpp
 * @brief  Linux and Android power and timing.
 *
 * - Timer resolution: already precise (timer slack of 50 us): nothing to do.
 * - Activity: nothing here (Linux: systemd-logind over D-Bus; Android:
 *   window flags, in the engine's UI layer).
 * - Android only, resolved at run time from libandroid.so so that the
 *   minimum API level stays 26:
 *   - performance hint sessions (APerformanceHint_*, API 33),
 *   - thermal status and listener (AThermal_*, API 30), headroom (API 31).
 *********************************************************************/
#include "internal.hpp"

#if !(OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID)
  #error "power_linux.cpp is for Linux and Android only"
#endif

#if OSPRIM_OS_ANDROID
  #include <dlfcn.h>
  #include <errno.h>
  #include <math.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
#if OSPRIM_OS_ANDROID
    namespace
    {
      // The NDK headers are not included: their declarations carry API
      // levels above our minimum. These match the documented signatures.
      using hint_manager_t    = void;
      using hint_session_t    = void;
      using thermal_manager_t = void;

      struct android_api
      {
        void* lib;

        hint_manager_t* (*hint_get_manager)();
        int64_t (*hint_get_preferred_rate)(hint_manager_t*);
        hint_session_t* (*hint_create_session)(
            hint_manager_t*, const int32_t*, size_t, int64_t);
        int (*hint_update_target)(hint_session_t*, int64_t);
        int (*hint_report_actual)(hint_session_t*, int64_t);
        void (*hint_close_session)(hint_session_t*);
        hint_manager_t* hint_manager;

        thermal_manager_t* (*thermal_acquire)();
        void (*thermal_release)(thermal_manager_t*);
        int (*thermal_get_status)(thermal_manager_t*);
        int (*thermal_register)(thermal_manager_t*, void (*)(void*, int), void*);
        int (*thermal_unregister)(thermal_manager_t*, void (*)(void*, int), void*);
        float (*thermal_get_headroom)(thermal_manager_t*, int);
        thermal_manager_t* thermal_manager;
      };

      android_api g_api{};

      template<class F>
      void resolve(F& fn, const char* name) noexcept
      {
        void* p = ::dlsym(g_api.lib, name);
        // Object to function pointer: conditionally-supported, fine on POSIX
        fn = reinterpret_cast<F>(p);
      }

      // AThermalStatus values
      constexpr int ATHERMAL_STATUS_ERROR    = -1;
      constexpr int ATHERMAL_STATUS_LIGHT    = 1;
      constexpr int ATHERMAL_STATUS_MODERATE = 2;
      constexpr int ATHERMAL_STATUS_SEVERE   = 3;

      thermal_level map_status(int status) noexcept
      {
        if (status >= ATHERMAL_STATUS_SEVERE + 1) // CRITICAL, EMERGENCY, SHUTDOWN
          return thermal_level::THERMAL_LEVEL_CRITICAL;
        if (status == ATHERMAL_STATUS_SEVERE)
          return thermal_level::THERMAL_LEVEL_SERIOUS;
        if (status == ATHERMAL_STATUS_LIGHT || status == ATHERMAL_STATUS_MODERATE)
          return thermal_level::THERMAL_LEVEL_FAIR;
        return thermal_level::THERMAL_LEVEL_NOMINAL;
      }

      /// @brief Thermal listener: the data is the watch's counter
      void on_thermal_status(void* data, int) noexcept
      {
        auto* counter = static_cast<std::atomic<wait_word_t>*>(data);
        counter->fetch_add(1, std::memory_order_release);
        wake_all(*counter);
      }

      status<perf_session_result> hint_error(int err) noexcept
      {
        return {
            perf_session_result::PERF_SESSION_SYSTEM_ERROR,
            native_error_kind::NATIVE_ERROR_POSIX_ERRNO, static_cast<int32_t>(err)};
      }
    } // namespace
#endif // OSPRIM_OS_ANDROID

    namespace detail
    {
      void platform_power_initialize(power_features* out) noexcept
      {
        (void)out;
#if OSPRIM_OS_ANDROID
        g_api     = android_api{};
        g_api.lib = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (g_api.lib == nullptr)
          return;

        resolve(g_api.hint_get_manager, "APerformanceHint_getManager");
        resolve(
            g_api.hint_get_preferred_rate,
            "APerformanceHint_getPreferredUpdateRateNanos");
        resolve(g_api.hint_create_session, "APerformanceHint_createSession");
        resolve(
            g_api.hint_update_target, "APerformanceHint_updateTargetWorkDuration");
        resolve(
            g_api.hint_report_actual, "APerformanceHint_reportActualWorkDuration");
        resolve(g_api.hint_close_session, "APerformanceHint_closeSession");
        if (g_api.hint_get_manager && g_api.hint_create_session
            && g_api.hint_update_target && g_api.hint_report_actual
            && g_api.hint_close_session)
        {
          g_api.hint_manager = g_api.hint_get_manager();
          // A negative preferred rate: the device does not support sessions
          out->perf_session =
              g_api.hint_manager != nullptr
              && (g_api.hint_get_preferred_rate == nullptr
                  || g_api.hint_get_preferred_rate(g_api.hint_manager) > 0);
        }

        resolve(g_api.thermal_acquire, "AThermal_acquireManager");
        resolve(g_api.thermal_release, "AThermal_releaseManager");
        resolve(g_api.thermal_get_status, "AThermal_getCurrentThermalStatus");
        resolve(g_api.thermal_register, "AThermal_registerThermalStatusListener");
        resolve(
            g_api.thermal_unregister, "AThermal_unregisterThermalStatusListener");
        resolve(g_api.thermal_get_headroom, "AThermal_getThermalHeadroom");
        if (g_api.thermal_acquire && g_api.thermal_release
            && g_api.thermal_get_status)
        {
          g_api.thermal_manager = g_api.thermal_acquire();
          if (g_api.thermal_manager != nullptr)
          {
            out->thermal_level = true;
            out->thermal_watch = g_api.thermal_register && g_api.thermal_unregister;
            out->thermal_headroom = g_api.thermal_get_headroom != nullptr;
          }
        }
#endif
      }

      void platform_power_deinitialize() noexcept
      {
#if OSPRIM_OS_ANDROID
        if (g_api.thermal_manager != nullptr)
          g_api.thermal_release(g_api.thermal_manager);
        if (g_api.lib != nullptr)
          ::dlclose(g_api.lib);
        g_api = android_api{};
#endif
      }

      status<timer_resolution_result> platform_timer_resolution_begin(
          int64_t, uint32_t* period_ms) noexcept
      {
        *period_ms = 0; // already precise
        return {};
      }

      void platform_timer_resolution_end(uint32_t) noexcept
      {
      }

      status<activity_result> platform_activity_begin(
          const activity_desc&, uint64_t*, activity_desc*) noexcept
      {
        return {
            activity_result::ACTIVITY_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      void platform_activity_end(uint64_t, const activity_desc&) noexcept
      {
      }

      status<perf_session_result> platform_perf_session_create(
          const uint64_t* tids, size_t count, int64_t target_ns,
          uint64_t* native) noexcept
      {
#if OSPRIM_OS_ANDROID
        if (!g_power_features.perf_session)
          return {
              perf_session_result::PERF_SESSION_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        int32_t ids[PERF_SESSION_MAX_THREADS];
        for (size_t i = 0; i < count; ++i)
          ids[i] = static_cast<int32_t>(tids[i]);
        hint_session_t* session =
            g_api.hint_create_session(g_api.hint_manager, ids, count, target_ns);
        // No error code: the system declined to create a session
        if (session == nullptr)
          return {
              perf_session_result::PERF_SESSION_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        *native = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(session));
        return {};
#else
        (void)tids;
        (void)count;
        (void)target_ns;
        (void)native;
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#endif
      }

      status<perf_session_result> platform_perf_session_set_target(
          uint64_t native, int64_t target_ns) noexcept
      {
#if OSPRIM_OS_ANDROID
        const int err = g_api.hint_update_target(
            reinterpret_cast<hint_session_t*>(static_cast<uintptr_t>(native)),
            target_ns);
        return err == 0 ? status<perf_session_result>{} : hint_error(err);
#else
        (void)native;
        (void)target_ns;
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#endif
      }

      status<perf_session_result> platform_perf_session_report(
          uint64_t native, int64_t actual_ns) noexcept
      {
#if OSPRIM_OS_ANDROID
        const int err = g_api.hint_report_actual(
            reinterpret_cast<hint_session_t*>(static_cast<uintptr_t>(native)),
            actual_ns);
        return err == 0 ? status<perf_session_result>{} : hint_error(err);
#else
        (void)native;
        (void)actual_ns;
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#endif
      }

      void platform_perf_session_close(uint64_t native) noexcept
      {
#if OSPRIM_OS_ANDROID
        g_api.hint_close_session(
            reinterpret_cast<hint_session_t*>(static_cast<uintptr_t>(native)));
#else
        (void)native;
#endif
      }

      status<thermal_result> platform_thermal_level(thermal_level* out) noexcept
      {
#if OSPRIM_OS_ANDROID
        if (!g_power_features.thermal_level)
          return {
              thermal_result::THERMAL_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        const int st = g_api.thermal_get_status(g_api.thermal_manager);
        if (st == ATHERMAL_STATUS_ERROR)
          return {
              thermal_result::THERMAL_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        *out = map_status(st);
        return {};
#else
        (void)out;
        return {
            thermal_result::THERMAL_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#endif
      }

      status<thermal_result> platform_thermal_watch_begin(
          std::atomic<wait_word_t>& counter, uint64_t* native,
          int32_t* token) noexcept
      {
        *token = 0;
#if OSPRIM_OS_ANDROID
        if (!g_power_features.thermal_watch)
          return {
              thermal_result::THERMAL_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        const int err = g_api.thermal_register(
            g_api.thermal_manager, &on_thermal_status, &counter);
        if (err != 0)
          return {
              thermal_result::THERMAL_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_POSIX_ERRNO,
              static_cast<int32_t>(err)};
        *native = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&counter));
        return {};
#else
        (void)counter;
        (void)native;
        return {
            thermal_result::THERMAL_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#endif
      }

      void platform_thermal_watch_end(uint64_t native, int32_t) noexcept
      {
#if OSPRIM_OS_ANDROID
        g_api.thermal_unregister(
            g_api.thermal_manager, &on_thermal_status,
            reinterpret_cast<void*>(static_cast<uintptr_t>(native)));
#else
        (void)native;
#endif
      }

      status<thermal_result> platform_thermal_headroom(
          int64_t forecast_ns, float* out) noexcept
      {
#if OSPRIM_OS_ANDROID
        if (!g_power_features.thermal_headroom)
          return {
              thermal_result::THERMAL_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        // Whole seconds, rounded to the nearest
        const int64_t seconds = (forecast_ns + 500000000) / 1000000000;
        const float h         = g_api.thermal_get_headroom(
            g_api.thermal_manager,
            static_cast<int>(seconds > 3600 ? 3600 : seconds));
        // NaN: unsupported by the device, or asked too often
        if (isnan(h))
          return {
              thermal_result::THERMAL_UNAVAILABLE,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        *out = h;
        return {};
#else
        (void)forecast_ns;
        (void)out;
        return {
            thermal_result::THERMAL_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#endif
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
