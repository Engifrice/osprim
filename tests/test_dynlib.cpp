/*****************************************************************/ /**
 * @file   test_dynlib.cpp
 * @brief  Dynamic libraries: loading the test plugin by path, symbols,
 * shared handles and their tracking, failures and their messages, system
 * libraries.
 *
 * The plugin's path is OSPRIM_TEST_PLUGIN when set (devices where the
 * tests run away from the build tree), else the one CMake built.
 *********************************************************************/
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
  #define _CRT_SECURE_NO_WARNINGS // getenv
#endif

#include "files.hpp"
#include "internal.hpp"
#include "support.hpp"

#include <osprim/dynlib.hpp>

#include <doctest/doctest.h>

#include <stdlib.h>
#include <string.h>

#include <string>

#if !OSPRIM_OS_WINDOWS
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace osprim;
using namespace osprim_test;

namespace
{
  std::string plugin_path()
  {
    const char* env = ::getenv("OSPRIM_TEST_PLUGIN");
    if (env != nullptr && *env != '\0')
      return env;
    return OSPRIM_TEST_PLUGIN_PATH;
  }

  dynlib_handle open_plugin()
  {
    const std::string p = plugin_path();
    char message[512];
    dynlib_open_desc desc;
    desc.message          = message;
    desc.message_capacity = sizeof(message);
    size_t message_size   = 0;
    dynlib_handle lib{};
    const auto s = dynlib_open(p.data(), p.size(), desc, lib, &message_size);
    INFO(
        "path: " << p << " result " << static_cast<int>(s.result) << " native "
                 << s.native_code
                 << " message: " << std::string(message, message_size));
    REQUIRE(s.ok());
    CHECK(message_size == 0);
    REQUIRE(lib.native != nullptr);
    return lib;
  }

  status<dynlib_open_result> try_open(
      const std::string& name, dynlib_search search, dynlib_handle& out,
      std::string* message = nullptr)
  {
    char buffer[512];
    dynlib_open_desc desc;
    desc.search           = search;
    desc.message          = buffer;
    desc.message_capacity = sizeof(buffer);
    size_t size           = 0;
    const auto s          = dynlib_open(name.data(), name.size(), desc, out, &size);
    if (message != nullptr)
      *message = std::string(buffer, size);
    return s;
  }

  using add_fn   = int (*)(int, int);
  using count_fn = int (*)();
} // namespace

TEST_CASE("dynlib: file name constants")
{
  const std::string p = plugin_path();
  const std::string expected =
      std::string(DYNLIB_PREFIX) + "osprim_test_plugin" + DYNLIB_SUFFIX;
  REQUIRE(p.size() > expected.size());
  CHECK(p.compare(p.size() - expected.size(), expected.size(), expected) == 0);
  const char before = p[p.size() - expected.size() - 1];
  CHECK((before == '/' || before == '\\'));
}

TEST_CASE("dynlib: open by path and look up symbols")
{
  session s;
  const dynlib_handle lib = open_plugin();

  void* sym = nullptr;
  REQUIRE(dynlib_symbol(lib, "osprim_test_plugin_add", sym).ok());
  REQUIRE(sym != nullptr);
  CHECK(reinterpret_cast<add_fn>(sym)(40, 2) == 42);

  // The pointer-and-size overload: no terminator needed
  const char name[] = "osprim_test_plugin_valueXYZ";
  void* value       = nullptr;
  REQUIRE(
      dynlib_symbol(lib, name, sizeof("osprim_test_plugin_value") - 1, value).ok());
  REQUIRE(value != nullptr);
  CHECK(*static_cast<int*>(value) == 42);
  // Through the data symbol, writes are seen by the library's code
  *static_cast<int*>(value) = 7;
  void* again               = nullptr;
  REQUIRE(dynlib_symbol(lib, std::string("osprim_test_plugin_value").c_str(), again)
              .ok());
  CHECK(again == value);
  *static_cast<int*>(value) = 42;

  SUBCASE("missing symbol")
  {
    void* none   = &sym;
    const auto r = dynlib_symbol(lib, "osprim_test_plugin_missing", none);
    CHECK(r.result == dynlib_symbol_result::DYNLIB_SYMBOL_NOT_FOUND);
    CHECK(none == nullptr);
    // Not a prefix match
    CHECK(
        dynlib_symbol(lib, "osprim_test_plugin_ad", none).result
        == dynlib_symbol_result::DYNLIB_SYMBOL_NOT_FOUND);
  }
  SUBCASE("invalid names")
  {
    void* none = &sym;
    CHECK(
        dynlib_symbol(lib, "", none).result
        == dynlib_symbol_result::DYNLIB_SYMBOL_INVALID_NAME);
    CHECK(none == nullptr);
    CHECK(
        dynlib_symbol(lib, "osprim", 0, none).result
        == dynlib_symbol_result::DYNLIB_SYMBOL_INVALID_NAME);
    const char nul[] = "osprim_test_plugin_add\0x";
    CHECK(
        dynlib_symbol(lib, nul, sizeof(nul) - 1, none).result
        == dynlib_symbol_result::DYNLIB_SYMBOL_INVALID_NAME);
    const std::string longest(DYNLIB_SYMBOL_MAX, 'a');
    CHECK(
        dynlib_symbol(lib, longest.data(), longest.size(), none).result
        == dynlib_symbol_result::DYNLIB_SYMBOL_NOT_FOUND);
    const std::string too_long(DYNLIB_SYMBOL_MAX + 1, 'a');
    CHECK(
        dynlib_symbol(lib, too_long.data(), too_long.size(), none).result
        == dynlib_symbol_result::DYNLIB_SYMBOL_INVALID_NAME);
  }

  dynlib_close(lib);
}

