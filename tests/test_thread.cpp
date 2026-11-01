#include "support.hpp"

#include "internal.hpp"

#include <osprim/thread.hpp>

#include <doctest/doctest.h>

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <set>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <pthread.h>
  #if defined(__APPLE__)
    #include <pthread/qos.h>
  #else
    #include <errno.h>
    #include <sys/resource.h>
    #include <sys/syscall.h>
    #include <unistd.h>
  #endif
#endif

using namespace osprim;
using osprim_test::note;
using osprim_test::session;

namespace
{
  constexpr int64_t US = 1000;
  constexpr int64_t MS = 1000 * US;
  constexpr int64_t S  = 1000 * MS;

  /// @brief Starts `f()` on a new thread: the context and the callable live
  /// in this object, which must outlive the thread.
  template<class F>
  struct runner
  {
    F f;
    thread_context ctx;

    explicit runner(F fn)
        : f(static_cast<F&&>(fn))
    {
      ctx.entry = [](void* user) noexcept { static_cast<runner*>(user)->f(); };
      ctx.user  = this;
    }

    status<thread_create_result> start(thread_desc desc = {})
    {
      return thread_create(ctx, desc);
    }
    thread_join_result join(int64_t timeout_ns = 10 * S)
    {
      return thread_join(ctx, deadline_after(timeout_ns));
    }
  };

  /// @brief Runs `f()` on a new thread and joins it.
  template<class F>
  void run_on_new_thread(F f, thread_desc desc = {})
  {
    runner<F> r(static_cast<F&&>(f));
    REQUIRE(r.start(desc).ok());
    CHECK(r.join() == thread_join_result::THREAD_JOIN_OK);
  }

  /// @brief A one-shot gate threads can block on.
  struct gate
  {
    std::atomic<wait_word_t> open{0};

    void pass() const
    {
      const awake_instant deadline = deadline_after(10 * S);
      while (open.load(std::memory_order_acquire) == 0)
        if (wait(open, 0, deadline) == wait_result::WAIT_TIMED_OUT)
          return;
    }
    void release()
    {
      open.store(1, std::memory_order_release);
      wake_all(open);
    }
  };

  /// @brief Blocks until `ctx` reaches `s` (or 10 s).
  bool wait_for_state(const thread_context& ctx, thread_state s)
  {
    const awake_instant deadline = deadline_after(10 * S);
    for (;;)
    {
      const thread_state now = thread_state_of(ctx);
      if (static_cast<wait_word_t>(now) >= static_cast<wait_word_t>(s))
        return now == s;
      if (wait(ctx.state, static_cast<wait_word_t>(now), deadline)
          == wait_result::WAIT_TIMED_OUT)
        return false;
    }
  }

