/*****************************************************************/ /**
 * @file   file.cpp
 * @brief  Files and regions: argument checks, path validation, tracking
 * and its Debug checks. The OS calls live in file_posix.cpp and
 * file_windows.cpp.
 *
 * Tracking metadata:
 * - file:   words[0] access (file_access, or ACCESS_UNKNOWN when adopted),
 *           words[1] unbuffered, words[2] memory alignment, words[3] I/O
 *           alignment (both 0 when unknown).
 * - region: words[0] region_access, words[1..3] file identity,
 *           words[4] end of the mapped range in the file.
 *********************************************************************/
#include "internal.hpp"

#include <string.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      size_t g_page_size   = 4096;
      size_t g_granularity = 4096;

      uintptr_t align_down(uintptr_t v, size_t a) noexcept
      {
        return v & ~static_cast<uintptr_t>(a - 1);
      }

#if OSPRIM_TRACKING
      constexpr uint64_t ACCESS_UNKNOWN = 0xff;

      /// @brief The file metadata of `h`, if tracked.
      bool file_meta(file_handle h, detail::track_meta* m) noexcept
      {
        return detail::track_get(
            detail::FILE_TRACK_TYPE, detail::file_key(h.native), m);
      }

      /// @brief Checks access and alignment against the tracked metadata.
      void check_io(
          file_handle h, uint64_t offset, const void* buffer, size_t size,
          bool write) noexcept
      {
        detail::track_meta m{};
        if (!file_meta(h, &m))
          return;
        const uint64_t access = m.words[0];
        (void)access; // only read by OSPRIM_ASSERT, which may be compiled out
        if (write)
          OSPRIM_ASSERT(
              access != static_cast<uint64_t>(file_access::FILE_ACCESS_READ),
              "osprim::file_write_at: handle not opened for writing");
        else
          OSPRIM_ASSERT(
              access != static_cast<uint64_t>(file_access::FILE_ACCESS_WRITE),
              "osprim::file_read_at: handle not opened for reading");
        if (m.words[1] != 0 && m.words[2] != 0 && m.words[3] != 0)
        {
          const bool aligned =
              offset % m.words[3] == 0 && size % m.words[3] == 0
              && reinterpret_cast<uintptr_t>(buffer) % m.words[2] == 0;
          OSPRIM_ASSERT(
              aligned, "osprim::file_read_at/file_write_at: unbuffered handle: "
                       "offset, size and "
                       "buffer must follow file_unbuffered_alignment()");
          (void)aligned;
        }
      }

      /// @brief File metadata for a newly tracked handle.
      detail::track_meta make_file_meta(
          native_file_t native, uint64_t access, bool unbuffered) noexcept
      {
        detail::track_meta m{};
        m.words[0] = access;
        m.words[1] = unbuffered ? 1 : 0;
        file_alignment a{};
        if (unbuffered && detail::platform_file_alignment(native, &a).ok())
        {
          m.words[2] = a.memory;
          m.words[3] = a.io;
        }
        return m;
      }

      struct shrink_query
      {
        file_identity identity;
        uint64_t size;
      };

      bool region_beyond(const detail::track_meta& m, const void* ctx) noexcept
      {
        const auto* q = static_cast<const shrink_query*>(ctx);
        return m.words[1] == q->identity.volume && m.words[2] == q->identity.id_low
               && m.words[3] == q->identity.id_high && m.words[4] > q->size;
      }
