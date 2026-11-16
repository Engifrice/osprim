/*****************************************************************/ /**
 * @file   test_async_io.cpp
 * @brief  Async I/O: availability, reads and writes, end of file,
 * capacity, batches, deadlines, wakes, cancellation, errors, unbuffered
 * files and tracking.
 *
 * Where the platform has no backend (Android, Apple), creation must
 * return UNSUPPORTED; on Linux, a kernel or policy refusing io_uring
 * (DISABLED, TOO_OLD, OUT_OF_MEMORY) skips the tests with a note.
 *********************************************************************/
#include "files.hpp"
#include "internal.hpp"
#include "support.hpp"

#include <osprim/async_io.hpp>

#include <doctest/doctest.h>

#include <string.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace osprim;
using namespace osprim_test;

namespace
{
  /// @brief Whether the OSPRIM_ASSERT of this build aborts.
  [[maybe_unused]] constexpr bool asserts_abort() noexcept
  {
#if defined(OSPRIM_TEST_USER_CONFIG)
    return false;
#elif defined(NDEBUG)
    return false;
#else
    return true;
#endif
  }

  constexpr int64_t MS = 1'000'000;

  /// @brief Creates a queue, or explains why this platform has none.
  /// @return false if the tests should skip
  bool create_queue(async_io_queue& q, uint32_t capacity = 64)
  {
    async_io_queue_desc desc;
    desc.capacity = capacity;
    const auto s  = async_io_queue_create(desc, q);
    if constexpr (!ASYNC_IO_AVAILABLE)
    {
      CHECK(
          s.result
          == async_io_queue_create_result::ASYNC_IO_QUEUE_CREATE_UNSUPPORTED);
      CHECK(s.native_kind == native_error_kind::NATIVE_ERROR_NONE);
      return false;
    }
    if (s.ok())
      return true;
#if OSPRIM_OS_LINUX
    note(
        "io_uring unavailable here: result %d, native code %d",
        static_cast<int>(s.result), s.native_code);
    CHECK(
        (s.result == async_io_queue_create_result::ASYNC_IO_QUEUE_CREATE_DISABLED
         || s.result == async_io_queue_create_result::ASYNC_IO_QUEUE_CREATE_TOO_OLD
         || s.result
                == async_io_queue_create_result::
                    ASYNC_IO_QUEUE_CREATE_OUT_OF_MEMORY));
    return false;
#else
    INFO("native code: " << s.native_code);
    REQUIRE(s.ok());
    return true;
#endif
  }

  void attach_ok(async_io_queue& q, osprim::file_handle h)
  {
    const auto s = async_io_queue_attach(q, h);
    INFO("native code: " << s.native_code);
    REQUIRE(s.ok());
  }

  async_io_op make_op(
      osprim::file_handle h, async_io_op_kind kind, uint64_t offset, void* buffer,
      size_t size)
  {
    async_io_op op{};
    op.file   = h;
    op.offset = offset;
    op.buffer = buffer;
    op.size   = static_cast<uint32_t>(size);
    op.kind   = kind;
    return op;
  }

  /// @brief Submits every operation, requiring that all are accepted.
  void submit_all(async_io_queue& q, std::vector<async_io_op*>& ops)
  {
    size_t submitted = 0;
    const auto s     = async_io_queue_submit(q, ops.data(), ops.size(), submitted);
    INFO("native code: " << s.native_code);
    REQUIRE(s.ok());
    REQUIRE(submitted == ops.size());
  }

  /// @brief Collects exactly `n` completions (10 s at most).
  std::vector<async_io_op*> collect(async_io_queue& q, size_t n)
  {
    std::vector<async_io_op*> done;
    std::vector<async_io_op*> out(n == 0 ? 1 : n);
    const awake_instant deadline = deadline_after(10'000 * MS);
    while (done.size() < n)
    {
      const size_t missing = n - done.size();
      size_t count         = 0;
      const auto s =
          async_io_queue_wait(q, out.data(), missing, missing, deadline, count);
      INFO("native code: " << s.native_code);
      REQUIRE(s.result != async_io_wait_result::ASYNC_IO_WAIT_SYSTEM_ERROR);
      REQUIRE(count <= missing);
      done.insert(
          done.end(), out.begin(), out.begin() + static_cast<ptrdiff_t>(count));
      if (s.result == async_io_wait_result::ASYNC_IO_WAIT_TIMED_OUT)
        break;
    }
    REQUIRE(done.size() == n);
    return done;
  }

  /// @brief Collects whatever is ready without blocking.
  size_t ready_count(async_io_queue& q)
  {
    async_io_op* out[16];
    size_t count = 0;
    const auto s = async_io_queue_wait(q, out, 16, 0, awake_now(), count);
    CHECK(s.ok());
    return count;
  }
} // namespace

TEST_CASE("async_io: availability")
{
  CHECK(ASYNC_IO_AVAILABLE == (OSPRIM_OS_WINDOWS || OSPRIM_OS_LINUX));
  session s;
  async_io_queue q{};
  if (create_queue(q))
    async_io_queue_destroy(q);
}

TEST_CASE("async_io: reads and writes")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  constexpr size_t CHUNK = 4096;
  constexpr size_t N     = 16;
  osprim::file_handle h  = make_pattern_file(dir.path("rw.bin"), CHUNK * N);
  attach_ok(q, h);

