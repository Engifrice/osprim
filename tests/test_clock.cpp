#include "support.hpp"

#include "internal.hpp"

#include <osprim/clock.hpp>

#include <doctest/doctest.h>

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

using namespace osprim;
using osprim_test::note;
using osprim_test::session;

namespace
{
  constexpr int64_t US = 1000;
  constexpr int64_t MS = 1000 * US;
  constexpr int64_t S  = 1000 * MS;

  int64_t abs64(int64_t v) noexcept
  {
    return v < 0 ? -v : v;
  }

  void sleep_ms(int ms)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

  const char* source_name(tick_source s) noexcept
  {
    switch (s)
    {
    case tick_source::TICK_SOURCE_AWAKE:
      return "awake clock";
    case tick_source::TICK_SOURCE_X86_TSC:
      return "x86 TSC";
    case tick_source::TICK_SOURCE_ARM64_CNTVCT:
      return "arm64 CNTVCT_EL0";
    }
    return "?";
  }
} // namespace

/*****************************/
/* COMPILE-TIME PROPERTIES   */
/*****************************/

static_assert(
    sizeof(awake_instant) == 8 && std::is_trivially_copyable_v<awake_instant>);
static_assert(
    sizeof(continuous_instant) == 8
    && std::is_trivially_copyable_v<continuous_instant>);
static_assert(
    sizeof(wall_instant) == 8 && std::is_trivially_copyable_v<wall_instant>);
static_assert(std::is_aggregate_v<awake_instant>);
static_assert(std::is_trivially_copyable_v<ticks_info>);
static_assert(std::is_trivially_copyable_v<ticks_sample>);
static_assert(std::is_same_v<std::underlying_type_t<clock_id>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<tick_source>, uint8_t>);

// The instant types do not convert into each other
static_assert(!std::is_convertible_v<awake_instant, continuous_instant>);
static_assert(!std::is_convertible_v<continuous_instant, awake_instant>);
static_assert(!std::is_convertible_v<wall_instant, awake_instant>);

static_assert(FOREVER_NS == INT64_MAX);
static_assert(awake_instant::forever().is_forever());
static_assert(!awake_instant{0}.is_forever());

// after(): plain addition, saturating to forever()
static_assert(awake_instant{10}.after(5).ns == 15);
static_assert(awake_instant{10}.after(0).ns == 10);
static_assert(awake_instant{10}.after(-20).ns == -10);
static_assert(awake_instant{10}.after(FOREVER_NS).is_forever());
static_assert(awake_instant{10}.after(FOREVER_NS - 10).is_forever());
static_assert(awake_instant{10}.after(FOREVER_NS - 11).ns == FOREVER_NS - 1);
static_assert(awake_instant{0}.after(FOREVER_NS - 1).ns == FOREVER_NS - 1);
static_assert(awake_instant::forever().after(-5).is_forever());
static_assert(awake_instant::forever().after(5).is_forever());

// remaining(): signed, FOREVER_NS without deadline
static_assert(awake_instant{100}.remaining(awake_instant{30}) == 70);
static_assert(awake_instant{30}.remaining(awake_instant{100}) == -70);
static_assert(awake_instant::forever().remaining(awake_instant{100}) == FOREVER_NS);

// Ordering, forever() after everything
static_assert(awake_instant{1} < awake_instant{2});
static_assert(awake_instant{2} == awake_instant{2});
static_assert(awake_instant{FOREVER_NS - 1} < awake_instant::forever());
static_assert(continuous_instant{1} < continuous_instant{2});
static_assert(wall_instant{-1} < wall_instant{0});

static_assert(elapsed_ns(awake_instant{3}, awake_instant{5}) == 2);
static_assert(elapsed_ns(awake_instant{5}, awake_instant{3}) == -2);
static_assert(elapsed_ns(continuous_instant{3}, continuous_instant{10}) == 7);

static_assert(
    noexcept(awake_now()) && noexcept(continuous_now()) && noexcept(wall_now()));
