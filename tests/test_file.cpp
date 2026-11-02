/*****************************************************************/ /**
 * @file   test_file.cpp
 * @brief  Files: open, positioned I/O, end of file, growth, info,
 * adoption, unbuffered I/O, limits and tracking.
 *********************************************************************/
#include "files.hpp"
#include "internal.hpp"
#include "support.hpp"

#include <osprim/file.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#if !OSPRIM_OS_WINDOWS
  #include <errno.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/resource.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace osprim;
using namespace osprim_test;

namespace
{
  status<file_open_result> try_open(
      const std::string& p, file_access access, file_disposition disposition, osprim::file_handle& h)
  {
    file_open_desc desc;
    desc.access      = access;
    desc.disposition = disposition;
    return file_open(p.data(), p.size(), desc, h);
  }

  file_open_result open_result(const std::string& p, file_access access, file_disposition disposition)
  {
    osprim::file_handle h{};
    const auto s = try_open(p, access, disposition, h);
    if (s.ok())
      file_close(h);
    else
      note("open: result %d, native code %d", static_cast<int>(s.result), s.native_code);
    return s.result;
  }

  uint64_t size_of(osprim::file_handle h)
  {
    file_info info{};
    REQUIRE(file_info_of(h, info).ok());
    return info.size;
  }

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
} // namespace

TEST_CASE("file: limits")
{
#if OSPRIM_OS_APPLE
  CHECK(MAX_PATH_BYTES == 1023);
  CHECK(FILE_IO_MAX == 0x7fffffff);
#elif OSPRIM_OS_WINDOWS
  CHECK(MAX_PATH_BYTES == 4095);
  CHECK(FILE_IO_MAX == 0xffffffff);
#else
  CHECK(MAX_PATH_BYTES == 4095);
  CHECK(FILE_IO_MAX == 0x7ffff000);
#endif
  CHECK(PORTABLE_MAX_PATH_BYTES <= MAX_PATH_BYTES);
  CHECK(PORTABLE_FILE_IO_MAX <= FILE_IO_MAX);
  CHECK(sizeof(osprim::file_handle) == sizeof(native_file_t));
}

TEST_CASE("file: UTF-8 validation")
{
  CHECK(detail::utf8_valid("abc", 3));
  CHECK(detail::utf8_valid("\xc3\xa9", 2));         // e acute
  CHECK(detail::utf8_valid("\xe2\x82\xac", 3));     // euro sign
  CHECK(detail::utf8_valid("\xf0\x9f\x98\x80", 4)); // emoji
  CHECK(detail::utf8_valid("\xf4\x8f\xbf\xbf", 4)); // U+10FFFF
  CHECK_FALSE(detail::utf8_valid("a\0b", 3));       // embedded NUL
  CHECK_FALSE(detail::utf8_valid("\xc0\xaf", 2));   // overlong
  CHECK_FALSE(detail::utf8_valid("\xe0\x80\xaf", 3)); // overlong
  CHECK_FALSE(detail::utf8_valid("\xf0\x80\x80\xaf", 4)); // overlong
  CHECK_FALSE(detail::utf8_valid("\xed\xa0\x80", 3)); // surrogate
  CHECK_FALSE(detail::utf8_valid("\xf4\x90\x80\x80", 4)); // above U+10FFFF
  CHECK_FALSE(detail::utf8_valid("\xc3", 1));       // truncated
  CHECK_FALSE(detail::utf8_valid("\xe2\x82", 2));   // truncated
  CHECK_FALSE(detail::utf8_valid("\x80", 1));       // stray continuation
  CHECK_FALSE(detail::utf8_valid("\xff", 1));
}