  // N reads in one batch, in reverse order of offsets
  std::vector<unsigned char> data(CHUNK * N);
  std::vector<async_io_op> ops(N);
  std::vector<async_io_op*> ptrs(N);
  for (size_t i = 0; i < N; ++i)
  {
    const size_t k = N - 1 - i;
    ops[i]         = make_op(
        h, async_io_op_kind::ASYNC_IO_OP_READ, k * CHUNK, data.data() + k * CHUNK,
        CHUNK);
    ops[i].user = &ops[i];
    ptrs[i]     = &ops[i];
  }
  submit_all(q, ptrs);
  for (async_io_op* op : collect(q, N))
  {
    CHECK(op->user == op);
    CHECK(op->result.ok());
    CHECK(op->transferred == CHUNK);
  }
  CHECK(has_pattern(data.data(), data.size(), 0));

  // Overwrite every other chunk
  std::vector<unsigned char> ones(CHUNK * N, 0x5a);
  std::vector<async_io_op*> writes;
  for (size_t k = 0; k < N; k += 2)
  {
    ops[k] = make_op(
        h, async_io_op_kind::ASYNC_IO_OP_WRITE, k * CHUNK, ones.data(), CHUNK);
    writes.push_back(&ops[k]);
  }
  submit_all(q, writes);
  for (async_io_op* op : collect(q, writes.size()))
  {
    CHECK(op->result.ok());
    CHECK(op->transferred == CHUNK);
  }
  const auto back = read_all(h, 0, CHUNK * N);
  REQUIRE(back.size() == CHUNK * N);
  for (size_t k = 0; k < N; ++k)
  {
    if (k % 2 == 0)
      CHECK(back[k * CHUNK] == 0x5a);
    else
      CHECK(has_pattern(back.data() + k * CHUNK, CHUNK, k * CHUNK));
  }

  // Writing past the end grows the file
  ops[0] = make_op(
      h, async_io_op_kind::ASYNC_IO_OP_WRITE, CHUNK * N + 100, ones.data(), 10);
  std::vector<async_io_op*> grow{&ops[0]};
  submit_all(q, grow);
  CHECK(collect(q, 1)[0]->result.ok());
  file_info info{};
  REQUIRE(file_info_of(h, info).ok());
  CHECK(info.size == CHUNK * N + 110);