TEST_CASE("dynlib: loading twice shares one image")
{
  session s;
  const dynlib_handle a = open_plugin();
  const dynlib_handle b = open_plugin();
  CHECK(a.native == b.native);
  if (tracking_compiled)
    CHECK(detail::tracked_count() == 1);

  void* fa = nullptr;
  void* fb = nullptr;
  REQUIRE(dynlib_symbol(a, "osprim_test_plugin_count", fa).ok());
  REQUIRE(dynlib_symbol(b, "osprim_test_plugin_count", fb).ok());
  CHECK(fa == fb);
  const int first = reinterpret_cast<count_fn>(fa)();
  CHECK(reinterpret_cast<count_fn>(fb)() == first + 1);

  // One release: still loaded, still tracked
  dynlib_close(a);
  if (tracking_compiled)
    CHECK(detail::tracked_count() == 1);
  CHECK(reinterpret_cast<count_fn>(fb)() == first + 2);
  dynlib_close(b);
  if (tracking_compiled)
    CHECK(detail::tracked_count() == 0);
  // The session checks that no diagnostic was reported
}

TEST_CASE("dynlib: tracking")
{
  if (!tracking_compiled)
    return;
  {
    // A library still opened at deinitialization is a leak, reported once
    session s;
    const dynlib_handle a     = open_plugin();
    const dynlib_handle b     = open_plugin();
    const diagnostic_log& log = s.end();
    REQUIRE(log.count == 1);
    CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
    CHECK(::strcmp(log.entries[0].handle_type, "dynlib") == 0);
    dynlib_close(a); // after deinitialize: not tracked any more
    dynlib_close(b);
  }
  {
    // More closes than opens is misuse (checked before the native call,
    // which is skipped here by keeping one extra native reference)
    session s;
    const dynlib_handle keep = open_plugin();
    detail::track_close(
        detail::DYNLIB_TRACK_TYPE,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(keep.native)),
        std::source_location::current());
    CHECK(detail::tracked_count() == 0);
    detail::track_close(
        detail::DYNLIB_TRACK_TYPE,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(keep.native)),
        std::source_location::current());
    const diagnostic_log& log = s.end();
    REQUIRE(log.count == 1);
    CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_MISUSE);
    dynlib_close(keep);
  }
}

