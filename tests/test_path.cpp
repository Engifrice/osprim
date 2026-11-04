/*****************************************************************/ /**
 * @file   test_path.cpp
 * @brief  Paths and directories: handles, name rules, entry information,
 * creation, removal, renaming, symlinks, listing, canonical paths, the
 * current directory, and the race-resistance of handle-relative calls.
 *********************************************************************/
#include "files.hpp"
#include "internal.hpp"
#include "support.hpp"

#include <osprim/path.hpp>

#include <doctest/doctest.h>

#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#if !OSPRIM_OS_WINDOWS
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace osprim;
using namespace osprim_test;

namespace
{
  osprim::dir_handle open_dir_ok(
      osprim::dir_handle at, const std::string& name,
      path_resolve resolve = path_resolve::PATH_RESOLVE_NORMAL)
  {
    osprim::dir_handle d{};
    const auto s = dir_open_at(at, name.data(), name.size(), resolve, d);
    INFO(
        "name: " << name << " result " << static_cast<int>(s.result) << " native "
                 << s.native_code);
    REQUIRE(s.ok());
    return d;
  }

  osprim::file_handle create_at(
      osprim::dir_handle d, const std::string& name, size_t size = 0)
  {
    file_open_desc desc;
    desc.access      = file_access::FILE_ACCESS_READ_WRITE;
    desc.disposition = file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE;
    osprim::file_handle h{};
    const auto s = file_open_at(
        d, name.data(), name.size(), path_resolve::PATH_RESOLVE_NORMAL, desc, h);
    INFO(
        "name: " << name << " result " << static_cast<int>(s.result) << " native "
                 << s.native_code);
    REQUIRE(s.ok());
    if (size != 0)
    {
      std::vector<unsigned char> data(size);
      fill_pattern(data.data(), size, 0);
      size_t done = 0;
      REQUIRE(file_write_at(h, 0, data.data(), size, done).ok());
    }
    return h;
  }

  void touch(osprim::dir_handle d, const std::string& name, size_t size = 0)
  {
    file_close(create_at(d, name, size));
  }

  file_open_result open_result_at(
      osprim::dir_handle d, const std::string& name, path_resolve resolve,
      file_disposition disposition =
          file_disposition::FILE_DISPOSITION_OPEN_EXISTING)
  {
    file_open_desc desc;
    desc.access      = file_access::FILE_ACCESS_READ_WRITE;
    desc.disposition = disposition;
    osprim::file_handle h{};
    const auto s = file_open_at(d, name.data(), name.size(), resolve, desc, h);
    if (s.ok())
      file_close(h);
    return s.result;
  }

  path_info info_ok(
      osprim::dir_handle d, const std::string& name, path_resolve resolve)
  {
    path_info info{};
    const auto s = path_info_at(d, name.data(), name.size(), resolve, info);
    INFO(
        "name: " << name << " result " << static_cast<int>(s.result) << " native "
                 << s.native_code);
    REQUIRE(s.ok());
    return info;
  }

  path_info_result info_result(
      osprim::dir_handle d, const std::string& name, path_resolve resolve)
  {
    path_info info{};
    return path_info_at(d, name.data(), name.size(), resolve, info).result;
  }

  path_remove_result remove_at(
      osprim::dir_handle d, const std::string& name, path_kind kind,
      path_resolve resolve = path_resolve::PATH_RESOLVE_NORMAL)
  {
    return path_remove_at(d, name.data(), name.size(), resolve, kind).result;
  }

  void mkdir_at(osprim::dir_handle d, const std::string& name)
  {
    const auto s = dir_create_at(
        d, name.data(), name.size(), path_resolve::PATH_RESOLVE_NORMAL);
    INFO("name: " << name << " native " << s.native_code);
    REQUIRE(s.ok());
  }

  /// @brief A symlink, created with the platform's own call (the library
  /// does not create them). False where not permitted (Windows without
  /// elevation or developer mode).
  bool make_symlink(
      const std::string& target, const std::string& link, bool directory)
  {
#if OSPRIM_OS_WINDOWS
    std::wstring t = widen(target);
    std::replace(t.begin(), t.end(), L'/', L'\\');
    const DWORD flags = (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0)
                        | 0x2; // ALLOW_UNPRIVILEGED_CREATE
    return ::CreateSymbolicLinkW(
               (L"\\\\?\\" + widen(link)).c_str(), t.c_str(), flags)
           != 0;
#else
    (void)directory;
    return ::symlink(target.c_str(), link.c_str()) == 0;
#endif
  }

