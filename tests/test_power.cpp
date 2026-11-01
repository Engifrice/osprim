#include "support.hpp"

#include "internal.hpp"

#include <osprim/power.hpp>
#include <osprim/thread.hpp>

#include <doctest/doctest.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <type_traits>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <TargetConditionals.h>
#endif

using namespace osprim;
using osprim_test::note;
using osprim_test::session;

namespace
{
  constexpr int64_t US = 1000;
  constexpr int64_t MS = 1000 * US;
  constexpr int64_t S  = 1000 * MS;

  /// @brief Average lateness of `n` timed-out waits of `timeout_ns`.
  double average_wait_lateness_ns(int64_t timeout_ns, int n)
  {
    std::atomic<wait_word_t> word{0};
    int64_t total = 0;
    for (int i = 0; i < n; ++i)
    {
      const awake_instant deadline = deadline_after(timeout_ns);
      while (wait(word, 0, deadline) != wait_result::WAIT_TIMED_OUT)
      {
      }
      total += elapsed_ns(deadline, awake_now());
    }
    return static_cast<double>(total) / n;
  }

  const char* yes_no(bool b)
  {
    return b ? "yes" : "no";
  }

  /// @brief Wine implements some Windows APIs as no-ops or not at all.
  bool running_under_wine()
  {
#if defined(_WIN32)
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr
           && ::GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
  }

  const char* activity_result_name(activity_result r)
  {
    const char* n[3] = {"OK", "UNSUPPORTED", "SYSTEM_ERROR"};
    return n[static_cast<size_t>(r)];
  }

  const char* perf_result_name(perf_session_result r)
  {
    const char* n[3] = {"OK", "UNSUPPORTED", "SYSTEM_ERROR"};
    return n[static_cast<size_t>(r)];
  }

  activity_desc make_activity(bool latency, bool awake, bool display)
  {
    activity_desc d{};
    d.latency_critical  = latency;
    d.keep_system_awake = awake;
    d.keep_display_on   = display;
    d.reason            = "osprim tests";
    return d;
  }
} // namespace

/*****************************/
/* COMPILE-TIME PROPERTIES   */
/*****************************/

static_assert(static_cast<int>(timer_resolution_result::TIMER_RESOLUTION_OK) == 0);
static_assert(static_cast<int>(activity_result::ACTIVITY_OK) == 0);
static_assert(static_cast<int>(perf_session_result::PERF_SESSION_OK) == 0);
static_assert(static_cast<int>(thermal_result::THERMAL_OK) == 0);
static_assert(sizeof(status<activity_result>) == 8);
static_assert(std::is_trivially_copyable_v<timer_resolution_handle>);
static_assert(std::is_trivially_copyable_v<activity_handle>);
static_assert(std::is_trivially_copyable_v<perf_session>);
static_assert(std::is_trivially_copyable_v<thermal_watch>);
static_assert(std::is_aggregate_v<power_features>);

/*****************************/
/* FEATURES                  */
/*****************************/

TEST_CASE("power: features")
{
  session s;
  const power_features f = get_power_features();
  note(
      "power features: activity latency_critical=%s keep_system_awake=%s "
      "keep_display_on=%s, "
      "perf_session=%s, thermal level=%s watch=%s headroom=%s",
      yes_no(f.activity_latency_critical), yes_no(f.activity_keep_system_awake),
      yes_no(f.activity_keep_display_on), yes_no(f.perf_session),
      yes_no(f.thermal_level), yes_no(f.thermal_watch), yes_no(f.thermal_headroom));

#if defined(_WIN32)
  if (!running_under_wine()) // no process power throttling in Wine
    CHECK(f.activity_latency_critical);
  CHECK(f.activity_keep_system_awake);
  CHECK(f.activity_keep_display_on);
  CHECK(!f.perf_session);
  CHECK(!f.thermal_level);
  CHECK(!f.thermal_headroom);
#elif defined(__APPLE__)
  CHECK(!f.perf_session);
  CHECK(!f.thermal_headroom);
  CHECK(f.thermal_level); // the pressure level is always published
  CHECK(f.thermal_watch);
  #if TARGET_OS_OSX
  CHECK(f.activity_latency_critical);
  #endif
#elif defined(__ANDROID__)
  CHECK(!f.activity_latency_critical);
  CHECK(!f.activity_keep_display_on);
#else // Linux
  CHECK(!f.activity_latency_critical);
  CHECK(!f.activity_keep_system_awake);
  CHECK(!f.activity_keep_display_on);
  CHECK(!f.perf_session);
  CHECK(!f.thermal_level);
  CHECK(!f.thermal_watch);
  CHECK(!f.thermal_headroom);
#endif

  // Constant after initialize()
  const power_features again = get_power_features();
  CHECK(memcmp(&f, &again, sizeof(f)) == 0);
}

