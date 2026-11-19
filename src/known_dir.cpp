/*****************************************************************/ /**
 * @file   known_dir.cpp
 * @brief  Known directories: the table filled by initialize() (overrides
 * and the platform's snapshot), queries, same_as, creation of missing
 * components. The platform parts live in known_dir_posix.cpp and
 * known_dir_windows.cpp.
 *
 * same_as compares every other purpose's path as text: osprim's own
 * mappings produce identical text for the purposes they share (one known
 * folder, one root), so the table and the comparison agree.
 *********************************************************************/
#include "internal.hpp"

#include <string.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace detail
    {
      known_dir_table g_known_dirs;

      void known_dir_set(known_dir_slot& s, const char* p, size_t n) noexcept
      {
        if (n > MAX_PATH_BYTES)
        {
          s.set   = false;
          s.error = known_dir_get_result::KNOWN_DIR_GET_PATH_TOO_LONG;
          return;
        }
        ::memcpy(s.path, p, n);
        s.path[n] = '\0';
        s.size    = n;
        s.set     = true;
        s.error   = known_dir_get_result::KNOWN_DIR_GET_OK;
      }

      void known_dir_join(
          known_dir_slot& s, const char* p, size_t n, const char* suffix) noexcept
      {
        const size_t k = ::strlen(suffix);
        if (n > MAX_PATH_BYTES || k > MAX_PATH_BYTES - n)
        {
          s.set   = false;
          s.error = known_dir_get_result::KNOWN_DIR_GET_PATH_TOO_LONG;
          return;
        }
        ::memcpy(s.path, p, n);
        ::memcpy(s.path + n, suffix, k);
        s.path[n + k] = '\0';
        s.size        = n + k;
        s.set         = true;
        s.error       = known_dir_get_result::KNOWN_DIR_GET_OK;
      }

      status<initialize_result> known_dir_initialize(const init_desc& desc) noexcept
      {
        ::memset(&g_known_dirs, 0, sizeof(g_known_dirs));
        const status<initialize_result> invalid{
            initialize_result::INITIALIZE_INVALID_OVERRIDE,
            native_error_kind::NATIVE_ERROR_NONE, 0};
        if (desc.known_dir_override_count != 0
            && desc.known_dir_overrides == nullptr)
          return invalid;
        for (size_t i = 0; i < desc.known_dir_override_count; ++i)
        {
          const known_dir_override& o = desc.known_dir_overrides[i];
          const size_t which          = static_cast<size_t>(o.which);
          bool ok = which < KNOWN_DIR_COUNT && o.path != nullptr && o.path_size != 0
                    && o.path_size <= MAX_PATH_BYTES
                    && utf8_valid(o.path, o.path_size);
          if (ok)
          {
            known_dir_slot& s = g_known_dirs.slots[which];
            size_t n          = 0;
            ok = !s.overridden
                 && platform_known_dir_normalize(o.path, o.path_size, s.path, &n);
            if (ok)
            {
              s.size       = n;
              s.set        = true;
              s.overridden = true;
            }
          }
          if (!ok)
          {
            ::memset(&g_known_dirs, 0, sizeof(g_known_dirs));
            return invalid;
          }
        }
        platform_known_dir_initialize(g_known_dirs);
        return {};
      }

      void known_dir_deinitialize() noexcept
      {
        platform_known_dir_deinitialize();
        ::memset(&g_known_dirs, 0, sizeof(g_known_dirs));
      }
    } // namespace detail

    namespace
    {
      using GR = known_dir_get_result;
      using OR = known_dir_open_result;

      template<class R>
      status<R> fail(R r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      size_t index_of(known_dir d) noexcept
      {
        return static_cast<size_t>(d);
      }

      /// @brief The path of a purpose into `out` (MAX_PATH_BYTES + 1 bytes,
      /// terminated).
      status<GR> resolve(known_dir which, char* out, size_t* size) noexcept
      {
        const detail::known_dir_slot& s =
            detail::g_known_dirs.slots[index_of(which)];
        if (s.set)
        {
          ::memcpy(out, s.path, s.size + 1);
          *size = s.size;
          return {};
        }
        if (s.error != GR::KNOWN_DIR_GET_OK)
          return {s.error, s.native_kind, s.native_code};
        return detail::platform_known_dir_resolve(which, out, size);
      }

      void describe(
          known_dir which, const char* path, size_t size,
          known_dir_info* info) noexcept
      {
        *info = known_dir_info{};
        detail::platform_known_dir_traits(which, detail::g_known_dirs.flags, info);
        if (detail::g_known_dirs.slots[index_of(which)].overridden)
        {
          info->overridden  = true;
          info->app_private = true;
        }
        char other[MAX_PATH_BYTES + 1];
        for (size_t i = 0; i < KNOWN_DIR_COUNT; ++i)
        {
          const known_dir d = static_cast<known_dir>(i);
          size_t n          = 0;
          if (d != which && resolve(d, other, &n).ok()
              && detail::platform_known_dir_same(path, size, other, n))
            info->same_as = static_cast<uint8_t>(info->same_as | known_dir_bit(d));
        }
      }

      status<OR> from_get(status<GR> s) noexcept
      {
        switch (s.result)
        {
        case GR::KNOWN_DIR_GET_UNSUPPORTED:
          return {OR::KNOWN_DIR_OPEN_UNSUPPORTED, s.native_kind, s.native_code};
        case GR::KNOWN_DIR_GET_NOT_CONFIGURED:
          return {OR::KNOWN_DIR_OPEN_NOT_CONFIGURED, s.native_kind, s.native_code};
        case GR::KNOWN_DIR_GET_PATH_TOO_LONG:
          return {OR::KNOWN_DIR_OPEN_PATH_TOO_LONG, s.native_kind, s.native_code};
        default:
          return {OR::KNOWN_DIR_OPEN_SYSTEM_ERROR, s.native_kind, s.native_code};
        }
      }

      status<OR> from_dir_open(status<dir_open_result> s) noexcept
      {
        using D = dir_open_result;
        switch (s.result)
        {
        case D::DIR_OPEN_OK:
          return {};
        case D::DIR_OPEN_NOT_FOUND:
          return {OR::KNOWN_DIR_OPEN_NOT_FOUND, s.native_kind, s.native_code};
        case D::DIR_OPEN_NOT_A_DIRECTORY:
          return {OR::KNOWN_DIR_OPEN_NOT_A_DIRECTORY, s.native_kind, s.native_code};
        case D::DIR_OPEN_ACCESS_DENIED:
          return {OR::KNOWN_DIR_OPEN_ACCESS_DENIED, s.native_kind, s.native_code};
        case D::DIR_OPEN_PATH_TOO_LONG:
          return {OR::KNOWN_DIR_OPEN_PATH_TOO_LONG, s.native_kind, s.native_code};
        case D::DIR_OPEN_TOO_MANY_OPEN:
          return {OR::KNOWN_DIR_OPEN_TOO_MANY_OPEN, s.native_kind, s.native_code};
        default:
          return {OR::KNOWN_DIR_OPEN_SYSTEM_ERROR, s.native_kind, s.native_code};
        }
      }

      bool is_separator(char c) noexcept
      {
#if OSPRIM_OS_WINDOWS
        return c == '\\' || c == '/';
#else
        return c == '/';
#endif
      }

      /// @brief Length of the root of an absolute path, which is never
      /// created: "/", "C:\", "\\server\share", "\\?\C:\", "\\?\UNC\s\share".
      size_t root_size(const char* p, size_t n) noexcept
      {
#if OSPRIM_OS_WINDOWS
        size_t i = 0;
        if (n >= 4 && p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        {
          if (n >= 8 && ::memcmp(p + 4, "UNC\\", 4) == 0)
            i = 8;
          else
            return n < 7 ? n : 7;
        }
        else if (n >= 2 && is_separator(p[0]) && is_separator(p[1]))
          i = 2;
        else
          return n < 3 ? n : 3;
        // \\server\share: two components
        for (int part = 0; part < 2; ++part)
        {
          while (i < n && !is_separator(p[i]))
            ++i;
          if (part == 0 && i < n)
            ++i;
        }
        return i;
#else
        (void)p;
        (void)n;
        return 1;
#endif
      }

      /// @brief Creates the missing components of `path` (absolute,
      /// terminated, written to in place and restored): back to the deepest
      /// existing ancestor, then forward.
      status<OR> create_missing(char* path, size_t size) noexcept
      {
        using detail::mkdir_outcome;
        const size_t root = root_size(path, size);
        status<OR> error{};
        auto make = [&](size_t upto) noexcept
        {
          const char saved = path[upto];
          path[upto]       = '\0';
          const mkdir_outcome o =
              detail::platform_known_dir_mkdir(path, upto, &error);
          path[upto] = saved;
          return o;
        };

        size_t end = size;
        for (;;)
        {
          const mkdir_outcome o = make(end);
          if (o == mkdir_outcome::CREATED || o == mkdir_outcome::EXISTS)
            break;
          if (o == mkdir_outcome::FAILED)
            return error;
          size_t start = end; // start of the last component
          while (start > root && !is_separator(path[start - 1]))
            --start;
          if (start <= root)
            return fail(OR::KNOWN_DIR_OPEN_NOT_FOUND);
          end = start - 1;
          while (end > root && is_separator(path[end - 1]))
            --end;
        }
        while (end < size)
        {
          size_t next = end + 1;
          while (next < size && !is_separator(path[next]))
            ++next;
          const mkdir_outcome o = make(next);
          if (o == mkdir_outcome::FAILED)
            return error;
          if (o == mkdir_outcome::PARENT_MISSING) // removed meanwhile
            return fail(OR::KNOWN_DIR_OPEN_NOT_FOUND);
          end = next;
        }
        return {};
      }
    } // namespace

    status<known_dir_get_result> known_dir_get(
        known_dir which, char* buffer, size_t capacity, size_t& needed,
        known_dir_info* info) noexcept
    {
      OSPRIM_ASSERT(
          index_of(which) < KNOWN_DIR_COUNT,
          "osprim::known_dir_get: unknown purpose");
      needed = 0;
      if (info != nullptr)
        *info = known_dir_info{};
      char path[MAX_PATH_BYTES + 1];
      size_t n           = 0;
      const status<GR> s = resolve(which, path, &n);
      if (!s.ok())
        return s;
      if (info != nullptr)
        describe(which, path, n, info);
      if (!detail::copy_out(path, n, buffer, capacity, &needed))
        return fail(GR::KNOWN_DIR_GET_BUFFER_TOO_SMALL);
      return {};
    }

    status<known_dir_open_result> known_dir_open(
        known_dir which, bool create, dir_handle& out, known_dir_info* info,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          index_of(which) < KNOWN_DIR_COUNT,
          "osprim::known_dir_open: unknown purpose");
      if (info != nullptr)
        *info = known_dir_info{};
      char path[MAX_PATH_BYTES + 1];
      size_t n           = 0;
      const status<GR> g = resolve(which, path, &n);
      if (!g.ok())
        return from_get(g);
      const bool creatable      = which != known_dir::KNOWN_DIR_EXECUTABLE
                                  && which != known_dir::KNOWN_DIR_RESOURCES;
      status<dir_open_result> o = dir_open_at(
          NO_DIRECTORY, path, n, path_resolve::PATH_RESOLVE_NORMAL, out, loc);
      if (o.result == dir_open_result::DIR_OPEN_NOT_FOUND && create && creatable)
      {
        const status<OR> c = create_missing(path, n);
        if (!c.ok())
          return c;
        o = dir_open_at(
            NO_DIRECTORY, path, n, path_resolve::PATH_RESOLVE_NORMAL, out, loc);
      }
      if (!o.ok())
        return from_dir_open(o);
      if (info != nullptr)
        describe(which, path, n, info);
      return {};
    }
  } // namespace v0_1
} // namespace osprim
