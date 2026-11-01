#include "support.hpp"

#include <osprim/clock.hpp>
#include <osprim/wait.hpp>

#include <doctest/doctest.h>

#include <stdint.h>

#include <atomic>
#include <chrono>
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

  void sleep_ms(int ms)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

  /// @brief How long a test waits before deciding that a wake was lost.
  constexpr int64_t LOST_WAKE_NS = 10 * S;

  /// @brief Waits until `word` stops holding `expected`, the usual loop
  /// around wait().
  /// @return false if `deadline` passed first
  bool wait_until_changed(
      const std::atomic<wait_word_t>& word, wait_word_t expected,
      awake_instant deadline)
  {
    while (word.load(std::memory_order_acquire) == expected)
    {
      if (wait(word, expected, deadline) == wait_result::WAIT_TIMED_OUT)
        return word.load(std::memory_order_acquire) != expected;
    }
    return true;
  }
} // namespace

/*****************************/
/* COMPILE-TIME PROPERTIES   */
/*****************************/

static_assert(std::is_same_v<std::underlying_type_t<wait_result>, uint8_t>);
static_assert(static_cast<int>(wait_result::WAIT_WOKEN) == 0);
static_assert(std::is_unsigned_v<wait_word_t>);
static_assert(noexcept(
    wait(std::declval<const std::atomic<wait_word_t>&>(), 0, awake_instant{})));
static_assert(noexcept(wake_one(std::declval<const std::atomic<wait_word_t>&>())));
static_assert(noexcept(wake_all(std::declval<const std::atomic<wait_word_t>&>())));

/*****************************/
/* SINGLE THREAD             */
/*****************************/

TEST_CASE("wait: returns at once when the word differs")
{
  session s;
  std::atomic<wait_word_t> word{1};
  const awake_instant start = awake_now();
  CHECK(wait(word, 0, awake_instant::forever()) == wait_result::WAIT_WOKEN);
  CHECK(wait(word, 0, deadline_after(10 * S)) == wait_result::WAIT_WOKEN);
  CHECK(elapsed_ns(start, awake_now()) < 1 * S);
}

TEST_CASE("wait: a deadline that has passed times out at once")
{
  session s;
  std::atomic<wait_word_t> word{0};
  const awake_instant start = awake_now();
  CHECK(wait(word, 0, awake_instant{0}) == wait_result::WAIT_TIMED_OUT);
  CHECK(wait(word, 0, awake_instant{-5}) == wait_result::WAIT_TIMED_OUT);
  CHECK(wait(word, 0, awake_instant{INT64_MIN}) == wait_result::WAIT_TIMED_OUT);
  CHECK(wait(word, 0, deadline_after(-1 * MS)) == wait_result::WAIT_TIMED_OUT);
  CHECK(wait(word, 0, start) == wait_result::WAIT_TIMED_OUT);
  CHECK(elapsed_ns(start, awake_now()) < 1 * S);

  // Zero: has passed, or passes within the call (spurious returns allowed)
  int tries                    = 0;
  const awake_instant deadline = deadline_after(0);
  while (wait(word, 0, deadline) != wait_result::WAIT_TIMED_OUT && tries < 1000)
    ++tries;
  CHECK(tries < 1000);
}

TEST_CASE("wait: a timeout is never reported before the deadline")
{
  session s;
  std::atomic<wait_word_t> word{0};
  const int64_t durations[] = {0, 1 * US, 50 * US, 1 * MS, 5 * MS, 20 * MS};
  for (const int64_t d : durations)
  {
    CAPTURE(d);
    const int repeats = d >= 20 * MS ? 5 : 20;
    int64_t worst     = 0;
    int64_t total     = 0;
    int spurious      = 0;
    for (int i = 0; i < repeats; ++i)
    {
      const awake_instant deadline = deadline_after(d);
      while (wait(word, 0, deadline) != wait_result::WAIT_TIMED_OUT)
        ++spurious;
      const awake_instant now = awake_now();
      CHECK(now >= deadline);
      const int64_t late = elapsed_ns(deadline, now);
      worst              = late > worst ? late : worst;
      total += late;
    }
    CHECK(worst < 1 * S);
    note(
        "timeout %6lld us: late by %8.1f us on average, %8.1f us at worst (%d "
        "spurious)",
        static_cast<long long>(d / US),
        static_cast<double>(total) / repeats / 1000.0,
        static_cast<double>(worst) / 1000.0, spurious);
  }
}

TEST_CASE("wait: wake without waiters is harmless")
{
  session s;
  std::atomic<wait_word_t> word{0};
  wake_one(word);
  wake_all(word);
  CHECK(wait(word, 0, awake_instant{0}) == wait_result::WAIT_TIMED_OUT);
}

/*****************************/
/* WAKES                     */
/*****************************/

TEST_CASE("wait: wake_one releases a blocked waiter")
{
  session s;
  std::atomic<wait_word_t> word{0};
  std::atomic<bool> released{false};
  std::thread waiter(
      [&] { released = wait_until_changed(word, 0, deadline_after(LOST_WAKE_NS)); });
  sleep_ms(20); // most likely blocked by now
  const awake_instant woke_at = awake_now();
  word.store(1, std::memory_order_release);
  wake_one(word);
  waiter.join();
  CHECK(released.load());
  CHECK(elapsed_ns(woke_at, awake_now()) < LOST_WAKE_NS);
}