TEST_CASE("dynlib: path failures")
{
  session s;
  temp_dir dir;
  dynlib_handle lib{};
  lib.native = &lib; // left alone on failure

  SUBCASE("missing file")
  {
    std::string message;
    const std::string p =
        dir.path(std::string(DYNLIB_PREFIX) + "missing" + DYNLIB_SUFFIX);
    const auto r = try_open(p, dynlib_search::DYNLIB_SEARCH_PATH, lib, &message);
    CHECK(r.result == dynlib_open_result::DYNLIB_OPEN_NOT_FOUND);
    CHECK(!message.empty());
    note("missing library: %s", message.c_str());

    // Truncated to the capacity; no message size wanted is fine
    char small[4];
    dynlib_open_desc desc;
    desc.message          = small;
    desc.message_capacity = sizeof(small);
    size_t size           = 99;
    CHECK(
        dynlib_open(p.data(), p.size(), desc, lib, &size).result
        == dynlib_open_result::DYNLIB_OPEN_NOT_FOUND);
    CHECK(size == sizeof(small));
    CHECK(
        dynlib_open(p.data(), p.size(), desc, lib).result
        == dynlib_open_result::DYNLIB_OPEN_NOT_FOUND);
    // No buffer: no message, size 0
    size = 99;
    CHECK(
        dynlib_open(p.data(), p.size(), dynlib_open_desc{}, lib, &size).result
        == dynlib_open_result::DYNLIB_OPEN_NOT_FOUND);
    CHECK(size == 0);
  }
  SUBCASE("not a library")
  {
    const std::string p =
        dir.path(std::string(DYNLIB_PREFIX) + "text" + DYNLIB_SUFFIX);
    osprim::file_handle f = make_pattern_file(p, 4096);
    file_close(f);
    std::string message;
    const auto r = try_open(p, dynlib_search::DYNLIB_SEARCH_PATH, lib, &message);
    CHECK(r.result == dynlib_open_result::DYNLIB_OPEN_LOAD_FAILED);
    CHECK(!message.empty());
    note("not a library: %s", message.c_str());
  }
#if !OSPRIM_OS_WINDOWS
  SUBCASE("unreadable file")
  {
    // (no early return: doctest would not discover the later subcases)
    if (::geteuid() == 0)
      note("running as root: permissions not enforced, skipped");
    else
    {
      const std::string p =
          dir.path(std::string(DYNLIB_PREFIX) + "locked" + DYNLIB_SUFFIX);
      file_close(make_pattern_file(p, 16));
      REQUIRE(::chmod(p.c_str(), 0) == 0);
      CHECK(
          try_open(p, dynlib_search::DYNLIB_SEARCH_PATH, lib).result
          == dynlib_open_result::DYNLIB_OPEN_ACCESS_DENIED);
    }
  }
#endif
  SUBCASE("a directory")
  {
    const std::string p = dir.make_dir("lib.d");
    CHECK(
        try_open(p, dynlib_search::DYNLIB_SEARCH_PATH, lib).result
        == dynlib_open_result::DYNLIB_OPEN_LOAD_FAILED);
  }
  SUBCASE("relative path")
  {
    const std::string p =
        std::string(DYNLIB_PREFIX) + "osprim_test_plugin" + DYNLIB_SUFFIX;
    CHECK(
        try_open(p, dynlib_search::DYNLIB_SEARCH_PATH, lib).result
        == dynlib_open_result::DYNLIB_OPEN_PATH_NOT_ABSOLUTE);
    CHECK(
        try_open("./" + p, dynlib_search::DYNLIB_SEARCH_PATH, lib).result
        == dynlib_open_result::DYNLIB_OPEN_PATH_NOT_ABSOLUTE);
  }
  SUBCASE("invalid and long paths")
  {
    const std::string bad = dir.root() + dir.sep() + "\xff\xfe.so";
    CHECK(
        try_open(bad, dynlib_search::DYNLIB_SEARCH_PATH, lib).result
        == dynlib_open_result::DYNLIB_OPEN_INVALID_PATH);
    const std::string too_long =
        dir.root() + dir.sep() + std::string(MAX_PATH_BYTES, 'a');
    CHECK(
        try_open(too_long, dynlib_search::DYNLIB_SEARCH_PATH, lib).result
        == dynlib_open_result::DYNLIB_OPEN_PATH_TOO_LONG);
  }
  CHECK(lib.native == &lib);
}

TEST_CASE("dynlib: system libraries")
{
  session s;
#if OSPRIM_OS_WINDOWS
  const std::string system_lib = "kernel32.dll";
  const char* system_symbol    = "GetCurrentProcessId";
#elif OSPRIM_OS_APPLE
  const std::string system_lib = "libSystem.B.dylib";
  const char* system_symbol    = "getpid";
#elif OSPRIM_OS_ANDROID
  const std::string system_lib = "libc.so";
  const char* system_symbol    = "getpid";
#else
  const std::string system_lib = "libc.so.6";
  const char* system_symbol    = "getpid";
#endif
  dynlib_handle lib{};
  std::string message;
  const auto r =
      try_open(system_lib, dynlib_search::DYNLIB_SEARCH_SYSTEM, lib, &message);
  INFO(
      "result " << static_cast<int>(r.result) << " native " << r.native_code
                << " message: " << message);
  REQUIRE(r.ok());
  void* sym = nullptr;
  CHECK(dynlib_symbol(lib, system_symbol, sym).ok());
  CHECK(sym != nullptr);
  dynlib_close(lib);

  SUBCASE("missing system library")
  {
    const std::string name =
        std::string(DYNLIB_PREFIX) + "osprim_no_such_library" + DYNLIB_SUFFIX;
    const auto m =
        try_open(name, dynlib_search::DYNLIB_SEARCH_SYSTEM, lib, &message);
    CHECK(m.result == dynlib_open_result::DYNLIB_OPEN_LOAD_FAILED);
    CHECK(!message.empty());
  }
  SUBCASE("SYSTEM takes bare names only")
  {
    CHECK(
        try_open("sub/" + system_lib, dynlib_search::DYNLIB_SEARCH_SYSTEM, lib)
            .result
        == dynlib_open_result::DYNLIB_OPEN_INVALID_PATH);
    CHECK(
        try_open(plugin_path(), dynlib_search::DYNLIB_SEARCH_SYSTEM, lib).result
        == dynlib_open_result::DYNLIB_OPEN_INVALID_PATH);
    CHECK(
        try_open("..", dynlib_search::DYNLIB_SEARCH_SYSTEM, lib).result
        == dynlib_open_result::DYNLIB_OPEN_INVALID_PATH);
#if OSPRIM_OS_WINDOWS
    CHECK(
        try_open("sub\\kernel32.dll", dynlib_search::DYNLIB_SEARCH_SYSTEM, lib)
            .result
        == dynlib_open_result::DYNLIB_OPEN_INVALID_PATH);
    CHECK(
        try_open("C:kernel32.dll", dynlib_search::DYNLIB_SEARCH_SYSTEM, lib).result
        == dynlib_open_result::DYNLIB_OPEN_INVALID_PATH);
#endif
  }
}
