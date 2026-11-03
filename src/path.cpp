/*****************************************************************/ /**
 * @file   path.cpp
 * @brief  Paths and directories: name checks and normalization, tracking.
 * The OS calls live in path_posix.cpp and path_windows.cpp.
 *
 * Relative names (with a directory handle) are normalized here, the same
 * way on every platform: components separated by '/' (and '\' on
 * Windows), empty and "." components dropped, ".." refused, and on
 * Windows the names Win32 cannot handle refused. Absolute names (with
 * NO_DIRECTORY) are passed as given; the platform applies the rules of
 * file_open().
 *********************************************************************/
#include "internal.hpp"

#include <string.h>

#include <type_traits>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      static_assert(
          std::is_same_v<native_dir_t, native_file_t>,
          "path_canonical() passes directory handles as file handles");

      enum class name_status : uint8_t
      {
        OK,
        INVALID,
        NOT_ABSOLUTE,
        TOO_LONG,
      };

      /// @brief A checked name, with its storage.
      struct checked_name
      {
        char text[MAX_PATH_BYTES + 1];
        detail::path_ref ref;
      };

      bool is_no_directory(dir_handle d) noexcept
      {
        return d.native == NO_DIRECTORY.native;
      }

      bool is_separator(char c) noexcept
      {
#if OSPRIM_OS_WINDOWS
        return c == '/' || c == '\\';
#else
        return c == '/';
#endif
      }

