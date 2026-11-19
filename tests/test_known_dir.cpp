/*****************************************************************/ /**
 * @file   test_known_dir.cpp
 * @brief  Known directories: every purpose on this platform, the flags and
 * same_as, overrides (copy, normalization, creation of missing components,
 * the Android root rule, refusals), dynlib_path_of(), backup exclusion.
 *
 * Android runs the tests from adb shell, outside any application: there is
 * no data root there unless an override gives one (TMPDIR is
 * /data/local/tmp), so the platform purposes are NOT_CONFIGURED.
 *********************************************************************/
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
  #define _CRT_SECURE_NO_WARNINGS // getenv
#endif

#include "files.hpp"
#include "support.hpp"

#include <osprim/dynlib.hpp>
#include <osprim/file.hpp>
#include <osprim/known_dir.hpp>
#include <osprim/path.hpp>

#include <doctest/doctest.h>

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#if OSPRIM_OS_APPLE
  #include <TargetConditionals.h>
#endif
#if !OSPRIM_OS_WINDOWS
  #include <sys/stat.h>
#endif

using namespace osprim;
using namespace osprim_test;

namespace
{
  constexpr known_dir ALL[] = {
      known_dir::KNOWN_DIR_DATA,       known_dir::KNOWN_DIR_STATE,
      known_dir::KNOWN_DIR_CACHE,      known_dir::KNOWN_DIR_TEMP,
      known_dir::KNOWN_DIR_EXECUTABLE, known_dir::KNOWN_DIR_RESOURCES,
  };

  const char* name_of(known_dir d)
  {
    switch (d)
    {
    case known_dir::KNOWN_DIR_DATA:
      return "DATA";
    case known_dir::KNOWN_DIR_STATE:
      return "STATE";
    case known_dir::KNOWN_DIR_CACHE:
      return "CACHE";
    case known_dir::KNOWN_DIR_TEMP:
      return "TEMP";
    case known_dir::KNOWN_DIR_EXECUTABLE:
      return "EXECUTABLE";
    case known_dir::KNOWN_DIR_RESOURCES:
      return "RESOURCES";
    }
    return "?";
  }

  bool is_install(known_dir d)
  {
    return d == known_dir::KNOWN_DIR_EXECUTABLE || d == known_dir::KNOWN_DIR_RESOURCES;
  }

  struct query
  {
    status<known_dir_get_result> s{};
    std::string path;
    known_dir_info info{};
  };

  query get(known_dir d)
  {
    query q;
    char buffer[MAX_PATH_BYTES + 1];
    size_t needed = 0;
    q.s           = known_dir_get(d, buffer, sizeof(buffer), needed, &q.info);
    if (q.s.ok())
      q.path.assign(buffer, needed);
    return q;
  }

