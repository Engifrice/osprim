/*****************************************************************/ /**
 * @file   power.cpp
 * @brief  Platform-independent power code: preconditions, handle ids and
 * tracking. The mechanisms live in the platform sources.
 *********************************************************************/
#include "internal.hpp"

#include <string.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      constexpr const char* TIMER_RESOLUTION_TYPE = "timer_resolution";
      constexpr const char* ACTIVITY_TYPE         = "activity";
      constexpr const char* PERF_SESSION_TYPE     = "perf_session";
      constexpr const char* THERMAL_WATCH_TYPE    = "thermal_watch";
    } // namespace

    namespace detail
    {
      power_features g_power_features{};

      void power_initialize() noexcept
      {
        g_power_features = power_features{};
        platform_power_initialize(&g_power_features);
      }

      void power_deinitialize() noexcept
      {
        platform_power_deinitialize();
        g_power_features = power_features{};
      }
    } // namespace detail

    power_features get_power_features() noexcept
    {
      return detail::g_power_features;
    }

    /*****************************/
    /* TIMER RESOLUTION          */
    /*****************************/

    status<timer_resolution_result> process_timer_resolution_begin(
        int64_t period_ns, timer_resolution_handle* out,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          period_ns > 0,
          "osprim::process_timer_resolution_begin: period must be positive");
      OSPRIM_ASSERT(
          out != nullptr, "osprim::process_timer_resolution_begin: null handle");
      *out         = timer_resolution_handle{};
      uint32_t ms  = 0;
      const auto s = detail::platform_timer_resolution_begin(period_ns, &ms);
      if (!s.ok())
        return s;
      *out = timer_resolution_handle{detail::next_handle_id(), ms};
      detail::track_open(TIMER_RESOLUTION_TYPE, out->id, loc);
      return s;
    }

    void process_timer_resolution_end(
        timer_resolution_handle h, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          h.id != 0, "osprim::process_timer_resolution_end: not a request");
      detail::track_close(TIMER_RESOLUTION_TYPE, h.id, loc);
      detail::platform_timer_resolution_end(h.period_ms);
    }

    /*****************************/
    /* PROCESS ACTIVITY          */
    /*****************************/

    status<activity_result> process_activity_begin(
        const activity_desc& desc, activity_handle* out,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(out != nullptr, "osprim::process_activity_begin: null handle");
      OSPRIM_ASSERT(
          desc.latency_critical || desc.keep_system_awake || desc.keep_display_on,
          "osprim::process_activity_begin: nothing requested");
      OSPRIM_ASSERT(
          desc.reason != nullptr, "osprim::process_activity_begin: null reason");
      OSPRIM_ASSERT(
          ::strlen(desc.reason) <= ACTIVITY_REASON_MAX,
          "osprim::process_activity_begin: reason longer than ACTIVITY_REASON_MAX "
          "bytes");
      *out            = activity_handle{};
      uint64_t native = 0;
      activity_desc granted{};
      const auto s = detail::platform_activity_begin(desc, &native, &granted);
      if (!s.ok())
        return s;
      granted.reason = nullptr;
      *out           = activity_handle{detail::next_handle_id(), native, granted};
      detail::track_open(ACTIVITY_TYPE, out->id, loc);
      return s;
    }

    void process_activity_end(activity_handle h, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(h.id != 0, "osprim::process_activity_end: not an activity");
      detail::track_close(ACTIVITY_TYPE, h.id, loc);
      detail::platform_activity_end(h.native, h.granted);
    }

    /*****************************/
    /* PERFORMANCE SESSIONS      */
    /*****************************/

    status<perf_session_result> perf_session_create(
        const uint64_t* thread_ids, size_t count, int64_t target_ns,
        perf_session* out, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(out != nullptr, "osprim::perf_session_create: null session");
      OSPRIM_ASSERT(
          thread_ids != nullptr && count >= 1 && count <= PERF_SESSION_MAX_THREADS,
          "osprim::perf_session_create: between 1 and PERF_SESSION_MAX_THREADS "
          "threads");
      OSPRIM_ASSERT(
          target_ns > 0, "osprim::perf_session_create: target must be positive");
      *out            = perf_session{};
      uint64_t native = 0;
      const auto s    = detail::platform_perf_session_create(
          thread_ids, count, target_ns, &native);
      if (!s.ok())
        return s;
      *out = perf_session{detail::next_handle_id(), native};
      detail::track_open(PERF_SESSION_TYPE, out->id, loc);
      return s;
    }

    status<perf_session_result> perf_session_set_target(
        perf_session s, int64_t target_ns) noexcept
    {
      OSPRIM_ASSERT(s.id != 0, "osprim::perf_session_set_target: not a session");
      OSPRIM_ASSERT(
          target_ns > 0, "osprim::perf_session_set_target: target must be positive");
      return detail::platform_perf_session_set_target(s.native, target_ns);
    }

    status<perf_session_result> perf_session_report(
        perf_session s, int64_t actual_ns) noexcept
    {
      OSPRIM_ASSERT(s.id != 0, "osprim::perf_session_report: not a session");
      OSPRIM_ASSERT(
          actual_ns > 0, "osprim::perf_session_report: duration must be positive");
      return detail::platform_perf_session_report(s.native, actual_ns);
    }

    void perf_session_close(perf_session s, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(s.id != 0, "osprim::perf_session_close: not a session");
      detail::track_close(PERF_SESSION_TYPE, s.id, loc);
      detail::platform_perf_session_close(s.native);
    }

    /*****************************/
    /* THERMAL                   */
    /*****************************/

    status<thermal_result> current_thermal_level(thermal_level* out) noexcept
    {
      OSPRIM_ASSERT(out != nullptr, "osprim::current_thermal_level: null output");
      return detail::platform_thermal_level(out);
    }

    status<thermal_result> thermal_watch_begin(
        std::atomic<wait_word_t>& counter, thermal_watch* out,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(out != nullptr, "osprim::thermal_watch_begin: null watch");
      *out            = thermal_watch{};
      uint64_t native = 0;
      int32_t token   = 0;
      const auto s = detail::platform_thermal_watch_begin(counter, &native, &token);
      if (!s.ok())
        return s;
      *out = thermal_watch{detail::next_handle_id(), native, token};
      detail::track_open(THERMAL_WATCH_TYPE, out->id, loc);
      return s;
    }

    void thermal_watch_end(thermal_watch w, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(w.id != 0, "osprim::thermal_watch_end: not a watch");
      detail::track_close(THERMAL_WATCH_TYPE, w.id, loc);
      detail::platform_thermal_watch_end(w.native, w.token);
    }

    status<thermal_result> thermal_headroom(int64_t forecast_ns, float* out) noexcept
    {
      OSPRIM_ASSERT(forecast_ns >= 0, "osprim::thermal_headroom: negative forecast");
      OSPRIM_ASSERT(out != nullptr, "osprim::thermal_headroom: null output");
      return detail::platform_thermal_headroom(forecast_ns, out);
    }
  } // namespace v0_1
} // namespace osprim