TEST_CASE("wait: wake_all releases every waiter")
{
  session s;
  constexpr int N = 8;
  std::atomic<wait_word_t> word{0};
  std::atomic<int> released{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < N; ++i)
    waiters.emplace_back(
        [&]
        {
          if (wait_until_changed(word, 0, deadline_after(LOST_WAKE_NS)))
            ++released;
        });
  sleep_ms(20);
  word.store(1, std::memory_order_release);
  wake_all(word);
  for (auto& t : waiters)
    t.join();
  CHECK(released.load() == N);
}

TEST_CASE("wait: one wake_one per waiter releases them all")
{
  session s;
  constexpr int N = 8;
  std::atomic<wait_word_t> word{0};
  std::atomic<int> released{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < N; ++i)
    waiters.emplace_back(
        [&]
        {
          if (wait_until_changed(word, 0, deadline_after(LOST_WAKE_NS)))
            ++released;
        });
  sleep_ms(20);
  word.store(1, std::memory_order_release);
  // A waiter not yet blocked sees 1 and does not need its wake
  for (int i = 0; i < N; ++i)
    wake_one(word);
  for (auto& t : waiters)
    t.join();
  CHECK(released.load() == N);
}

TEST_CASE("wait: wakes do not cross words")
{
  session s;
  std::atomic<wait_word_t> waited{0};
  std::atomic<wait_word_t> other{0};
  std::atomic<bool> done{false};
  int woken = 0;
  std::thread waiter(
      [&]
      {
        const awake_instant deadline = deadline_after(100 * MS);
        while (wait(waited, 0, deadline) != wait_result::WAIT_TIMED_OUT)
          ++woken;
        done = true;
      });
  int wakes = 0;
  while (!done.load())
  {
    wake_all(other);
    ++wakes;
  }
  waiter.join();
  note("%d wakes on another word, %d returns before the timeout", wakes, woken);
  CHECK(woken < 10); // spurious returns are allowed, crosstalk would be thousands
}

/*****************************/
/* STRESS                    */
/*****************************/

TEST_CASE("wait: ping-pong between two threads loses no wake")
{
  session s;
  constexpr wait_word_t ROUNDS = 10000;
  std::atomic<wait_word_t> turn{0};
  std::atomic<int> lost{0};

  // Player p moves when turn % 2 == p, then hands over
  auto player = [&](wait_word_t p)
  {
    for (wait_word_t i = 0; i < ROUNDS; ++i)
    {
      const awake_instant deadline = deadline_after(LOST_WAKE_NS);
      wait_word_t v                = turn.load(std::memory_order_acquire);
      while (v % 2 != p)
      {
        if (wait(turn, v, deadline) == wait_result::WAIT_TIMED_OUT
            && turn.load(std::memory_order_acquire) % 2 != p)
        {
          ++lost;
          return;
        }
        v = turn.load(std::memory_order_acquire);
      }
      turn.store(v + 1, std::memory_order_release);
      wake_one(turn);
    }
  };

  const awake_instant start = awake_now();
  std::thread a(player, 0);
  std::thread b(player, 1);
  a.join();
  b.join();
  CHECK(lost.load() == 0);
  CHECK(turn.load() == 2 * ROUNDS);
  note(
      "ping-pong: %.2f us per hand-over",
      static_cast<double>(elapsed_ns(start, awake_now())) / (2.0 * ROUNDS) / 1000.0);
}

TEST_CASE("wait: a futex mutex built on wait/wake")
{
  // The classic three-state mutex (0 free, 1 locked, 2 locked with waiters)
  session s;
  std::atomic<wait_word_t> state{0};
  std::atomic<int> suspicious{0};
  long long counter = 0; // protected by the mutex

  auto lock = [&]
  {
    wait_word_t c = 0;
    if (state.compare_exchange_strong(
            c, 1, std::memory_order_acquire, std::memory_order_relaxed))
      return;
    if (c != 2)
      c = state.exchange(2, std::memory_order_acquire);
    while (c != 0)
    {
      // Critical sections are tiny: blocked this long means a lost wake
      if (wait(state, 2, deadline_after(LOST_WAKE_NS))
          == wait_result::WAIT_TIMED_OUT)
        ++suspicious;
      c = state.exchange(2, std::memory_order_acquire);
    }
  };
  auto unlock = [&]
  {
    if (state.exchange(0, std::memory_order_release) == 2)
      wake_one(state);
  };

  constexpr int THREADS    = 4;
  constexpr int ITERATIONS = 20000;
  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; ++t)
    threads.emplace_back(
        [&]
        {
          for (int i = 0; i < ITERATIONS; ++i)
          {
            lock();
            ++counter;
            unlock();
          }
        });
  for (auto& t : threads)
    t.join();
  CHECK(counter == static_cast<long long>(THREADS) * ITERATIONS);
  CHECK(suspicious.load() == 0);
  CHECK(state.load() == 0);
}

/*****************************/
/* COST                      */
/*****************************/

TEST_CASE("wait: cost of the calls that do not block")
{
  session s;
  constexpr int N = 100'000;
  std::atomic<wait_word_t> word{1};

  auto measure = [&](const char* name, auto fn)
  {
    const awake_instant start = awake_now();
    for (int i = 0; i < N; ++i)
      fn();
    const double ns = static_cast<double>(elapsed_ns(start, awake_now())) / N;
    note("%-28s %8.2f ns per call", name, ns);
    CHECK(ns < 100'000.0);
  };

  measure(
      "wait (word differs)", [&] { (void)wait(word, 0, awake_instant::forever()); });
  measure("wait (deadline passed)", [&] { (void)wait(word, 1, awake_instant{0}); });
  measure("wake_one (nobody waiting)", [&] { wake_one(word); });
  measure("wake_all (nobody waiting)", [&] { wake_all(word); });
}