#if OSPRIM_OS_WINDOWS
      char ascii_upper(char c) noexcept
      {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
      }

      bool equals_upper(const char* p, size_t n, const char* upper) noexcept
      {
        size_t i = 0;
        for (; i < n && upper[i] != '\0'; ++i)
          if (ascii_upper(p[i]) != upper[i])
            return false;
        return i == n && upper[i] == '\0';
      }

    } // namespace

    namespace detail
    {
      bool windows_component_refused(const char* c, size_t n) noexcept
      {
        if (c[n - 1] == '.' || c[n - 1] == ' ')
          return true;
        for (size_t i = 0; i < n; ++i)
        {
          const unsigned char ch = static_cast<unsigned char>(c[i]);
          if (ch < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"'
              || ch == '|' || ch == '?' || ch == '*')
            return true;
        }
        // Device names, alone or with any extension (Windows 10 rules)
        size_t base = 0;
        while (base < n && c[base] != '.')
          ++base;
        while (base > 0 && c[base - 1] == ' ')
          --base;
        static const char* const devices[] = {"CON", "PRN",    "AUX",
                                              "NUL", "CONIN$", "CONOUT$"};
        for (const char* d : devices)
          if (equals_upper(c, base, d))
            return true;
        if ((base == 4 || base == 5)
            && (equals_upper(c, 3, "COM") || equals_upper(c, 3, "LPT")))
        {
          // COM1-COM9, LPT1-LPT9, and the superscripts 1, 2, 3 (UTF-8 C2 B9/B2/B3)
          if (base == 4 && c[3] >= '1' && c[3] <= '9')
            return true;
          if (base == 5 && static_cast<unsigned char>(c[3]) == 0xc2
              && (static_cast<unsigned char>(c[4]) == 0xb9
                  || static_cast<unsigned char>(c[4]) == 0xb2
                  || static_cast<unsigned char>(c[4]) == 0xb3))
            return true;
        }
        return false;
      }
    } // namespace detail

    namespace
    {
      using detail::windows_component_refused;
#endif

      /// @brief Checks and normalizes a name. `allow_self`: an empty result
      /// (the directory itself, as ".") is accepted.
      name_status check_name(
          dir_handle dir, const char* name, size_t size, bool allow_self,
          checked_name* out) noexcept
      {
        if (name == nullptr || size == 0)
          return name_status::INVALID;
        if (size > MAX_PATH_BYTES)
          return name_status::TOO_LONG;
        if (!detail::utf8_valid(name, size))
          return name_status::INVALID;

        out->ref.dir = dir.native;
        if (is_no_directory(dir))
        {
#if !OSPRIM_OS_WINDOWS
          if (name[0] != '/')
            return name_status::NOT_ABSOLUTE;
          while (size > 1 && name[size - 1] == '/') // trailing separators
            --size;
#endif
          ::memcpy(out->text, name, size);
          out->text[size]   = '\0';
          out->ref.name     = out->text;
          out->ref.size     = size;
          out->ref.absolute = true;
          return name_status::OK;
        }

        // Relative to a handle: no root, no drive
        if (is_separator(name[0]))
          return name_status::INVALID;
#if OSPRIM_OS_WINDOWS
        if (size >= 2 && name[1] == ':')
          return name_status::INVALID;
#endif
        size_t written = 0;
        size_t i       = 0;
        while (i < size)
        {
          size_t j = i;
          while (j < size && !is_separator(name[j]))
            ++j;
          const size_t n = j - i;
          if (n == 2 && name[i] == '.' && name[i + 1] == '.')
            return name_status::INVALID;
          if (n != 0 && !(n == 1 && name[i] == '.'))
          {
#if OSPRIM_OS_WINDOWS
            if (windows_component_refused(name + i, n))
              return name_status::INVALID;
#endif
            if (written != 0)
              out->text[written++] = '/';
            ::memcpy(out->text + written, name + i, n);
            written += n;
          }
          i = j + 1;
        }
        if (written == 0 && !allow_self)
          return name_status::INVALID;
        out->text[written] = '\0';
        out->ref.name      = out->text;
        out->ref.size      = written;
        out->ref.absolute  = false;
        return name_status::OK;
      }

      /// @brief The result of a name check, in the operation's own enum.
      template<class R>
      status<R> name_error(
          name_status st, R invalid, R not_absolute, R too_long) noexcept
      {
        const R r = st == name_status::TOO_LONG       ? too_long
                    : st == name_status::NOT_ABSOLUTE ? not_absolute
                                                      : invalid;
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      /// @brief Copies `n` bytes to a caller buffer, or reports the size needed.
      bool fits(size_t n, size_t capacity, size_t& needed) noexcept
      {
        needed = n;
        return n <= capacity;
      }
    } // namespace

    /*****************************/
    /* DIRECTORY HANDLES         */
    /*****************************/

    status<dir_open_result> dir_open_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        dir_handle& out, std::source_location loc) noexcept
    {
      using R = dir_open_result;
      checked_name n;
      const name_status st = check_name(dir, name, name_size, true, &n);
      if (st != name_status::OK)
        return name_error(
            st, R::DIR_OPEN_INVALID_PATH, R::DIR_OPEN_PATH_NOT_ABSOLUTE,
            R::DIR_OPEN_PATH_TOO_LONG);
      native_dir_t native{};
      const status<R> s = detail::platform_dir_open(n.ref, resolve, &native);
      if (!s.ok())
        return s;
      out.native = native;
      detail::track_open(detail::DIR_TRACK_TYPE, detail::dir_key(native), loc);
      return s;
    }

    void dir_close(dir_handle d, std::source_location loc) noexcept
    {
      detail::track_close(detail::DIR_TRACK_TYPE, detail::dir_key(d.native), loc);
      detail::platform_dir_close(d.native);
    }

    status<dir_adopt_result> dir_adopt(
        native_dir_t native, dir_handle& out, std::source_location loc) noexcept
    {
      const status<dir_adopt_result> s = detail::platform_dir_adopt(native);
      if (!s.ok())
        return s;
      out.native = native;
      detail::track_open(detail::DIR_TRACK_TYPE, detail::dir_key(native), loc);
      return s;
    }

    native_dir_t dir_release(dir_handle d, std::source_location loc) noexcept
    {
      detail::track_close(detail::DIR_TRACK_TYPE, detail::dir_key(d.native), loc);
      return d.native;
    }

    /*****************************/
    /* FILES                     */
    /*****************************/

    status<file_open_result> file_open_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        const file_open_desc& desc, file_handle& out,
        std::source_location loc) noexcept
    {
      using R = file_open_result;
      if (is_no_directory(dir) && resolve == path_resolve::PATH_RESOLVE_NORMAL)
        return file_open(name, name_size, desc, out, loc);
      OSPRIM_ASSERT(
          desc.access != file_access::FILE_ACCESS_READ
              || (desc.disposition
                      != file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE
                  && desc.disposition
                         != file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING),
          "osprim::file_open_at: truncating needs write access");
      checked_name n;
      const name_status st = check_name(dir, name, name_size, false, &n);
      if (st != name_status::OK)
        return name_error(
            st, R::FILE_OPEN_INVALID_PATH, R::FILE_OPEN_PATH_NOT_ABSOLUTE,
            R::FILE_OPEN_PATH_TOO_LONG);
      native_file_t native{};
      const status<R> s =
          detail::platform_file_open_at(n.ref, resolve, desc, &native);
      if (!s.ok())
        return s;
      out.native = native;
      detail::file_track_open(native, desc, loc);
      return s;
    }

    /*****************************/
    /* ENTRY INFORMATION         */
    /*****************************/

    status<path_info_result> path_info_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        path_info& out) noexcept
    {
      using R = path_info_result;
      checked_name n;
      const name_status st = check_name(dir, name, name_size, true, &n);
      if (st != name_status::OK)
        return name_error(
            st, R::PATH_INFO_INVALID_PATH, R::PATH_INFO_PATH_NOT_ABSOLUTE,
            R::PATH_INFO_PATH_TOO_LONG);
      return detail::platform_path_info(n.ref, resolve, &out);
    }

    /*****************************/
    /* CREATE, REMOVE, RENAME    */
    /*****************************/

    status<dir_create_result> dir_create_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        dir_handle* out, std::source_location loc) noexcept
    {
      using R = dir_create_result;
      checked_name n;
      const name_status st = check_name(dir, name, name_size, false, &n);
      if (st != name_status::OK)
        return name_error(
            st, R::DIR_CREATE_INVALID_PATH, R::DIR_CREATE_PATH_NOT_ABSOLUTE,
            R::DIR_CREATE_PATH_TOO_LONG);
      native_dir_t native{};
      const status<R> s = detail::platform_dir_create(
          n.ref, resolve, out != nullptr ? &native : nullptr);
      if (s.ok() && out != nullptr)
      {
        out->native = native;
        detail::track_open(detail::DIR_TRACK_TYPE, detail::dir_key(native), loc);
      }
      return s;
    }

    status<path_remove_result> path_remove_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        path_kind kind) noexcept
    {
      using R = path_remove_result;
      checked_name n;
      const name_status st = check_name(dir, name, name_size, false, &n);
      if (st != name_status::OK)
        return name_error(
            st, R::PATH_REMOVE_INVALID_PATH, R::PATH_REMOVE_PATH_NOT_ABSOLUTE,
            R::PATH_REMOVE_PATH_TOO_LONG);
      return detail::platform_path_remove(n.ref, resolve, kind);
    }

    status<path_rename_result> path_rename_at(
        dir_handle from_dir, const char* from, size_t from_size, dir_handle to_dir,
        const char* to, size_t to_size, const path_rename_desc& desc) noexcept
    {
      using R = path_rename_result;
      checked_name a;
      checked_name b;
      name_status st = check_name(from_dir, from, from_size, false, &a);
      if (st == name_status::OK)
        st = check_name(to_dir, to, to_size, false, &b);
      if (st != name_status::OK)
        return name_error(
            st, R::PATH_RENAME_INVALID_PATH, R::PATH_RENAME_PATH_NOT_ABSOLUTE,
            R::PATH_RENAME_PATH_TOO_LONG);
      return detail::platform_path_rename(a.ref, b.ref, desc);
    }

    /*****************************/
    /* SYMLINKS                  */
    /*****************************/

    status<path_read_link_result> path_read_link_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        char* buffer, size_t capacity, size_t& needed, link_info& info) noexcept
    {
      using R = path_read_link_result;
      needed  = 0;
      info    = link_info{};
      checked_name n;
      const name_status st = check_name(dir, name, name_size, false, &n);
      if (st != name_status::OK)
        return name_error(
            st, R::PATH_READ_LINK_INVALID_PATH, R::PATH_READ_LINK_PATH_NOT_ABSOLUTE,
            R::PATH_READ_LINK_PATH_TOO_LONG);
      return detail::platform_path_read_link(
          n.ref, resolve, buffer, capacity, &needed, &info);
    }

    /*****************************/
    /* LISTING                   */
    /*****************************/

    status<dir_list_result> dir_list_begin(dir_handle d, dir_reader& r) noexcept
    {
      OSPRIM_ASSERT(
          r.buffer != nullptr && r.size >= DIR_LIST_MIN_BUFFER,
          "osprim::dir_list_begin: buffer below DIR_LIST_MIN_BUFFER");
      OSPRIM_ASSERT(
          reinterpret_cast<uintptr_t>(r.buffer) % 8 == 0,
          "osprim::dir_list_begin: buffer not 8-byte aligned");
      r.internal_offset = 0;
      r.internal_end    = 0;
      r.internal_flags  = 0;
      return detail::platform_dir_list_begin(d.native, r);
    }

    status<dir_list_result> dir_list_next(
        dir_handle d, dir_reader& r, dir_entry& out) noexcept
    {
      return detail::platform_dir_list_next(d.native, r, &out);
    }

    /*****************************/
    /* CANONICAL PATHS           */
    /*****************************/

    status<path_canonical_result> path_canonical(
        file_handle h, char* buffer, size_t capacity, size_t& needed) noexcept
    {
      needed = 0;
      return detail::platform_canonical(h.native, buffer, capacity, &needed);
    }

    status<path_canonical_result> path_canonical(
        dir_handle d, char* buffer, size_t capacity, size_t& needed) noexcept
    {
      needed = 0;
      return detail::platform_canonical(d.native, buffer, capacity, &needed);
    }

    /*****************************/
    /* CURRENT DIRECTORY         */
    /*****************************/

    status<current_directory_get_result> current_directory_get(
        char* buffer, size_t capacity, size_t& needed) noexcept
    {
      needed = 0;
      return detail::platform_current_directory_get(buffer, capacity, &needed);
    }

    status<current_directory_set_result> current_directory_set(
        const char* path, size_t path_size) noexcept
    {
      using R = current_directory_set_result;
      checked_name n;
      const name_status st = check_name(NO_DIRECTORY, path, path_size, false, &n);
      if (st != name_status::OK)
        return name_error(
            st, R::CURRENT_DIRECTORY_SET_INVALID_PATH,
            R::CURRENT_DIRECTORY_SET_PATH_NOT_ABSOLUTE,
            R::CURRENT_DIRECTORY_SET_PATH_TOO_LONG);
      return detail::platform_current_directory_set(n.ref.name, n.ref.size);
    }

    namespace detail
    {
      bool copy_out(
          const char* src, size_t n, char* buffer, size_t capacity,
          size_t* needed) noexcept
      {
        if (!fits(n, capacity, *needed))
          return false;
        if (n != 0)
          ::memcpy(buffer, src, n);
        return true;
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