TEST_CASE("file: round trip, end of file and short reads")
{
  session s;
  temp_dir dir;
  const std::string p = dir.path("roundtrip.bin");
  osprim::file_handle h       = make_pattern_file(p, 10);
  CHECK(size_of(h) == 10);

  CHECK(read_all(h, 0, 100).size() == 10); // short only at the end
  CHECK(has_pattern(read_all(h, 0, 10).data(), 10, 0));
  CHECK(read_all(h, 5, 5).size() == 5);
  CHECK(has_pattern(read_all(h, 5, 5).data(), 5, 5));
  CHECK(read_all(h, 10, 5).empty()); // at the end: 0, not an error
  CHECK(read_all(h, 1'000'000, 5).empty()); // past the end: 0
  CHECK(read_all(h, 0, 0).empty());

  // Overwrite in the middle
  const unsigned char x[3] = {0xaa, 0xbb, 0xcc};
  size_t done              = 0;
  REQUIRE(file_write_at(h, 2, x, 3, done).ok());
  CHECK(done == 3);
  const auto back = read_all(h, 0, 10);
  CHECK(back[2] == 0xaa);
  CHECK(back[4] == 0xcc);
  CHECK(back[5] == pattern_byte(5));
  CHECK(file_flush(h).ok());
  file_close(h);
}

TEST_CASE("file: a path needs no terminator")
{
  session s;
  temp_dir dir;
  const std::string p = dir.path("unterminated.bin");
  file_close(make_pattern_file(p, 4));
  const std::string padded = p + "XYZ"; // the size excludes "XYZ"
  file_open_desc desc;
  osprim::file_handle h{};
  REQUIRE(file_open(padded.data(), p.size(), desc, h).ok());
  CHECK(size_of(h) == 4);
  file_close(h);
}

TEST_CASE("file: dispositions")
{
  session s;
  temp_dir dir;
  const std::string p = dir.path("disposition.bin");
  using A             = file_access;
  using D             = file_disposition;
  using R             = file_open_result;

  // Missing
  CHECK(open_result(p, A::FILE_ACCESS_READ, D::FILE_DISPOSITION_OPEN_EXISTING) == R::FILE_OPEN_NOT_FOUND);
  CHECK(open_result(p, A::FILE_ACCESS_WRITE, D::FILE_DISPOSITION_TRUNCATE_EXISTING) == R::FILE_OPEN_NOT_FOUND);
  CHECK(open_result(p, A::FILE_ACCESS_WRITE, D::FILE_DISPOSITION_CREATE_NEW) == R::FILE_OPEN_OK);

  // Exists (empty)
  CHECK(open_result(p, A::FILE_ACCESS_WRITE, D::FILE_DISPOSITION_CREATE_NEW) == R::FILE_OPEN_ALREADY_EXISTS);
  CHECK(open_result(p, A::FILE_ACCESS_READ, D::FILE_DISPOSITION_OPEN_EXISTING) == R::FILE_OPEN_OK);

  // OPEN_OR_CREATE keeps the content, CREATE_OR_TRUNCATE and TRUNCATE_EXISTING drop it
  file_close(make_pattern_file(p, 100));
  osprim::file_handle h = open_ok(p, A::FILE_ACCESS_READ_WRITE, D::FILE_DISPOSITION_OPEN_OR_CREATE);
  CHECK(size_of(h) == 100);
  file_close(h);
  h = open_ok(p, A::FILE_ACCESS_WRITE, D::FILE_DISPOSITION_TRUNCATE_EXISTING);
  CHECK(size_of(h) == 0);
  file_close(h);
  file_close(make_pattern_file(p, 100));
  h = open_ok(p, A::FILE_ACCESS_WRITE, D::FILE_DISPOSITION_CREATE_OR_TRUNCATE);
  CHECK(size_of(h) == 0);
  file_close(h);

  const std::string q = dir.path("created.bin");
  h                   = open_ok(q, A::FILE_ACCESS_READ, D::FILE_DISPOSITION_OPEN_OR_CREATE);
  CHECK(size_of(h) == 0);
  file_close(h);
}