  /// @brief Name of the calling thread, read back from the OS.
  bool read_current_thread_name(char* out, size_t size)
  {
#if defined(_WIN32)
    // Not declared by every SDK (MinGW)
    using get_description_fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const FARPROC proc       = ::GetProcAddress(
        ::GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (proc == nullptr)
      return false;
    const auto get_description =
        reinterpret_cast<get_description_fn>(reinterpret_cast<void (*)(void)>(proc));
    PWSTR wide = nullptr;
    if (FAILED(get_description(::GetCurrentThread(), &wide)))
      return false;
    const int n = ::WideCharToMultiByte(
        CP_UTF8, 0, wide, -1, out, static_cast<int>(size), nullptr, nullptr);
    ::LocalFree(wide);
    return n != 0;
#else
    return ::pthread_getname_np(::pthread_self(), out, size) == 0;
#endif
  }
} // namespace

/*****************************/
/* COMPILE-TIME PROPERTIES   */
/*****************************/

static_assert(std::is_same_v<std::underlying_type_t<thread_state>, wait_word_t>);
static_assert(static_cast<int>(thread_state::THREAD_STATE_STARTING) == 0);
static_assert(static_cast<int>(thread_create_result::THREAD_CREATE_OK) == 0);
static_assert(static_cast<int>(thread_join_result::THREAD_JOIN_OK) == 0);
static_assert(static_cast<int>(set_thread_name_result::SET_THREAD_NAME_OK) == 0);
static_assert(static_cast<int>(set_thread_class_result::SET_THREAD_CLASS_OK) == 0);
static_assert(sizeof(status<thread_create_result>) == 8);
static_assert(THREAD_NAME_MAX == 15);
static_assert(
    noexcept(cpu_relax()) && noexcept(yield_thread())
    && noexcept(current_thread_id()));

/*****************************/
/* LIFECYCLE                 */
/*****************************/

TEST_CASE("thread: create, run and join")
{
  session s;
  std::atomic<int> ran{0};
  std::atomic<uint64_t> inner_id{0};
  runner r(
      [&]
      {
        ++ran;
        inner_id = current_thread_id();
      });
  REQUIRE(r.start().ok());
  CHECK(r.ctx.native != 0);
  CHECK(r.join() == thread_join_result::THREAD_JOIN_OK);
  CHECK(ran.load() == 1);
  CHECK(thread_state_of(r.ctx) == thread_state::THREAD_STATE_EXITED);
  CHECK(r.ctx.native == 0);
  CHECK(r.ctx.id != 0);
  CHECK(r.ctx.id == inner_id.load());
  CHECK(r.ctx.id != current_thread_id());
}

TEST_CASE("thread: states and id are published before the entry runs")
{
  session s;
  gate g;
  std::atomic<uint64_t> inner_id{0};
  runner r(
      [&]
      {
        inner_id = current_thread_id();
        g.pass();
      });
  REQUIRE(r.start().ok());
  REQUIRE(wait_for_state(r.ctx, thread_state::THREAD_STATE_RUNNING));
  CHECK(r.ctx.id != 0); // readable as soon as RUNNING is seen
  g.release();
  CHECK(r.join() == thread_join_result::THREAD_JOIN_OK);
  CHECK(r.ctx.id == inner_id.load());
}

TEST_CASE("thread: join with a deadline")
{
  session s;
  gate g;
  runner r([&] { g.pass(); });
  REQUIRE(r.start().ok());

  // Running: the deadline expires, the thread is still ours
  const awake_instant start = awake_now();
  CHECK(
      thread_join(r.ctx, deadline_after(20 * MS))
      == thread_join_result::THREAD_JOIN_TIMED_OUT);
  CHECK(elapsed_ns(start, awake_now()) >= 20 * MS);
  CHECK(
      thread_join(r.ctx, awake_instant{0})
      == thread_join_result::THREAD_JOIN_TIMED_OUT);
  CHECK(r.ctx.native != 0);

  g.release();
  CHECK(
      thread_join(r.ctx, awake_instant::forever())
      == thread_join_result::THREAD_JOIN_OK);
  CHECK(r.ctx.native == 0);
}

TEST_CASE("thread: join from a thread other than the creator")
{
  session s;
  runner worker([] {});
  REQUIRE(worker.start().ok());
  run_on_new_thread([&]
                    { CHECK(worker.join() == thread_join_result::THREAD_JOIN_OK); });
}

TEST_CASE("thread: a detached thread runs to the end on its own")
{
  session s;
  gate g;
  std::atomic<int> ran{0};
  runner r(
      [&]
      {
        g.pass();
        ++ran;
      });
  REQUIRE(r.start().ok());
  thread_detach(r.ctx);
  CHECK(r.ctx.native == 0);
  g.release();
  CHECK(wait_for_state(r.ctx, thread_state::THREAD_STATE_EXITED));
  CHECK(ran.load() == 1);
  // From here on, the library no longer touches r.ctx
}

TEST_CASE("thread: a context can be reused after join")
{
  session s;
  std::atomic<int> ran{0};
  runner r([&] { ++ran; });
  for (int i = 0; i < 10; ++i)
  {
    REQUIRE(r.start().ok());
    CHECK(r.join() == thread_join_result::THREAD_JOIN_OK);
  }
  CHECK(ran.load() == 10);
}

TEST_CASE("thread: many threads at once have distinct ids")
{
  session s;
  constexpr int N = 64;
  struct slot
  {
    thread_context ctx;
    std::atomic<uint64_t> id{0};
  };
  std::vector<slot> slots(N);
  gate g;
  struct arg
  {
    slot* sl;
    gate* g;
  };
  std::vector<arg> args(N);
  for (int i = 0; i < N; ++i)
  {
    args[static_cast<size_t>(i)] = arg{&slots[static_cast<size_t>(i)], &g};
    slots[static_cast<size_t>(i)].ctx.entry = [](void* user) noexcept
    {
      auto* a   = static_cast<arg*>(user);
      a->sl->id = current_thread_id();
      a->g->pass(); // all alive together: ids cannot be reused
    };
    slots[static_cast<size_t>(i)].ctx.user = &args[static_cast<size_t>(i)];
    REQUIRE(thread_create(slots[static_cast<size_t>(i)].ctx, thread_desc{}).ok());
  }
  for (auto& sl : slots)
    REQUIRE(wait_for_state(sl.ctx, thread_state::THREAD_STATE_RUNNING));
  g.release();
  std::set<uint64_t> ids;
  for (auto& sl : slots)
  {
    CHECK(
        thread_join(sl.ctx, deadline_after(10 * S))
        == thread_join_result::THREAD_JOIN_OK);
    CHECK(sl.ctx.id == sl.id.load());
    ids.insert(sl.ctx.id);
  }
  CHECK(ids.size() == static_cast<size_t>(N));
  CHECK(ids.count(current_thread_id()) == 0);
}

TEST_CASE("thread: stack size")
{
  session s;
  SUBCASE("a large stack is usable")
  {
    std::atomic<int> sum{0};
    run_on_new_thread(
        [&]
        {
          // 6 MiB of locals: more than the default stack on Apple (512 KiB)
          // and Windows (1 MiB)
          volatile char big[6 << 20];
          big[0]               = 1;
          big[sizeof(big) - 1] = 2;
          sum                  = big[0] + big[sizeof(big) - 1];
        },
        thread_desc{8 << 20});
    CHECK(sum.load() == 3);
  }
  SUBCASE("a tiny stack is rounded up to the platform minimum")
  {
    std::atomic<int> ran{0};
    run_on_new_thread([&] { ++ran; }, thread_desc{1});
    CHECK(ran.load() == 1);
  }
}

#if OSPRIM_TRACKING
TEST_CASE("thread: tracking")
{
  SUBCASE("joined threads are forgotten")
  {
    session s;
    runner r([] {});
    REQUIRE(r.start().ok());
    CHECK(detail::tracked_count() == 1);
    CHECK(r.join() == thread_join_result::THREAD_JOIN_OK);
    CHECK(detail::tracked_count() == 0);
  }
  SUBCASE("detached threads are forgotten")
  {
    session s;
    runner r([] {});
    REQUIRE(r.start().ok());
    thread_detach(r.ctx);
    CHECK(detail::tracked_count() == 0);
    CHECK(wait_for_state(r.ctx, thread_state::THREAD_STATE_EXITED));
  }
  SUBCASE("a thread neither joined nor detached leaks")
  {
    runner r([] {});
    {
      session s;
      const auto where = std::source_location::current();
      REQUIRE(thread_create(r.ctx, thread_desc{}, where).ok());
      REQUIRE(wait_for_state(r.ctx, thread_state::THREAD_STATE_EXITED));
      const auto& log = s.end();
      REQUIRE(log.count == 1);
      CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
      CHECK(strcmp(log.entries[0].handle_type, "thread") == 0);
      CHECK(log.entries[0].created_at.line() == where.line());
    }
    // Clean up without tracking
    init_desc desc;
    desc.track_handles = false;
    REQUIRE(initialize(desc).ok());
    CHECK(r.join() == thread_join_result::THREAD_JOIN_OK);
    deinitialize();
  }
}
#endif

/*****************************/
/* CALLING THREAD SETTINGS   */
/*****************************/

TEST_CASE("thread: names")
{
  session s;
  const char* names[] = {
      "osprim-worker",
      "123456789012345", // THREAD_NAME_MAX bytes
      "caf\xC3\xA9",     // UTF-8
  };
  for (const char* name : names)
  {
    CAPTURE(name);
    run_on_new_thread(
        [name]
        {
          const auto st = set_current_thread_name(name);
          CHECK(st.ok());
          char back[64] = {};
          CHECK(read_current_thread_name(back, sizeof(back)));
          CHECK(strcmp(back, name) == 0);
        });
  }
}

namespace
{
  /// @brief The class as the OS reports it, as the native value.
  long long read_native_class()
  {
#if defined(_WIN32)
    return ::GetThreadPriority(::GetCurrentThread());
#elif defined(__APPLE__)
    return static_cast<long long>(::qos_class_self());
#else
    errno = 0;
    return ::getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)));
