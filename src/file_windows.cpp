/*****************************************************************/ /**
 * @file   file_windows.cpp
 * @brief  Files and regions on Windows.
 *
 * - Paths: UTF-8 to UTF-16, GetFullPathNameW (separators, dot segments,
 *   Win32 name rules), then the \\?\ (or \\?\UNC\) prefix. Device
 *   paths (\\.\, reserved names such as NUL) are refused before opening.
 * - Handles are overlapped with full sharing. Each blocking call passes an
 *   OVERLAPPED whose event comes from a pool (FILE_EVENT_POOL_SIZE events,
 *   created on first use, closed by deinitialize(); a temporary event when
 *   all are busy). The event's low bit is set so that no completion is
 *   queued to a completion port the handle may be bound to. Synchronous
 *   (adopted) handles complete inside the call and work the same way.
 * - Regions: a mapping object sized to the end of the view, closed at once
 *   (the view keeps it alive).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "file_windows.cpp is for Windows only"
#endif

#include "nt.hpp"
#include "win32.hpp"

#include <string.h>
#include <winioctl.h>

#include <atomic>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      template<class R>
      status<R> fail(R r, DWORD err) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_WIN32, static_cast<int32_t>(err)};
      }

      template<class R>
      status<R> fail(R r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      HANDLE as_handle(native_file_t n) noexcept
      {
        return static_cast<HANDLE>(n);
      }

      bool is_io_error(DWORD e) noexcept
      {
        return e == ERROR_CRC || e == ERROR_IO_DEVICE || e == ERROR_READ_FAULT
               || e == ERROR_WRITE_FAULT || e == ERROR_GEN_FAILURE
               || e == ERROR_SECTOR_NOT_FOUND || e == ERROR_DEVICE_HARDWARE_ERROR
               || e == ERROR_NOT_READY;
      }

      bool is_no_space(DWORD e) noexcept
      {
        return e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL
               || e == ERROR_DISK_QUOTA_EXCEEDED;
      }

      /*****************************/
      /* EVENT POOL                */
      /*****************************/

      constexpr size_t POOL_WORDS = (detail::FILE_EVENT_POOL_SIZE + 63) / 64;

      HANDLE g_events[detail::FILE_EVENT_POOL_SIZE];
      std::atomic<uint64_t> g_busy[POOL_WORDS];

      /// @brief Bits of word `w` that are pool slots.
      uint64_t word_mask(size_t w) noexcept
      {
        const size_t left = detail::FILE_EVENT_POOL_SIZE - w * 64;
        return left >= 64 ? ~uint64_t{0} : (uint64_t{1} << left) - 1;
      }

      /// @brief The event of an OVERLAPPED: low bit set, no completion packet.
      HANDLE tagged(HANDLE e) noexcept
      {
        return reinterpret_cast<HANDLE>(reinterpret_cast<uintptr_t>(e) | 1);
      }

      /// @brief An OVERLAPPED with a pooled event, for one call.
      struct io_call
      {
        OVERLAPPED ov{};
        detail::file_event ev{};

        explicit io_call(uint64_t offset) noexcept
        {
          ov.Offset     = static_cast<DWORD>(offset);
          ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
          ev            = detail::file_event_acquire();
          ov.hEvent     = tagged(static_cast<HANDLE>(ev.handle));
        }
        ~io_call() { detail::file_event_release(ev); }
        io_call(const io_call&)            = delete;
        io_call& operator=(const io_call&) = delete;

        bool ready() const noexcept { return ev.handle != nullptr; }

        /// @brief Completes a started call: `started` is what ReadFile,
        /// WriteFile or DeviceIoControl returned.
        /// @return 0 on success, else the error
        DWORD finish(HANDLE h, BOOL started, DWORD* bytes) noexcept
        {
          if (!started)
          {
            const DWORD e = ::GetLastError();
            if (e != ERROR_IO_PENDING)
              return e;
            ::WaitForSingleObject(static_cast<HANDLE>(ev.handle), INFINITE);
          }
          if (!::GetOverlappedResult(h, &ov, bytes, FALSE))
            return ::GetLastError();
          return 0;
        }
      };

      /*****************************/
      /* NATIVE QUERIES            */
      /*****************************/

      constexpr ULONG FILE_NO_INTERMEDIATE_BUFFERING_FLAG = 0x00000008;

      int64_t filetime_ns(FILETIME ft) noexcept
      {
        const uint64_t t =
            (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        // 100 ns units since 1601-01-01
        return (static_cast<int64_t>(t) - 116444736000000000LL) * 100;
      }

      bool starts_with(const wchar_t* s, const wchar_t* prefix) noexcept
      {
        for (; *prefix != L'\0'; ++s, ++prefix)
          if (*s != *prefix)
            return false;
        return true;
      }

      bool is_sep(char c) noexcept
      {
        return c == '\\' || c == '/';
      }
    } // namespace

    namespace detail
    {
      nt_api g_nt{};

      /*****************************/
      /* EVENTS                    */
      /*****************************/

      file_event file_event_acquire() noexcept
      {
        for (size_t w = 0; w < POOL_WORDS; ++w)
        {
          const uint64_t mask = word_mask(w);
          uint64_t busy       = g_busy[w].load(std::memory_order_relaxed);
          while ((busy & mask) != mask)
          {
            const uint64_t free_bits = ~busy & mask;
            const uint64_t bit       = free_bits & (~free_bits + 1);
            if (g_busy[w].compare_exchange_weak(
                    busy, busy | bit, std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
              unsigned index = 0;
              while ((uint64_t{1} << index) != bit)
                ++index;
              const size_t slot = w * 64 + index;
              if (g_events[slot] == nullptr)
              {
                g_events[slot] = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (g_events[slot] == nullptr)
                {
                  const DWORD e = ::GetLastError();
                  g_busy[w].fetch_and(~bit, std::memory_order_release);
                  ::SetLastError(e);
                  return {nullptr, UINT32_MAX};
                }
              }
              return {g_events[slot], static_cast<uint32_t>(slot)};
            }
          }
        }
        // Pool exhausted: a temporary event (a kernel object, not memory)
        return {::CreateEventW(nullptr, TRUE, FALSE, nullptr), UINT32_MAX};
      }

      void file_event_release(file_event e) noexcept
      {
        if (e.handle == nullptr)
          return;
        if (e.slot == UINT32_MAX)
        {
          ::CloseHandle(static_cast<HANDLE>(e.handle));
          return;
        }
        g_busy[e.slot / 64].fetch_and(
            ~(uint64_t{1} << (e.slot % 64)), std::memory_order_release);
      }

      void platform_file_initialize(size_t* page, size_t* granularity) noexcept
      {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        *page = si.dwPageSize != 0 ? si.dwPageSize : 4096;
        *granularity =
            si.dwAllocationGranularity != 0 ? si.dwAllocationGranularity : 65536;
        if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"))
        {
          g_nt.query_information_file =
              reinterpret_cast<nt_query_information_file_fn>(reinterpret_cast<void*>(
                  ::GetProcAddress(ntdll, "NtQueryInformationFile")));
          g_nt.create_file = reinterpret_cast<nt_create_file_fn>(
              reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtCreateFile")));
          g_nt.set_information_file =
              reinterpret_cast<nt_set_information_file_fn>(reinterpret_cast<void*>(
                  ::GetProcAddress(ntdll, "NtSetInformationFile")));
        }
      }

      void platform_file_deinitialize() noexcept
      {
        for (size_t i = 0; i < FILE_EVENT_POOL_SIZE; ++i)
        {
          if (g_events[i] != nullptr)
            ::CloseHandle(g_events[i]);
          g_events[i] = nullptr;
        }
        for (size_t w = 0; w < POOL_WORDS; ++w)
          g_busy[w].store(0, std::memory_order_relaxed);
      }

      /*****************************/
      /* PATHS                     */
      /*****************************/

      status<file_open_result> windows_file_path(
          const char* path, size_t size, wchar_t* out, size_t capacity,
          bool* verbatim) noexcept
      {
        using R   = file_open_result;
        *verbatim = false;
        // \\?\ exactly: verbatim, passed through
        if (size >= 4 && path[0] == '\\' && path[1] == '\\' && path[2] == '?'
            && path[3] == '\\')
          *verbatim = true;
        else if (size >= 2 && is_sep(path[0]) && is_sep(path[1]))
        {
          // \\.\ and other device forms are refused; \\server\share is absolute
          if (size >= 3 && (path[2] == '.' || path[2] == '?')
              && (size == 3 || is_sep(path[3])))
            return fail(R::FILE_OPEN_NOT_A_FILE);
        }
        else if (!(size >= 3
                   && ((path[0] >= 'A' && path[0] <= 'Z')
                       || (path[0] >= 'a' && path[0] <= 'z'))
                   && path[1] == ':' && is_sep(path[2])))
          return fail(R::FILE_OPEN_PATH_NOT_ABSOLUTE);

        wchar_t wide[MAX_PATH_BYTES + 1];
        const int n = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, path, static_cast<int>(size), wide,
            static_cast<int>(MAX_PATH_BYTES));
        if (n <= 0)
          return fail(R::FILE_OPEN_INVALID_PATH, ::GetLastError());
        wide[n] = L'\0';

        if (*verbatim)
        {
          if (static_cast<size_t>(n) + 1 > capacity)
            return fail(R::FILE_OPEN_PATH_TOO_LONG);
          for (int i = 0; i <= n; ++i)
            out[i] = wide[i];
          return {};
        }

        // Room for the longest prefix, \\?\UNC\ (8 units, replacing 2)
        constexpr size_t PREFIX = 8;
        if (capacity <= PREFIX)
          return fail(R::FILE_OPEN_PATH_TOO_LONG);
        wchar_t* full    = out + PREFIX;
        const DWORD room = static_cast<DWORD>(capacity - PREFIX);
        const DWORD len  = ::GetFullPathNameW(wide, room, full, nullptr);
        if (len == 0)
          return fail(R::FILE_OPEN_INVALID_PATH, ::GetLastError());
        if (len >= room)
          return fail(R::FILE_OPEN_PATH_TOO_LONG);

        // Reserved device names (NUL, CON, ...) come back as \\.\NAME
        if (starts_with(full, L"\\\\.\\") || starts_with(full, L"\\\\?\\"))
          return fail(R::FILE_OPEN_NOT_A_FILE);
        if (starts_with(full, L"\\\\"))
        {
          // \\server\share\x -> \\?\UNC\server\share\x: the 7 units "\\?\UNC"
          // replace the first '\' of full (which sits at out[PREFIX])
          static constexpr wchar_t unc[] = L"\\\\?\\UNC";
          for (size_t i = 0; i < 7; ++i)
            out[PREFIX - 6 + i] = unc[i];
          const size_t total = 7 + (len - 1);
          for (size_t i = 0; i <= total; ++i)
            out[i] = out[PREFIX - 6 + i];
          return {};
        }
        // C:\x -> \\?\C:\x
        static constexpr wchar_t vb[] = L"\\\\?\\";
        for (size_t i = 0; i < 4; ++i)
          out[PREFIX - 4 + i] = vb[i];
        for (size_t i = 0; i <= len + 4; ++i)
          out[i] = out[PREFIX - 4 + i];
        return {};
      }

      /*****************************/
      /* FILES                     */
      /*****************************/

      status<file_open_result> platform_file_open(
          const char* path, size_t size, const file_open_desc& desc,
          native_file_t* out) noexcept
      {
        using R = file_open_result;
        wchar_t wpath[MAX_PATH_BYTES + 16];
        bool verbatim = false;
        const status<R> conv =
            windows_file_path(path, size, wpath, MAX_PATH_BYTES + 16, &verbatim);
        if (!conv.ok())
          return conv;

        DWORD access = 0;
        switch (desc.access)
        {
        case file_access::FILE_ACCESS_READ:
          access = GENERIC_READ;
          break;
        case file_access::FILE_ACCESS_WRITE:
          access = GENERIC_WRITE;
          break;
        case file_access::FILE_ACCESS_READ_WRITE:
          access = GENERIC_READ | GENERIC_WRITE;
          break;
        }
        DWORD disposition = OPEN_EXISTING;
        switch (desc.disposition)
        {
        case file_disposition::FILE_DISPOSITION_OPEN_EXISTING:
          disposition = OPEN_EXISTING;
          break;
        case file_disposition::FILE_DISPOSITION_CREATE_NEW:
          disposition = CREATE_NEW;
          break;
        case file_disposition::FILE_DISPOSITION_OPEN_OR_CREATE:
          disposition = OPEN_ALWAYS;
          break;
        case file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE:
          disposition = CREATE_ALWAYS;
          break;
        case file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING:
          disposition = TRUNCATE_EXISTING;
          break;
        }
        DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;
        if (desc.unbuffered)
          flags |= FILE_FLAG_NO_BUFFERING;
        if (desc.pattern == file_pattern::FILE_PATTERN_SEQUENTIAL)
          flags |= FILE_FLAG_SEQUENTIAL_SCAN;
        else if (desc.pattern == file_pattern::FILE_PATTERN_RANDOM)
          flags |= FILE_FLAG_RANDOM_ACCESS;

        const HANDLE h = ::CreateFileW(
            wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, disposition, flags, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
          const DWORD e = ::GetLastError();
          switch (e)
          {
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
          case ERROR_BAD_NETPATH:
          case ERROR_BAD_NET_NAME:
            return fail(R::FILE_OPEN_NOT_FOUND, e);
          case ERROR_FILE_EXISTS:
          case ERROR_ALREADY_EXISTS:
            return fail(R::FILE_OPEN_ALREADY_EXISTS, e);
          case ERROR_ACCESS_DENIED:
          {
            // Opening a directory fails this way too
            const DWORD attr = ::GetFileAttributesW(wpath);
            if (attr != INVALID_FILE_ATTRIBUTES
                && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
              return fail(R::FILE_OPEN_NOT_A_FILE, e);
            return fail(R::FILE_OPEN_ACCESS_DENIED, e);
          }
          case ERROR_WRITE_PROTECT:
            return fail(R::FILE_OPEN_ACCESS_DENIED, e);
          case ERROR_SHARING_VIOLATION:
          case ERROR_LOCK_VIOLATION:
          case ERROR_USER_MAPPED_FILE:
            return fail(R::FILE_OPEN_BUSY, e);
          case ERROR_INVALID_NAME:
          case ERROR_BAD_PATHNAME:
          case ERROR_DIRECTORY:
            return fail(R::FILE_OPEN_INVALID_PATH, e);
          case ERROR_FILENAME_EXCED_RANGE:
            return fail(R::FILE_OPEN_PATH_TOO_LONG, e);
          case ERROR_TOO_MANY_OPEN_FILES:
            return fail(R::FILE_OPEN_TOO_MANY_OPEN, e);
          case ERROR_DISK_FULL:
          case ERROR_HANDLE_DISK_FULL:
          case ERROR_DISK_QUOTA_EXCEEDED:
            return fail(R::FILE_OPEN_NO_SPACE, e);
          case ERROR_INVALID_PARAMETER:
          case ERROR_NOT_SUPPORTED:
            if (desc.unbuffered)
              return fail(R::FILE_OPEN_UNSUPPORTED, e);
            return fail(R::FILE_OPEN_SYSTEM_ERROR, e);
          default:
            return fail(R::FILE_OPEN_SYSTEM_ERROR, e);
          }
        }
        // A verbatim path can name a device: only disk files are accepted
        if (verbatim)
        {
          BY_HANDLE_FILE_INFORMATION bhfi{};
          if (::GetFileType(h) != FILE_TYPE_DISK
              || (::GetFileInformationByHandle(h, &bhfi)
                  && (bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
          {
            ::CloseHandle(h);
            return fail(R::FILE_OPEN_NOT_A_FILE);
          }
        }
        *out = h;
        return {};
      }

      void platform_file_close(native_file_t native) noexcept
      {
        ::CloseHandle(as_handle(native));
      }

      status<file_adopt_result> platform_file_adopt(
          native_file_t native, bool* unbuffered) noexcept
      {
        using R          = file_adopt_result;
        const HANDLE h   = as_handle(native);
        const DWORD type = ::GetFileType(h);
        if (type == FILE_TYPE_UNKNOWN && ::GetLastError() != NO_ERROR)
          return fail(R::FILE_ADOPT_SYSTEM_ERROR, ::GetLastError());
        if (type != FILE_TYPE_DISK)
          return fail(R::FILE_ADOPT_NOT_A_FILE);
        BY_HANDLE_FILE_INFORMATION bhfi{};
        if (!::GetFileInformationByHandle(h, &bhfi))
          return fail(R::FILE_ADOPT_SYSTEM_ERROR, ::GetLastError());
        if ((bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
          return fail(R::FILE_ADOPT_NOT_A_FILE);
        if (unbuffered != nullptr)
        {
          *unbuffered = false;
          ULONG mode  = 0;
          nt_io_status_block iosb{};
          if (g_nt.query_information_file != nullptr
              && g_nt.query_information_file(
                     h, &iosb, &mode, sizeof(mode), NT_FILE_MODE_INFORMATION)
                     >= 0)
            *unbuffered = (mode & FILE_NO_INTERMEDIATE_BUFFERING_FLAG) != 0;
        }
        return {};
      }

      status<file_read_result> platform_file_read(
          native_file_t native, uint64_t offset, void* buffer, size_t size,
          size_t* transferred) noexcept
      {
        using R = file_read_result;
        io_call call(offset);
        if (!call.ready())
          return fail(R::FILE_READ_SYSTEM_ERROR, ::GetLastError());
        DWORD n        = 0;
        const HANDLE h = as_handle(native);
        const BOOL start =
            ::ReadFile(h, buffer, static_cast<DWORD>(size), nullptr, &call.ov);
        const DWORD e = call.finish(h, start, &n);
        if (e == 0 || e == ERROR_HANDLE_EOF)
        {
          *transferred = e == 0 ? n : 0;
          return {};
        }
        if (is_io_error(e))
          return fail(R::FILE_READ_IO_ERROR, e);
        if (e == ERROR_INVALID_PARAMETER)
          return fail(R::FILE_READ_MISALIGNED, e);
        return fail(R::FILE_READ_SYSTEM_ERROR, e);
      }

      status<file_write_result> platform_file_write(
          native_file_t native, uint64_t offset, const void* buffer, size_t size,
          size_t* transferred) noexcept
      {
        using R = file_write_result;
        io_call call(offset);
        if (!call.ready())
          return fail(R::FILE_WRITE_SYSTEM_ERROR, ::GetLastError());
        DWORD n        = 0;
        const HANDLE h = as_handle(native);
        const BOOL start =
            ::WriteFile(h, buffer, static_cast<DWORD>(size), nullptr, &call.ov);
        const DWORD e = call.finish(h, start, &n);
        if (e == 0)
        {
          *transferred = n;
          return {};
        }
        if (is_no_space(e))
          return fail(R::FILE_WRITE_NO_SPACE, e);
        if (e == ERROR_FILE_TOO_LARGE)
          return fail(R::FILE_WRITE_FILE_TOO_LARGE, e);
        if (is_io_error(e))
          return fail(R::FILE_WRITE_IO_ERROR, e);
        if (e == ERROR_INVALID_PARAMETER)
          return fail(R::FILE_WRITE_MISALIGNED, e);
        return fail(R::FILE_WRITE_SYSTEM_ERROR, e);
      }

      status<file_flush_result> platform_file_flush(native_file_t native) noexcept
      {
        using R = file_flush_result;
        if (::FlushFileBuffers(as_handle(native)))
          return {};
        const DWORD e = ::GetLastError();
        if (is_no_space(e))
          return fail(R::FILE_FLUSH_NO_SPACE, e);
        if (is_io_error(e))
          return fail(R::FILE_FLUSH_IO_ERROR, e);
        return fail(R::FILE_FLUSH_SYSTEM_ERROR, e);
      }

      status<file_info_result> platform_file_info(
          native_file_t native, file_info* out) noexcept
      {
        const HANDLE h = as_handle(native);
        BY_HANDLE_FILE_INFORMATION bhfi{};
        if (!::GetFileInformationByHandle(h, &bhfi))
          return fail(file_info_result::FILE_INFO_SYSTEM_ERROR, ::GetLastError());
        out->size =
            (static_cast<uint64_t>(bhfi.nFileSizeHigh) << 32) | bhfi.nFileSizeLow;
        out->last_write = wall_instant{filetime_ns(bhfi.ftLastWriteTime)};
        FILE_ID_INFO id{};
        if (::GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)))
        {
          uint64_t lo = 0;
          uint64_t hi = 0;
          static_assert(sizeof(id.FileId.Identifier) == 16);
          ::memcpy(&lo, id.FileId.Identifier, 8);
          ::memcpy(&hi, id.FileId.Identifier + 8, 8);
          out->identity = file_identity{id.VolumeSerialNumber, lo, hi};
        }
        else // FAT and other filesystems without 128-bit ids
          out->identity = file_identity{
              bhfi.dwVolumeSerialNumber,
              (static_cast<uint64_t>(bhfi.nFileIndexHigh) << 32)
                  | bhfi.nFileIndexLow,
              0};
        return {};
      }

      status<file_set_size_result> platform_file_set_size(
          native_file_t native, uint64_t size, file_size_mode mode) noexcept
      {
        using R        = file_set_size_result;
        const HANDLE h = as_handle(native);
        auto error     = [](DWORD e) noexcept -> status<R>
        {
          if (is_no_space(e))
            return fail(R::FILE_SET_SIZE_NO_SPACE, e);
          if (e == ERROR_FILE_TOO_LARGE)
            return fail(R::FILE_SET_SIZE_FILE_TOO_LARGE, e);
          if (e == ERROR_USER_MAPPED_FILE)
            return fail(R::FILE_SET_SIZE_MAPPED, e);
          if (e == ERROR_CALL_NOT_IMPLEMENTED || e == ERROR_NOT_SUPPORTED)
            return fail(R::FILE_SET_SIZE_UNSUPPORTED, e);
          if (is_io_error(e))
            return fail(R::FILE_SET_SIZE_IO_ERROR, e);
          return fail(R::FILE_SET_SIZE_SYSTEM_ERROR, e);
        };
        if (mode == file_size_mode::FILE_SIZE_MODE_RESERVE)
        {
          LARGE_INTEGER current{};
          if (!::GetFileSizeEx(h, &current))
            return error(::GetLastError());
          if (size > static_cast<uint64_t>(current.QuadPart))
          {
            FILE_ALLOCATION_INFO alloc{};
            alloc.AllocationSize.QuadPart = static_cast<LONGLONG>(size);
            if (!::SetFileInformationByHandle(
                    h, FileAllocationInfo, &alloc, sizeof(alloc)))
              return error(::GetLastError());
          }
        }
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        if (!::SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof)))
          return error(::GetLastError());
        return {};
      }

      status<file_alignment_result> platform_file_alignment(
          native_file_t native, file_alignment* out) noexcept
      {
        FILE_STORAGE_INFO si{};
        if (!::GetFileInformationByHandleEx(
                as_handle(native), FileStorageInfo, &si, sizeof(si)))
        {
          const DWORD e = ::GetLastError();
          if (e == ERROR_CALL_NOT_IMPLEMENTED || e == ERROR_NOT_SUPPORTED)
            return fail(file_alignment_result::FILE_ALIGNMENT_UNSUPPORTED, e);
          return fail(file_alignment_result::FILE_ALIGNMENT_SYSTEM_ERROR, e);
        }
        uint32_t a = si.LogicalBytesPerSector;
        if (si.PhysicalBytesPerSectorForPerformance > a)
          a = si.PhysicalBytesPerSectorForPerformance;
        if (a == 0)
          a = 4096;
        *out = file_alignment{a, a};
        return {};
      }

      bool platform_file_has_hole(
          native_file_t native, uint64_t offset, size_t size) noexcept
      {
        // Only sparse files have holes; osprim never makes files sparse
        const HANDLE h = as_handle(native);
        BY_HANDLE_FILE_INFORMATION bhfi{};
        if (!::GetFileInformationByHandle(h, &bhfi)
            || (bhfi.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0)
          return false;
        FILE_ALLOCATED_RANGE_BUFFER query{};
        query.FileOffset.QuadPart = static_cast<LONGLONG>(offset);
        query.Length.QuadPart     = static_cast<LONGLONG>(size);
        FILE_ALLOCATED_RANGE_BUFFER ranges[16]{};
        io_call call(0);
        if (!call.ready())
          return false;
        DWORD bytes      = 0;
        const BOOL start = ::DeviceIoControl(
            h, FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query), ranges,
            sizeof(ranges), nullptr, &call.ov);
        const DWORD e = call.finish(h, start, &bytes);
        if (e != 0 && e != ERROR_MORE_DATA)
          return false;
        uint64_t pos       = offset;
        const uint64_t end = offset + size;
        const size_t count = bytes / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        for (size_t i = 0; i < count && pos < end; ++i)
        {
          const uint64_t s = static_cast<uint64_t>(ranges[i].FileOffset.QuadPart);
          if (s > pos)
            return true;
          const uint64_t r_end =
              s + static_cast<uint64_t>(ranges[i].Length.QuadPart);
          if (r_end > pos)
            pos = r_end;
        }
        return pos < end && e != ERROR_MORE_DATA;
      }

      /*****************************/
      /* REGIONS                   */
      /*****************************/

      status<region_map_result> platform_region_map(
          native_file_t native, uint64_t aligned_offset, size_t length, uint64_t end,
          region_access access, void** base) noexcept
      {
        using R        = region_map_result;
        const HANDLE h = as_handle(native);
        LARGE_INTEGER fs{};
        if (!::GetFileSizeEx(h, &fs))
          return fail(R::REGION_MAP_SYSTEM_ERROR, ::GetLastError());
        if (end > static_cast<uint64_t>(fs.QuadPart))
          return fail(R::REGION_MAP_OUT_OF_RANGE);

        DWORD protect = PAGE_READONLY;
        DWORD map     = FILE_MAP_READ;
        if (access == region_access::REGION_ACCESS_READ_WRITE)
        {
          protect = PAGE_READWRITE;
          map     = FILE_MAP_WRITE;
        }
        else if (access == region_access::REGION_ACCESS_COPY_ON_WRITE)
        {
          protect = PAGE_WRITECOPY;
          map     = FILE_MAP_COPY;
        }
        auto error = [](DWORD e) noexcept -> status<R>
        {
          if (e == ERROR_ACCESS_DENIED)
            return fail(R::REGION_MAP_ACCESS_DENIED, e);
          if (e == ERROR_NOT_ENOUGH_MEMORY || e == ERROR_COMMITMENT_LIMIT
              || e == ERROR_NOT_ENOUGH_QUOTA)
            return fail(R::REGION_MAP_OUT_OF_MEMORY, e);
          return fail(R::REGION_MAP_SYSTEM_ERROR, e);
        };
        // Sized to the end of the view. Windows refuses to shrink the file
        // while any view exists, whatever the sizes (ERROR_USER_MAPPED_FILE).
        const HANDLE mapping = ::CreateFileMappingW(
            h, nullptr, protect, static_cast<DWORD>(end >> 32),
            static_cast<DWORD>(end), nullptr);
        if (mapping == nullptr)
          return error(::GetLastError());
        void* p = ::MapViewOfFile(
            mapping, map, static_cast<DWORD>(aligned_offset >> 32),
            static_cast<DWORD>(aligned_offset), length);
        const DWORD e = p == nullptr ? ::GetLastError() : 0;
        ::CloseHandle(mapping); // the view keeps the mapping alive
        if (p == nullptr)
          return error(e);
        *base = p;
        return {};
      }

      void platform_region_unmap(void* base, size_t) noexcept
      {
        ::UnmapViewOfFile(base);
      }

      status<region_flush_result> platform_region_flush(
          void* address, size_t length) noexcept
      {
        // Pages being written by the system can make the flush fail
        // transiently with ERROR_LOCK_VIOLATION: retried a few times.
        DWORD e = 0;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
          if (::FlushViewOfFile(address, length))
            return {};
          e = ::GetLastError();
          if (e != ERROR_LOCK_VIOLATION)
            break;
          ::SwitchToThread();
        }
        if (is_io_error(e))
          return fail(region_flush_result::REGION_FLUSH_IO_ERROR, e);
        return fail(region_flush_result::REGION_FLUSH_SYSTEM_ERROR, e);
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
