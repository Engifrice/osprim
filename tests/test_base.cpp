#include "support.hpp"

#include "internal.hpp"

#include <osprim/base.hpp>

#include <doctest/doctest.h>

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

using namespace osprim;
using osprim_test::session;

/*****************************/
/* COMPILE-TIME PROPERTIES   */
/*****************************/

// status<> must stay an aggregate of 8 bytes: that is what makes MSVC
// return it in a register (any constructor would force a hidden pointer).
static_assert(sizeof(status<initialize_result>) == 8);
static_assert(alignof(status<initialize_result>) == 4);
static_assert(std::is_aggregate_v<status<initialize_result>>);
static_assert(std::is_trivially_copyable_v<status<initialize_result>>);
static_assert(std::is_standard_layout_v<status<initialize_result>>);
static_assert(status<initialize_result>{}.ok());
static_assert(!status<initialize_result>{
    initialize_result::INITIALIZE_OUT_OF_MEMORY, {}, 0}
                   .ok());

static_assert(std::is_same_v<std::underlying_type_t<native_error_kind>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<diagnostic_kind>, uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<initialize_result>, uint8_t>);
static_assert(static_cast<int>(initialize_result::INITIALIZE_OK) == 0);
static_assert(static_cast<int>(native_error_kind::NATIVE_ERROR_NONE) == 0);

static_assert(std::is_nothrow_invocable_v<decltype(&initialize), const init_desc&>);
static_assert(std::is_nothrow_invocable_v<decltype(&deinitialize)>);

static_assert(tracking_compiled == (OSPRIM_TRACKING != 0));

namespace
{
  /// @brief Allocator hooks that count, and can be told to fail.
  struct counting_allocator
  {
    std::atomic<size_t> allocations{0};
    std::atomic<size_t> frees{0};
    std::atomic<size_t> live_bytes{0};
    /// @brief Allocations left before failing (SIZE_MAX: never fail)
    std::atomic<size_t> budget{SIZE_MAX};

    static void* alloc(void* user, size_t size, size_t align) noexcept
    {
      auto* self = static_cast<counting_allocator*>(user);
      size_t b   = self->budget.load();
      if (b == 0)
        return nullptr;
      if (b != SIZE_MAX)
        self->budget.store(b - 1);
      CHECK(align != 0);
      CHECK((align & (align - 1)) == 0);
      void* p = ::malloc(size);
      if (p != nullptr)
      {
        ++self->allocations;
        self->live_bytes += size;
      }
      return p;
    }

    static void free(void* user, void* ptr, size_t size, size_t) noexcept
    {
      auto* self = static_cast<counting_allocator*>(user);
      ++self->frees;
      self->live_bytes -= size;
      ::free(ptr);
    }

    allocator hooks() noexcept { return allocator{this, &alloc, &free}; }
  };
} // namespace

/*****************************/
/* LIFETIME                  */
/*****************************/

TEST_CASE("base: initialize and deinitialize")
{
  for (int i = 0; i < 3; ++i)
  {
    CHECK(!detail::is_initialized());
    const auto s = initialize(init_desc{});
    REQUIRE(s.ok());
    CHECK(s.native_kind == native_error_kind::NATIVE_ERROR_NONE);
    CHECK(s.native_code == 0);
    CHECK(detail::is_initialized());
    deinitialize();
    CHECK(!detail::is_initialized());
  }
}

TEST_CASE("base: the session helper reports nothing on a clean run")
{
  session s;
  CHECK(detail::is_initialized());
  CHECK(s.end().count == 0);
  CHECK(!detail::is_initialized());
}

TEST_CASE("base: allocator hooks are used only by tracking, and balanced")
{
  counting_allocator a;
  {
    init_desc desc;
    desc.alloc = a.hooks();
    session s(desc);
    detail::track_open("test", 1, std::source_location::current());
    detail::track_close("test", 1, std::source_location::current());
  }
  if (tracking_compiled)
    CHECK(a.allocations > 0);
  else
    CHECK(a.allocations == 0);
  CHECK(a.allocations == a.frees);
  CHECK(a.live_bytes == 0);
}