#endif
    } // namespace

    namespace detail
    {
      bool utf8_valid(const char* p, size_t size) noexcept
      {
        const auto* s = reinterpret_cast<const unsigned char*>(p);
        size_t i      = 0;
        while (i < size)
        {
          const unsigned c = s[i];
          if (c == 0)
            return false;
          if (c < 0x80)
          {
            ++i;
            continue;
          }
          size_t n    = 0;
          unsigned lo = 0x80;
          unsigned hi = 0xbf;
          if (c >= 0xc2 && c <= 0xdf)
            n = 1;
          else if (c >= 0xe0 && c <= 0xef)
          {
            n = 2;
            if (c == 0xe0)
              lo = 0xa0; // overlong
            else if (c == 0xed)
              hi = 0x9f; // surrogates
          }
          else if (c >= 0xf0 && c <= 0xf4)
          {
            n = 3;
            if (c == 0xf0)
              lo = 0x90; // overlong
            else if (c == 0xf4)
              hi = 0x8f; // above U+10FFFF
          }
          else
            return false;
          if (size - i - 1 < n)
            return false;
          for (size_t k = 1; k <= n; ++k)
          {
            const unsigned t = s[i + k];
            if (k == 1 ? (t < lo || t > hi) : (t < 0x80 || t > 0xbf))
              return false;
          }
          i += n + 1;
        }
        return true;
      }

      void file_initialize() noexcept
      {
        platform_file_initialize(&g_page_size, &g_granularity);
      }

      void file_deinitialize() noexcept
      {
        platform_file_deinitialize();
      }

      size_t page_size() noexcept
      {
        return g_page_size;
      }
    } // namespace detail

    /*****************************/
    /* FILES                     */
    /*****************************/

    status<file_open_result> file_open(
        const char* path, size_t path_size, const file_open_desc& desc,
        file_handle& out, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          path != nullptr && path_size > 0, "osprim::file_open: empty path");
      OSPRIM_ASSERT(
          desc.access != file_access::FILE_ACCESS_READ
              || (desc.disposition
                      != file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE
                  && desc.disposition
                         != file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING),
          "osprim::file_open: truncating needs write access");
      if (path_size > MAX_PATH_BYTES)
        return {
            file_open_result::FILE_OPEN_PATH_TOO_LONG,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      if (!detail::utf8_valid(path, path_size))
        return {
            file_open_result::FILE_OPEN_INVALID_PATH,
            native_error_kind::NATIVE_ERROR_NONE, 0};

      native_file_t native{};
      const status<file_open_result> s =
          detail::platform_file_open(path, path_size, desc, &native);
      if (!s.ok())
        return s;
      out.native = native;
#if OSPRIM_TRACKING
      const detail::track_meta m = make_file_meta(
          native, static_cast<uint64_t>(desc.access), desc.unbuffered);
      detail::track_open(detail::FILE_TRACK_TYPE, detail::file_key(native), loc, &m);
#else
      detail::track_open(detail::FILE_TRACK_TYPE, detail::file_key(native), loc);
#endif
      return s;
    }

    void file_close(file_handle h, std::source_location loc) noexcept
    {
      detail::track_close(detail::FILE_TRACK_TYPE, detail::file_key(h.native), loc);
      detail::platform_file_close(h.native);
    }

    status<file_adopt_result> file_adopt(
        native_file_t native, file_handle& out, std::source_location loc) noexcept
    {
      bool unbuffered                   = false;
      const status<file_adopt_result> s = detail::platform_file_adopt(
          native, tracking_compiled ? &unbuffered : nullptr);
      if (!s.ok())
        return s;
      out.native = native;
#if OSPRIM_TRACKING
      const detail::track_meta m =
          make_file_meta(native, ACCESS_UNKNOWN, unbuffered);
      detail::track_open(detail::FILE_TRACK_TYPE, detail::file_key(native), loc, &m);
#else
      (void)unbuffered;
      detail::track_open(detail::FILE_TRACK_TYPE, detail::file_key(native), loc);
#endif
      return s;
    }

    native_file_t file_release(file_handle h, std::source_location loc) noexcept
    {
      detail::track_close(detail::FILE_TRACK_TYPE, detail::file_key(h.native), loc);
      return h.native;
    }

    status<file_read_result> file_read_at(
        file_handle h, uint64_t offset, void* buffer, size_t size,
        size_t& transferred) noexcept
    {
      OSPRIM_ASSERT(
          size <= FILE_IO_MAX, "osprim::file_read_at: size above FILE_IO_MAX");
      OSPRIM_ASSERT(
          offset <= static_cast<uint64_t>(INT64_MAX) - size,
          "osprim::file_read_at: offset + size above INT64_MAX");
      OSPRIM_ASSERT(
          buffer != nullptr || size == 0, "osprim::file_read_at: null buffer");
#if OSPRIM_TRACKING
      check_io(h, offset, buffer, size, false);
#endif
      transferred = 0;
      return detail::platform_file_read(
          h.native, offset, buffer, size, &transferred);
    }

    status<file_write_result> file_write_at(
        file_handle h, uint64_t offset, const void* buffer, size_t size,
        size_t& transferred) noexcept
    {
      OSPRIM_ASSERT(
          size <= FILE_IO_MAX, "osprim::file_write_at: size above FILE_IO_MAX");
      OSPRIM_ASSERT(
          offset <= static_cast<uint64_t>(INT64_MAX) - size,
          "osprim::file_write_at: offset + size above INT64_MAX");
      OSPRIM_ASSERT(
          buffer != nullptr || size == 0, "osprim::file_write_at: null buffer");
#if OSPRIM_TRACKING
      check_io(h, offset, buffer, size, true);
#endif
      transferred = 0;
      return detail::platform_file_write(
          h.native, offset, buffer, size, &transferred);
    }

    status<file_flush_result> file_flush(file_handle h) noexcept
    {
#if OSPRIM_TRACKING
      detail::track_meta m{};
      if (file_meta(h, &m))
        OSPRIM_ASSERT(
            m.words[0] != static_cast<uint64_t>(file_access::FILE_ACCESS_READ),
            "osprim::file_flush: handle not opened for writing");
#endif
      return detail::platform_file_flush(h.native);
    }

    status<file_info_result> file_info_of(file_handle h, file_info& out) noexcept
    {
      return detail::platform_file_info(h.native, &out);
    }

    status<file_set_size_result> file_set_size(
        file_handle h, uint64_t size, file_size_mode mode,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          size <= static_cast<uint64_t>(INT64_MAX),
          "osprim::file_set_size: size above INT64_MAX");