  std::vector<std::pair<std::string, path_type>> list_all(
      osprim::dir_handle d, size_t buffer_size)
  {
    aligned_buffer buffer(buffer_size, 8);
    dir_reader r;
    r.buffer = buffer.data();
    r.size   = buffer.size();
    REQUIRE(dir_list_begin(d, r).ok());
    std::vector<std::pair<std::string, path_type>> out;
    for (;;)
    {
      dir_entry e{};
      const auto s = dir_list_next(d, r, e);
      if (s.result == dir_list_result::DIR_LIST_END)
        break;
      INFO("native " << s.native_code);
      REQUIRE(s.ok());
      out.emplace_back(std::string(e.name, e.name_size), e.type);
    }
    std::sort(out.begin(), out.end());
    return out;
  }
} // namespace

TEST_CASE("path: constants")
{
  CHECK(DIR_LIST_MIN_BUFFER <= DIR_LIST_RECOMMENDED_BUFFER);
#if OSPRIM_OS_WINDOWS
  CHECK(DIR_LIST_RECOMMENDED_BUFFER == 65536);
  CHECK(NO_DIRECTORY.native == nullptr);
#else
  CHECK(DIR_LIST_RECOMMENDED_BUFFER == 32768);
  CHECK(NO_DIRECTORY.native == -1);
#endif
  CHECK(sizeof(osprim::dir_handle) == sizeof(native_dir_t));
}

