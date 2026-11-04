/*****************************************************************/ /**
 * @file   path_windows.cpp
 * @brief  Paths and directories on Windows.
 *
 * - Win32 has no *at functions: relative opens use NtCreateFile with a
 *   RootDirectory; absolute names become \??\ NT paths. Rename and delete
 *   go through the opened handle (SetFileInformationByHandle).
 * - NO_SYMLINKS walks the name one component at a time, opening each with
 *   FILE_OPEN_REPARSE_POINT. Name surrogates (symlinks, junctions, WSL
 *   symlinks) are refused; other reparse points (cloud placeholders,
 *   deduplicated and compressed files) are reopened normally and checked
 *   to be the same file, so a swap in between is detected. Files are never
 *   truncated by the open itself, only after that check.
 * - Directory handles are synchronous, with full sharing.
 * - Removal: FileDispositionInfoEx with POSIX semantics and ignoring the
 *   read-only attribute, falling back to the older classes.
 * - Rename: FileRenameInfoEx (POSIX semantics, replace flag), falling back
 *   to FileRenameInfo. Durable: flush of the target directory.
 * - Listing: GetFileInformationByHandleEx with FileIdExtdDirectoryInfo,
 *   falling back to FileFullDirectoryInfo then FileIdBothDirectoryInfo,
 *   at most 64 KiB per call.
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "path_windows.cpp is for Windows only"
#endif

#include "nt.hpp"
#include "win32.hpp"

#include <winioctl.h>

#include <stddef.h>
#include <string.h>
#include <wchar.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      using namespace detail;

      template<class R>
      status<R> fail_nt(R r, LONG st) noexcept
      {
        return {
            r, native_error_kind::NATIVE_ERROR_NT_STATUS, static_cast<int32_t>(st)};
      }

      template<class R>
      status<R> fail_win(R r, DWORD e) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_WIN32, static_cast<int32_t>(e)};
      }

      template<class R>
      status<R> fail(R r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      bool nofollow(path_resolve r) noexcept
      {
        return r == path_resolve::PATH_RESOLVE_NO_SYMLINKS;
      }

      FILE_INFO_BY_HANDLE_CLASS info_class(int c) noexcept
      {
        return static_cast<FILE_INFO_BY_HANDLE_CLASS>(c);
      }

      // FILE_INFO_BY_HANDLE_CLASS values (not every SDK defines the newer ones)
      constexpr int CLASS_BASIC               = 0;
      constexpr int CLASS_STANDARD            = 1;
      constexpr int CLASS_DISPOSITION         = 4;
      constexpr int CLASS_ATTRIBUTE_TAG       = 9;
      constexpr int CLASS_ID_BOTH_DIR         = 10;
      constexpr int CLASS_ID_BOTH_DIR_RESTART = 11;
      constexpr int CLASS_FULL_DIR            = 14;
      constexpr int CLASS_FULL_DIR_RESTART    = 15;
      constexpr int CLASS_ID                  = 18;
      constexpr int CLASS_ID_EXTD_DIR         = 19;
      constexpr int CLASS_ID_EXTD_DIR_RESTART = 20;
      constexpr int CLASS_DISPOSITION_EX      = 21;

      constexpr DWORD DISPOSITION_DELETE          = 0x1;
      constexpr DWORD DISPOSITION_POSIX           = 0x2;
      constexpr DWORD DISPOSITION_IGNORE_READONLY = 0x10;
      constexpr DWORD RENAME_REPLACE_IF_EXISTS    = 0x1;
      constexpr DWORD RENAME_POSIX_SEMANTICS      = 0x2;

      constexpr DWORD TAG_MOUNT_POINT = 0xA0000003UL;
      constexpr DWORD TAG_SYMLINK     = 0xA000000CUL;
      constexpr DWORD TAG_LX_SYMLINK  = 0xA000001DUL;

      bool name_surrogate(DWORD tag) noexcept
      {
        return (tag & 0x20000000UL) != 0;
      }

      constexpr ACCESS_MASK DIR_ACCESS =
          FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
      constexpr ACCESS_MASK WALK_ACCESS =
          FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
      constexpr ULONG SHARE_ALL =
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

      bool nt_ok(LONG st) noexcept
      {
        return st >= 0;
      }

      /// @brief NtCreateFile relative to `root` (null: `name` is an NT path).
      LONG nt_open(
          HANDLE root, const wchar_t* name, size_t units, ACCESS_MASK access,
          ULONG disposition, ULONG options, HANDLE* out,
          ULONG_PTR* info = nullptr) noexcept
      {
        if (g_nt.create_file == nullptr)
          return NT_STATUS_NOT_SUPPORTED;
        nt_unicode_string us{};
        us.Length        = static_cast<USHORT>(units * sizeof(wchar_t));
        us.MaximumLength = us.Length;
        us.Buffer        = const_cast<PWSTR>(name);
        nt_object_attributes oa{};
        oa.Length        = sizeof(oa);
        oa.RootDirectory = root;
        oa.ObjectName    = &us;
        oa.Attributes    = NT_OBJ_CASE_INSENSITIVE; // the Win32 default
        nt_io_status_block iosb{};
        *out          = nullptr;
        const LONG st = g_nt.create_file(
            out, access, &oa, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, SHARE_ALL,
            disposition, options, nullptr, 0);
        if (info != nullptr)
          *info = iosb.Information;
        if (!nt_ok(st))
          *out = nullptr;
        return st;
      }

      bool attribute_tag(HANDLE h, DWORD* attributes, DWORD* tag) noexcept
      {
        FILE_ATTRIBUTE_TAG_INFO t{};
        if (!::GetFileInformationByHandleEx(
                h, info_class(CLASS_ATTRIBUTE_TAG), &t, sizeof(t)))
          return false;
        *attributes = t.FileAttributes;
        *tag = (t.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? t.ReparseTag
                                                                      : 0;
        return true;
      }

      /// @brief Volume and file id, for "is it still the same file?".
      bool identity_of(HANDLE h, file_identity* out) noexcept
      {
        FILE_ID_INFO id{};
        if (::GetFileInformationByHandleEx(h, info_class(CLASS_ID), &id, sizeof(id)))
        {
          uint64_t lo = 0;
          uint64_t hi = 0;
          ::memcpy(&lo, id.FileId.Identifier, 8);
          ::memcpy(&hi, id.FileId.Identifier + 8, 8);
          *out = file_identity{id.VolumeSerialNumber, lo, hi};
          return true;
        }
        BY_HANDLE_FILE_INFORMATION bhfi{};
        if (!::GetFileInformationByHandle(h, &bhfi))
          return false;
        *out = file_identity{
            bhfi.dwVolumeSerialNumber,
            (static_cast<uint64_t>(bhfi.nFileIndexHigh) << 32) | bhfi.nFileIndexLow,
            0};
        return true;
      }

      /// @brief Reads a reparse point (handle opened with FILE_OPEN_REPARSE_POINT).
      /// @return the reparse data size, 0 on failure (GetLastError())
      DWORD read_reparse(HANDLE h, unsigned char* buf, DWORD size) noexcept
      {
        DWORD got = 0;
        if (!::DeviceIoControl(
                h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf, size, &got, nullptr))
          return 0;
        return got;
      }

      /// @brief A MOUNT_POINT reparse point is a volume mount point (not a junction).
      bool is_volume_mount(HANDLE h) noexcept
      {
        alignas(8) unsigned char buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
        const DWORD got = read_reparse(h, buf, sizeof(buf));
        if (got < 16)
          return false;
        USHORT off = 0;
        USHORT len = 0;
        ::memcpy(&off, buf + 8, 2);
        ::memcpy(&len, buf + 10, 2);
        const wchar_t* sub = reinterpret_cast<const wchar_t*>(buf + 16 + off);
        static constexpr wchar_t volume[] = L"\\??\\Volume{";
        const size_t n                    = sizeof(volume) / sizeof(wchar_t) - 1;
        if (len / sizeof(wchar_t) < n)
          return false;
        for (size_t i = 0; i < n; ++i)
          if (sub[i] != volume[i])
            return false;
        return true;
      }

      /// @brief A name in UTF-16 for NtCreateFile, with its root.
      struct wide_name
      {
        wchar_t text[MAX_PATH_BYTES + 16];
        size_t size;
        HANDLE root;
        /// @brief Units of the volume prefix of an absolute name (\??\C:\ or \??\UNC\server\share\)
        size_t prefix;
      };

      enum class conv : uint8_t
      {
        OK,
        INVALID,
        NOT_ABSOLUTE,
        TOO_LONG,
        DEVICE,
      };

      conv to_wide(const path_ref& p, wide_name* out) noexcept
      {
        out->size   = 0;
        out->root   = nullptr;
        out->prefix = 0;
        if (p.absolute)
        {
          bool verbatim                    = false;
          const status<file_open_result> s = windows_file_path(
              p.name, p.size, out->text, MAX_PATH_BYTES + 16, &verbatim);
          switch (s.result)
          {
          case file_open_result::FILE_OPEN_OK:
            break;
          case file_open_result::FILE_OPEN_PATH_NOT_ABSOLUTE:
            return conv::NOT_ABSOLUTE;
          case file_open_result::FILE_OPEN_PATH_TOO_LONG:
            return conv::TOO_LONG;
          case file_open_result::FILE_OPEN_NOT_A_FILE:
            return conv::DEVICE;
          default:
            return conv::INVALID;
          }
          out->text[1] = L'?'; // \\?\ -> \??\ (the NT form)
          out->size    = ::wcslen(out->text);
          // Volume prefix: \??\C:\ or \??\UNC\server\share\ (minus the final '\' if absent)
          size_t i = 4;
          if (out->size >= 8 && ::wcsncmp(out->text + 4, L"UNC\\", 4) == 0)
          {
            i = 8;
            for (int seps = 0; i < out->size && seps < 2; ++i)
              if (out->text[i] == L'\\')
                ++seps;
          }
          else
          {
            while (i < out->size && out->text[i] != L'\\')
              ++i;
            if (i < out->size)
              ++i;
          }
          out->prefix = i;
          return conv::OK;
        }
        out->root = p.dir;
        if (p.size == 0)
          return conv::OK;
        const int n = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, p.name, static_cast<int>(p.size),
            out->text, static_cast<int>(MAX_PATH_BYTES));
        if (n <= 0)
          return conv::INVALID;
        for (int i = 0; i < n; ++i)
          if (out->text[i] == L'/')
            out->text[i] = L'\\';
        out->text[n] = L'\0';
        out->size    = static_cast<size_t>(n);
        return conv::OK;
      }

      template<class R>
      status<R> conv_error(conv c, R invalid, R not_absolute, R too_long) noexcept
      {
        return fail(
            c == conv::NOT_ABSOLUTE ? not_absolute
            : c == conv::TOO_LONG   ? too_long
                                    : invalid);
      }

      /// @brief The parent of the last component, and that component.
      struct win_parent
      {
        HANDLE handle       = nullptr;
        bool owned          = false;
        const wchar_t* last = L"";
        size_t last_size    = 0;

        win_parent() noexcept                    = default;
        win_parent(const win_parent&)            = delete;
        win_parent& operator=(const win_parent&) = delete;
        ~win_parent()
        {
          if (owned && handle != nullptr)
            ::CloseHandle(handle);
        }
      };

      /// @brief Opens component `name` of `parent` as a directory or file,
      /// without following a name surrogate. Other reparse points are
      /// reopened normally and checked to be the same file.
      /// @return NTSTATUS; NT_STATUS_STOPPED_ON_SYMLINK for a name surrogate
      LONG open_nofollow(
          HANDLE parent, const wchar_t* name, size_t units, ACCESS_MASK access,
          ULONG disposition, ULONG options, HANDLE* out,
          ULONG_PTR* info = nullptr) noexcept
      {
        HANDLE h = nullptr;
        LONG st  = nt_open(
            parent, name, units, access | FILE_READ_ATTRIBUTES, disposition,
            options | NT_FILE_OPEN_REPARSE_POINT, &h, info);
        if (!nt_ok(st))
          return st;
        DWORD attributes = 0;
        DWORD tag        = 0;
        if (!attribute_tag(h, &attributes, &tag))
        {
          ::CloseHandle(h);
          return NT_STATUS_ACCESS_DENIED;
        }
        if (tag == 0)
        {
          *out = h;
          return st;
        }
        if (name_surrogate(tag) && !(tag == TAG_MOUNT_POINT && is_volume_mount(h)))
        {
          ::CloseHandle(h);
          return NT_STATUS_STOPPED_ON_SYMLINK;
        }
        // A reparse point that is not a link (or a volume mount point): reopen it normally
        HANDLE again = nullptr;
        st           = nt_open(
            parent, name, units, access | FILE_READ_ATTRIBUTES, NT_FILE_OPEN,
            options, &again);
        if (!nt_ok(st))
        {
          ::CloseHandle(h);
          return st;
        }
        file_identity a{};
        file_identity b{};
        const bool same = identity_of(h, &a) && identity_of(again, &b) && a == b;
        ::CloseHandle(h);
        if (!same)
        {
          ::CloseHandle(again);
          return NT_STATUS_STOPPED_ON_SYMLINK; // swapped between the two opens
        }
        *out = again;
        return st;
      }

      /// @brief Resolves the parent of the last component of `w`.
      LONG resolve_parent(
          const wide_name& w, path_resolve resolve, win_parent* out) noexcept
      {
        size_t last = w.size;
        while (last > w.prefix && w.text[last - 1] != L'\\')
          --last;
        if (!nofollow(resolve) || (w.root != nullptr && last == 0))
        {
          // NORMAL: the system resolves the whole name. One component: the parent is `dir`.
          out->handle    = w.root;
          out->last      = w.text;
          out->last_size = w.size;
          return NT_STATUS_SUCCESS;
        }
        // NO_SYMLINKS: walk from the root (or the volume root of an absolute name)
        HANDLE cur   = w.root;
        bool owned   = false;
        size_t start = 0;
        if (w.root == nullptr)
        {
          const LONG st = nt_open(
              nullptr, w.text, w.prefix, WALK_ACCESS, NT_FILE_OPEN,
              NT_FILE_DIRECTORY_FILE | NT_FILE_SYNCHRONOUS_IO_NONALERT, &cur);
          if (!nt_ok(st))
            return st;
          owned = true;
          start = w.prefix;
        }
        size_t i = start;
        while (i < last)
        {
          size_t j = i;
          while (j < last && w.text[j] != L'\\')
            ++j;
          if (j > i)
          {
            HANDLE next   = nullptr;
            const LONG st = open_nofollow(
                cur, w.text + i, j - i, WALK_ACCESS, NT_FILE_OPEN,
                NT_FILE_DIRECTORY_FILE | NT_FILE_SYNCHRONOUS_IO_NONALERT, &next);
            if (owned)
              ::CloseHandle(cur);
            if (!nt_ok(st) || st == NT_STATUS_STOPPED_ON_SYMLINK)
              return st;
            cur   = next;
            owned = true;
          }
          i = j + 1;
        }
        out->handle    = cur;
        out->owned     = owned;
        out->last      = w.text + last;
        out->last_size = w.size - last;
        return NT_STATUS_SUCCESS;
      }

      /// @brief Broad classes of NTSTATUS and Win32 errors.
      enum class err : uint8_t
      {
        NOT_FOUND,
        EXISTS,
        ACCESS,
        NOT_DIR,
        IS_DIR,
        BUSY,
        INVALID,
        TOO_LONG,
        NOT_EMPTY,
        NO_SPACE,
        TOO_MANY,
        CROSS,
        SYMLINK,
        UNSUPPORTED,
        OTHER,
      };

      err classify_nt(LONG st) noexcept
      {
        switch (st)
        {
        case NT_STATUS_OBJECT_NAME_NOT_FOUND:
        case NT_STATUS_OBJECT_PATH_NOT_FOUND:
        case NT_STATUS_NO_SUCH_FILE:
        case NT_STATUS_DELETE_PENDING:
        case NT_STATUS_REPARSE_POINT_NOT_RESOLVED:
          return err::NOT_FOUND;
        case NT_STATUS_OBJECT_NAME_COLLISION:
          return err::EXISTS;
        case NT_STATUS_ACCESS_DENIED:
        case NT_STATUS_CANNOT_DELETE:
        case NT_STATUS_MEDIA_WRITE_PROTECTED:
          return err::ACCESS;
        case NT_STATUS_NOT_A_DIRECTORY:
          return err::NOT_DIR;
        case NT_STATUS_FILE_IS_A_DIRECTORY:
          return err::IS_DIR;
        case NT_STATUS_SHARING_VIOLATION:
          return err::BUSY;
        case NT_STATUS_OBJECT_NAME_INVALID:
        case NT_STATUS_OBJECT_PATH_INVALID:
        case NT_STATUS_OBJECT_PATH_SYNTAX_BAD:
          return err::INVALID;
        case NT_STATUS_NAME_TOO_LONG:
          return err::TOO_LONG;
        case NT_STATUS_DIRECTORY_NOT_EMPTY:
          return err::NOT_EMPTY;
        case NT_STATUS_DISK_FULL:
          return err::NO_SPACE;
        case NT_STATUS_TOO_MANY_OPENED_FILES:
          return err::TOO_MANY;
        case NT_STATUS_NOT_SAME_DEVICE:
          return err::CROSS;
        case NT_STATUS_STOPPED_ON_SYMLINK:
        case NT_STATUS_REPARSE_POINT_ENCOUNTERED:
          return err::SYMLINK;
        case NT_STATUS_NOT_SUPPORTED:
          return err::UNSUPPORTED;
        default:
          return err::OTHER;
        }
      }

      err classify_win(DWORD e) noexcept
      {
        switch (e)
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
          return err::NOT_FOUND;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
          return err::EXISTS;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
          return err::ACCESS;
        case ERROR_DIRECTORY:
          return err::NOT_DIR;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
          return err::BUSY;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
          return err::INVALID;
        case ERROR_FILENAME_EXCED_RANGE:
          return err::TOO_LONG;
        case ERROR_DIR_NOT_EMPTY:
          return err::NOT_EMPTY;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
          return err::NO_SPACE;
        case ERROR_NOT_SAME_DEVICE:
          return err::CROSS;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_CALL_NOT_IMPLEMENTED:
          return err::UNSUPPORTED;
        default:
          return err::OTHER;
        }
      }

      /// @brief UTF-16 to UTF-8 into a caller buffer.
      /// @return false if conversion failed (GetLastError())
      bool to_utf8(
          const wchar_t* w, size_t units, char* buffer, size_t capacity,
          size_t* needed) noexcept
      {
        if (units == 0)
        {
          *needed = 0;
          return true;
        }
        const int n = ::WideCharToMultiByte(
            CP_UTF8, 0, w, static_cast<int>(units), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
          return false;
        *needed = static_cast<size_t>(n);
        if (static_cast<size_t>(n) <= capacity)
          ::WideCharToMultiByte(
              CP_UTF8, 0, w, static_cast<int>(units), buffer, n, nullptr, nullptr);
        return true;
      }

      /// @brief \\?\C:\x -> C:\x and \\?\UNC\s\x -> \\s\x, in place, when the
      /// plain form is valid Win32 (shorter than MAX_PATH, no component
      /// ending in a dot or space, no device name).
      void strip_verbatim(char* p, size_t* n) noexcept
      {
        if (*n < 4 || ::memcmp(p, "\\\\?\\", 4) != 0)
          return;
        const bool unc   = *n >= 8 && ::memcmp(p + 4, "UNC\\", 4) == 0;
        const size_t cut = unc ? 6 : 4; // keep "\\" before server for UNC
        const size_t len = *n - cut;
        if (len >= static_cast<size_t>(MAX_PATH))
          return;
        const char* body      = p + cut + (unc ? 2 : 0);
        const size_t body_len = len - (unc ? 2 : 0);
        size_t i              = unc ? 0 : 3; // skip "C:\"
        while (i < body_len)
        {
          size_t j = i;
          while (j < body_len && body[j] != '\\')
            ++j;
          if (j > i && windows_component_refused(body + i, j - i))
            return;
          i = j + 1;
        }
        if (unc)
        {
          p[0] = '\\';
          p[1] = '\\';
          ::memmove(p + 2, p + 8, *n - 8);
          *n -= 6;
        }
        else
        {
          ::memmove(p, p + 4, *n - 4);
          *n -= 4;
        }
      }

      /// @brief The plain Win32 form of a converted absolute NT name (for
      /// SetCurrentDirectoryW).
      size_t plain_form(const wchar_t* nt, size_t n, wchar_t* out) noexcept
      {
        if (n >= 8 && ::wcsncmp(nt, L"\\??\\UNC\\", 8) == 0)
        {
          out[0] = L'\\';
          out[1] = L'\\';
          ::memcpy(out + 2, nt + 8, (n - 8) * sizeof(wchar_t));
          out[n - 6] = L'\0';
          return n - 6;
        }
        ::memcpy(out, nt + 4, (n - 4) * sizeof(wchar_t));
        out[n - 4] = L'\0';
        return n - 4;
      }

      /// @brief FILE_DISPOSITION_INFO_EX and FILE_DISPOSITION_INFO (not every SDK has the former).
      struct disposition_info_ex
      {
        DWORD Flags;
      };
      struct disposition_info
      {
        BOOLEAN Delete;
      };

      /// @brief FILE_RENAME_INFO with room for a name.
      struct rename_info
      {
        union
        {
          BOOLEAN ReplaceIfExists;
          DWORD Flags;
        };
        HANDLE RootDirectory;
        DWORD FileNameLength;
        WCHAR FileName[MAX_PATH_BYTES + 16];
      };

      // Directory records (layouts of FILE_ID_EXTD_DIR_INFO, FILE_FULL_DIR_INFO, FILE_ID_BOTH_DIR_INFO)
      constexpr size_t REC_NEXT       = 0;
      constexpr size_t REC_ATTRIBUTES = 56;
      constexpr size_t REC_NAME_SIZE  = 60;
      constexpr size_t REC_EA_SIZE =
          64; // the reparse tag for reparse points (full, id both)
      constexpr size_t EXTD_TAG        = 68;
      constexpr size_t EXTD_NAME       = 88;
      constexpr size_t FULL_NAME       = 68;
      constexpr size_t ID_BOTH_NAME    = 104;
      constexpr DWORD WINDOWS_LIST_MAX = 65536;

      // dir_reader::internal_flags
      constexpr uint32_t LIST_RESTART     = 0x1;
      constexpr uint32_t LIST_CLASS_SHIFT = 1; // 0 extd, 1 full, 2 id both
      constexpr uint32_t LIST_CLASS_MASK  = 0x6;
    } // namespace

    namespace detail
    {
      /*****************************/
      /* DIRECTORY HANDLES         */
      /*****************************/

      status<dir_open_result> platform_dir_open(
          const path_ref& p, path_resolve resolve, native_dir_t* out) noexcept
      {
        using R = dir_open_result;
        wide_name w;
        const conv c = to_wide(p, &w);
        if (c != conv::OK)
          return conv_error(
              c, R::DIR_OPEN_INVALID_PATH, R::DIR_OPEN_PATH_NOT_ABSOLUTE,
              R::DIR_OPEN_PATH_TOO_LONG);
        constexpr ULONG options =
            NT_FILE_DIRECTORY_FILE | NT_FILE_SYNCHRONOUS_IO_NONALERT;
        HANDLE h = nullptr;
        LONG st  = 0;
        if (!nofollow(resolve) || (w.root != nullptr && w.size == 0))
          st =
              nt_open(w.root, w.text, w.size, DIR_ACCESS, NT_FILE_OPEN, options, &h);
        else
        {
          win_parent par;
          st = resolve_parent(w, resolve, &par);
          if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
            st = open_nofollow(
                par.handle, par.last, par.last_size, DIR_ACCESS, NT_FILE_OPEN,
                options, &h);
        }
        if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
        {
          *out = h;
          return {};
        }
        switch (classify_nt(st))
        {
        case err::NOT_FOUND:
          return fail_nt(R::DIR_OPEN_NOT_FOUND, st);
        case err::NOT_DIR:
          return fail_nt(R::DIR_OPEN_NOT_A_DIRECTORY, st);
        case err::ACCESS:
          return fail_nt(R::DIR_OPEN_ACCESS_DENIED, st);
        case err::SYMLINK:
          return fail_nt(R::DIR_OPEN_SYMLINK, st);
        case err::INVALID:
          return fail_nt(R::DIR_OPEN_INVALID_PATH, st);
        case err::TOO_LONG:
          return fail_nt(R::DIR_OPEN_PATH_TOO_LONG, st);
        case err::TOO_MANY:
          return fail_nt(R::DIR_OPEN_TOO_MANY_OPEN, st);
        default:
          return fail_nt(R::DIR_OPEN_SYSTEM_ERROR, st);
        }
      }

      void platform_dir_close(native_dir_t d) noexcept
      {
        ::CloseHandle(static_cast<HANDLE>(d));
      }

      status<dir_adopt_result> platform_dir_adopt(native_dir_t d) noexcept
      {
        BY_HANDLE_FILE_INFORMATION bhfi{};
        if (!::GetFileInformationByHandle(static_cast<HANDLE>(d), &bhfi))
          return fail_win(
              dir_adopt_result::DIR_ADOPT_SYSTEM_ERROR, ::GetLastError());
        if ((bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
          return fail(dir_adopt_result::DIR_ADOPT_NOT_A_DIRECTORY);
        return {};
      }

      /*****************************/
      /* FILES                     */
      /*****************************/

      status<file_open_result> platform_file_open_at(
          const path_ref& p, path_resolve resolve, const file_open_desc& desc,
          native_file_t* out) noexcept
      {
        using R = file_open_result;
        wide_name w;
        const conv c = to_wide(p, &w);
        if (c == conv::DEVICE)
          return fail(R::FILE_OPEN_NOT_A_FILE);
        if (c != conv::OK)
          return conv_error(
              c, R::FILE_OPEN_INVALID_PATH, R::FILE_OPEN_PATH_NOT_ABSOLUTE,
              R::FILE_OPEN_PATH_TOO_LONG);

        ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        switch (desc.access)
        {
        case file_access::FILE_ACCESS_READ:
          access |= GENERIC_READ;
          break;
        case file_access::FILE_ACCESS_WRITE:
          access |= GENERIC_WRITE;
          break;
        case file_access::FILE_ACCESS_READ_WRITE:
          access |= GENERIC_READ | GENERIC_WRITE;
          break;
        }
        ULONG options =
            NT_FILE_NON_DIRECTORY_FILE; // overlapped: no synchronous flag
        if (desc.unbuffered)
          options |= NT_FILE_NO_INTERMEDIATE_BUFFERING;
        if (desc.pattern == file_pattern::FILE_PATTERN_SEQUENTIAL)
          options |= NT_FILE_SEQUENTIAL_ONLY;
        else if (desc.pattern == file_pattern::FILE_PATTERN_RANDOM)
          options |= NT_FILE_RANDOM_ACCESS;
        const bool truncate =
            desc.disposition == file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE
            || desc.disposition
                   == file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING;

        HANDLE h = nullptr;
        LONG st  = 0;
        if (!nofollow(resolve))
        {
          ULONG disposition = NT_FILE_OPEN;
          switch (desc.disposition)
          {
          case file_disposition::FILE_DISPOSITION_OPEN_EXISTING:
            disposition = NT_FILE_OPEN;
            break;
          case file_disposition::FILE_DISPOSITION_CREATE_NEW:
            disposition = NT_FILE_CREATE;
            break;
          case file_disposition::FILE_DISPOSITION_OPEN_OR_CREATE:
            disposition = NT_FILE_OPEN_IF;
            break;
          case file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE:
            disposition = NT_FILE_OVERWRITE_IF;
            break;
          case file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING:
            disposition = NT_FILE_OVERWRITE;
            break;
          }
          st = nt_open(w.root, w.text, w.size, access, disposition, options, &h);
        }
        else
        {
          // Never truncate through a name that might be a link: open, check, then truncate
          ULONG disposition = NT_FILE_OPEN;
          switch (desc.disposition)
          {
          case file_disposition::FILE_DISPOSITION_OPEN_EXISTING:
          case file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING:
            disposition = NT_FILE_OPEN;
            break;
          case file_disposition::FILE_DISPOSITION_CREATE_NEW:
            disposition = NT_FILE_CREATE;
            break;
          case file_disposition::FILE_DISPOSITION_OPEN_OR_CREATE:
          case file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE:
            disposition = NT_FILE_OPEN_IF;
            break;
          }
          win_parent par;
          st             = resolve_parent(w, resolve, &par);
          ULONG_PTR info = 0;
          if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
            st = open_nofollow(
                par.handle, par.last, par.last_size, access, disposition, options,
                &h, &info);
          if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK && truncate
              && info != NT_FILE_CREATED)
          {
            FILE_END_OF_FILE_INFO eof{};
            if (!::SetFileInformationByHandle(
                    h, FileEndOfFileInfo, &eof, sizeof(eof)))
            {
              const DWORD e = ::GetLastError();
              ::CloseHandle(h);
              return fail_win(R::FILE_OPEN_SYSTEM_ERROR, e);
            }
          }
        }
        if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
        {
          *out = h;
          return {};
        }
        switch (classify_nt(st))
        {
        case err::NOT_FOUND:
          return fail_nt(R::FILE_OPEN_NOT_FOUND, st);
        case err::EXISTS:
          return fail_nt(R::FILE_OPEN_ALREADY_EXISTS, st);
        case err::ACCESS:
          return fail_nt(R::FILE_OPEN_ACCESS_DENIED, st);
        case err::IS_DIR:
          return fail_nt(R::FILE_OPEN_NOT_A_FILE, st);
        case err::NOT_DIR:
          return fail_nt(R::FILE_OPEN_NOT_FOUND, st);
        case err::BUSY:
          return fail_nt(R::FILE_OPEN_BUSY, st);
        case err::SYMLINK:
          return fail_nt(R::FILE_OPEN_SYMLINK, st);
        case err::INVALID:
          return fail_nt(R::FILE_OPEN_INVALID_PATH, st);
        case err::TOO_LONG:
          return fail_nt(R::FILE_OPEN_PATH_TOO_LONG, st);
        case err::TOO_MANY:
          return fail_nt(R::FILE_OPEN_TOO_MANY_OPEN, st);
        case err::NO_SPACE:
          return fail_nt(R::FILE_OPEN_NO_SPACE, st);
        case err::UNSUPPORTED:
          if (desc.unbuffered)
            return fail_nt(R::FILE_OPEN_UNSUPPORTED, st);
          return fail_nt(R::FILE_OPEN_SYSTEM_ERROR, st);
        default:
          return fail_nt(R::FILE_OPEN_SYSTEM_ERROR, st);
        }
      }

      /*****************************/
      /* ENTRY INFORMATION         */
      /*****************************/

      status<path_info_result> platform_path_info(
          const path_ref& p, path_resolve resolve, path_info* out) noexcept
      {
        using R = path_info_result;
        wide_name w;
        const conv c = to_wide(p, &w);
        if (c != conv::OK)
          return conv_error(
              c, R::PATH_INFO_INVALID_PATH, R::PATH_INFO_PATH_NOT_ABSOLUTE,
              R::PATH_INFO_PATH_TOO_LONG);
        constexpr ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        HANDLE h                     = nullptr;
        LONG st                      = 0;
        if (!nofollow(resolve))
          st = nt_open(
              w.root, w.text, w.size, access, NT_FILE_OPEN,
              NT_FILE_SYNCHRONOUS_IO_NONALERT, &h);
        else
        {
          win_parent par;
          st = resolve_parent(w, resolve, &par);
          if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
          {
            // lstat: the last component is opened as itself
            st = nt_open(
                par.handle, par.last, par.last_size, access, NT_FILE_OPEN,
                NT_FILE_SYNCHRONOUS_IO_NONALERT | NT_FILE_OPEN_REPARSE_POINT, &h);
          }
        }
        if (!nt_ok(st) || st == NT_STATUS_STOPPED_ON_SYMLINK)
        {
          switch (classify_nt(st))
          {
          case err::NOT_FOUND:
          case err::NOT_DIR:
            return fail_nt(R::PATH_INFO_NOT_FOUND, st);
          case err::ACCESS:
            return fail_nt(R::PATH_INFO_ACCESS_DENIED, st);
          case err::SYMLINK:
            return fail_nt(R::PATH_INFO_SYMLINK, st);
          case err::INVALID:
            return fail_nt(R::PATH_INFO_INVALID_PATH, st);
          case err::TOO_LONG:
            return fail_nt(R::PATH_INFO_PATH_TOO_LONG, st);
          default:
            return fail_nt(R::PATH_INFO_SYSTEM_ERROR, st);
          }
        }
        BY_HANDLE_FILE_INFORMATION bhfi{};
        DWORD attributes = 0;
        DWORD tag        = 0;
        if (!::GetFileInformationByHandle(h, &bhfi)
            || !attribute_tag(h, &attributes, &tag)
            || !identity_of(h, &out->identity))
        {
          const DWORD e = ::GetLastError();
          ::CloseHandle(h);
          return fail_win(R::PATH_INFO_SYSTEM_ERROR, e);
        }
        const bool dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (tag != 0 && name_surrogate(tag)
            && !(tag == TAG_MOUNT_POINT && is_volume_mount(h)))
          out->type = path_type::PATH_TYPE_SYMLINK;
        else if (dir)
          out->type = path_type::PATH_TYPE_DIRECTORY;
        else if (::GetFileType(h) == FILE_TYPE_DISK)
          out->type = path_type::PATH_TYPE_FILE;
        else
          out->type = path_type::PATH_TYPE_OTHER;
        out->size = out->type == path_type::PATH_TYPE_FILE
                        ? (static_cast<uint64_t>(bhfi.nFileSizeHigh) << 32)
                              | bhfi.nFileSizeLow
                        : 0;
        const uint64_t t =
            (static_cast<uint64_t>(bhfi.ftLastWriteTime.dwHighDateTime) << 32)
            | bhfi.ftLastWriteTime.dwLowDateTime;
        out->last_write =
            wall_instant{(static_cast<int64_t>(t) - 116444736000000000LL) * 100};
        ::CloseHandle(h);
        return {};
      }

      /*****************************/
      /* CREATE, REMOVE, RENAME    */
      /*****************************/

      status<dir_create_result> platform_dir_create(
          const path_ref& p, path_resolve resolve, native_dir_t* out) noexcept
      {
        using R = dir_create_result;
        wide_name w;
        const conv c = to_wide(p, &w);
        if (c != conv::OK)
          return conv_error(
              c, R::DIR_CREATE_INVALID_PATH, R::DIR_CREATE_PATH_NOT_ABSOLUTE,
              R::DIR_CREATE_PATH_TOO_LONG);
        win_parent par;
        LONG st  = resolve_parent(w, resolve, &par);
        HANDLE h = nullptr;
        if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
          st = nt_open(
              par.handle, par.last, par.last_size, DIR_ACCESS, NT_FILE_CREATE,
              NT_FILE_DIRECTORY_FILE | NT_FILE_SYNCHRONOUS_IO_NONALERT, &h);
        if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
        {
          if (out != nullptr)
            *out = h;
          else
            ::CloseHandle(h);
          return {};
        }
        switch (classify_nt(st))
        {
        case err::EXISTS:
          return fail_nt(R::DIR_CREATE_ALREADY_EXISTS, st);
        case err::NOT_FOUND:
        case err::NOT_DIR:
          return fail_nt(R::DIR_CREATE_NOT_FOUND, st);
        case err::ACCESS:
          return fail_nt(R::DIR_CREATE_ACCESS_DENIED, st);
        case err::SYMLINK:
          return fail_nt(R::DIR_CREATE_SYMLINK, st);
        case err::INVALID:
          return fail_nt(R::DIR_CREATE_INVALID_PATH, st);
        case err::TOO_LONG:
          return fail_nt(R::DIR_CREATE_PATH_TOO_LONG, st);
        case err::NO_SPACE:
          return fail_nt(R::DIR_CREATE_NO_SPACE, st);
        default:
          return fail_nt(R::DIR_CREATE_SYSTEM_ERROR, st);
        }
      }

      status<path_remove_result> platform_path_remove(
          const path_ref& p, path_resolve resolve, path_kind kind) noexcept
      {
        using R = path_remove_result;
        wide_name w;
        const conv c = to_wide(p, &w);
        if (c != conv::OK)
          return conv_error(
              c, R::PATH_REMOVE_INVALID_PATH, R::PATH_REMOVE_PATH_NOT_ABSOLUTE,
              R::PATH_REMOVE_PATH_TOO_LONG);
        auto nt_error = [](LONG st) noexcept -> status<R>
        {
          switch (classify_nt(st))
          {
          case err::NOT_FOUND:
          case err::NOT_DIR:
            return fail_nt(R::PATH_REMOVE_NOT_FOUND, st);
          case err::ACCESS:
            return fail_nt(R::PATH_REMOVE_ACCESS_DENIED, st);
          case err::BUSY:
            return fail_nt(R::PATH_REMOVE_BUSY, st);
          case err::SYMLINK:
            return fail_nt(R::PATH_REMOVE_SYMLINK, st);
          case err::INVALID:
            return fail_nt(R::PATH_REMOVE_INVALID_PATH, st);
          case err::TOO_LONG:
            return fail_nt(R::PATH_REMOVE_PATH_TOO_LONG, st);
          case err::NOT_EMPTY:
            return fail_nt(R::PATH_REMOVE_NOT_EMPTY, st);
          default:
            return fail_nt(R::PATH_REMOVE_SYSTEM_ERROR, st);
          }
        };
        win_parent par;
        LONG st = resolve_parent(w, resolve, &par);
        if (!nt_ok(st) || st == NT_STATUS_STOPPED_ON_SYMLINK)
          return nt_error(st);
        // The last component is never followed: a link is removed, not its target
        constexpr ULONG options =
            NT_FILE_SYNCHRONOUS_IO_NONALERT | NT_FILE_OPEN_REPARSE_POINT;
        HANDLE h                  = nullptr;
        bool can_write_attributes = true;
        st                        = nt_open(
            par.handle, par.last, par.last_size,
            DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
            NT_FILE_OPEN, options, &h);
        if (st == NT_STATUS_ACCESS_DENIED)
        {
          can_write_attributes = false;
          st                   = nt_open(
              par.handle, par.last, par.last_size,
              DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, NT_FILE_OPEN, options,
              &h);
        }
        if (!nt_ok(st))
          return nt_error(st);
        DWORD attributes = 0;
        DWORD tag        = 0;
        if (!attribute_tag(h, &attributes, &tag))
        {
          const DWORD e = ::GetLastError();
          ::CloseHandle(h);
          return fail_win(R::PATH_REMOVE_SYSTEM_ERROR, e);
        }
        const bool link = tag != 0 && name_surrogate(tag)
                          && !(tag == TAG_MOUNT_POINT && is_volume_mount(h));
        const bool dir  = !link && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dir != (kind == path_kind::PATH_KIND_DIRECTORY))
        {
          ::CloseHandle(h);
          return fail(R::PATH_REMOVE_WRONG_KIND);
        }

        disposition_info_ex ex{};
        ex.Flags =
            DISPOSITION_DELETE | DISPOSITION_POSIX | DISPOSITION_IGNORE_READONLY;
        bool done = ::SetFileInformationByHandle(
                        h, info_class(CLASS_DISPOSITION_EX), &ex, sizeof(ex))
                    != 0;
        DWORD e   = done ? 0 : ::GetLastError();
        if (!done && (e == 0 || e == ERROR_INVALID_PARAMETER))
        {
          // Before Windows 10 1809: no IGNORE_READONLY
          ex.Flags = DISPOSITION_DELETE | DISPOSITION_POSIX;
          done     = ::SetFileInformationByHandle(
                         h, info_class(CLASS_DISPOSITION_EX), &ex, sizeof(ex))
                     != 0;
          e        = done ? 0 : ::GetLastError();
        }
        if (!done
            && (e == 0 || e == ERROR_INVALID_PARAMETER
                || classify_win(e) == err::UNSUPPORTED || e == ERROR_ACCESS_DENIED))
        {
          // No POSIX semantics (FAT, network shares, older Windows), or a read-only file
          if ((attributes & FILE_ATTRIBUTE_READONLY) != 0 && can_write_attributes)
          {
            FILE_BASIC_INFO basic{};
            basic.FileAttributes =
                (attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) | 0;
            if (basic.FileAttributes == 0)
              basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            ::SetFileInformationByHandle(
                h, info_class(CLASS_BASIC), &basic, sizeof(basic));
          }
          disposition_info legacy{};
          legacy.Delete = TRUE;
          done          = ::SetFileInformationByHandle(
                              h, info_class(CLASS_DISPOSITION), &legacy, sizeof(legacy))
                          != 0;
          e             = done ? 0 : ::GetLastError();
        }
        ::CloseHandle(h);
        if (done)
          return {};
        switch (classify_win(e))
        {
        case err::NOT_EMPTY:
          return fail_win(R::PATH_REMOVE_NOT_EMPTY, e);
        case err::ACCESS:
          return fail_win(R::PATH_REMOVE_ACCESS_DENIED, e);
        case err::BUSY:
          return fail_win(R::PATH_REMOVE_BUSY, e);
        case err::NOT_FOUND:
          return fail_win(R::PATH_REMOVE_NOT_FOUND, e);
        default:
          return fail_win(R::PATH_REMOVE_SYSTEM_ERROR, e);
        }
      }

      status<path_rename_result> platform_path_rename(
          const path_ref& from, const path_ref& to,
          const path_rename_desc& desc) noexcept
      {
        using R       = path_rename_result;
        auto nt_error = [&](LONG st) noexcept -> status<R>
        {
          switch (classify_nt(st))
          {
          case err::NOT_FOUND:
          case err::NOT_DIR:
            return fail_nt(R::PATH_RENAME_NOT_FOUND, st);
          case err::ACCESS:
            return fail_nt(R::PATH_RENAME_ACCESS_DENIED, st);
          case err::BUSY:
            return fail_nt(R::PATH_RENAME_BUSY, st);
          case err::SYMLINK:
            return fail_nt(R::PATH_RENAME_SYMLINK, st);
          case err::INVALID:
            return fail_nt(R::PATH_RENAME_INVALID_PATH, st);
          case err::TOO_LONG:
            return fail_nt(R::PATH_RENAME_PATH_TOO_LONG, st);
          default:
            return fail_nt(R::PATH_RENAME_SYSTEM_ERROR, st);
          }
        };
        wide_name wa{};
        wide_name wb{};
        conv c = to_wide(from, &wa);
        if (c == conv::OK)
          c = to_wide(to, &wb);
        if (c != conv::OK)
          return conv_error(
              c, R::PATH_RENAME_INVALID_PATH, R::PATH_RENAME_PATH_NOT_ABSOLUTE,
              R::PATH_RENAME_PATH_TOO_LONG);
        win_parent pa;
        win_parent pb;
        LONG st = resolve_parent(wa, desc.resolve, &pa);
        if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
          st = resolve_parent(wb, desc.resolve, &pb);
        if (!nt_ok(st) || st == NT_STATUS_STOPPED_ON_SYMLINK)
          return nt_error(st);

        // The source itself, never followed (renaming a link renames the link)
        HANDLE h = nullptr;
        st       = nt_open(
            pa.handle, pa.last, pa.last_size,
            DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, NT_FILE_OPEN,
            NT_FILE_SYNCHRONOUS_IO_NONALERT | NT_FILE_OPEN_REPARSE_POINT, &h);
        if (!nt_ok(st))
          return nt_error(st);

        // NtSetInformationFile, as Go does: the Win32 wrapper refuses some
        // names relative to a root directory (ERROR_INVALID_PARAMETER)
        rename_info info{};
        info.RootDirectory  = pb.handle;
        info.FileNameLength = static_cast<DWORD>(pb.last_size * sizeof(wchar_t));
        ::memcpy(info.FileName, pb.last, pb.last_size * sizeof(wchar_t));
        const ULONG size   = static_cast<ULONG>(sizeof(info));
        const bool replace = desc.mode == path_rename_mode::PATH_RENAME_MODE_REPLACE;
        info.Flags =
            (replace ? RENAME_REPLACE_IF_EXISTS : 0) | RENAME_POSIX_SEMANTICS;
        nt_io_status_block iosb{};
        st = g_nt.set_information_file != nullptr
                 ? g_nt.set_information_file(
                       h, &iosb, &info, size, NT_FILE_RENAME_INFORMATION_EX)
                 : NT_STATUS_NOT_SUPPORTED;
        if (st == NT_STATUS_INVALID_INFO_CLASS || st == NT_STATUS_INVALID_PARAMETER
            || st == NT_STATUS_NOT_SUPPORTED)
        {
          // Before Windows 10 1607, FAT: the classic rename
          info.Flags           = 0;
          info.ReplaceIfExists = replace ? TRUE : FALSE;
          st                   = g_nt.set_information_file != nullptr
                                     ? g_nt.set_information_file(
                                           h, &iosb, &info, size, NT_FILE_RENAME_INFORMATION)
                                     : NT_STATUS_NOT_SUPPORTED;
        }
        ::CloseHandle(h);
        if (!nt_ok(st))
        {
          switch (classify_nt(st))
          {
          case err::EXISTS:
            return fail_nt(
                replace ? R::PATH_RENAME_ACCESS_DENIED
                        : R::PATH_RENAME_ALREADY_EXISTS,
                st);
          case err::NOT_FOUND:
          case err::NOT_DIR:
            return fail_nt(R::PATH_RENAME_NOT_FOUND, st);
          case err::IS_DIR:
            return fail_nt(R::PATH_RENAME_WRONG_KIND, st);
          case err::NOT_EMPTY:
            return fail_nt(R::PATH_RENAME_NOT_EMPTY, st);
          case err::CROSS:
            return fail_nt(R::PATH_RENAME_CROSS_DEVICE, st);
          case err::ACCESS:
            return fail_nt(R::PATH_RENAME_ACCESS_DENIED, st);
          case err::BUSY:
            return fail_nt(R::PATH_RENAME_BUSY, st);
          case err::INVALID:
            return fail_nt(R::PATH_RENAME_INVALID_PATH, st);
          case err::TOO_LONG:
            return fail_nt(R::PATH_RENAME_PATH_TOO_LONG, st);
          case err::NO_SPACE:
            return fail_nt(R::PATH_RENAME_NO_SPACE, st);
          default:
            return fail_nt(R::PATH_RENAME_SYSTEM_ERROR, st);
          }
        }

        if (desc.durable)
        {
          // Flush the directory now holding the name (its index and the NTFS log)
          size_t cut = pb.last_size;
          while (cut > 0 && pb.last[cut - 1] != L'\\')
            --cut;
          HANDLE d = nullptr;
          st       = nt_open(
              pb.handle, pb.last,
              cut > 0 ? cut - (pb.handle == nullptr && cut <= wb.prefix ? 0 : 1) : 0,
              FILE_ADD_FILE | SYNCHRONIZE, NT_FILE_OPEN,
              NT_FILE_DIRECTORY_FILE | NT_FILE_SYNCHRONOUS_IO_NONALERT, &d);
          if (!nt_ok(st))
            return fail_nt(R::PATH_RENAME_NOT_DURABLE, st);
          const bool flushed = ::FlushFileBuffers(d) != 0;
          const DWORD fe     = flushed ? 0 : ::GetLastError();
          ::CloseHandle(d);
          if (!flushed)
            return fail_win(R::PATH_RENAME_NOT_DURABLE, fe);
        }
        return {};
      }

      /*****************************/
      /* SYMLINKS                  */
      /*****************************/

      status<path_read_link_result> platform_path_read_link(
          const path_ref& p, path_resolve resolve, char* buffer, size_t capacity,
          size_t* needed, link_info* info) noexcept
      {
        using R = path_read_link_result;
        wide_name w;
        const conv c = to_wide(p, &w);
        if (c != conv::OK)
          return conv_error(
              c, R::PATH_READ_LINK_INVALID_PATH, R::PATH_READ_LINK_PATH_NOT_ABSOLUTE,
              R::PATH_READ_LINK_PATH_TOO_LONG);
        win_parent par;
        LONG st  = resolve_parent(w, resolve, &par);
        HANDLE h = nullptr;
        if (nt_ok(st) && st != NT_STATUS_STOPPED_ON_SYMLINK)
          st = nt_open(
              par.handle, par.last, par.last_size,
              FILE_READ_ATTRIBUTES | SYNCHRONIZE, NT_FILE_OPEN,
              NT_FILE_SYNCHRONOUS_IO_NONALERT | NT_FILE_OPEN_REPARSE_POINT, &h);
        if (!nt_ok(st) || st == NT_STATUS_STOPPED_ON_SYMLINK)
        {
          switch (classify_nt(st))
          {
          case err::NOT_FOUND:
          case err::NOT_DIR:
            return fail_nt(R::PATH_READ_LINK_NOT_FOUND, st);
          case err::ACCESS:
            return fail_nt(R::PATH_READ_LINK_ACCESS_DENIED, st);
          case err::SYMLINK:
            return fail_nt(R::PATH_READ_LINK_SYMLINK, st);
          case err::INVALID:
            return fail_nt(R::PATH_READ_LINK_INVALID_PATH, st);
          case err::TOO_LONG:
            return fail_nt(R::PATH_READ_LINK_PATH_TOO_LONG, st);
          default:
            return fail_nt(R::PATH_READ_LINK_SYSTEM_ERROR, st);
          }
        }
        DWORD attributes = 0;
        DWORD tag        = 0;
        alignas(8) unsigned char buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
        DWORD got = 0;
        if (attribute_tag(h, &attributes, &tag) && tag != 0)
          got = read_reparse(h, buf, sizeof(buf));
        const DWORD e = ::GetLastError();
        ::CloseHandle(h);
        if (tag == 0 || !name_surrogate(tag))
          return fail(R::PATH_READ_LINK_NOT_A_LINK);
        if (got < 8)
          return fail_win(R::PATH_READ_LINK_SYSTEM_ERROR, e);

        info->directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (tag == TAG_LX_SYMLINK)
        {
          // WSL: a version (4 bytes), then the target in UTF-8
          USHORT len = 0;
          ::memcpy(&len, buf + 4, 2);
          if (len < 4 || got < 8u + len)
            return fail(R::PATH_READ_LINK_SYSTEM_ERROR);
          const char* target = reinterpret_cast<const char*>(buf + 12);
          const size_t n     = len - 4u;
          info->relative     = n == 0 || target[0] != '/';
          if (!copy_out(target, n, buffer, capacity, needed))
            return fail(R::PATH_READ_LINK_BUFFER_TOO_SMALL);
          return {};
        }
        if (tag != TAG_SYMLINK && tag != TAG_MOUNT_POINT)
          return fail(R::PATH_READ_LINK_NOT_A_LINK); // another kind of surrogate
        USHORT off = 0;
        USHORT len = 0;
        ::memcpy(&off, buf + 8, 2);
        ::memcpy(&len, buf + 10, 2);
        size_t path_at = 16;
        if (tag == TAG_SYMLINK)
        {
          ULONG flags = 0;
          ::memcpy(&flags, buf + 16, 4);
          info->relative = (flags & 1u) != 0; // SYMLINK_FLAG_RELATIVE
          path_at        = 20;
        }
        else
        {
          info->junction = true;
          info->relative = false;
        }
        if (path_at + off + len > got)
          return fail(R::PATH_READ_LINK_SYSTEM_ERROR);
        if (tag == TAG_MOUNT_POINT)
        {
          static constexpr wchar_t volume[] = L"\\??\\Volume{";
          const wchar_t* sub = reinterpret_cast<const wchar_t*>(buf + path_at + off);
          if (len / 2 >= 11 && ::wcsncmp(sub, volume, 11) == 0)
            return fail(
                R::PATH_READ_LINK_NOT_A_LINK); // a volume mount point: a directory
        }
        const wchar_t* target =
            reinterpret_cast<const wchar_t*>(buf + path_at + off);
        size_t units = len / sizeof(wchar_t);
        wchar_t tmp[MAX_PATH_BYTES + 16];
        if (!info->relative && units >= 4 && ::wcsncmp(target, L"\\??\\", 4) == 0)
        {
          // \??\UNC\s\x -> \\s\x ; \??\C:\x -> C:\x
          if (units >= 8 && ::wcsncmp(target, L"\\??\\UNC\\", 8) == 0
              && units - 6 <= MAX_PATH_BYTES + 15)
          {
            tmp[0] = L'\\';
            tmp[1] = L'\\';
            ::memcpy(tmp + 2, target + 8, (units - 8) * sizeof(wchar_t));
            units -= 6;
            target = tmp;
          }
          else
          {
            target += 4;
            units -= 4;
          }
        }
        if (!to_utf8(target, units, buffer, capacity, needed))
          return fail_win(R::PATH_READ_LINK_SYSTEM_ERROR, ::GetLastError());
        if (*needed > capacity)
          return fail(R::PATH_READ_LINK_BUFFER_TOO_SMALL);
        return {};
      }

      /*****************************/
      /* LISTING                   */
      /*****************************/

      status<dir_list_result> platform_dir_list_begin(
          native_dir_t, dir_reader& r) noexcept
      {
        r.internal_flags = LIST_RESTART;
        return {};
      }

      status<dir_list_result> platform_dir_list_next(
          native_dir_t d, dir_reader& r, dir_entry* out) noexcept
      {
        using R    = dir_list_result;
        auto* base = static_cast<unsigned char*>(r.buffer);
        for (;;)
        {
          if (r.internal_end == 0)
          {
            // Fetch a batch: the newest class first, older ones where refused
            for (;;)
            {
              const uint32_t cls =
                  (r.internal_flags & LIST_CLASS_MASK) >> LIST_CLASS_SHIFT;
              const bool restart      = (r.internal_flags & LIST_RESTART) != 0;
              const int classes[3][2] = {
                  {CLASS_ID_EXTD_DIR, CLASS_ID_EXTD_DIR_RESTART},
                  {CLASS_FULL_DIR, CLASS_FULL_DIR_RESTART},
                  {CLASS_ID_BOTH_DIR, CLASS_ID_BOTH_DIR_RESTART}};
              const DWORD size = r.size < WINDOWS_LIST_MAX
                                     ? static_cast<DWORD>(r.size)
                                     : WINDOWS_LIST_MAX;
              if (::GetFileInformationByHandleEx(
                      static_cast<HANDLE>(d),
                      info_class(classes[cls][restart ? 1 : 0]), r.buffer, size))
              {
                r.internal_flags &= ~LIST_RESTART;
                r.internal_offset = 0;
                r.internal_end    = 1; // a chain to walk
                break;
              }
              const DWORD e = ::GetLastError();
              if (e == ERROR_NO_MORE_FILES)
              {
                r.internal_flags &= ~LIST_RESTART;
                return fail(R::DIR_LIST_END);
              }
              if (cls < 2
                  && (e == ERROR_INVALID_PARAMETER
                      || classify_win(e) == err::UNSUPPORTED
                      || e == ERROR_INVALID_LEVEL))
              {
                r.internal_flags = (r.internal_flags & ~LIST_CLASS_MASK)
                                   | ((cls + 1) << LIST_CLASS_SHIFT) | LIST_RESTART;
                continue;
              }
              return fail_win(R::DIR_LIST_SYSTEM_ERROR, e);
            }
          }
          const uint32_t cls =
              (r.internal_flags & LIST_CLASS_MASK) >> LIST_CLASS_SHIFT;
          unsigned char* rec = base + r.internal_offset;
          ULONG next         = 0;
          ULONG attributes   = 0;
          ULONG name_bytes   = 0;
          ULONG tag          = 0;
          ::memcpy(&next, rec + REC_NEXT, 4);
          ::memcpy(&attributes, rec + REC_ATTRIBUTES, 4);
          ::memcpy(&name_bytes, rec + REC_NAME_SIZE, 4);
          ::memcpy(&tag, rec + (cls == 0 ? EXTD_TAG : REC_EA_SIZE), 4);
          const size_t name_at = cls == 0   ? EXTD_NAME
                                 : cls == 1 ? FULL_NAME
                                            : ID_BOTH_NAME;
          const wchar_t* name  = reinterpret_cast<const wchar_t*>(rec + name_at);
          const size_t units   = name_bytes / sizeof(wchar_t);
          if (next == 0)
            r.internal_end = 0;
          else
            r.internal_offset += next;

          if ((units == 1 && name[0] == L'.')
              || (units == 2 && name[0] == L'.' && name[1] == L'.'))
            continue;
          if (units > 255)
            return fail(R::DIR_LIST_NAME_TOO_LONG);
          size_t n = 0;
          if (!to_utf8(name, units, r.internal_name, sizeof(r.internal_name) - 1, &n)
              || n >= sizeof(r.internal_name))
            return fail_win(R::DIR_LIST_SYSTEM_ERROR, ::GetLastError());
          r.internal_name[n] = '\0';
          if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
              && name_surrogate(tag))
            out->type =
                tag == TAG_MOUNT_POINT
                    ? path_type::
                          PATH_TYPE_UNKNOWN // junction or volume mount: ask path_info_at()
                    : path_type::PATH_TYPE_SYMLINK;
          else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            out->type = path_type::PATH_TYPE_DIRECTORY;
          else
            out->type = path_type::PATH_TYPE_FILE;
          out->name      = r.internal_name;
          out->name_size = n;
          return {};
        }
      }

      /*****************************/
      /* CANONICAL PATHS           */
      /*****************************/

      status<path_canonical_result> platform_canonical(
          native_file_t native, char* buffer, size_t capacity,
          size_t* needed) noexcept
      {
        using R        = path_canonical_result;
        const HANDLE h = static_cast<HANDLE>(native);
        FILE_STANDARD_INFO standard{};
        if (::GetFileInformationByHandleEx(
                h, info_class(CLASS_STANDARD), &standard, sizeof(standard))
            && standard.DeletePending)
          return fail(R::PATH_CANONICAL_DELETED);
        wchar_t w[MAX_PATH_BYTES + 16];
        const DWORD n = ::GetFinalPathNameByHandleW(h, w, MAX_PATH_BYTES + 16, 0);
        if (n == 0)
          return fail_win(R::PATH_CANONICAL_SYSTEM_ERROR, ::GetLastError());
        if (n >= MAX_PATH_BYTES + 16)
          return fail_win(
              R::PATH_CANONICAL_SYSTEM_ERROR, ERROR_FILENAME_EXCED_RANGE);
        if (::wcsstr(w, L"\\$Extend\\$Deleted\\") != nullptr)
          return fail(
              R::PATH_CANONICAL_DELETED); // removed with POSIX semantics, still open
        char utf8[3 * (MAX_PATH_BYTES + 16)];
        size_t len = 0;
        if (!to_utf8(w, n, utf8, sizeof(utf8), &len) || len > sizeof(utf8))
          return fail_win(R::PATH_CANONICAL_SYSTEM_ERROR, ::GetLastError());
        strip_verbatim(utf8, &len);
        if (!copy_out(utf8, len, buffer, capacity, needed))
          return fail(R::PATH_CANONICAL_BUFFER_TOO_SMALL);
        return {};
      }

      /*****************************/
      /* CURRENT DIRECTORY         */
      /*****************************/

      status<current_directory_get_result> platform_current_directory_get(
          char* buffer, size_t capacity, size_t* needed) noexcept
      {
        using R = current_directory_get_result;
        wchar_t w[MAX_PATH_BYTES + 1];
        const DWORD n = ::GetCurrentDirectoryW(MAX_PATH_BYTES + 1, w);
        if (n == 0)
          return fail_win(R::CURRENT_DIRECTORY_GET_SYSTEM_ERROR, ::GetLastError());
        if (n > MAX_PATH_BYTES)
          return fail(R::CURRENT_DIRECTORY_GET_PATH_TOO_LONG);
        size_t units = n;
        if (units > 3 && w[units - 1] == L'\\')
          --units; // "C:\dir\" -> "C:\dir" (the root keeps its separator)
        char utf8[3 * (MAX_PATH_BYTES + 1)];
        size_t len = 0;
        if (!to_utf8(w, units, utf8, sizeof(utf8), &len) || len > sizeof(utf8))
          return fail_win(R::CURRENT_DIRECTORY_GET_SYSTEM_ERROR, ::GetLastError());
        if (len > MAX_PATH_BYTES)
          return fail(R::CURRENT_DIRECTORY_GET_PATH_TOO_LONG);
        if (!copy_out(utf8, len, buffer, capacity, needed))
          return fail(R::CURRENT_DIRECTORY_GET_BUFFER_TOO_SMALL);
        return {};
      }

      status<current_directory_set_result> platform_current_directory_set(
          const char* path, size_t size) noexcept
      {
        using R = current_directory_set_result;
        path_ref p{nullptr, path, size, true};
        wide_name w;
        const conv c = to_wide(p, &w);
        if (c != conv::OK)
          return conv_error(
              c, R::CURRENT_DIRECTORY_SET_INVALID_PATH,
              R::CURRENT_DIRECTORY_SET_PATH_NOT_ABSOLUTE,
              R::CURRENT_DIRECTORY_SET_PATH_TOO_LONG);
        // The current directory cannot be a \\?\ path: the plain form, limited to MAX_PATH
        wchar_t plain[MAX_PATH_BYTES + 16];
        plain_form(w.text, w.size, plain);
        if (::SetCurrentDirectoryW(plain))
          return {};
        const DWORD e = ::GetLastError();
        switch (classify_win(e))
        {
        case err::NOT_FOUND:
          return fail_win(R::CURRENT_DIRECTORY_SET_NOT_FOUND, e);
        case err::NOT_DIR:
          return fail_win(R::CURRENT_DIRECTORY_SET_NOT_A_DIRECTORY, e);
        case err::ACCESS:
          return fail_win(R::CURRENT_DIRECTORY_SET_ACCESS_DENIED, e);
        case err::INVALID:
          return fail_win(R::CURRENT_DIRECTORY_SET_INVALID_PATH, e);
        case err::TOO_LONG:
          return fail_win(R::CURRENT_DIRECTORY_SET_PATH_TOO_LONG, e);
        default:
          return fail_win(R::CURRENT_DIRECTORY_SET_SYSTEM_ERROR, e);
        }
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
