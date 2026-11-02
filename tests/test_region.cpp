/*****************************************************************/ /**
 * @file   test_region.cpp
 * @brief  Mapped regions: any offset, access modes, coherence through
 * flush, bounds, independence from the file handle, Debug diagnostics.
 *********************************************************************/
#include "files.hpp"
#include "internal.hpp"
#include "support.hpp"

#include <osprim/region.hpp>

#include <doctest/doctest.h>

#include <string.h>

#include <string>
#include <vector>

#if !OSPRIM_OS_WINDOWS
  #include <sys/stat.h>
#endif

using namespace osprim;
using namespace osprim_test;

namespace
{
  region map_ok(osprim::file_handle h, uint64_t offset, size_t size, region_access access)
  {
    region r{};
    const auto s = region_map(h, offset, size, access, r);
    INFO("native_code: " << s.native_code);
    REQUIRE(s.ok());
    REQUIRE(r.data != nullptr);
    REQUIRE(r.size == size);
    return r;
  }

  const unsigned char* bytes(region r) { return static_cast<const unsigned char*>(r.data); }
} // namespace

TEST_CASE("region: granularity")
{
  session s;
  const size_t g = region_granularity();
  CHECK(g >= 4096);
  CHECK((g & (g - 1)) == 0);
#if OSPRIM_OS_WINDOWS
  CHECK(g == 65536);
#endif
  CHECK(detail::page_size() <= g);
}

TEST_CASE("region: any offset, and the file handle may be closed")
{
  session s;
  temp_dir dir;
  const size_t g      = region_granularity();
  const size_t size   = 3 * g + 1234;
  osprim::file_handle h       = make_pattern_file(dir.path("map.bin"), size);
  const region odd    = map_ok(h, 12345, 1000, region_access::REGION_ACCESS_READ);
  const region across = map_ok(h, g - 7, 20, region_access::REGION_ACCESS_READ);
  const region whole  = map_ok(h, 0, size, region_access::REGION_ACCESS_READ);
  const region tail   = map_ok(h, size - 1, 1, region_access::REGION_ACCESS_READ);
  file_close(h); // regions do not need the handle
  CHECK(has_pattern(bytes(odd), 1000, 12345));
  CHECK(has_pattern(bytes(across), 20, g - 7));
  CHECK(has_pattern(bytes(whole), size, 0));
  CHECK(has_pattern(bytes(tail), 1, size - 1));
  region_unmap(odd);
  region_unmap(across);
  region_unmap(whole);
  region_unmap(tail);
}

TEST_CASE("region: bounds and empty ranges")
{
  session s;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("bounds.bin"), 100);
  region r{};
  CHECK(region_map(h, 0, 0, region_access::REGION_ACCESS_READ, r).result == region_map_result::REGION_MAP_EMPTY);
  CHECK(region_map(h, 0, 101, region_access::REGION_ACCESS_READ, r).result
        == region_map_result::REGION_MAP_OUT_OF_RANGE);
  CHECK(region_map(h, 100, 1, region_access::REGION_ACCESS_READ, r).result
        == region_map_result::REGION_MAP_OUT_OF_RANGE);
  CHECK(region_map(h, UINT64_MAX - 10, 100, region_access::REGION_ACCESS_READ, r).result
        == region_map_result::REGION_MAP_OUT_OF_RANGE);
  file_close(h);

  osprim::file_handle empty = make_pattern_file(dir.path("empty.bin"), 0);
  CHECK(region_map(empty, 0, 1, region_access::REGION_ACCESS_READ, r).result
        == region_map_result::REGION_MAP_OUT_OF_RANGE);
  CHECK(region_map(empty, 0, 0, region_access::REGION_ACCESS_READ, r).result
        == region_map_result::REGION_MAP_EMPTY);
  file_close(empty);
}

TEST_CASE("region: access needs a matching handle")
{
  session s;
  temp_dir dir;
  const std::string p = dir.path("access.bin");
  file_close(make_pattern_file(p, 4096));
  osprim::file_handle ro = open_ok(p, file_access::FILE_ACCESS_READ);
  region r{};
  CHECK(region_map(ro, 0, 100, region_access::REGION_ACCESS_READ_WRITE, r).result
        == region_map_result::REGION_MAP_ACCESS_DENIED);
  // Copy-on-write only needs read access
  r = map_ok(ro, 0, 100, region_access::REGION_ACCESS_COPY_ON_WRITE);
  region_unmap(r);
  file_close(ro);

  osprim::file_handle wo = open_ok(p, file_access::FILE_ACCESS_WRITE);
  CHECK(region_map(wo, 0, 100, region_access::REGION_ACCESS_READ, r).result
        == region_map_result::REGION_MAP_ACCESS_DENIED);
  file_close(wo);
}

TEST_CASE("region: writes, flush and visibility")
{
  session s;
  temp_dir dir;
  const size_t g = region_granularity();
  osprim::file_handle h  = make_pattern_file(dir.path("write.bin"), 2 * g);
  region a       = map_ok(h, 100, g, region_access::REGION_ACCESS_READ_WRITE);
  region b       = map_ok(h, 0, 2 * g, region_access::REGION_ACCESS_READ);

  static_cast<unsigned char*>(a.data)[0]     = 0xa1;
  static_cast<unsigned char*>(a.data)[g - 1] = 0xa2;
  // Every region of the file sees it at once
  CHECK(bytes(b)[100] == 0xa1);
  CHECK(bytes(b)[100 + g - 1] == 0xa2);
  // Reads through the handle after region_flush()
  REQUIRE(region_flush(a, 0, g).ok());
  REQUIRE(region_flush(a, g - 1, 1).ok());
  REQUIRE(region_flush(a, 5, 0).ok());
  REQUIRE(file_flush(h).ok());
  const auto back = read_all(h, 99, 3);
  CHECK(back[0] == pattern_byte(99));
  CHECK(back[1] == 0xa1);
  CHECK(back[2] == pattern_byte(101));
  CHECK(read_all(h, 100 + g - 1, 1)[0] == 0xa2);
  region_unmap(a);
  region_unmap(b);
  file_close(h);
}