  CHECK(ready_count(q) == 0);
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: end of file")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("eof.bin"), 10);
  attach_ok(q, h);
  unsigned char a[32] = {};
  unsigned char b[32] = {};
  unsigned char c[32] = {};
  async_io_op straddle =
      make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, 4, a, sizeof(a));
  async_io_op at_end =
      make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, 10, b, sizeof(b));
  async_io_op past =
      make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, 1000, c, sizeof(c));
  std::vector<async_io_op*> ops{&straddle, &at_end, &past};
  submit_all(q, ops);
  collect(q, 3);
  CHECK(straddle.result.ok());
  CHECK(straddle.transferred == 6);
  CHECK(has_pattern(a, 6, 4));
  CHECK(at_end.result.ok());
  CHECK(at_end.transferred == 0);
  CHECK(past.result.ok());
  CHECK(past.transferred == 0);
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: capacity and partial collection")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q, 4))
    return;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("capacity.bin"), 600);
  attach_ok(q, h);
  unsigned char buf[6][100] = {};
  std::vector<async_io_op> ops(6);
  std::vector<async_io_op*> ptrs(6);
  for (size_t i = 0; i < 6; ++i)
  {
    ops[i]  = make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, i * 100, buf[i], 100);
    ptrs[i] = &ops[i];
  }

  // Submission stops at the capacity
  size_t submitted = 0;
  REQUIRE(async_io_queue_submit(q, ptrs.data(), 6, submitted).ok());
  CHECK(submitted == 4);
  size_t again = 0;
  REQUIRE(async_io_queue_submit(q, ptrs.data() + 4, 2, again).ok());
  CHECK(again == 0); // still full

  // Collect fewer than are done: the rest stays for the next wait
  CHECK(collect(q, 1).size() == 1);
  REQUIRE(async_io_queue_submit(q, ptrs.data() + 4, 2, again).ok());
  CHECK(again == 1);
  CHECK(collect(q, 3).size() == 3);
  REQUIRE(async_io_queue_submit(q, ptrs.data() + 5, 1, again).ok());
  CHECK(again == 1);
  CHECK(collect(q, 2).size() == 2);
  for (size_t i = 0; i < 6; ++i)
  {
    CHECK(ops[i].result.ok());
    CHECK(ops[i].transferred == 100);
    CHECK(has_pattern(buf[i], 100, i * 100));
  }
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: large batches")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q, 512))
    return;
  temp_dir dir;
  constexpr size_t N    = 300; // above the Windows backend's batch of 64
  constexpr size_t SIZE = 512;
  osprim::file_handle h = make_pattern_file(dir.path("batch.bin"), N * SIZE);
  attach_ok(q, h);
  std::vector<unsigned char> data(N * SIZE);
  std::vector<async_io_op> ops(N);
  std::vector<async_io_op*> ptrs(N);
  for (size_t i = 0; i < N; ++i)
  {
    ops[i] = make_op(
        h, async_io_op_kind::ASYNC_IO_OP_READ, i * SIZE, data.data() + i * SIZE,
        SIZE);
    ptrs[i] = &ops[i];
  }
  submit_all(q, ptrs);
  std::vector<async_io_op*> out(N);
  size_t count = 0;
  const auto w =
      async_io_queue_wait(q, out.data(), N, N, deadline_after(10'000 * MS), count);
  CHECK(w.ok());
  CHECK(count == N);
  if (count < N)
    collect(q, N - count);
  CHECK(has_pattern(data.data(), data.size(), 0));
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: deadlines")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  async_io_op* out[4];
  size_t count = 0;

  // Nothing in flight, min_count 0: returns at once
  CHECK(async_io_queue_wait(q, out, 4, 0, awake_instant::forever(), count).ok());
  CHECK(count == 0);

  // A timeout is never early
  const awake_instant deadline = deadline_after(20 * MS);
  const auto w                 = async_io_queue_wait(q, out, 4, 1, deadline, count);
  CHECK(w.result == async_io_wait_result::ASYNC_IO_WAIT_TIMED_OUT);
  CHECK(count == 0);
  CHECK(awake_now() >= deadline);

  // A deadline already passed returns at once
  const auto p = async_io_queue_wait(q, out, 4, 1, deadline, count);
  CHECK(p.result == async_io_wait_result::ASYNC_IO_WAIT_TIMED_OUT);
  CHECK(count == 0);

  // A deadline already passed still collects what is ready
  temp_dir dir;
  constexpr size_t SIZE = 4096;
  osprim::file_handle h = make_pattern_file(dir.path("ready.bin"), 2 * SIZE);
  attach_ok(q, h);
  std::vector<unsigned char> data(2 * SIZE);
  async_io_op a =
      make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, 0, data.data(), SIZE);
  async_io_op b =
      make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, SIZE, data.data() + SIZE, SIZE);
  std::vector<async_io_op*> ops{&a, &b};
  submit_all(q, ops);
  // Two small reads of cached data: finished long before this
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // Asking for one: satisfied by what is ready
  const auto one = async_io_queue_wait(q, out, 1, 1, deadline, count);
  CHECK(one.ok());
  CHECK(count == 1);
  if (count == 0)
    collect(q, 1);
  // Asking for more than there is: the other one, then TIMED_OUT
  const auto more = async_io_queue_wait(q, out, 4, 4, deadline, count);
  CHECK(more.result == async_io_wait_result::ASYNC_IO_WAIT_TIMED_OUT);
  CHECK(count == 1);
  if (count == 0)
    collect(q, 1);
  CHECK(has_pattern(data.data(), data.size(), 0));
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: wakes")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  async_io_op* out[4];
  size_t count = 0;

  // Sent before the wait: never lost. Several coalesce into one return.
  async_io_queue_wake(q);
  async_io_queue_wake(q);
  async_io_queue_wake(q);
  CHECK(async_io_queue_wait(q, out, 4, 1, deadline_after(10'000 * MS), count).ok());
  CHECK(count == 0);
  const auto t = async_io_queue_wait(q, out, 4, 1, deadline_after(20 * MS), count);
  CHECK(t.result == async_io_wait_result::ASYNC_IO_WAIT_TIMED_OUT);

  // From another thread, while the owner waits
  for (int round = 0; round < 3; ++round)
  {
    std::thread waker(
        [&q]
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          async_io_queue_wake(q);
        });
    const awake_instant deadline = deadline_after(10'000 * MS);
    const auto w = async_io_queue_wait(q, out, 4, 1, deadline, count);
    waker.join();
    CHECK(w.ok());
    CHECK(count == 0);
    CHECK(awake_now() < deadline);
  }

  // A wake ends a wait whatever it still asks for (one wake is not one of
  // the `min_count` completions, and must not be waited for as such)
  for (const size_t min_count : {size_t{2}, size_t{4}})
  {
    std::thread waker(
        [&q]
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          async_io_queue_wake(q);
        });
    const awake_instant deadline = deadline_after(10'000 * MS);
    const auto w = async_io_queue_wait(q, out, 4, min_count, deadline, count);
    waker.join();
    CHECK(w.ok());
    CHECK(count == 0);
    CHECK(awake_now() < deadline);
  }
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: a wake ends a wait that has collected part of its count")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  constexpr size_t SIZE = 4096;
  osprim::file_handle h = make_pattern_file(dir.path("partial.bin"), SIZE);
  attach_ok(q, h);
  std::vector<unsigned char> data(SIZE);
  async_io_op op =
      make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, 0, data.data(), SIZE);
  std::vector<async_io_op*> ops{&op};
  submit_all(q, ops);

  // One operation in flight, three asked for: only the wake can end the wait
  std::thread waker(
      [&q]
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        async_io_queue_wake(q);
      });
  async_io_op* out[4];
  size_t count                 = 0;
  const awake_instant deadline = deadline_after(10'000 * MS);
  const auto w                 = async_io_queue_wait(q, out, 4, 3, deadline, count);
  waker.join();
  CHECK(w.ok());
  CHECK(count <= 1);
  CHECK(awake_now() < deadline);
  if (count == 0)
    collect(q, 1);
  CHECK(has_pattern(data.data(), data.size(), 0));
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: cancellation completes exactly once")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  constexpr size_t SIZE = 1 << 20;
  osprim::file_handle h = make_pattern_file(dir.path("cancel.bin"), SIZE);
  attach_ok(q, h);
  std::vector<unsigned char> data(SIZE);
  async_io_op op =
      make_op(h, async_io_op_kind::ASYNC_IO_OP_READ, 0, data.data(), SIZE);
  std::vector<async_io_op*> ops{&op};
  submit_all(q, ops);
  async_io_queue_cancel(q, op);
  CHECK(collect(q, 1)[0] == &op);
  // Regular files usually finish first: either outcome is valid
  CHECK(
      (op.result.ok()
       || op.result.result == async_io_op_result::ASYNC_IO_OP_CANCELED));
  if (op.result.ok())
    CHECK(op.transferred == SIZE);
  CHECK(ready_count(q) == 0); // no second completion
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: failures arrive as completions")
{
  // A write on a read-only handle: asserted in tracked builds
  if (tracking_compiled && asserts_abort())
    return;
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  const std::string p = dir.path("readonly.bin");
  file_close(make_pattern_file(p, 16));
  osprim::file_handle h = open_ok(p, file_access::FILE_ACCESS_READ);
  attach_ok(q, h);
#if defined(OSPRIM_TEST_USER_CONFIG)
  const int before = osprim_test_assert_failures.load();
#endif
  unsigned char b[4] = {1, 2, 3, 4};
  async_io_op op     = make_op(h, async_io_op_kind::ASYNC_IO_OP_WRITE, 0, b, 4);
  std::vector<async_io_op*> ops{&op};
  submit_all(q, ops);
#if defined(OSPRIM_TEST_USER_CONFIG)
  CHECK(osprim_test_assert_failures.load() == before + (tracking_compiled ? 1 : 0));
#endif
  CHECK(collect(q, 1)[0] == &op);
  CHECK(op.result.result == async_io_op_result::ASYNC_IO_OP_SYSTEM_ERROR);
  CHECK(op.result.native_code != 0);
  CHECK(op.transferred == 0);
  file_close(h);
  async_io_queue_destroy(q);
}

TEST_CASE("async_io: unbuffered files")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  file_open_desc desc;
  desc.access         = file_access::FILE_ACCESS_READ_WRITE;
  desc.disposition    = file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE;
  desc.unbuffered     = true;
  const std::string p = dir.path("unbuffered.bin");
  osprim::file_handle h{};
  const auto o = file_open(p.data(), p.size(), desc, h);
  if (o.result == file_open_result::FILE_OPEN_UNSUPPORTED)
  {
    note("unbuffered I/O unsupported here (native code %d)", o.native_code);
    async_io_queue_destroy(q);
    return;
  }
  REQUIRE(o.ok());
  file_alignment a{};
  if (!file_unbuffered_alignment(h, a).ok())
  {
    note("no unbuffered alignment here");
    file_close(h);
    async_io_queue_destroy(q);
    return;
  }
  attach_ok(q, h);
  const size_t block = a.io > a.memory ? a.io : a.memory;
  aligned_buffer out(4 * block, block);
  aligned_buffer in(4 * block, block);
  fill_pattern(out.data(), 4 * block, 0);

  std::vector<async_io_op> ops(4);
  std::vector<async_io_op*> ptrs(4);
  for (size_t i = 0; i < 4; ++i)
  {
    ops[i] = make_op(
        h, async_io_op_kind::ASYNC_IO_OP_WRITE, i * block, out.data() + i * block,
        block);
    ptrs[i] = &ops[i];
  }
  submit_all(q, ptrs);
  for (async_io_op* op : collect(q, 4))
  {
    CHECK(op->result.ok());
    CHECK(op->transferred == block);
  }
  for (size_t i = 0; i < 4; ++i)
    ops[i] = make_op(
        h, async_io_op_kind::ASYNC_IO_OP_READ, i * block, in.data() + i * block,
        block);
  submit_all(q, ptrs);
  for (async_io_op* op : collect(q, 4))
  {
    CHECK(op->result.ok());
    CHECK(op->transferred == block);
  }
  CHECK(has_pattern(in.data(), 4 * block, 0));
  REQUIRE(file_flush(h).ok());
  file_close(h);
  async_io_queue_destroy(q);
}

#if OSPRIM_OS_WINDOWS
TEST_CASE("async_io: synchronous handles are refused (Windows)")
{
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  const std::string p = dir.path("sync.bin");
  file_close(make_pattern_file(p, 16));
  const HANDLE raw = ::CreateFileW(
      widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(raw != INVALID_HANDLE_VALUE);
  osprim::file_handle h{};
  REQUIRE(file_adopt(raw, h).ok());
  const auto a = async_io_queue_attach(q, h);
  CHECK(a.result == async_io_attach_result::ASYNC_IO_ATTACH_NOT_OVERLAPPED);
  file_close(h);
  async_io_queue_destroy(q);
}
#endif

TEST_CASE("async_io: tracking")
{
  if (!tracking_compiled)
    return;
  temp_dir dir;
  {
    // A queue never destroyed is a leak
    session s;
    async_io_queue q{};
    if (!create_queue(q))
    {
      s.end();
      return;
    }
    const diagnostic_log& log = s.end();
    REQUIRE(log.count == 1);
    CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
    CHECK(::strcmp(log.entries[0].handle_type, "async_io_queue") == 0);
    async_io_queue_destroy(q); // after deinitialize: not tracked any more
  }
  {
    // Attaching records the queue in the file's metadata
    session s;
    async_io_queue q{};
    if (!create_queue(q))
      return;
    osprim::file_handle h = make_pattern_file(dir.path("attached.bin"), 8);
    attach_ok(q, h);
    detail::track_meta m{};
    REQUIRE(
        detail::track_get(detail::FILE_TRACK_TYPE, detail::file_key(h.native), &m));
    CHECK(m.words[detail::FILE_META_QUEUE] != 0);
    file_close(h);
    async_io_queue_destroy(q);
  }
}

#if defined(OSPRIM_TEST_USER_CONFIG)
TEST_CASE("async_io: attachment checks (user config)")
{
  if (!tracking_compiled)
    return;
  session s;
  async_io_queue q{};
  if (!create_queue(q))
    return;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("checks.bin"), 8);
  attach_ok(q, h);
  const int before = osprim_test_assert_failures.load();
  (void)async_io_queue_attach(q, h); // twice (Windows refuses it too)
  CHECK(osprim_test_assert_failures.load() == before + 1);
  file_close(h);
  async_io_queue_destroy(q);
}
#endif