static_assert(
    noexcept(ticks_now()) && noexcept(ticks_to_ns(0)) && noexcept(sample_ticks()));
static_assert(
    noexcept(deadline_after(0))
    && noexcept(clock_resolution_ns(clock_id::CLOCK_ID_AWAKE)));

/*****************************/
/* READING                   */
/*****************************/

TEST_CASE("clock: readings are plausible")
{
  session s;
  CHECK(awake_now().ns > 0);
  CHECK(continuous_now().ns > 0);
  // After 2020-01-01 and before 2100-01-01
  CHECK(wall_now().ns > 1577836800 * S);
  CHECK(wall_now().ns < 4102444800 * S);
  CHECK(ticks_now() > 0);
}

TEST_CASE("clock: readings never decrease on one thread")
{
  session s;
  constexpr int N = 1'000'000;

  awake_instant a      = awake_now();
  continuous_instant c = continuous_now();
  uint64_t t           = ticks_now();
  int a_back = 0, c_back = 0, t_back = 0;
  int a_moves = 0, t_moves = 0;
  for (int i = 0; i < N; ++i)
  {
    const awake_instant a2      = awake_now();
    const continuous_instant c2 = continuous_now();
    const uint64_t t2           = ticks_now();
    a_back += a2 < a;
    c_back += c2 < c;
    t_back += t2 < t;
    a_moves += a2 != a;
    t_moves += t2 != t;
    a = a2;
    c = c2;
    t = t2;
  }
  CHECK(a_back == 0);
  CHECK(c_back == 0);
  CHECK(t_back == 0);
  // The clocks actually move
  CHECK(a_moves > 0);
  CHECK(t_moves > 0);
}

TEST_CASE("clock: awake and continuous never decrease across threads")
{
  // A reading published by one thread (release) and observed by another
  // (acquire) happened before the observer's own reading: the observer's
  // reading must not be smaller.
  session s;
  constexpr int PAIRS      = 2;
  constexpr int ITERATIONS = 200'000;
  std::atomic<int> violations{0};
  std::atomic<int> observed{0};

  auto run = [&](auto now_fn)
  {
    std::vector<std::thread> threads;
    for (int p = 0; p < PAIRS; ++p)
    {
      auto slot = std::make_shared<std::atomic<int64_t>>(INT64_MIN);
      auto done = std::make_shared<std::atomic<bool>>(false);
      threads.emplace_back(
          [=]
          {
            for (int i = 0; i < ITERATIONS; ++i)
              slot->store(now_fn(), std::memory_order_release);
            done->store(true, std::memory_order_release);
          });
      threads.emplace_back(
          [=, &violations, &observed]
          {
            int local_violations = 0, local_observed = 0;
            while (!done->load(std::memory_order_acquire))
            {
              const int64_t published = slot->load(std::memory_order_acquire);
              const int64_t mine      = now_fn();
              if (published != INT64_MIN)
              {
                ++local_observed;
                local_violations += mine < published;
              }
            }
            violations += local_violations;
            observed += local_observed;
          });
    }
    for (auto& th : threads)
      th.join();
  };

  run([]() noexcept { return awake_now().ns; });
  CHECK(violations.load() == 0);
  run([]() noexcept { return continuous_now().ns; });
  CHECK(violations.load() == 0);
  note("cross-thread comparisons: %d", observed.load());
}

TEST_CASE("clock: continuous minus awake is stable without suspend")
{
  session s;
  const awake_instant a0      = awake_now();
  const continuous_instant c0 = continuous_now();
  sleep_ms(50);
  const awake_instant a1      = awake_now();
  const continuous_instant c1 = continuous_now();

  // continuous counts at least what awake counts
  const int64_t d0 = c0.ns - a0.ns;
  const int64_t d1 = c1.ns - a1.ns;
  CHECK(d0 >= -1 * MS);
  CHECK(abs64(d1 - d0) < 5 * MS);
  note(
      "continuous - awake: %lld ns (time spent suspended since boot, roughly)",
      static_cast<long long>(d0));
}