TEST_CASE("base: no allocation when tracking is disabled at runtime")
{
  counting_allocator a;
  {
    init_desc desc;
    desc.alloc         = a.hooks();
    desc.track_handles = false;
    session s(desc);
    detail::track_open("test", 1, std::source_location::current());
    CHECK(detail::tracked_count() == 0);
    detail::track_close("test", 2, std::source_location::current());
  }
  CHECK(a.allocations == 0);
  CHECK(a.frees == 0);
}

#if OSPRIM_TRACKING

/*****************************/
/* TRACKING                  */
/*****************************/

TEST_CASE("tracking: balanced open and close report nothing")
{
  session s;
  for (uint64_t i = 1; i <= 3; ++i)
    detail::track_open("test", i, std::source_location::current());
  CHECK(detail::tracked_count() == 3);
  detail::track_close("test", 2, std::source_location::current());
  detail::track_close("test", 1, std::source_location::current());
  detail::track_close("test", 3, std::source_location::current());
  CHECK(detail::tracked_count() == 0);
}

TEST_CASE("tracking: a leak is reported at deinitialize with its creation site")
{
  session s;
  const auto where = std::source_location::current();
  detail::track_open("leaky", 42, where);

  const auto& log = s.end();
  REQUIRE(log.count == 1);
  const diagnostic& d = log.entries[0];
  CHECK(d.kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
  CHECK(strcmp(d.handle_type, "leaky") == 0);
  CHECK(d.native_value == 42);
  CHECK(d.created_at.line() == where.line());
  CHECK(strcmp(d.created_at.file_name(), where.file_name()) == 0);
  CHECK(d.reported_at.line() == where.line());
}

TEST_CASE("tracking: leaks are reported oldest first")
{
  session s;
  for (uint64_t i = 1; i <= 5; ++i)
    detail::track_open("test", i, std::source_location::current());
  detail::track_close("test", 2, std::source_location::current());
  detail::track_close("test", 4, std::source_location::current());

  const auto& log = s.end();
  REQUIRE(log.count == 3);
  CHECK(log.entries[0].native_value == 1);
  CHECK(log.entries[1].native_value == 3);
  CHECK(log.entries[2].native_value == 5);
}

TEST_CASE("tracking: closing an unknown handle is misuse")
{
  session s;
  const auto where = std::source_location::current();
  detail::track_close("test", 7, where);

  const auto& log = s.end();
  REQUIRE(log.count == 1);
  const diagnostic& d = log.entries[0];
  CHECK(d.kind == diagnostic_kind::DIAGNOSTIC_HANDLE_MISUSE);
  CHECK(d.native_value == 7);
  CHECK(d.created_at.line() == 0); // unknown
  CHECK(d.reported_at.line() == where.line());
}

TEST_CASE("tracking: closing twice is misuse")
{
  session s;
  detail::track_open("test", 7, std::source_location::current());
  detail::track_close("test", 7, std::source_location::current());
  const auto where = std::source_location::current();
  detail::track_close("test", 7, where);

  const auto& log = s.end();
  REQUIRE(log.count == 1);
  CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_MISUSE);
  CHECK(log.entries[0].reported_at.line() == where.line());
}

TEST_CASE("tracking: opening a value that is still open is misuse")
{
  session s;
  const auto first = std::source_location::current();
  detail::track_open("test", 9, first);
  const auto second = std::source_location::current();
  detail::track_open("test", 9, second);
  CHECK(detail::tracked_count() == 1);
  detail::track_close("test", 9, std::source_location::current());
  CHECK(detail::tracked_count() == 0);

  const auto& log = s.end();
  REQUIRE(log.count == 1);
  const diagnostic& d = log.entries[0];
  CHECK(d.kind == diagnostic_kind::DIAGNOSTIC_HANDLE_MISUSE);
  CHECK(d.created_at.line() == first.line());
  CHECK(d.reported_at.line() == second.line());
}

TEST_CASE("tracking: handles are keyed by type name and value")
{
  session s;
  // Same contents at different addresses: the same type
  static const char type_a[] = "file";
  static char type_b[]       = "file";
  detail::track_open(type_a, 1, std::source_location::current());
  detail::track_open("mapping", 1, std::source_location::current());
  CHECK(detail::tracked_count() == 2);
  detail::track_close(type_b, 1, std::source_location::current());
  CHECK(detail::tracked_count() == 1);
  detail::track_close("mapping", 1, std::source_location::current());
  CHECK(detail::tracked_count() == 0);
}