#if OSPRIM_TRACKING
      detail::track_meta m{};
      if (file_meta(h, &m))
        OSPRIM_ASSERT(
            m.words[0] != static_cast<uint64_t>(file_access::FILE_ACCESS_READ),
            "osprim::file_set_size: handle not opened for writing");
      // Shrinking below a live region of the same file
      file_info info{};
      if (detail::tracked_count() != 0
          && detail::platform_file_info(h.native, &info).ok() && size < info.size)
      {
        const shrink_query q{info.identity, size};
        uint64_t region_key = 0;
        std::source_location created{};
        if (detail::track_find(
                detail::REGION_TRACK_TYPE, &region_beyond, &q, &region_key,
                &created))
          detail::report(
              diagnostic{
                  diagnostic_kind::DIAGNOSTIC_REGION_TRUNCATED,
                  detail::REGION_TRACK_TYPE, region_key, created, loc});
      }
#else
      (void)loc;
#endif
      return detail::platform_file_set_size(h.native, size, mode);
    }

    status<file_alignment_result> file_unbuffered_alignment(
        file_handle h, file_alignment& out) noexcept
    {
      return detail::platform_file_alignment(h.native, &out);
    }

    /*****************************/
    /* REGIONS                   */
    /*****************************/

    size_t region_granularity() noexcept
    {
      return g_granularity;
    }

    status<region_map_result> region_map(
        file_handle file, uint64_t offset, size_t size, region_access access,
        region& out, std::source_location loc) noexcept
    {
      if (size == 0)
        return {
            region_map_result::REGION_MAP_EMPTY,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      if (offset > static_cast<uint64_t>(INT64_MAX) - size)
        return {
            region_map_result::REGION_MAP_OUT_OF_RANGE,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      const uint64_t aligned = offset & ~static_cast<uint64_t>(g_granularity - 1);
      const size_t delta     = static_cast<size_t>(offset - aligned);
      if (size > SIZE_MAX - delta)
        return {
            region_map_result::REGION_MAP_OUT_OF_MEMORY,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      void* base                        = nullptr;
      const status<region_map_result> s = detail::platform_region_map(
          file.native, aligned, delta + size, offset + size, access, &base);
      if (!s.ok())
        return s;
      out.data = static_cast<unsigned char*>(base) + delta;
      out.size = size;

#if OSPRIM_TRACKING
      detail::track_meta m{};
      m.words[0] = static_cast<uint64_t>(access);
      m.words[4] = offset + size;
      file_info info{};
      if (detail::platform_file_info(file.native, &info).ok())
      {
        m.words[1] = info.identity.volume;
        m.words[2] = info.identity.id_low;
        m.words[3] = info.identity.id_high;
      }
      const uint64_t key =
          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out.data));
      detail::track_open(detail::REGION_TRACK_TYPE, key, loc, &m);
      if (access == region_access::REGION_ACCESS_READ_WRITE
          && detail::tracked_count() != 0
          && detail::platform_file_has_hole(file.native, offset, size))
        detail::report(
            diagnostic{
                diagnostic_kind::DIAGNOSTIC_REGION_OVER_HOLE,
                detail::REGION_TRACK_TYPE, key, loc, loc});
#else
      (void)loc;
#endif
      return s;
    }

    void region_unmap(region r, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          r.data != nullptr && r.size != 0,
          "osprim::region_unmap: not a mapped region");
      detail::track_close(
          detail::REGION_TRACK_TYPE,
          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(r.data)), loc);
      const uintptr_t data = reinterpret_cast<uintptr_t>(r.data);
      const uintptr_t base = align_down(data, g_granularity);
      detail::platform_region_unmap(
          reinterpret_cast<void*>(base), r.size + (data - base));
    }

    status<region_flush_result> region_flush(
        region r, size_t offset, size_t size) noexcept
    {
      OSPRIM_ASSERT(
          offset <= r.size && size <= r.size - offset,
          "osprim::region_flush: range outside the region");
#if OSPRIM_TRACKING
      detail::track_meta m{};
      if (detail::track_get(
              detail::REGION_TRACK_TYPE,
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(r.data)), &m))
        OSPRIM_ASSERT(
            m.words[0]
                == static_cast<uint64_t>(region_access::REGION_ACCESS_READ_WRITE),
            "osprim::region_flush: not a READ_WRITE region");
#endif
      if (size == 0)
        return {};
      const uintptr_t first = reinterpret_cast<uintptr_t>(r.data) + offset;
      const uintptr_t start = align_down(first, g_page_size);
      return detail::platform_region_flush(
          reinterpret_cast<void*>(start), size + (first - start));
    }
  } // namespace v0_1
} // namespace osprim