#endif
  }

  long long expected_native_class(thread_class c)
  {
    const size_t i = static_cast<size_t>(c);
#if defined(_WIN32)
    const long long v[4] = {
        THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
        THREAD_PRIORITY_ABOVE_NORMAL};
#elif defined(__APPLE__)
    const long long v[4] = {
        QOS_CLASS_BACKGROUND, QOS_CLASS_UTILITY, QOS_CLASS_USER_INITIATED,
        QOS_CLASS_USER_INTERACTIVE};
#else
    const long long v[4] = {10, 5, 0, -6};
#endif
    return v[i];
  }

  const char* class_name(thread_class c)
  {
    const char* n[4] = {"BACKGROUND", "UTILITY", "NORMAL", "INTERACTIVE"};
    return n[static_cast<size_t>(c)];
  }

  const char* class_result_name(set_thread_class_result r)
  {
    const char* n[3] = {"OK", "DENIED", "SYSTEM_ERROR"};
    return n[static_cast<size_t>(r)];
  }
} // namespace

TEST_CASE("thread: scheduling classes")
{
  session s;
  const thread_class classes[] = {
      thread_class::THREAD_CLASS_BACKGROUND, thread_class::THREAD_CLASS_UTILITY,
      thread_class::THREAD_CLASS_NORMAL, thread_class::THREAD_CLASS_INTERACTIVE};
  for (const thread_class c : classes)
  {
    CAPTURE(class_name(c));
    // A fresh thread each time: Linux refuses to go back up after going down
    run_on_new_thread(
        [c]
        {
          const long long before = read_native_class();
          const auto st          = set_current_thread_class(c);
          const long long after  = read_native_class();
          note(
              "set_current_thread_class(%s): %s (native %lld -> %lld, error %d)",
              class_name(c), class_result_name(st.result), before, after,
              static_cast<int>(st.native_code));
          CHECK(st.result != set_thread_class_result::SET_THREAD_CLASS_SYSTEM_ERROR);
          if (st.ok())
            CHECK(after == expected_native_class(c));
          else
            CHECK(after == before); // refused: nothing changed
#if defined(_WIN32) || defined(__APPLE__)
          // Every class is open to every process
          CHECK(st.ok());
#else
          // Only raising priority can be refused (unprivileged desktop Linux)
          if (c != thread_class::THREAD_CLASS_INTERACTIVE)
            CHECK(st.ok());
#endif
        });
  }

  SUBCASE("going back to NORMAL after BACKGROUND")
  {
    run_on_new_thread(
        []
        {
          CHECK(
              set_current_thread_class(thread_class::THREAD_CLASS_BACKGROUND).ok());
          const auto st =
              set_current_thread_class(thread_class::THREAD_CLASS_NORMAL);
          note("NORMAL after BACKGROUND: %s", class_result_name(st.result));
#if defined(_WIN32) || defined(__APPLE__)
          CHECK(st.ok());
#else
          // Refused to unprivileged users on desktop Linux (RLIMIT_NICE)
          CHECK(st.result != set_thread_class_result::SET_THREAD_CLASS_SYSTEM_ERROR);
#endif
          if (st.ok())
            CHECK(
                read_native_class()
                == expected_native_class(thread_class::THREAD_CLASS_NORMAL));
        });
  }
}