TEST_CASE("clock: wall agrees with std::chrono::system_clock")
{
  session s;
  const wall_instant w = wall_now();
  const int64_t sys    = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  CHECK(abs64(sys - w.ns) < 100 * MS);
}

TEST_CASE("clock: sleeping advances every clock by about the same amount")
{
  session s;
  constexpr int SLEEP_MS = 20;

  const ticks_sample s0       = sample_ticks();
  const continuous_instant c0 = continuous_now();
  const wall_instant w0       = wall_now();
  sleep_ms(SLEEP_MS);
  const ticks_sample s1       = sample_ticks();
  const continuous_instant c1 = continuous_now();
  const wall_instant w1       = wall_now();

  const int64_t awake = elapsed_ns(s0.awake, s1.awake);
  const int64_t cont  = elapsed_ns(c0, c1);
  const int64_t wall  = w1.ns - w0.ns;
  const int64_t ticks = ticks_to_ns(s1.ticks - s0.ticks);

  // The standard library may sleep on another clock source: 1 ms of slack
  CHECK(awake >= SLEEP_MS * MS - 1 * MS);
  CHECK(awake < SLEEP_MS * MS + 5 * S); // loaded CI machines oversleep
  CHECK(abs64(cont - awake) < 1 * MS);
  CHECK(abs64(wall - awake) < 50 * MS); // the wall clock may be slewed
  CHECK(abs64(ticks - awake) < awake / 100 + 100 * US);
  note(
      "slept %d ms: awake %lld ns, continuous %lld ns, wall %lld ns, ticks %lld ns",
      SLEEP_MS, static_cast<long long>(awake), static_cast<long long>(cont),
      static_cast<long long>(wall), static_cast<long long>(ticks));
}

TEST_CASE("clock: deadline_after")
{
  session s;
  const awake_instant before = awake_now();
  const awake_instant d      = deadline_after(1 * MS);
  const awake_instant after  = awake_now();
  CHECK(d.ns >= before.ns + 1 * MS);
  CHECK(d.ns <= after.ns + 1 * MS);
  CHECK(!d.is_forever());

  const awake_instant zero = deadline_after(0);
  CHECK(zero.ns >= before.ns);
  CHECK(zero.remaining(awake_now()) <= 0);

  CHECK(deadline_after(-5 * MS) < awake_now());
  CHECK(deadline_after(FOREVER_NS).is_forever());
  CHECK(deadline_after(FOREVER_NS - 1).is_forever()); // saturates
  const awake_instant far = deadline_after(INT64_MAX / 2);
  CHECK(!far.is_forever());
  CHECK(far.remaining(awake_now()) > INT64_MAX / 2 - 1 * S);
}

/*****************************/
/* INFORMATION               */
/*****************************/