TEST_CASE("file: path checks")
{
  session s;
  temp_dir dir;
  using A = file_access;
  using D = file_disposition;
  using R = file_open_result;

  CHECK(open_result("relative/file.bin", A::FILE_ACCESS_READ, D::FILE_DISPOSITION_OPEN_EXISTING)
        == R::FILE_OPEN_PATH_NOT_ABSOLUTE);
  CHECK(open_result(dir.root() + temp_dir::sep() + "bad\xff.bin", A::FILE_ACCESS_READ,
                    D::FILE_DISPOSITION_OPEN_EXISTING)
        == R::FILE_OPEN_INVALID_PATH);
  CHECK(open_result(dir.root() + temp_dir::sep() + std::string("nul\0in.bin", 10), A::FILE_ACCESS_READ,
                    D::FILE_DISPOSITION_OPEN_EXISTING)
        == R::FILE_OPEN_INVALID_PATH);
  CHECK(open_result(dir.root() + temp_dir::sep() + std::string(MAX_PATH_BYTES, 'a'), A::FILE_ACCESS_READ,
                    D::FILE_DISPOSITION_OPEN_EXISTING)
        == R::FILE_OPEN_PATH_TOO_LONG);
  CHECK(open_result(dir.path("missing") + temp_dir::sep() + "file.bin", A::FILE_ACCESS_READ,
                    D::FILE_DISPOSITION_OPEN_OR_CREATE)
        == R::FILE_OPEN_NOT_FOUND);

  // Directories are refused
  CHECK(open_result(dir.root(), A::FILE_ACCESS_READ, D::FILE_DISPOSITION_OPEN_EXISTING) == R::FILE_OPEN_NOT_A_FILE);
  CHECK(open_result(dir.root(), A::FILE_ACCESS_WRITE, D::FILE_DISPOSITION_OPEN_EXISTING) == R::FILE_OPEN_NOT_A_FILE);

  // A UTF-8 name works
  const std::string utf8 = dir.path("caf\xc3\xa9_\xe2\x82\xac.bin");
  CHECK(open_result(utf8, A::FILE_ACCESS_WRITE, D::FILE_DISPOSITION_CREATE_NEW) == R::FILE_OPEN_OK);
  CHECK(open_result(utf8, A::FILE_ACCESS_READ, D::FILE_DISPOSITION_OPEN_EXISTING) == R::FILE_OPEN_OK);
}

TEST_CASE("file: long paths")
{
  // Beyond the 260 characters of Windows' legacy limit, within every MAX_PATH_BYTES
  session s;
  temp_dir dir;
  const std::string sub = dir.make_dir(std::string(200, 'd'));
  (void)sub;
  const std::string p = dir.path(std::string(200, 'd') + temp_dir::sep() + std::string(200, 'f'));
  REQUIRE(p.size() > 400);
  osprim::file_handle h = make_pattern_file(p, 16);
  CHECK(size_of(h) == 16);
  file_close(h);
  h = open_ok(p, file_access::FILE_ACCESS_READ);
  CHECK(has_pattern(read_all(h, 0, 16).data(), 16, 0));
  file_close(h);
}

#if OSPRIM_OS_WINDOWS
TEST_CASE("file: Windows path conversion")
{
  wchar_t out[MAX_PATH_BYTES + 16];
  bool verbatim = false;
  auto conv     = [&](const char* in) -> status<file_open_result>
  {
    return detail::windows_file_path(in, ::strlen(in), out, MAX_PATH_BYTES + 16, &verbatim);
  };
  using R = file_open_result;

  REQUIRE(conv("C:/a/./b/../c.txt").ok());
  CHECK(std::wstring(out) == L"\\\\?\\C:\\a\\c.txt");
  CHECK_FALSE(verbatim);
  REQUIRE(conv("\\\\server\\share\\x.bin").ok());
  CHECK(std::wstring(out) == L"\\\\?\\UNC\\server\\share\\x.bin");
  REQUIRE(conv("//server/share/y").ok());
  CHECK(std::wstring(out) == L"\\\\?\\UNC\\server\\share\\y");
  REQUIRE(conv("\\\\?\\C:\\verbatim\\..\\kept").ok());
  CHECK(std::wstring(out) == L"\\\\?\\C:\\verbatim\\..\\kept");
  CHECK(verbatim);

  CHECK(conv("C:relative").result == R::FILE_OPEN_PATH_NOT_ABSOLUTE);
  CHECK(conv("\\rooted").result == R::FILE_OPEN_PATH_NOT_ABSOLUTE);
  CHECK(conv("relative").result == R::FILE_OPEN_PATH_NOT_ABSOLUTE);
  CHECK(conv("\\\\.\\NUL").result == R::FILE_OPEN_NOT_A_FILE);
  CHECK(conv("//./pipe/x").result == R::FILE_OPEN_NOT_A_FILE);
  CHECK(conv("C:/somewhere/nul").result == R::FILE_OPEN_NOT_A_FILE); // NUL is reserved everywhere
}