/*****************************/
/* TIMER RESOLUTION          */
/*****************************/

TEST_CASE("power: timer resolution requests")
{
  session s;
  timer_resolution_handle a{}, b{};
  REQUIRE(process_timer_resolution_begin(1 * MS, &a).ok());
  CHECK(a.id != 0);
  // Overlapping requests are independent
  REQUIRE(process_timer_resolution_begin(500 * US, &b).ok());
  CHECK(b.id != a.id);
#if defined(_WIN32)
  CHECK(a.period_ms >= 1); // rounded up, at least what the system supports
  CHECK(b.period_ms >= 1);
#endif
  CHECK(detail::tracked_count() == (tracking_compiled ? 2u : 0u));
  process_timer_resolution_end(b);
  process_timer_resolution_end(a);
  CHECK(detail::tracked_count() == 0);
}

TEST_CASE("power: a timer resolution request makes timeouts precise")
{
  session s;
  const double before = average_wait_lateness_ns(1 * MS, 20);
  timer_resolution_handle h{};
  REQUIRE(process_timer_resolution_begin(1 * MS, &h).ok());
  const double during = average_wait_lateness_ns(1 * MS, 20);
  process_timer_resolution_end(h);
  note(
      "1 ms timeouts: late by %.1f us on average without a request, %.1f us with "
      "one",
      before / 1000.0, during / 1000.0);
#if defined(_WIN32)
  // The default is 15.6 ms: with a 1 ms period, about 1-2 ms late
  if (!running_under_wine()) // Wine ignores timer resolution requests
    CHECK(during < 8.0 * MS);
#endif
  CHECK(during < 1.0 * S);
}

/*****************************/
/* PROCESS ACTIVITY          */
/*****************************/

TEST_CASE("power: process activities")
{
  session s;
  const power_features f = get_power_features();

  struct request
  {
    const char* name;
    activity_desc desc;
    bool expected;
  };
  const request requests[] = {
      {"latency_critical", make_activity(true, false, false),
       f.activity_latency_critical},
      {"keep_system_awake", make_activity(false, true, false),
       f.activity_keep_system_awake},
      {"keep_display_on", make_activity(false, false, true),
       f.activity_keep_display_on},
      {"all", make_activity(true, true, true),
       f.activity_latency_critical || f.activity_keep_system_awake
           || f.activity_keep_display_on},
  };
  for (const request& r : requests)
  {
    CAPTURE(r.name);
    activity_handle h{};
    const auto st = process_activity_begin(r.desc, &h);
    note(
        "activity %s: %s (native error %d)", r.name, activity_result_name(st.result),
        static_cast<int>(st.native_code));
    if (!r.expected
        || (running_under_wine() && !st.ok())) // Wine: power request stubs
    {
      CHECK(st.result == activity_result::ACTIVITY_UNSUPPORTED);
      CHECK(h.id == 0);
      continue;
    }
    REQUIRE(st.ok());
    CHECK(h.id != 0);
    CHECK(h.granted.reason == nullptr);
    // Granted: what was asked for, where supported
    CHECK(
        h.granted.latency_critical
        == (r.desc.latency_critical && f.activity_latency_critical));
    CHECK(
        h.granted.keep_system_awake
        == (r.desc.keep_system_awake && f.activity_keep_system_awake));
    CHECK(
        h.granted.keep_display_on
        == (r.desc.keep_display_on && f.activity_keep_display_on));
    process_activity_end(h);
  }
}

TEST_CASE("power: overlapping latency-critical activities")
{
  session s;
  if (!get_power_features().activity_latency_critical)
    return;
  activity_handle a{}, b{};
  REQUIRE(process_activity_begin(make_activity(true, false, false), &a).ok());
  REQUIRE(process_activity_begin(make_activity(true, false, false), &b).ok());
  process_activity_end(a); // b still holds it
  activity_handle c{};
  REQUIRE(process_activity_begin(make_activity(true, false, false), &c).ok());
  process_activity_end(b);
  process_activity_end(c);
}

/*****************************/
/* PERFORMANCE SESSIONS      */
/*****************************/