/*****************************/
/* YIELD, SPIN, SLEEP        */
/*****************************/

TEST_CASE("thread: sleep_until never returns early")
{
  session s;
  const awake_instant start = awake_now();
  sleep_until(awake_instant{0}); // passed: returns at once
  sleep_until(start);
  CHECK(elapsed_ns(start, awake_now()) < 1 * S);

  const int64_t durations[] = {0, 100 * US, 1 * MS, 5 * MS, 20 * MS};
  for (const int64_t d : durations)
  {
    CAPTURE(d);
    const int repeats = d >= 20 * MS ? 5 : 20;
    int64_t worst = 0, total = 0;
    for (int i = 0; i < repeats; ++i)
    {
      const awake_instant deadline = deadline_after(d);
      sleep_until(deadline);
      const awake_instant now = awake_now();
      CHECK(now >= deadline);
      const int64_t late = elapsed_ns(deadline, now);
      worst              = late > worst ? late : worst;
      total += late;
    }
    CHECK(worst < 1 * S);
    note(
        "sleep %6lld us: late by %8.1f us on average, %8.1f us at worst",
        static_cast<long long>(d / US),
        static_cast<double>(total) / repeats / 1000.0,
        static_cast<double>(worst) / 1000.0);
  }
}

TEST_CASE("thread: cost of the calls that do not block")
{
  session s;
  constexpr int N        = 100'000;
  volatile uint64_t sink = 0;

  auto measure = [&](const char* name, int n, auto fn)
  {
    const awake_instant start = awake_now();
    for (int i = 0; i < n; ++i)
      fn();
    const double ns = static_cast<double>(elapsed_ns(start, awake_now())) / n;
    note("%-28s %10.2f ns per call", name, ns);
    CHECK(ns < 100'000'000.0); // only absurd values: emulators are slow
  };

  measure("current_thread_id", N, [&] { sink = sink ^ current_thread_id(); });
  measure("cpu_relax", N, [] { cpu_relax(); });
  // Few: on a busy machine every yield really gives the CPU away
  measure("yield_thread", 1000, [] { yield_thread(); });
  measure("sleep_until (passed)", N, [] { sleep_until(awake_instant{0}); });
  measure(
      "thread_create + thread_join", 200,
      []
      {
        thread_context ctx;
        ctx.entry = [](void*) noexcept {};
        if (thread_create(ctx, thread_desc{}).ok())
          (void)thread_join(ctx, awake_instant::forever());
      });
}