TEST_CASE("path: directory handles and name rules")
{
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  dir.path("sub");
  dir.path("sub" + std::string(temp_dir::sep()) + "inner");
  dir.path(
      "sub" + std::string(temp_dir::sep()) + "inner" + temp_dir::sep() + "f.bin");
  mkdir_at(root, "sub");
  mkdir_at(root, "sub/inner");
  touch(root, "sub/inner/f.bin", 10);

  // Separators, empty and "." components are normalized
  CHECK(
      info_ok(root, "sub//inner/./f.bin", path_resolve::PATH_RESOLVE_NORMAL).size
      == 10);
  CHECK(
      info_ok(root, "./sub/inner/f.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS).size
      == 10);
  CHECK(
      info_ok(root, ".", path_resolve::PATH_RESOLVE_NORMAL).type
      == path_type::PATH_TYPE_DIRECTORY);
#if OSPRIM_OS_WINDOWS
  CHECK(
      info_ok(root, "sub\\inner\\f.bin", path_resolve::PATH_RESOLVE_NORMAL).size
      == 10);
#endif

  // ".." and absolute names are refused next to a handle
  CHECK(
      info_result(root, "sub/../sub/inner", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_INVALID_PATH);
  CHECK(
      info_result(root, "..", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_INVALID_PATH);
  CHECK(
      info_result(root, "/sub", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_INVALID_PATH);
  CHECK(
      info_result(NO_DIRECTORY, "relative", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_PATH_NOT_ABSOLUTE);
  CHECK(
      info_result(root, "bad\xff", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_INVALID_PATH);
  CHECK(
      info_result(
          root, std::string(MAX_PATH_BYTES + 1, 'a'),
          path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_PATH_TOO_LONG);
#if OSPRIM_OS_WINDOWS
  // Names Win32 cannot handle are refused
  for (const char* bad :
       {"NUL", "nul.txt", "COM1", "lpt9.log", "CONOUT$", "trail.", "trail ", "a:b",
        "q?", "s*"})
    CHECK(
        open_result_at(
            root, bad, path_resolve::PATH_RESOLVE_NORMAL,
            file_disposition::FILE_DISPOSITION_CREATE_NEW)
        == file_open_result::FILE_OPEN_INVALID_PATH);
  CHECK(
      info_result(root, "C:x", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_INVALID_PATH);
#endif

  // Opening
  osprim::dir_handle sub = open_dir_ok(root, "sub");
  osprim::dir_handle inner =
      open_dir_ok(sub, "inner", path_resolve::PATH_RESOLVE_NO_SYMLINKS);
  osprim::dir_handle again = open_dir_ok(inner, ".");
  CHECK(info_ok(again, "f.bin", path_resolve::PATH_RESOLVE_NORMAL).size == 10);
  osprim::dir_handle d{};
  CHECK(
      dir_open_at(root, "missing", 7, path_resolve::PATH_RESOLVE_NORMAL, d).result
      == dir_open_result::DIR_OPEN_NOT_FOUND);
  CHECK(
      dir_open_at(inner, "f.bin", 5, path_resolve::PATH_RESOLVE_NORMAL, d).result
      == dir_open_result::DIR_OPEN_NOT_A_DIRECTORY);
  CHECK(
      dir_open_at(inner, "f.bin", 5, path_resolve::PATH_RESOLVE_NO_SYMLINKS, d)
          .result
      == dir_open_result::DIR_OPEN_NOT_A_DIRECTORY);

  // Adopt and release
  const native_dir_t n = dir_release(again);
  osprim::dir_handle adopted{};
  REQUIRE(dir_adopt(n, adopted).ok());
  dir_close(adopted);

  dir_close(inner);
  dir_close(sub);
  dir_close(root);
}

TEST_CASE("path: files relative to a directory")
{
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  dir.path("a.bin");
  osprim::file_handle h = create_at(root, "a.bin", 100);
  file_info fi{};
  REQUIRE(file_info_of(h, fi).ok());
  CHECK(fi.size == 100);

  // path_info_at sees the same file
  const path_info pi =
      info_ok(root, "a.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS);
  CHECK(pi.type == path_type::PATH_TYPE_FILE);
  CHECK(pi.size == 100);
  CHECK(pi.identity == fi.identity);
  file_close(h);

  CHECK(
      open_result_at(
          root, "a.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS,
          file_disposition::FILE_DISPOSITION_CREATE_NEW)
      == file_open_result::FILE_OPEN_ALREADY_EXISTS);
  CHECK(
      open_result_at(root, "missing.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS)
      == file_open_result::FILE_OPEN_NOT_FOUND);
  CHECK(
      open_result_at(root, ".", path_resolve::PATH_RESOLVE_NORMAL)
      == file_open_result::FILE_OPEN_INVALID_PATH);

  // NO_SYMLINKS truncates only after checking
  file_open_desc desc;
  desc.access      = file_access::FILE_ACCESS_READ_WRITE;
  desc.disposition = file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING;
  REQUIRE(
      file_open_at(root, "a.bin", 5, path_resolve::PATH_RESOLVE_NO_SYMLINKS, desc, h)
          .ok());
  REQUIRE(file_info_of(h, fi).ok());
  CHECK(fi.size == 0);
  // Reads and writes work (overlapped on Windows)
  const unsigned char x[3] = {1, 2, 3};
  size_t done              = 0;
  REQUIRE(file_write_at(h, 5, x, 3, done).ok());
  CHECK(read_all(h, 0, 8).size() == 8);
  file_close(h);

  // An absolute name with NO_DIRECTORY and NO_SYMLINKS
  const std::string abs = dir.root() + temp_dir::sep() + "a.bin";
  desc.disposition      = file_disposition::FILE_DISPOSITION_OPEN_EXISTING;
  REQUIRE(file_open_at(
              NO_DIRECTORY, abs.data(), abs.size(),
              path_resolve::PATH_RESOLVE_NORMAL, desc, h)
              .ok());
  file_close(h);
  dir_close(root);
}

TEST_CASE("path: create, remove, kinds")
{
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  dir.path("d");
  dir.path("d" + std::string(temp_dir::sep()) + "x");
  dir.path("f");

  osprim::dir_handle d{};
  REQUIRE(
      dir_create_at(root, "d", 1, path_resolve::PATH_RESOLVE_NO_SYMLINKS, &d).ok());
  touch(d, "x"); // through the handle of the new directory
  CHECK(
      dir_create_at(root, "d", 1, path_resolve::PATH_RESOLVE_NORMAL).result
      == dir_create_result::DIR_CREATE_ALREADY_EXISTS);
  CHECK(
      dir_create_at(root, "missing/d", 9, path_resolve::PATH_RESOLVE_NORMAL).result
      == dir_create_result::DIR_CREATE_NOT_FOUND);
  touch(root, "f");
  CHECK(
      dir_create_at(root, "f", 1, path_resolve::PATH_RESOLVE_NORMAL).result
      == dir_create_result::DIR_CREATE_ALREADY_EXISTS);

  CHECK(
      remove_at(root, "d", path_kind::PATH_KIND_FILE)
      == path_remove_result::PATH_REMOVE_WRONG_KIND);
  CHECK(
      remove_at(root, "f", path_kind::PATH_KIND_DIRECTORY)
      == path_remove_result::PATH_REMOVE_WRONG_KIND);
  CHECK(
      remove_at(root, "d", path_kind::PATH_KIND_DIRECTORY)
      == path_remove_result::PATH_REMOVE_NOT_EMPTY);
  CHECK(
      remove_at(root, "missing", path_kind::PATH_KIND_FILE)
      == path_remove_result::PATH_REMOVE_NOT_FOUND);
  CHECK(
      remove_at(
          d, "x", path_kind::PATH_KIND_FILE, path_resolve::PATH_RESOLVE_NO_SYMLINKS)
      == path_remove_result::PATH_REMOVE_OK);
  dir_close(d);
  CHECK(
      remove_at(root, "d", path_kind::PATH_KIND_DIRECTORY)
      == path_remove_result::PATH_REMOVE_OK);
  CHECK(
      info_result(root, "d", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_NOT_FOUND);

  // A file still open is removed at once (POSIX semantics)
  osprim::file_handle h = create_at(root, "f", 16);
  CHECK(
      remove_at(root, "f", path_kind::PATH_KIND_FILE)
      == path_remove_result::PATH_REMOVE_OK);
  bool posix_delete = true;
#if OSPRIM_OS_WINDOWS
  posix_delete = !under_wine(); // Wine keeps the name until the last handle closes
#endif
  if (posix_delete)
    CHECK(
        info_result(root, "f", path_resolve::PATH_RESOLVE_NORMAL)
        == path_info_result::PATH_INFO_NOT_FOUND);
  CHECK(read_all(h, 0, 16).size() == 16); // still readable through the handle
  char buf[16];
  size_t needed = 0;
  if (posix_delete)
    CHECK(
        path_canonical(h, buf, sizeof(buf), needed).result
        == path_canonical_result::PATH_CANONICAL_DELETED);
  file_close(h);

  // Read-only files are removed too
#if OSPRIM_OS_WINDOWS
  touch(root, "ro");
  REQUIRE(
      ::SetFileAttributesW(
          (L"\\\\?\\" + widen(dir.root() + "\\ro")).c_str(),
          FILE_ATTRIBUTE_READONLY));
#else
  touch(root, "ro");
  REQUIRE(::chmod((dir.root() + "/ro").c_str(), 0444) == 0);
#endif
  CHECK(
      remove_at(root, "ro", path_kind::PATH_KIND_FILE)
      == path_remove_result::PATH_REMOVE_OK);
  dir_close(root);
}

TEST_CASE("path: rename")
{
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  dir.path("a");
  dir.path("b");
  dir.path("c");
  dir.path("sub");
  dir.path("sub" + std::string(temp_dir::sep()) + "moved");
  touch(root, "a", 1);
  touch(root, "b", 2);

  path_rename_desc desc;
  desc.mode = path_rename_mode::PATH_RENAME_MODE_NO_REPLACE;
  CHECK(
      path_rename_at(root, "a", 1, root, "b", 1, desc).result
      == path_rename_result::PATH_RENAME_ALREADY_EXISTS);
  CHECK(
      info_ok(root, "b", path_resolve::PATH_RESOLVE_NORMAL).size == 2); // untouched
  const auto nr = path_rename_at(root, "a", 1, root, "c", 1, desc);
  if (nr.result == path_rename_result::PATH_RENAME_UNSUPPORTED)
    note("NO_REPLACE unsupported here (native %d)", nr.native_code);
  else
  {
    REQUIRE(nr.ok());
    CHECK(
        info_result(root, "a", path_resolve::PATH_RESOLVE_NORMAL)
        == path_info_result::PATH_INFO_NOT_FOUND);
    desc.mode = path_rename_mode::PATH_RENAME_MODE_REPLACE;
    REQUIRE(path_rename_at(root, "c", 1, root, "a", 1, desc).ok());
  }

  // Replace, durably, over a file that is still open (POSIX semantics on Windows)
  osprim::file_handle open_target = create_at(root, "b", 2);
#if OSPRIM_OS_WINDOWS
  if (under_wine()) // no POSIX semantics: an open target cannot be replaced
  {
    file_close(open_target);
    open_target = osprim::file_handle{};
  }
#endif
  desc.mode    = path_rename_mode::PATH_RENAME_MODE_REPLACE;
  desc.durable = true;
  const auto r = path_rename_at(root, "a", 1, root, "b", 1, desc);
  INFO("native " << r.native_code);
  CHECK(r.ok());
  CHECK(info_ok(root, "b", path_resolve::PATH_RESOLVE_NORMAL).size == 1);
#if OSPRIM_OS_WINDOWS
  if (!under_wine())
#endif
    file_close(open_target);

  // Between directories, through two handles
  mkdir_at(root, "sub");
  osprim::dir_handle sub = open_dir_ok(root, "sub");
  desc.resolve           = path_resolve::PATH_RESOLVE_NO_SYMLINKS;
  REQUIRE(path_rename_at(root, "b", 1, sub, "moved", 5, desc).ok());
  CHECK(info_ok(sub, "moved", path_resolve::PATH_RESOLVE_NORMAL).size == 1);
  CHECK(
      path_rename_at(root, "b", 1, sub, "x", 1, desc).result
      == path_rename_result::PATH_RENAME_NOT_FOUND);

  // A file onto a directory
  desc.durable = false;
  CHECK(
      path_rename_at(sub, "moved", 5, root, "sub", 3, desc).result
      != path_rename_result::PATH_RENAME_OK);
  dir_close(sub);
  dir_close(root);
}

TEST_CASE("path: symlinks")
{
#if OSPRIM_OS_WINDOWS
  if (under_wine())
  {
    note("Wine's symlinks are Unix symlinks without reparse points: skipped");
    return;
  }
#endif
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  const std::string sep   = temp_dir::sep();
  dir.path("real");
  dir.path("real" + sep + "f.bin");
  dir.path("target.bin");
  const std::string link_file = dir.path("link.bin");
  const std::string link_dir  = dir.path("linkdir");
  const std::string dangling  = dir.path("dangling");
  mkdir_at(root, "real");
  touch(root, "real/f.bin", 7);
  touch(root, "target.bin", 3);
  if (!make_symlink("target.bin", link_file, false)
      || !make_symlink("real", link_dir, true)
      || !make_symlink("nowhere", dangling, false))
  {
    note("symlinks cannot be created here: skipped");
    dir_close(root);
    return;
  }

  // Information: followed or not
  CHECK(
      info_ok(root, "link.bin", path_resolve::PATH_RESOLVE_NORMAL).type
      == path_type::PATH_TYPE_FILE);
  CHECK(info_ok(root, "link.bin", path_resolve::PATH_RESOLVE_NORMAL).size == 3);
  CHECK(
      info_ok(root, "link.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS).type
      == path_type::PATH_TYPE_SYMLINK);
  CHECK(
      info_ok(root, "linkdir", path_resolve::PATH_RESOLVE_NO_SYMLINKS).type
      == path_type::PATH_TYPE_SYMLINK);
  CHECK(
      info_result(root, "dangling", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_NOT_FOUND);
  CHECK(
      info_ok(root, "dangling", path_resolve::PATH_RESOLVE_NO_SYMLINKS).type
      == path_type::PATH_TYPE_SYMLINK);
  // A symlink before the last component
  CHECK(info_ok(root, "linkdir/f.bin", path_resolve::PATH_RESOLVE_NORMAL).size == 7);
  CHECK(
      info_result(root, "linkdir/f.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS)
      == path_info_result::PATH_INFO_SYMLINK);

  // Opening
  CHECK(
      open_result_at(root, "link.bin", path_resolve::PATH_RESOLVE_NORMAL)
      == file_open_result::FILE_OPEN_OK);
  CHECK(
      open_result_at(root, "link.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS)
      == file_open_result::FILE_OPEN_SYMLINK);
  CHECK(
      open_result_at(root, "linkdir/f.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS)
      == file_open_result::FILE_OPEN_SYMLINK);
  // Never truncates a target through a link
  CHECK(
      open_result_at(
          root, "link.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS,
          file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE)
      == file_open_result::FILE_OPEN_SYMLINK);
  CHECK(info_ok(root, "target.bin", path_resolve::PATH_RESOLVE_NORMAL).size == 3);
  osprim::dir_handle d{};
  CHECK(
      dir_open_at(root, "linkdir", 7, path_resolve::PATH_RESOLVE_NO_SYMLINKS, d)
          .result
      == dir_open_result::DIR_OPEN_SYMLINK);
  d = open_dir_ok(root, "linkdir");
  dir_close(d);

  // Reading the target
  char buf[256];
  size_t needed = 0;
  link_info info{};
  REQUIRE(path_read_link_at(
              root, "link.bin", 8, path_resolve::PATH_RESOLVE_NO_SYMLINKS, buf,
              sizeof(buf), needed, info)
              .ok());
  CHECK(std::string(buf, needed) == "target.bin");
  CHECK(info.relative);
  CHECK(
      path_read_link_at(
          root, "link.bin", 8, path_resolve::PATH_RESOLVE_NORMAL, buf, 4, needed,
          info)
          .result
      == path_read_link_result::PATH_READ_LINK_BUFFER_TOO_SMALL);
  CHECK(needed == 10);
  CHECK(
      path_read_link_at(
          root, "target.bin", 10, path_resolve::PATH_RESOLVE_NORMAL, buf,
          sizeof(buf), needed, info)
          .result
      == path_read_link_result::PATH_READ_LINK_NOT_A_LINK);
  REQUIRE(path_read_link_at(
              root, "linkdir", 7, path_resolve::PATH_RESOLVE_NORMAL, buf,
              sizeof(buf), needed, info)
              .ok());
  CHECK(std::string(buf, needed) == "real");
#if OSPRIM_OS_WINDOWS
  CHECK(info.directory);
#endif

  // Listing reports links as links
  bool saw_link = false;
  for (const auto& e : list_all(root, DIR_LIST_RECOMMENDED_BUFFER))
    if (e.first == "linkdir")
      saw_link = e.second == path_type::PATH_TYPE_SYMLINK;
  CHECK(saw_link);

  // Removing and renaming act on the link
  path_rename_desc rd;
  REQUIRE(path_rename_at(root, "link.bin", 8, root, "link2.bin", 9, rd).ok());
  dir.path("link2.bin");
  CHECK(
      info_ok(root, "link2.bin", path_resolve::PATH_RESOLVE_NO_SYMLINKS).type
      == path_type::PATH_TYPE_SYMLINK);
  CHECK(
      remove_at(root, "link2.bin", path_kind::PATH_KIND_FILE)
      == path_remove_result::PATH_REMOVE_OK);
  CHECK(
      remove_at(root, "linkdir", path_kind::PATH_KIND_FILE)
      == path_remove_result::PATH_REMOVE_OK); // a directory link: FILE
  CHECK(
      remove_at(root, "dangling", path_kind::PATH_KIND_FILE)
      == path_remove_result::PATH_REMOVE_OK);
  CHECK(
      info_ok(root, "target.bin", path_resolve::PATH_RESOLVE_NORMAL).size
      == 3); // targets untouched
  CHECK(info_ok(root, "real/f.bin", path_resolve::PATH_RESOLVE_NORMAL).size == 7);
  dir_close(root);
}

TEST_CASE("path: listing")
{
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  std::vector<std::string> expected;
  for (int i = 0; i < 300; ++i)
  {
    const std::string name =
        "entry_" + std::to_string(i) + (i % 7 == 0 ? "_caf\xc3\xa9" : "");
    dir.path(name);
    touch(root, name);
    expected.push_back(name);
  }
  dir.path("subdir");
  mkdir_at(root, "subdir");
  expected.push_back("subdir");
  std::sort(expected.begin(), expected.end());

  for (const size_t size :
       {DIR_LIST_MIN_BUFFER, size_t{4096}, DIR_LIST_RECOMMENDED_BUFFER})
  {
    const auto entries = list_all(root, size);
    REQUIRE(entries.size() == expected.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
      CHECK(entries[i].first == expected[i]);
      const path_type want = entries[i].first == "subdir"
                                 ? path_type::PATH_TYPE_DIRECTORY
                                 : path_type::PATH_TYPE_FILE;
      CHECK(
          (entries[i].second == want
           || entries[i].second == path_type::PATH_TYPE_UNKNOWN));
    }
  }

  // Restart in the middle
  aligned_buffer buffer(DIR_LIST_MIN_BUFFER, 8);
  dir_reader r;
  r.buffer = buffer.data();
  r.size   = buffer.size();
  REQUIRE(dir_list_begin(root, r).ok());
  dir_entry e{};
  for (int i = 0; i < 10; ++i)
    REQUIRE(dir_list_next(root, r, e).ok());
  REQUIRE(dir_list_begin(root, r).ok());
  size_t count = 0;
  while (dir_list_next(root, r, e).ok())
    ++count;
  CHECK(count == expected.size());

  // An empty directory
  osprim::dir_handle sub = open_dir_ok(root, "subdir");
  CHECK(list_all(sub, DIR_LIST_MIN_BUFFER).empty());
  dir_close(sub);
  dir_close(root);
}

TEST_CASE("path: canonical paths and the current directory")
{
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  dir.path("Canon.bin");
  osprim::file_handle h = create_at(root, "Canon.bin");

  char buf[MAX_PATH_BYTES + 1];
  size_t needed = 0;
  REQUIRE(path_canonical(h, buf, sizeof(buf), needed).ok());
  const std::string file_path(buf, needed);
  REQUIRE(path_canonical(root, buf, sizeof(buf), needed).ok());
  const std::string dir_path(buf, needed);
  INFO("file " << file_path << " dir " << dir_path);
  CHECK(file_path.size() > dir_path.size());
  CHECK(file_path.compare(0, dir_path.size(), dir_path) == 0);
  CHECK(file_path.substr(file_path.size() - 9) == "Canon.bin");
#if OSPRIM_OS_WINDOWS
  CHECK(file_path.rfind("\\\\?\\", 0) == std::string::npos); // the plain form
#endif
  CHECK(
      path_canonical(h, buf, 3, needed).result
      == path_canonical_result::PATH_CANONICAL_BUFFER_TOO_SMALL);
  CHECK(needed == file_path.size());
  // The canonical path opens the same file
  osprim::file_handle again = open_ok(file_path, file_access::FILE_ACCESS_READ);
  file_info a{};
  file_info b{};
  REQUIRE(file_info_of(h, a).ok());
  REQUIRE(file_info_of(again, b).ok());
  CHECK(a.identity == b.identity);
  file_close(again);
  file_close(h);

  // The current directory, restored afterwards
  char old[MAX_PATH_BYTES + 1];
  size_t old_size = 0;
  REQUIRE(current_directory_get(old, sizeof(old), old_size).ok());
  REQUIRE(current_directory_set(dir_path.data(), dir_path.size()).ok());
  REQUIRE(current_directory_get(buf, sizeof(buf), needed).ok());
  CHECK(std::string(buf, needed) == dir_path);
  CHECK(
      current_directory_get(buf, 2, needed).result
      == current_directory_get_result::CURRENT_DIRECTORY_GET_BUFFER_TOO_SMALL);
  CHECK(
      current_directory_set("relative", 8).result
      == current_directory_set_result::CURRENT_DIRECTORY_SET_PATH_NOT_ABSOLUTE);
  REQUIRE(current_directory_set(old, old_size).ok());
  dir_close(root);
}

TEST_CASE("path: a handle stays on its directory (TOCTOU)")
{
  session s;
  temp_dir dir;
  osprim::dir_handle root = open_dir_ok(NO_DIRECTORY, dir.root());
  const std::string sep   = temp_dir::sep();
  dir.path("before");
  dir.path("after");
  dir.path("after" + sep + "f.bin");
  mkdir_at(root, "before");
  osprim::dir_handle d = open_dir_ok(root, "before");

  // The directory is renamed under the handle: operations follow the directory, not the name
  path_rename_desc rd;
  const auto r = path_rename_at(root, "before", 6, root, "after", 5, rd);
  INFO("native " << r.native_code);
  REQUIRE(r.ok());
  touch(d, "f.bin", 4);
  CHECK(info_ok(root, "after/f.bin", path_resolve::PATH_RESOLVE_NORMAL).size == 4);
  CHECK(
      info_result(root, "before", path_resolve::PATH_RESOLVE_NORMAL)
      == path_info_result::PATH_INFO_NOT_FOUND);
  dir_close(d);
  dir_close(root);
}

TEST_CASE("path: tracking")
{
  if (!tracking_compiled)
    return;
  temp_dir dir;
  session s;
  osprim::dir_handle d      = open_dir_ok(NO_DIRECTORY, dir.root());
  const diagnostic_log& log = s.end();
  REQUIRE(log.count == 1);
  CHECK(log.entries[0].kind == diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK);
  CHECK(::strcmp(log.entries[0].handle_type, "dir") == 0);
  dir_close(d);
}