TEST_CASE("file: Windows event pool")
{
  session s;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("pool.bin"), 64);

  // Exhaust the pool: calls get a temporary event and still work
  std::vector<detail::file_event> taken;
  for (size_t i = 0; i < detail::FILE_EVENT_POOL_SIZE; ++i)
  {
    taken.push_back(detail::file_event_acquire());
    REQUIRE(taken.back().handle != nullptr);
    CHECK(taken.back().slot != UINT32_MAX);
  }
  const detail::file_event extra = detail::file_event_acquire();
  CHECK(extra.handle != nullptr);
  CHECK(extra.slot == UINT32_MAX);
  detail::file_event_release(extra);
  CHECK(has_pattern(read_all(h, 0, 64).data(), 64, 0));
  for (const detail::file_event& e : taken)
    detail::file_event_release(e);

  // Returned events are reused
  const detail::file_event again = detail::file_event_acquire();
  CHECK(again.slot != UINT32_MAX);
  detail::file_event_release(again);
  file_close(h);
}
#else
TEST_CASE("file: FIFOs are refused without blocking")
{
  session s;
  temp_dir dir;
  const std::string p = dir.path("fifo");
  if (::mkfifo(p.c_str(), 0600) != 0)
  {
    note("mkfifo failed (errno %d): skipped", errno);
    return;
  }
  // Without O_NONBLOCK, both opens would block forever (no other end)
  CHECK(open_result(p, file_access::FILE_ACCESS_READ, file_disposition::FILE_DISPOSITION_OPEN_EXISTING)
        == file_open_result::FILE_OPEN_NOT_A_FILE);
  CHECK(open_result(p, file_access::FILE_ACCESS_WRITE, file_disposition::FILE_DISPOSITION_OPEN_EXISTING)
        == file_open_result::FILE_OPEN_NOT_A_FILE);
}
#endif