  bool is_absolute(const std::string& p)
  {
#if OSPRIM_OS_WINDOWS
    return (p.size() >= 3 && p[1] == ':' && p[2] == '\\')
           || (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
#else
    return !p.empty() && p[0] == '/';
#endif
  }

  bool has_trailing_separator(const std::string& p)
  {
#if OSPRIM_OS_WINDOWS
    return p.size() > 3 && (p.back() == '\\' || p.back() == '/');
#else
    return p.size() > 1 && p.back() == '/';
#endif
  }

  [[maybe_unused]] std::string parent_of(const std::string& p)
  {
    const size_t i = p.find_last_of(temp_dir::sep());
    REQUIRE(i != std::string::npos);
#if OSPRIM_OS_WINDOWS
    return p.substr(0, i == 2 ? 3 : i);
#else
    return p.substr(0, i == 0 ? 1 : i);
#endif
  }

  bool ends_with(const std::string& s, const std::string& suffix)
  {
    return s.size() >= suffix.size()
           && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  std::string plugin_path()
  {
    const char* env = ::getenv("OSPRIM_TEST_PLUGIN");
    if (env != nullptr && *env != '\0')
      return env;
    return OSPRIM_TEST_PLUGIN_PATH;
  }

  known_dir_override make_override(known_dir d, const std::string& path)
  {
    return known_dir_override{d, path.data(), path.size()};
  }

  /// @brief Removes directories when it goes out of scope, last added
  /// first. Declared after the session, so that it runs before
  /// deinitialize().
  struct dir_cleanup
  {
    std::vector<std::string> paths;

    ~dir_cleanup()
    {
      for (size_t i = paths.size(); i-- > 0;)
        path_remove_at(
            NO_DIRECTORY, paths[i].data(), paths[i].size(),
            path_resolve::PATH_RESOLVE_NORMAL, path_kind::PATH_KIND_DIRECTORY);
    }
  };

  /// @brief A variable of the test executable, for dynlib_path_of().
  int g_marker = 0;
} // namespace

TEST_CASE("known_dir: every purpose on this platform")
{
  session s;
  for (known_dir d : ALL)
  {
    const query q = get(d);
    INFO(
        name_of(d) << ": result " << static_cast<int>(q.s.result) << " native "
                   << q.s.native_code << " path " << q.path);
#if OSPRIM_OS_ANDROID
    if (is_install(d))
    {
      CHECK(q.s.result == known_dir_get_result::KNOWN_DIR_GET_UNSUPPORTED);
      continue;
    }
    CHECK((q.s.ok() || q.s.result == known_dir_get_result::KNOWN_DIR_GET_NOT_CONFIGURED));
    if (!q.s.ok())
      continue;
#elif OSPRIM_OS_APPLE && !TARGET_OS_OSX
    // The simulator may launch the tests without HOME
    CHECK((q.s.ok() || q.s.result == known_dir_get_result::KNOWN_DIR_GET_NOT_CONFIGURED));
    if (!q.s.ok())
      continue;
#else
    REQUIRE(q.s.ok());
#endif
    note("%s: %s", name_of(d), q.path.c_str());
    CHECK(is_absolute(q.path));
    CHECK(!has_trailing_separator(q.path));
    CHECK(!q.info.overridden);
    CHECK((q.info.same_as & known_dir_bit(d)) == 0);

    // Present or not yet created: never created by a query
    dir_handle h{};
    known_dir_info info{};
    const auto o = known_dir_open(d, false, h, &info);
    CHECK((o.ok() || o.result == known_dir_open_result::KNOWN_DIR_OPEN_NOT_FOUND));
    if (o.ok())
    {
      CHECK(info.same_as == q.info.same_as);
      dir_close(h);
    }
    if (is_install(d) || d == known_dir::KNOWN_DIR_TEMP)
      CHECK(o.ok());
    if (is_install(d))
    {
      CHECK(q.info.app_private);
      CHECK(!q.info.backed_up);
      CHECK(!q.info.may_be_purged);
    }
    if (d == known_dir::KNOWN_DIR_TEMP)
      CHECK(q.info.may_be_purged);
  }

  // The directories osprim maps to one place
  const query data  = get(known_dir::KNOWN_DIR_DATA);
  const query exe   = get(known_dir::KNOWN_DIR_EXECUTABLE);
  const query cache = get(known_dir::KNOWN_DIR_CACHE);
#if OSPRIM_OS_WINDOWS
  CHECK(
      data.info.same_as
      == (known_dir_bit(known_dir::KNOWN_DIR_STATE)
          | known_dir_bit(known_dir::KNOWN_DIR_CACHE)));
  CHECK((exe.info.same_as & known_dir_bit(known_dir::KNOWN_DIR_RESOURCES)) != 0);
  CHECK(!data.info.app_private);
  CHECK(!data.info.backed_up);
#elif OSPRIM_OS_LINUX
  CHECK((exe.info.same_as & known_dir_bit(known_dir::KNOWN_DIR_RESOURCES)) != 0);
  CHECK(!data.info.backed_up);
#elif OSPRIM_OS_APPLE
  if (data.s.ok())
  {
    CHECK((data.info.same_as & known_dir_bit(known_dir::KNOWN_DIR_STATE)) != 0);
    CHECK(data.info.backed_up);
    CHECK(cache.info.may_be_purged);
  }
#endif
  (void)data;
  (void)exe;
  (void)cache;
}

TEST_CASE("known_dir: buffer too small")
{
  session s;
  const query temp = get(known_dir::KNOWN_DIR_TEMP);
  if (!temp.s.ok())
    return; // Android outside an application
  char one[1];
  size_t needed = 0;
  const auto r  = known_dir_get(known_dir::KNOWN_DIR_TEMP, one, 0, needed);
  CHECK(r.result == known_dir_get_result::KNOWN_DIR_GET_BUFFER_TOO_SMALL);
  CHECK(needed == temp.path.size());
}

TEST_CASE("known_dir: overrides are copied, normalized and created on demand")
{
  temp_dir tmp;
  const std::string sep  = temp_dir::sep();
  const std::string base = tmp.root() + sep + "known";
  std::string data       = base + sep + "data" + sep; // the separator goes
  std::string shared     = base + sep + "shared";
  std::string temp       = base + sep + "temp";
  const known_dir_override overrides[] = {
      make_override(known_dir::KNOWN_DIR_DATA, data),
      make_override(known_dir::KNOWN_DIR_STATE, shared),
      make_override(known_dir::KNOWN_DIR_CACHE, shared),
      make_override(known_dir::KNOWN_DIR_TEMP, temp),
  };
  init_desc desc;
  desc.known_dir_overrides      = overrides;
  desc.known_dir_override_count = sizeof(overrides) / sizeof(overrides[0]);
  session s(desc);
  dir_cleanup cleanup;

  // Copied by initialize()
  data.assign(data.size(), 'x');
  shared.assign(shared.size(), 'x');

  const query q = get(known_dir::KNOWN_DIR_DATA);
  REQUIRE(q.s.ok());
  CHECK(q.path == base + sep + "data");
  CHECK(q.info.overridden);
  CHECK(q.info.app_private);
  CHECK(
      (q.info.same_as
       & (known_dir_bit(known_dir::KNOWN_DIR_STATE)
          | known_dir_bit(known_dir::KNOWN_DIR_CACHE)))
      == 0);

  const query state = get(known_dir::KNOWN_DIR_STATE);
  const query cache = get(known_dir::KNOWN_DIR_CACHE);
  REQUIRE(state.s.ok());
  REQUIRE(cache.s.ok());
  CHECK(state.path == base + sep + "shared");
  CHECK((state.info.same_as & known_dir_bit(known_dir::KNOWN_DIR_CACHE)) != 0);
  CHECK((cache.info.same_as & known_dir_bit(known_dir::KNOWN_DIR_STATE)) != 0);

  // Missing until created, with every missing component
  dir_handle h{};
  CHECK(
      known_dir_open(known_dir::KNOWN_DIR_DATA, false, h).result
      == known_dir_open_result::KNOWN_DIR_OPEN_NOT_FOUND);
  cleanup.paths = {base, base + sep + "data", base + sep + "shared"};
  known_dir_info info{};
  const auto created = known_dir_open(known_dir::KNOWN_DIR_DATA, true, h, &info);
  INFO("result " << static_cast<int>(created.result) << " native " << created.native_code);
  REQUIRE(created.ok());
  CHECK(info.overridden);
  dir_close(h);
  REQUIRE(known_dir_open(known_dir::KNOWN_DIR_STATE, true, h).ok());
  dir_close(h);
  REQUIRE(known_dir_open(known_dir::KNOWN_DIR_CACHE, false, h).ok()); // the same one
  dir_close(h);
  CHECK(known_dir_open(known_dir::KNOWN_DIR_DATA, false, h).ok());
  dir_close(h);

#if !OSPRIM_OS_WINDOWS
  // Private, as the XDG specification asks (the umask can only remove bits)
  struct stat st{};
  REQUIRE(::stat((base + "/data").c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);
#endif
}

TEST_CASE("known_dir: an override naming a file")
{
  temp_dir tmp;
  const std::string file = tmp.path("not_a_directory");
  {
    file_open_desc fd;
    fd.access      = file_access::FILE_ACCESS_READ_WRITE;
    fd.disposition = file_disposition::FILE_DISPOSITION_CREATE_NEW;
    file_handle f{};
    session s;
    REQUIRE(file_open(file.data(), file.size(), fd, f).ok());
    file_close(f);
  }
  const known_dir_override o[] = {make_override(known_dir::KNOWN_DIR_DATA, file)};
  init_desc desc;
  desc.known_dir_overrides      = o;
  desc.known_dir_override_count = 1;
  session s(desc);
  dir_handle h{};
  CHECK(
      known_dir_open(known_dir::KNOWN_DIR_DATA, true, h).result
      == known_dir_open_result::KNOWN_DIR_OPEN_NOT_A_DIRECTORY);
}

TEST_CASE("known_dir: invalid overrides refuse initialization")
{
  temp_dir tmp;
  const std::string good = tmp.root();
#if OSPRIM_OS_WINDOWS
  const std::string relative = "relative\\dir";
#else
  const std::string relative = "relative/dir";
#endif
  const std::string not_utf8 = good + "\xff";

  struct bad
  {
    const char* what;
    std::vector<known_dir_override> list;
    bool null_list;
  };
  const bad cases[] = {
      {"relative", {make_override(known_dir::KNOWN_DIR_DATA, relative)}, false},
      {"empty", {known_dir_override{known_dir::KNOWN_DIR_DATA, good.data(), 0}}, false},
      {"null path", {known_dir_override{known_dir::KNOWN_DIR_DATA, nullptr, 4}}, false},
      {"not UTF-8", {make_override(known_dir::KNOWN_DIR_DATA, not_utf8)}, false},
      {"twice",
       {make_override(known_dir::KNOWN_DIR_CACHE, good),
        make_override(known_dir::KNOWN_DIR_CACHE, good)},
       false},
      {"unknown purpose",
       {make_override(static_cast<known_dir>(KNOWN_DIR_COUNT), good)},
       false},
      {"null list", {}, true},
  };
  for (const bad& c : cases)
  {
    INFO(c.what);
    init_desc desc;
    desc.known_dir_overrides      = c.null_list ? nullptr : c.list.data();
    desc.known_dir_override_count = c.null_list ? 1 : c.list.size();
    const auto r                  = initialize(desc);
    CHECK(r.result == initialize_result::INITIALIZE_INVALID_OVERRIDE);
    if (r.ok())
      deinitialize();
  }

  // Nothing was left behind
  session s;
  const query q = get(known_dir::KNOWN_DIR_CACHE);
  CHECK(!q.info.overridden);
}

TEST_CASE("known_dir: the Android data root comes from DATA named files")
{
  temp_dir tmp;
  const std::string files = tmp.root() + temp_dir::sep() + "files";
  const known_dir_override o[] = {make_override(known_dir::KNOWN_DIR_DATA, files)};
  init_desc desc;
  desc.known_dir_overrides      = o;
  desc.known_dir_override_count = 1;
  session s(desc);

  const query state = get(known_dir::KNOWN_DIR_STATE);
#if OSPRIM_OS_ANDROID
  const query cache = get(known_dir::KNOWN_DIR_CACHE);
  const query temp  = get(known_dir::KNOWN_DIR_TEMP);
  REQUIRE(state.s.ok());
  REQUIRE(cache.s.ok());
  REQUIRE(temp.s.ok());
  CHECK(state.path == tmp.root() + "/no_backup");
  CHECK(cache.path == tmp.root() + "/cache");
  CHECK(temp.path == cache.path);
  CHECK(state.info.overridden);
  CHECK(!state.info.backed_up);
  CHECK(cache.info.may_be_purged);
  CHECK((cache.info.same_as & known_dir_bit(known_dir::KNOWN_DIR_TEMP)) != 0);
  CHECK(get(known_dir::KNOWN_DIR_DATA).info.backed_up);
#else
  // Elsewhere the rule does not apply: the others keep the platform's
  CHECK(!state.info.overridden);
#endif
}

TEST_CASE("dynlib_path_of: the executable and a plugin")
{
  session s;
  char buffer[MAX_PATH_BYTES + 1];
  size_t needed = 0;
  const auto r  = dynlib_path_of(&g_marker, buffer, sizeof(buffer), needed);
  INFO("result " << static_cast<int>(r.result) << " native " << r.native_code);
  REQUIRE(r.ok());
  const std::string self(buffer, needed);
  note("executable: %s", self.c_str());
  CHECK(is_absolute(self));
#if !OSPRIM_OS_ANDROID
  const query exe = get(known_dir::KNOWN_DIR_EXECUTABLE);
  REQUIRE(exe.s.ok());
  CHECK(parent_of(self) == exe.path);
#endif

  size_t small = 0;
  CHECK(
      dynlib_path_of(&g_marker, buffer, 0, small).result
      == dynlib_path_result::DYNLIB_PATH_BUFFER_TOO_SMALL);
  CHECK(small == self.size());

  // Not inside any executable or library
  CHECK(
      dynlib_path_of(nullptr, buffer, sizeof(buffer), needed).result
      == dynlib_path_result::DYNLIB_PATH_NOT_FOUND);
  int on_stack = 0;
  CHECK(
      dynlib_path_of(&on_stack, buffer, sizeof(buffer), needed).result
      == dynlib_path_result::DYNLIB_PATH_NOT_FOUND);

  // A library opened by the test
  const std::string p = plugin_path();
  dynlib_open_desc od;
  dynlib_handle lib{};
  REQUIRE(dynlib_open(p.data(), p.size(), od, lib).ok());
  void* symbol = nullptr;
  REQUIRE(dynlib_symbol(lib, "osprim_test_plugin_value", symbol).ok());
  const auto pr = dynlib_path_of(symbol, buffer, sizeof(buffer), needed);
  INFO("result " << static_cast<int>(pr.result) << " native " << pr.native_code);
  REQUIRE(pr.ok());
  const std::string plugin(buffer, needed);
  note("plugin: %s", plugin.c_str());
  CHECK(is_absolute(plugin));
  CHECK(ends_with(
      plugin, std::string(DYNLIB_PREFIX) + "osprim_test_plugin" + DYNLIB_SUFFIX));
  dynlib_close(lib);
}

TEST_CASE("dir_set_backup_excluded: mark and unmark a directory")
{
  temp_dir tmp;
  const std::string sub = tmp.make_dir("backup");
  session s;
  dir_handle d{};
  REQUIRE(
      dir_open_at(NO_DIRECTORY, sub.data(), sub.size(), path_resolve::PATH_RESOLVE_NORMAL, d)
          .ok());
  using BR     = dir_set_backup_excluded_result;
  const auto r = dir_set_backup_excluded(d, true);
  INFO("result " << static_cast<int>(r.result) << " native " << r.native_code);
#if OSPRIM_OS_WINDOWS || OSPRIM_OS_ANDROID
  CHECK(r.result == BR::DIR_SET_BACKUP_EXCLUDED_UNSUPPORTED);
  CHECK(dir_set_backup_excluded(d, false).result == BR::DIR_SET_BACKUP_EXCLUDED_UNSUPPORTED);
#else
  CHECK(r.ok());
  CHECK(dir_set_backup_excluded(d, true).ok()); // idempotent
  #if OSPRIM_OS_LINUX
  {
    file_open_desc fd;
    file_handle f{};
    REQUIRE(file_open_at(
                d, "CACHEDIR.TAG", 12, path_resolve::PATH_RESOLVE_NO_SYMLINKS, fd, f)
                .ok());
    char head[43] = {};
    size_t got    = 0;
    REQUIRE(file_read_at(f, 0, head, sizeof(head), got).ok());
    file_close(f);
    CHECK(got == sizeof(head));
    CHECK(::memcmp(head, "Signature: 8a477f597d28d172789f06886806bc55", 43) == 0);
  }
  #endif
  CHECK(dir_set_backup_excluded(d, false).ok());
  CHECK(dir_set_backup_excluded(d, false).ok()); // idempotent
  #if OSPRIM_OS_LINUX
  path_info info{};
  CHECK(
      path_info_at(d, "CACHEDIR.TAG", 12, path_resolve::PATH_RESOLVE_NORMAL, info).result
      == path_info_result::PATH_INFO_NOT_FOUND);
  // Someone else's file under that name is left alone
  {
    file_open_desc fd;
    fd.access      = file_access::FILE_ACCESS_READ_WRITE;
    fd.disposition = file_disposition::FILE_DISPOSITION_CREATE_NEW;
    file_handle f{};
    REQUIRE(file_open_at(
                d, "CACHEDIR.TAG", 12, path_resolve::PATH_RESOLVE_NORMAL, fd, f)
                .ok());
    size_t written = 0;
    REQUIRE(file_write_at(f, 0, "not a tag", 9, written).ok());
    file_close(f);
  }
  CHECK(dir_set_backup_excluded(d, true).result == BR::DIR_SET_BACKUP_EXCLUDED_CONFLICT);
  CHECK(dir_set_backup_excluded(d, false).result == BR::DIR_SET_BACKUP_EXCLUDED_CONFLICT);
  CHECK(path_remove_at(
            d, "CACHEDIR.TAG", 12, path_resolve::PATH_RESOLVE_NORMAL,
            path_kind::PATH_KIND_FILE)
            .ok());
  #endif
#endif
  dir_close(d);
}