TEST_CASE("clock: resolutions")
{
  session s;
  const clock_id ids[] = {
      clock_id::CLOCK_ID_AWAKE, clock_id::CLOCK_ID_CONTINUOUS,
      clock_id::CLOCK_ID_WALL, clock_id::CLOCK_ID_TICKS};
  const char* names[] = {"awake", "continuous", "wall", "ticks"};
  for (int i = 0; i < 4; ++i)
  {
    const int64_t r = clock_resolution_ns(ids[i]);
    CAPTURE(i);
    CHECK(r > 0);
    CHECK(r <= 1 * MS);
    note("%s resolution: %lld ns", names[i], static_cast<long long>(r));
  }
  const ticks_info info = get_ticks_info();
  const uint64_t expected =
      (1'000'000'000 + info.frequency_hz - 1) / info.frequency_hz;
  CHECK(
      clock_resolution_ns(clock_id::CLOCK_ID_TICKS)
      == static_cast<int64_t>(expected < 1 ? 1 : expected));
}

TEST_CASE("clock: tick counter information")
{
  session s;
  const ticks_info info = get_ticks_info();
  note(
      "ticks: %s, %llu Hz, %s", source_name(info.source),
      static_cast<unsigned long long>(info.frequency_hz),
      info.frequency_exact ? "exact" : "calibrated");
  CHECK(info.frequency_hz > 0);

  switch (info.source)
  {
  case tick_source::TICK_SOURCE_AWAKE:
    CHECK(info.frequency_hz == 1'000'000'000);
    CHECK(info.frequency_exact);
    break;
  case tick_source::TICK_SOURCE_X86_TSC:
    CHECK(OSPRIM_ARCH_X86_64);
    CHECK(!info.frequency_exact);
    CHECK(info.frequency_hz > 100'000'000);    // 100 MHz
    CHECK(info.frequency_hz < 10'000'000'000); // 10 GHz
    break;
  case tick_source::TICK_SOURCE_ARM64_CNTVCT:
    CHECK(OSPRIM_ARCH_ARM64);
    CHECK(info.frequency_exact);
    CHECK(info.frequency_hz >= 1'000'000);      // 1 MHz
    CHECK(info.frequency_hz <= 10'000'000'000); // 10 GHz (1 GHz from Armv8.6)
    break;
  default:
    FAIL("unknown tick source");
  }

  // Constant after initialize()
  const ticks_info again = get_ticks_info();
  CHECK(again.frequency_hz == info.frequency_hz);
  CHECK(again.source == info.source);
}

TEST_CASE("clock: tick frequency agrees with the awake clock")
{
  session s;
  const ticks_sample s0 = sample_ticks();
  sleep_ms(100);
  const ticks_sample s1 = sample_ticks();

  const int64_t awake = elapsed_ns(s0.awake, s1.awake);
  const uint64_t dt   = s1.ticks - s0.ticks;
  const double measured_hz =
      static_cast<double>(dt) * 1e9 / static_cast<double>(awake);
  const double reported_hz = static_cast<double>(get_ticks_info().frequency_hz);
  const double error       = (measured_hz - reported_hz) / reported_hz;
  note(
      "measured %.0f Hz, reported %.0f Hz, relative error %.3g", measured_hz,
      reported_hz, error);
  CHECK(error > -0.01);
  CHECK(error < 0.01);
  CHECK(abs64(ticks_to_ns(dt) - awake) < awake / 100);
}

/*****************************/
/* TICKS                     */
/*****************************/

TEST_CASE("clock: sample_ticks brackets its readings")
{
  session s;
  for (int i = 0; i < 1000; ++i)
  {
    const uint64_t t0      = ticks_now();
    const awake_instant a0 = awake_now();
    const ticks_sample smp = sample_ticks();
    const awake_instant a1 = awake_now();
    const uint64_t t1      = ticks_now();
    CHECK(a0 <= smp.awake);
    CHECK(smp.awake <= a1);
    CHECK(t0 <= smp.ticks);
    CHECK(smp.ticks <= t1);
  }
}

TEST_CASE("clock: ticks_to_ns with the current counter")
{
  session s;
  const uint64_t f = get_ticks_info().frequency_hz;
  CHECK(ticks_to_ns(0) == 0);
  // One second of ticks: within 2 ns (the factor is truncated)
  // One second and one hour of ticks: exact
  CHECK(ticks_to_ns(f) == 1 * S);
  CHECK(ticks_to_ns(f * 3600) == 3600 * S);
  // Monotonic over the whole range, saturating at INT64_MAX
  int64_t prev = 0;
  for (int shift = 0; shift < 64; ++shift)
  {
    const int64_t ns = ticks_to_ns(uint64_t{1} << shift);
    CAPTURE(shift);
    CHECK(ns >= prev);
    prev = ns;
  }
  CHECK(ticks_to_ns(UINT64_MAX) >= prev);
}

TEST_CASE("clock: tick conversion factor")
{
  // 1 GHz: exactly 2^63 >> 63
  const auto ghz = detail::make_ticks_conversion(1'000'000'000);
  CHECK(ghz.mult == uint64_t{1} << 63);
  CHECK(ghz.shift == 63);
  // Every factor keeps as many bits as possible
  for (uint64_t hz :
       {uint64_t{1}, uint64_t{1'000}, uint64_t{10'000'000}, uint64_t{19'200'000},
        uint64_t{24'000'000}, uint64_t{1'000'000'000}, uint64_t{2'100'000'395},
        uint64_t{3'000'000'000}, uint64_t{5'999'999'999}, uint64_t{999'999'999'999}})
  {
    CAPTURE(hz);
    const auto c = detail::make_ticks_conversion(hz);
    CHECK(c.shift >= 32);
    CHECK(c.shift <= 63);
    // At least 59 significant bits for every real counter frequency
    if (hz >= 1'000'000 && hz <= 10'000'000'000)
      CHECK(c.mult >= (uint64_t{1} << 59));
  }
}

namespace
{
  /// @brief Runs ticks_to_ns() with the factor of another frequency.
  struct with_frequency
  {
    uint64_t saved_mult;
    uint32_t saved_shift;

    explicit with_frequency(uint64_t hz) noexcept
        : saved_mult(detail::g_clock.ticks_mult)
        , saved_shift(detail::g_clock.ticks_shift)
    {
      const auto c                = detail::make_ticks_conversion(hz);
      detail::g_clock.ticks_mult  = c.mult;
      detail::g_clock.ticks_shift = c.shift;
    }
    ~with_frequency()
    {
      detail::g_clock.ticks_mult  = saved_mult;
      detail::g_clock.ticks_shift = saved_shift;
    }
    with_frequency(const with_frequency&)            = delete;
    with_frequency& operator=(const with_frequency&) = delete;
  };

  /// @brief Deterministic pseudo-random values (xorshift64*)
  struct rng
  {
    uint64_t state = 0x9E3779B97F4A7C15u;
    uint64_t next() noexcept
    {
      state ^= state >> 12;
      state ^= state << 25;
      state ^= state >> 27;
      return state * 0x2545F4914F6CDD1Du;
    }
  };
} // namespace

TEST_CASE("clock: ticks_to_ns is exact for the frequencies met in practice")
{
  // ticks_to_ns(t) == floor(t * 1e9 / hz), for any t up to centuries
  session s;
  rng r;

  SUBCASE("1 GHz (awake fallback): identity, saturating at 2^63")
  {
    with_frequency f(1'000'000'000);
    CHECK(ticks_to_ns(0) == 0);
    CHECK(ticks_to_ns(1) == 1);
    for (int i = 0; i < 10000; ++i)
    {
      const uint64_t t = r.next() >> 1;
      CHECK(ticks_to_ns(t) == static_cast<int64_t>(t));
    }
    CHECK(ticks_to_ns(static_cast<uint64_t>(INT64_MAX)) == INT64_MAX);
    CHECK(ticks_to_ns(static_cast<uint64_t>(INT64_MAX) + 1) == INT64_MAX);
    CHECK(ticks_to_ns(UINT64_MAX) == INT64_MAX);
  }
  SUBCASE("24 MHz (Apple silicon and many arm64 SoCs): t * 125 / 3")
  {
    with_frequency f(24'000'000);
    CHECK(ticks_to_ns(24'000'000) == 1'000'000'000);
    CHECK(ticks_to_ns(3) == 125);
    CHECK(ticks_to_ns(1) == 41);
    for (int i = 0; i < 10000; ++i)
    {
      const uint64_t t = r.next() % 95'000'000'000'000'000u; // 125 years
      CHECK(
          ticks_to_ns(t) == static_cast<int64_t>((t / 3) * 125 + (t % 3) * 125 / 3));
    }
    // 2^63 ns is about 2.2e17 ticks: saturates long before 2^64
    CHECK(ticks_to_ns(uint64_t{1} << 57) < INT64_MAX);
    CHECK(ticks_to_ns(uint64_t{1} << 58) == INT64_MAX);
    CHECK(ticks_to_ns(UINT64_MAX) == INT64_MAX);
  }
  SUBCASE("19.2 MHz (Qualcomm): t * 625 / 12")
  {
    with_frequency f(19'200'000);
    CHECK(ticks_to_ns(19'200'000) == 1'000'000'000);
    for (int i = 0; i < 10000; ++i)
    {
      const uint64_t t = r.next() % 24'000'000'000'000'000u; // 39 years
      CHECK(
          ticks_to_ns(t)
          == static_cast<int64_t>((t / 12) * 625 + (t % 12) * 625 / 12));
    }
  }
  SUBCASE("10 MHz: t * 100")
  {
    with_frequency f(10'000'000);
    for (int i = 0; i < 10000; ++i)
    {
      const uint64_t t = r.next() >> 8;
      CHECK(ticks_to_ns(t) == static_cast<int64_t>(t * 100));
    }
  }
  SUBCASE("3 GHz: t / 3, never saturates")
  {
    with_frequency f(3'000'000'000);
    CHECK(ticks_to_ns(3'000'000'000) == 1'000'000'000);
    CHECK(ticks_to_ns(2) == 0);
    CHECK(ticks_to_ns(3) == 1);
    for (int i = 0; i < 10000; ++i)
    {
      const uint64_t t = r.next() % 3'000'000'000'000'000'000u; // 31 years
      CHECK(ticks_to_ns(t) == static_cast<int64_t>(t / 3));
    }
    // Beyond, at most 1 ns too large
    const int64_t max = ticks_to_ns(UINT64_MAX);
    CHECK(max >= static_cast<int64_t>(UINT64_MAX / 3));
    CHECK(max <= static_cast<int64_t>(UINT64_MAX / 3) + 1);
  }
  // 128-bit division needs compiler-rt, which clang-cl does not link
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
  SUBCASE("calibrated frequencies: within 1 ns of the exact value")
  {
    __extension__ using u128 = unsigned __int128;
    for (uint64_t hz :
         {uint64_t{2'100'000'395}, uint64_t{3'187'654'321}, uint64_t{1'999'999'999}})
    {
      CAPTURE(hz);
      with_frequency f(hz);
      for (int i = 0; i < 10000; ++i)
      {
        const uint64_t t     = r.next() >> 4;
        const u128 exact     = static_cast<u128>(t) * 1'000'000'000u / hz;
        const int64_t expect = exact > static_cast<u128>(INT64_MAX)
                                   ? INT64_MAX
                                   : static_cast<int64_t>(exact);
        const int64_t got    = ticks_to_ns(t);
        CHECK(got >= expect);
        CHECK(got - expect <= 1);
      }
    }
  }
#endif
}

/*****************************/
/* LIFETIME                  */
/*****************************/

TEST_CASE("clock: readings continue across re-initialization")
{
  awake_instant a0{};
  continuous_instant c0{};
  {
    session s;
    a0 = awake_now();
    c0 = continuous_now();
  }
  session s;
  CHECK(awake_now() >= a0);
  CHECK(continuous_now() >= c0);
}

/*****************************/
/* COST                      */
/*****************************/

TEST_CASE("clock: cost of a reading")
{
  // Informational: printed, only checked against absurd values
  session s;
  constexpr int N        = 200'000;
  volatile uint64_t sink = 0;

  auto measure = [&](const char* name, auto fn)
  {
    const awake_instant start = awake_now();
    for (int i = 0; i < N; ++i)
      sink = sink ^ static_cast<uint64_t>(fn());
    const double ns = static_cast<double>(elapsed_ns(start, awake_now())) / N;
    note("%-15s %8.2f ns per call", name, ns);
    CHECK(ns < 100'000.0);
  };

  measure("awake_now", []() noexcept { return awake_now().ns; });
  measure("continuous_now", []() noexcept { return continuous_now().ns; });
  measure("wall_now", []() noexcept { return wall_now().ns; });
  measure("ticks_now", []() noexcept { return static_cast<int64_t>(ticks_now()); });
  measure("ticks_to_ns", []() noexcept { return ticks_to_ns(12345); });
  measure("sample_ticks", []() noexcept { return sample_ticks().awake.ns; });
}