TEST_CASE("file: growth reads as zero")
{
  session s;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("growth.bin"), 0);

  // A write past the end
  const unsigned char x[4] = {1, 2, 3, 4};
  size_t done              = 0;
  REQUIRE(file_write_at(h, 100'000, x, 4, done).ok());
  CHECK(size_of(h) == 100'004);
  CHECK(all_zero(read_all(h, 0, 100'000).data(), 100'000));

  // FAST and RESERVE growth
  REQUIRE(file_set_size(h, 1 << 20, file_size_mode::FILE_SIZE_MODE_FAST).ok());
  CHECK(size_of(h) == 1 << 20);
  CHECK(all_zero(read_all(h, 100'004, (1 << 20) - 100'004).data(), (1 << 20) - 100'004));
  const auto r = file_set_size(h, 2 << 20, file_size_mode::FILE_SIZE_MODE_RESERVE);
  if (r.result == file_set_size_result::FILE_SET_SIZE_UNSUPPORTED)
    note("RESERVE unsupported on this filesystem (native code %d)", r.native_code);
  else
  {
    INFO("native code: " << r.native_code);
    REQUIRE(r.ok());
    CHECK(size_of(h) == 2 << 20);
    CHECK(all_zero(read_all(h, 1 << 20, 1 << 20).data(), 1 << 20));
  }

  // Shrinking
  REQUIRE(file_set_size(h, 10, file_size_mode::FILE_SIZE_MODE_FAST).ok());
  CHECK(size_of(h) == 10);
  CHECK(read_all(h, 0, 100).size() == 10);
  CHECK(read_all(h, 100'000, 4).empty());
  file_close(h);
}

TEST_CASE("file: info and identity")
{
  session s;
  temp_dir dir;
  const std::string p = dir.path("info.bin");
  osprim::file_handle a       = make_pattern_file(p, 1234);
  osprim::file_handle b       = open_ok(p, file_access::FILE_ACCESS_READ);
  osprim::file_handle c       = make_pattern_file(dir.path("other.bin"), 1);

  file_info ia{};
  file_info ib{};
  file_info ic{};
  REQUIRE(file_info_of(a, ia).ok());
  REQUIRE(file_info_of(b, ib).ok());
  REQUIRE(file_info_of(c, ic).ok());
  CHECK(ia.size == 1234);
  CHECK(ib.size == 1234);
  CHECK(ia.identity == ib.identity);
  CHECK_FALSE(ia.identity == ic.identity);
  const int64_t age_ns = wall_now().ns - ia.last_write.ns;
  CHECK(age_ns > -600'000'000'000LL); // filesystem clocks are coarse
  CHECK(age_ns < 600'000'000'000LL);
  file_close(a);
  file_close(b);
  file_close(c);
}

TEST_CASE("file: concurrent reads on one handle")
{
  session s;
  temp_dir dir;
  constexpr size_t SIZE = 1 << 20;
  osprim::file_handle h         = make_pattern_file(dir.path("concurrent.bin"), SIZE);
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < 8; ++t)
  {
    threads.emplace_back(
        [&, t]
        {
          unsigned char buffer[4096];
          uint64_t x = 0x9e3779b97f4a7c15ULL * (t + 1);
          for (int i = 0; i < 200; ++i)
          {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            const uint64_t offset = x % (SIZE - sizeof(buffer));
            size_t done           = 0;
            if (!file_read_at(h, offset, buffer, sizeof(buffer), done).ok() || done != sizeof(buffer)
                || !has_pattern(buffer, sizeof(buffer), offset))
              bad.fetch_add(1);
          }
        });
  }
  for (std::thread& th : threads)
    th.join();
  CHECK(bad.load() == 0);
  file_close(h);
}

TEST_CASE("file: adopt and release")
{
  session s;
  temp_dir dir;
  const std::string p = dir.path("adopt.bin");
  file_close(make_pattern_file(p, 32));

#if OSPRIM_OS_WINDOWS
  // A synchronous handle (no FILE_FLAG_OVERLAPPED) works the same way
  const HANDLE native = ::CreateFileW(
      widen(p).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(native != INVALID_HANDLE_VALUE);
#else
  const int native = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
  REQUIRE(native >= 0);
#endif
  osprim::file_handle h{};
  REQUIRE(file_adopt(native, h).ok());
  CHECK(file_native(h) == native);
  CHECK(has_pattern(read_all(h, 8, 8).data(), 8, 8));
  const unsigned char x = 0x55;
  size_t done           = 0;
  REQUIRE(file_write_at(h, 40, &x, 1, done).ok());
  CHECK(size_of(h) == 41);
  CHECK(file_release(h) == native);
#if OSPRIM_OS_WINDOWS
  CHECK(::CloseHandle(native));

  const HANDLE d = ::CreateFileW(
      widen(dir.root()).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  REQUIRE(d != INVALID_HANDLE_VALUE);
  CHECK(file_adopt(d, h).result == file_adopt_result::FILE_ADOPT_NOT_A_FILE);
  CHECK(::CloseHandle(d)); // ownership stayed with the caller
#else
  CHECK(::close(native) == 0);

  int pipe_fds[2];
  REQUIRE(::pipe(pipe_fds) == 0);
  CHECK(file_adopt(pipe_fds[0], h).result == file_adopt_result::FILE_ADOPT_NOT_A_FILE);
  CHECK(::close(pipe_fds[0]) == 0); // ownership stayed with the caller
  CHECK(::close(pipe_fds[1]) == 0);

  const int d = ::open(dir.root().c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE(d >= 0);
  CHECK(file_adopt(d, h).result == file_adopt_result::FILE_ADOPT_NOT_A_FILE);
  CHECK(::close(d) == 0);

  const int ap = ::open(p.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  REQUIRE(ap >= 0);
  CHECK(file_adopt(ap, h).result == file_adopt_result::FILE_ADOPT_APPEND_MODE);
  CHECK(::close(ap) == 0);
#endif
}

TEST_CASE("file: tracking")
{
  if (!tracking_compiled)
    return;
  temp_dir dir;
  {
    session s;
    osprim::file_handle h = make_pattern_file(dir.path("leak.bin"), 1);
    detail::track_meta m{};
    REQUIRE(detail::track_get(detail::FILE_TRACK_TYPE, detail::file_key(h.native), &m));
    CHECK(m.words[0] == static_cast<uint64_t>(file_access::FILE_ACCESS_READ_WRITE));
    CHECK(m.words[1] == 0);
    const diagnostic_log& log = s.end();
    REQUIRE(log.count == 1);
    CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
    CHECK(::strcmp(log.entries[0].handle_type, "file") == 0);
    file_close(h); // after deinitialize: not tracked any more
  }
  {
    // A released handle is not a leak
    session s;
    osprim::file_handle h            = make_pattern_file(dir.path("released.bin"), 1);
    const native_file_t n    = file_release(h);
    CHECK(detail::tracked_count() == 0);
    osprim::file_handle again{};
    REQUIRE(file_adopt(n, again).ok());
    detail::track_meta m{};
    REQUIRE(detail::track_get(detail::FILE_TRACK_TYPE, detail::file_key(n), &m));
    CHECK(m.words[0] == 0xff); // access unknown when adopted
    file_close(again);
  }
}

TEST_CASE("file: unbuffered I/O")
{
  session s;
  temp_dir dir;
  file_open_desc desc;
  desc.access      = file_access::FILE_ACCESS_READ_WRITE;
  desc.disposition = file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE;
  desc.unbuffered  = true;
  const std::string p = dir.path("unbuffered.bin");
  osprim::file_handle h{};
  const auto o = file_open(p.data(), p.size(), desc, h);
  if (o.result == file_open_result::FILE_OPEN_UNSUPPORTED)
  {
    note("unbuffered I/O unsupported here (native code %d)", o.native_code);
    return;
  }
  REQUIRE(o.ok());
  file_alignment a{};
  const auto q = file_unbuffered_alignment(h, a);
  if (!q.ok())
  {
    note("no unbuffered alignment here (result %d, native code %d)", static_cast<int>(q.result), q.native_code);
    file_close(h);
    return;
  }
  CHECK(a.memory != 0);
  CHECK(a.io != 0);
  CHECK((a.memory & (a.memory - 1)) == 0);
  CHECK((a.io & (a.io - 1)) == 0);
  CHECK(a.io <= 65536);
  note("unbuffered alignment: memory %u, io %u", a.memory, a.io);

  const size_t block = a.io > a.memory ? a.io : a.memory;
  aligned_buffer out(2 * block, block);
  aligned_buffer in(2 * block, block);
  fill_pattern(out.data(), 2 * block, 0);
  size_t done = 0;
  REQUIRE(file_write_at(h, 0, out.data(), 2 * block, done).ok());
  CHECK(done == 2 * block);
  REQUIRE(file_read_at(h, 0, in.data(), 2 * block, done).ok());
  CHECK(done == 2 * block);
  CHECK(has_pattern(in.data(), 2 * block, 0));

  // The tail: write whole blocks, trim, read a whole block (short)
  REQUIRE(file_set_size(h, 2 * block - 100, file_size_mode::FILE_SIZE_MODE_FAST).ok());
  REQUIRE(file_read_at(h, block, in.data(), block, done).ok());
  CHECK(done == block - 100);
  CHECK(has_pattern(in.data(), block - 100, block));
  REQUIRE(file_flush(h).ok());

  // Misaligned I/O is refused by the OS (Linux, Windows), and asserted in tracked builds
  // (not Android: encrypted storage without inline encryption silently
  // falls back to the cache, which accepts anything)
#if OSPRIM_OS_LINUX || OSPRIM_OS_WINDOWS
  #if OSPRIM_OS_WINDOWS
  const bool enforced = a.io > 1 && !under_wine();
  #else
  const bool enforced = a.io > 1;
  #endif
  if (enforced && !(tracking_compiled && asserts_abort()))
  {
  #if defined(OSPRIM_TEST_USER_CONFIG)
    const int before = osprim_test_assert_failures.load();
  #endif
    const auto r = file_read_at(h, 1, in.data(), block, done);
    CHECK(r.result == file_read_result::FILE_READ_MISALIGNED);
  #if defined(OSPRIM_TEST_USER_CONFIG)
    CHECK(osprim_test_assert_failures.load() == before + (tracking_compiled ? 1 : 0));
  #endif
  }
#endif
  file_close(h);
}

#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
TEST_CASE("file: adopted O_DIRECT handles are detected")
{
  if (!tracking_compiled)
    return;
  session s;
  temp_dir dir;
  const std::string p = dir.path("direct.bin");
  const int fd        = ::open(p.c_str(), O_RDWR | O_CREAT | O_DIRECT | O_CLOEXEC, 0600);
  if (fd < 0)
  {
    note("O_DIRECT refused here (errno %d): skipped", errno);
    return;
  }
  osprim::file_handle h{};
  REQUIRE(file_adopt(fd, h).ok());
  detail::track_meta m{};
  REQUIRE(detail::track_get(detail::FILE_TRACK_TYPE, detail::file_key(fd), &m));
  CHECK(m.words[1] == 1);
  file_close(h);
}
#endif

#if !OSPRIM_OS_WINDOWS
TEST_CASE("file: FILE_TOO_LARGE (RLIMIT_FSIZE)")
{
  session s;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("limit.bin"), 0);

  struct rlimit old{};
  REQUIRE(::getrlimit(RLIMIT_FSIZE, &old) == 0);
  struct sigaction ignore{};
  struct sigaction previous{};
  ignore.sa_handler = SIG_IGN;
  REQUIRE(::sigaction(SIGXFSZ, &ignore, &previous) == 0);
  struct rlimit lim = old;
  lim.rlim_cur      = 65536;
  REQUIRE(::setrlimit(RLIMIT_FSIZE, &lim) == 0);

  const unsigned char x = 1;
  size_t done           = 0;
  const auto w          = file_write_at(h, 65536, &x, 1, done);
  const auto z          = file_set_size(h, 1 << 20, file_size_mode::FILE_SIZE_MODE_FAST);

  REQUIRE(::setrlimit(RLIMIT_FSIZE, &old) == 0);
  REQUIRE(::sigaction(SIGXFSZ, &previous, nullptr) == 0);
  CHECK(w.result == file_write_result::FILE_WRITE_FILE_TOO_LARGE);
  CHECK(z.result == file_set_size_result::FILE_SET_SIZE_FILE_TOO_LARGE);
  file_close(h);
}
#endif

TEST_CASE("file: NO_SPACE (OSPRIM_TEST_SMALL_DIR)")
{
  // CI mounts a 1 MiB tmpfs and names it in OSPRIM_TEST_SMALL_DIR
  const char* small = ::getenv("OSPRIM_TEST_SMALL_DIR");
  if (small == nullptr || *small == '\0')
    return;
  session s;
  temp_dir dir(small, true);
  osprim::file_handle h = make_pattern_file(dir.path("full.bin"), 0);
  std::vector<unsigned char> chunk(256 * 1024, 0x5a);
  uint64_t offset = 0;
  status<file_write_result> w{};
  for (int i = 0; i < 64; ++i)
  {
    size_t done = 0;
    w           = file_write_at(h, offset, chunk.data(), chunk.size(), done);
    if (!w.ok())
      break;
    offset += done;
  }
  CHECK(w.result == file_write_result::FILE_WRITE_NO_SPACE);
  const auto r = file_set_size(h, 64 << 20, file_size_mode::FILE_SIZE_MODE_RESERVE);
  CHECK(r.result == file_set_size_result::FILE_SET_SIZE_NO_SPACE);
  file_close(h);
}

#if defined(OSPRIM_TEST_USER_CONFIG)
TEST_CASE("file: access checks (user config)")
{
  if (!tracking_compiled)
    return;
  session s;
  temp_dir dir;
  const std::string p = dir.path("access.bin");
  file_close(make_pattern_file(p, 8));
  osprim::file_handle w      = open_ok(p, file_access::FILE_ACCESS_WRITE);
  const int before   = osprim_test_assert_failures.load();
  unsigned char b[4] = {};
  size_t done        = 0;
  (void)file_read_at(w, 0, b, 4, done); // the OS refuses it too
  CHECK(osprim_test_assert_failures.load() == before + 1);
  file_close(w);

  osprim::file_handle r = open_ok(p, file_access::FILE_ACCESS_READ);
  (void)file_write_at(r, 0, b, 4, done);
  CHECK(osprim_test_assert_failures.load() == before + 2);
  file_close(r);
}
#endif