TEST_CASE("tracking: the registry grows through the allocator hooks")
{
  counting_allocator a;
  {
    init_desc desc;
    desc.alloc = a.hooks();
    session s(desc);
    constexpr uint64_t N = 1000;
    for (uint64_t i = 0; i < N; ++i)
      detail::track_open("test", i, std::source_location::current());
    CHECK(detail::tracked_count() == N);
    CHECK(a.allocations > 1);
    for (uint64_t i = N; i-- > 0;)
      detail::track_close("test", i, std::source_location::current());
    CHECK(detail::tracked_count() == 0);
  }
  CHECK(a.allocations == a.frees);
  CHECK(a.live_bytes == 0);
}

TEST_CASE("tracking: initialize fails cleanly when the registry cannot be allocated")
{
  counting_allocator a;
  a.budget = 0;
  init_desc desc;
  desc.alloc   = a.hooks();
  const auto s = initialize(desc);
  CHECK(s.result == initialize_result::INITIALIZE_OUT_OF_MEMORY);
  CHECK(s.native_kind == native_error_kind::NATIVE_ERROR_NONE);
  CHECK(!detail::is_initialized());

  // Nothing is left behind: initialization works afterwards
  session ok;
  CHECK(detail::is_initialized());
}

TEST_CASE("tracking: after a failed growth, unknown handles are not reported")
{
  counting_allocator a;
  a.budget = 1; // the initial array only
  {
    init_desc desc;
    desc.alloc = a.hooks();
    session s(desc);
    uint64_t opened = 0;
    for (; opened < 200; ++opened)
      detail::track_open("test", opened, std::source_location::current());
    const size_t recorded = detail::tracked_count();
    CHECK(recorded > 0);
    CHECK(recorded < 200);
    // Closing everything, recorded or not: no false misuse reports
    for (uint64_t i = 0; i < opened; ++i)
      detail::track_close("test", i, std::source_location::current());
    CHECK(detail::tracked_count() == 0);
  }
  CHECK(a.allocations == a.frees);
}

TEST_CASE("tracking: a null callback drops diagnostics")
{
  REQUIRE(initialize(init_desc{}).ok());
  detail::track_open("test", 1, std::source_location::current());
  detail::track_close("test", 2, std::source_location::current());
  deinitialize(); // one leak and one misuse, both dropped
  CHECK(!detail::is_initialized());
}

TEST_CASE("tracking: concurrent open and close")
{
  session s;
  constexpr int THREADS         = 4;
  constexpr uint64_t PER_THREAD = 2000;
  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; ++t)
  {
    threads.emplace_back(
        [t]
        {
          const uint64_t base = static_cast<uint64_t>(t) * PER_THREAD;
          for (uint64_t i = 0; i < PER_THREAD; ++i)
            detail::track_open("test", base + i, std::source_location::current());
          for (uint64_t i = 0; i < PER_THREAD; ++i)
            detail::track_close("test", base + i, std::source_location::current());
        });
  }
  for (auto& th : threads)
    th.join();
  CHECK(detail::tracked_count() == 0);
}

#else // OSPRIM_TRACKING

TEST_CASE("tracking: compiled out")
{
  session s;
  detail::track_open("test", 1, std::source_location::current());
  CHECK(detail::tracked_count() == 0);
  detail::track_close("test", 2, std::source_location::current());
  // ~session: no diagnostic
}

#endif // OSPRIM_TRACKING

#ifdef OSPRIM_TEST_USER_CONFIG

/*****************************/
/* USER CONFIGURATION        */
/*****************************/

TEST_CASE("user config: OSPRIM_ASSERT is overridden in the library")
{
  const int before = osprim_test_assert_failures.load();

  // Not initialized: the library's precondition check fires
  deinitialize();
  CHECK(osprim_test_assert_failures.load() == before + 1);

  // Mismatched hooks: checked, then the default allocator is used
  counting_allocator a;
  init_desc desc;
  desc.alloc.user     = &a;
  desc.alloc.alloc_fn = &counting_allocator::alloc;
  REQUIRE(initialize(desc).ok());
  CHECK(osprim_test_assert_failures.load() == before + 2);
  deinitialize();
  CHECK(a.allocations == 0);
}

#endif // OSPRIM_TEST_USER_CONFIG
