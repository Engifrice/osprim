/*****************************************************************/ /**
 * @file   test_shared.cpp
 * @brief  The shared build (OSPRIM_SHARED): one library state for every
 * module of the process. Handles opened in one module close in another
 * without diagnostics, and both read the same clocks. With a private copy
 * of osprim per module, the plugin's copy would be uninitialized: tracked
 * builds would report the exchanged handles as leaked and misused.
 *
 * Public API only: the internal functions the main suite uses are not
 * exported from the shared library.
 *********************************************************************/
#include "files.hpp"
#include "support.hpp"

#include <osprim/osprim.hpp>

#include <doctest/doctest.h>

#include <string>

extern "C"
{
  int osprim_shared_plugin_open(
      const char* path, size_t size, osprim::file_handle* out);
  void osprim_shared_plugin_close(const osprim::file_handle* h);
  int64_t osprim_shared_plugin_awake_ns();
}

using namespace osprim;
using namespace osprim_test;

static_assert(OSPRIM_SHARED == 1, "built without OSPRIM_SHARED");

TEST_CASE("shared: a handle opened here closes in another module")
{
  temp_dir dir;
  session s;
  INFO("tracking compiled: " << tracking_compiled);
  const std::string p = dir.path("a.bin");
  file_open_desc desc;
  desc.access      = file_access::FILE_ACCESS_READ_WRITE;
  desc.disposition = file_disposition::FILE_DISPOSITION_CREATE_NEW;
  osprim::file_handle h{};
  REQUIRE(file_open(p.data(), p.size(), desc, h).ok());
  osprim_shared_plugin_close(&h);
  // Checked at once, in its own session: a later open could reuse the
  // native value and hide a leak
  CHECK(s.end().count == 0);
}

TEST_CASE("shared: a handle opened in another module closes here")
{
  temp_dir dir;
  session s;
  INFO("tracking compiled: " << tracking_compiled);
  const std::string p = dir.path("b.bin");
  osprim::file_handle h{};
  REQUIRE(
      osprim_shared_plugin_open(p.data(), p.size(), &h)
      == static_cast<int>(file_open_result::FILE_OPEN_OK));
  const char data[] = "osprim";
  size_t done       = 0;
  REQUIRE(file_write_at(h, 0, data, sizeof(data), done).ok());
  CHECK(done == sizeof(data));
  file_close(h);
  CHECK(s.end().count == 0);
}

TEST_CASE("shared: a leak opened in another module is reported")
{
  if (!tracking_compiled)
    return;
  temp_dir dir;
  const std::string p = dir.path("leak.bin");
  osprim::file_handle h{};
  {
    session s;
    REQUIRE(
        osprim_shared_plugin_open(p.data(), p.size(), &h)
        == static_cast<int>(file_open_result::FILE_OPEN_OK));
    const diagnostic_log& log = s.end();
    REQUIRE(log.count == 1);
    CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
  }
  file_close(h); // after deinitialize: not tracked any more
}

TEST_CASE("shared: one clock state")
{
  session s;
  const awake_instant before = awake_now();
  const int64_t plugin       = osprim_shared_plugin_awake_ns();
  const awake_instant after  = awake_now();
  CHECK(before.ns <= plugin);
  CHECK(plugin <= after.ns);
}