TEST_CASE("power: performance sessions")
{
  session s;
  const uint64_t ids[] = {current_thread_id()};
  perf_session ps{};
  const auto st = perf_session_create(ids, 1, 16'666'667, &ps);
  note("perf_session_create: %s", perf_result_name(st.result));
  if (!get_power_features().perf_session)
  {
    CHECK(st.result == perf_session_result::PERF_SESSION_UNSUPPORTED);
    CHECK(ps.id == 0);
    return;
  }
  // Supported devices may still decline a session
  if (st.result == perf_session_result::PERF_SESSION_UNSUPPORTED)
    return;
  REQUIRE(st.ok());
  for (int i = 0; i < 5; ++i)
    CHECK(perf_session_report(ps, 12 * MS + i * MS).ok());
  CHECK(perf_session_set_target(ps, 8'333'333).ok());
  CHECK(perf_session_report(ps, 9 * MS).ok());
  perf_session_close(ps);
}

/*****************************/
/* THERMAL                   */
/*****************************/

TEST_CASE("power: thermal")
{
  session s;
  const power_features f = get_power_features();

  thermal_level level{};
  const auto st = current_thermal_level(&level);
  if (f.thermal_level)
  {
    REQUIRE(st.ok());
    const char* names[] = {"NOMINAL", "FAIR", "SERIOUS", "CRITICAL"};
    note("thermal level: %s", names[static_cast<size_t>(level)]);
  }
  else
    CHECK(st.result == thermal_result::THERMAL_UNSUPPORTED);

  std::atomic<wait_word_t> counter{0};
  thermal_watch w{};
  const auto ws = thermal_watch_begin(counter, &w);
  if (f.thermal_watch)
  {
    REQUIRE(ws.ok());
    CHECK(w.id != 0);
    // Nothing guarantees a change during the test: only the lifecycle
    (void)wait(counter, 0, deadline_after(10 * MS));
    thermal_watch_end(w);
  }
  else
  {
    CHECK(ws.result == thermal_result::THERMAL_UNSUPPORTED);
    CHECK(w.id == 0);
  }

  float headroom = 0.0f;
  const auto hs  = thermal_headroom(10 * S, &headroom);
  if (f.thermal_headroom)
  {
    CHECK(
        (hs.result == thermal_result::THERMAL_OK
         || hs.result == thermal_result::THERMAL_UNAVAILABLE));
    if (hs.ok())
    {
      CHECK(!isnan(headroom));
      note("thermal headroom in 10 s: %.3f", static_cast<double>(headroom));
    }
  }
  else
    CHECK(hs.result == thermal_result::THERMAL_UNSUPPORTED);
}

/*****************************/
/* TRACKING                  */
/*****************************/

#if OSPRIM_TRACKING
TEST_CASE("power: a request never ended leaks")
{
  timer_resolution_handle h{};
  {
    session s;
    const auto where = std::source_location::current();
    REQUIRE(process_timer_resolution_begin(1 * MS, &h, where).ok());
    const auto& log = s.end();
    REQUIRE(log.count == 1);
    CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
    CHECK(strcmp(log.entries[0].handle_type, "timer_resolution") == 0);
    CHECK(log.entries[0].created_at.line() == where.line());
  }
  // Give the period back to the OS, without tracking
  init_desc desc;
  desc.track_handles = false;
  REQUIRE(initialize(desc).ok());
  process_timer_resolution_end(h);
  deinitialize();
}
#endif

/*****************************/
/* THREAD CLASS AND ECOQOS   */
/*****************************/

#if defined(_WIN32)
TEST_CASE("power: BACKGROUND threads are EcoQoS on Windows")
{
  session s;
  if (running_under_wine()) // GetThreadInformation aborts the process in Wine
    return;
  struct state
  {
    ULONG Version, ControlMask, StateMask;
  };
  thread_context ctx;
  static std::atomic<int> outcome{0}; // 1 ok, -1 wrong, 0 not readable
  ctx.entry = [](void*) noexcept
  {
    auto read = [](state* st) -> bool
    {
      *st = state{1, 0, 0};
      return ::GetThreadInformation(
                 ::GetCurrentThread(), ThreadPowerThrottling, st, sizeof(*st))
             != FALSE;
    };
    state st{};
    if (!set_current_thread_class(thread_class::THREAD_CLASS_BACKGROUND).ok()
        || !read(&st))
      return;
    const bool eco = (st.ControlMask & 1) != 0 && (st.StateMask & 1) != 0;
    if (!set_current_thread_class(thread_class::THREAD_CLASS_NORMAL).ok()
        || !read(&st))
      return;
    const bool system = st.ControlMask == 0;
    outcome           = eco && system ? 1 : -1;
  };
  REQUIRE(thread_create(ctx, thread_desc{}).ok());
  REQUIRE(
      thread_join(ctx, deadline_after(10 * S))
      == thread_join_result::THREAD_JOIN_OK);
  if (outcome.load() == 0)
    note("thread power throttling state not readable here");
  else
    CHECK(outcome.load() == 1);
}
#endif