TEST_CASE("region: copy-on-write never reaches the file")
{
  session s;
  temp_dir dir;
  osprim::file_handle h = make_pattern_file(dir.path("cow.bin"), 8192);
  region c      = map_ok(h, 10, 100, region_access::REGION_ACCESS_COPY_ON_WRITE);
  region r      = map_ok(h, 0, 8192, region_access::REGION_ACCESS_READ);
  static_cast<unsigned char*>(c.data)[0] = static_cast<unsigned char>(~pattern_byte(10));
  CHECK(bytes(c)[0] == static_cast<unsigned char>(~pattern_byte(10)));
  CHECK(bytes(r)[10] == pattern_byte(10));
  CHECK(read_all(h, 10, 1)[0] == pattern_byte(10));
  region_unmap(c);
  region_unmap(r);
  file_close(h);
}

TEST_CASE("region: tracking and diagnostics")
{
  if (!tracking_compiled)
    return;
  temp_dir dir;
  const size_t g = region_granularity();

  SUBCASE("leak")
  {
    session s;
    osprim::file_handle h = make_pattern_file(dir.path("leak.bin"), 100);
    region r      = map_ok(h, 0, 100, region_access::REGION_ACCESS_READ);
    file_close(h);
    const diagnostic_log& log = s.end();
    REQUIRE(log.count == 1);
    CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
    CHECK(::strcmp(log.entries[0].handle_type, "region") == 0);
    CHECK(log.entries[0].native_value == reinterpret_cast<uintptr_t>(r.data));
    region_unmap(r);
  }

  SUBCASE("shrinking under a live region")
  {
    session s;
    osprim::file_handle h = make_pattern_file(dir.path("shrink.bin"), 2 * g);
    region r      = map_ok(h, 0, 100, region_access::REGION_ACCESS_READ);

    // Above the region: allowed everywhere, no diagnostic
    const auto above = file_set_size(h, g, file_size_mode::FILE_SIZE_MODE_FAST);
    INFO("native code: " << above.native_code);
#if OSPRIM_OS_WINDOWS
    // Wine refuses to truncate a file with any mapping alive
    if (under_wine())
      CHECK((above.ok() || above.result == file_set_size_result::FILE_SET_SIZE_MAPPED));
    else
      CHECK(above.ok());
#else
    CHECK(above.ok());
#endif
    CHECK(s.log.count == 0);

    // Below it: reported; Windows refuses
    const auto z = file_set_size(h, 50, file_size_mode::FILE_SIZE_MODE_FAST);
#if OSPRIM_OS_WINDOWS
    CHECK(z.result == file_set_size_result::FILE_SET_SIZE_MAPPED);
#else
    CHECK(z.ok());
#endif
    REQUIRE(s.log.count == 1);
    CHECK(s.log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_REGION_TRUNCATED);
    CHECK(s.log.entries[0].native_value == reinterpret_cast<uintptr_t>(r.data));
    s.log.count = 0;
    region_unmap(r);
    REQUIRE(file_set_size(h, 10, file_size_mode::FILE_SIZE_MODE_FAST).ok());
    file_close(h);
  }

  SUBCASE("writable regions over holes")
  {
    session s;
    osprim::file_handle h = make_pattern_file(dir.path("hole.bin"), 0);
    REQUIRE(file_set_size(h, 4 * g, file_size_mode::FILE_SIZE_MODE_FAST).ok());
    bool expect_hole = false;
#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
    struct stat st{};
    REQUIRE(::fstat(h.native, &st) == 0);
    expect_hole = static_cast<uint64_t>(st.st_blocks) * 512 < static_cast<uint64_t>(st.st_size);
#endif
    region r = map_ok(h, 0, g, region_access::REGION_ACCESS_READ_WRITE);
    region_unmap(r);
    if (expect_hole)
    {
      REQUIRE(s.log.count == 1);
      CHECK(s.log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_REGION_OVER_HOLE);
    }
    else
      note("holes detected: %llu (FAST growth may allocate here)", static_cast<unsigned long long>(s.log.count));
    s.log.count = 0;

    // Read-only regions are not checked
    r = map_ok(h, 0, g, region_access::REGION_ACCESS_READ);
    region_unmap(r);
    CHECK(s.log.count == 0);

    // RESERVE leaves no hole
    osprim::file_handle q = make_pattern_file(dir.path("reserved.bin"), 0);
    const auto z  = file_set_size(q, 4 * g, file_size_mode::FILE_SIZE_MODE_RESERVE);
    if (z.ok())
    {
      r = map_ok(q, g, 2 * g, region_access::REGION_ACCESS_READ_WRITE);
      region_unmap(r);
      CHECK(s.log.count == 0);
    }
    else
      note("RESERVE unsupported here (native code %d)", z.native_code);
    // Written data is not a hole either
    osprim::file_handle w = make_pattern_file(dir.path("written.bin"), 2 * g);
    r             = map_ok(w, 0, 2 * g, region_access::REGION_ACCESS_READ_WRITE);
    region_unmap(r);
    CHECK(s.log.count == 0);
    s.log.count = 0;
    file_close(h);
    file_close(q);
    file_close(w);
  }
}
