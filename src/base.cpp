/*****************************************************************/ /**
 * @file   base.cpp
 * @brief  Library lifetime, allocation hooks, diagnostics and the handle
 * tracking registry.
 *********************************************************************/
#include "internal.hpp"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#if OSPRIM_OS_WINDOWS
  #include "win32.hpp"
  #include <malloc.h>
#else
  #include <sched.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      struct library_state
      {
        diagnostic_fn on_diagnostic;
        void* diagnostic_user;
        allocator alloc;
        bool initialized;
        bool tracking;
      };

      library_state g_lib{};

      void* default_alloc(void*, size_t size, size_t align) noexcept
      {
        if (align <= alignof(max_align_t))
          return ::malloc(size);
#if OSPRIM_OS_WINDOWS
        return ::_aligned_malloc(size, align);
#else
        void* p = nullptr;
        return ::posix_memalign(&p, align, size) == 0 ? p : nullptr;
#endif
      }

      void default_free(void*, void* ptr, size_t, size_t align) noexcept
      {
#if OSPRIM_OS_WINDOWS
        if (align > alignof(max_align_t))
        {
          ::_aligned_free(ptr);
          return;
        }
#else
        (void)align;
#endif
        ::free(ptr);
      }

#if OSPRIM_TRACKING
      /*****************************/
      /* TRACKING REGISTRY         */
      /*****************************/

      // Debug-only bookkeeping: a flat array scanned linearly under a
      // spinlock. Simple and allocation-light rather than fast.

      struct record
      {
        const char* type;
        uint64_t native;
        std::source_location created_at;
        detail::track_meta meta;
        /// @brief Opens not yet closed (above 1 only for shared handles)
        uint32_t count;
      };

      struct registry
      {
        std::atomic_flag lock;
        record* records;
        size_t count;
        size_t capacity;
        /// @brief An allocation failed and a handle was not recorded:
        /// unknown handles can no longer be reported as misuse.
        bool incomplete;
      };

      registry g_registry{};

      constexpr size_t INITIAL_CAPACITY = 64;

      void cpu_yield() noexcept
      {
  #if OSPRIM_OS_WINDOWS
        ::SwitchToThread();
  #else
        ::sched_yield();
  #endif
      }

      struct registry_lock
      {
        registry_lock() noexcept
        {
          while (g_registry.lock.test_and_set(std::memory_order_acquire))
          {
            while (g_registry.lock.test(std::memory_order_relaxed))
            {
              cpu_yield();
            }
          }
        }
        ~registry_lock() { g_registry.lock.clear(std::memory_order_release); }
        registry_lock(const registry_lock&)            = delete;
        registry_lock& operator=(const registry_lock&) = delete;
      };

      bool same_type(const char* a, const char* b) noexcept
      {
        return a == b || ::strcmp(a, b) == 0;
      }

      /// @return Index of the record, or g_registry.count if absent
      size_t find(const char* type, uint64_t native) noexcept
      {
        for (size_t i = 0; i < g_registry.count; ++i)
        {
          const record& r = g_registry.records[i];
          if (r.native == native && same_type(r.type, type))
            return i;
        }
        return g_registry.count;
      }

      bool grow() noexcept
      {
        const size_t new_capacity = g_registry.capacity * 2;
        auto* fresh               = static_cast<record*>(
            detail::allocate(new_capacity * sizeof(record), alignof(record)));
        if (fresh == nullptr)
          return false;
        if (g_registry.count != 0)
          ::memcpy(fresh, g_registry.records, g_registry.count * sizeof(record));
        detail::deallocate(
            g_registry.records, g_registry.capacity * sizeof(record),
            alignof(record));
        g_registry.records  = fresh;
        g_registry.capacity = new_capacity;
        return true;
      }

      bool registry_initialize() noexcept
      {
        g_registry.records = static_cast<record*>(
            detail::allocate(INITIAL_CAPACITY * sizeof(record), alignof(record)));
        if (g_registry.records == nullptr)
          return false;
        g_registry.count      = 0;
        g_registry.capacity   = INITIAL_CAPACITY;
        g_registry.incomplete = false;
        return true;
      }

      void registry_deinitialize() noexcept
      {
        // Leaks are reported in creation order
        for (size_t i = 0; i < g_registry.count; ++i)
        {
          const record& r = g_registry.records[i];
          detail::report(
              diagnostic{
                  diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK, r.type, r.native,
                  r.created_at, r.created_at});
        }
        if (g_registry.records != nullptr)
          detail::deallocate(
              g_registry.records, g_registry.capacity * sizeof(record),
              alignof(record));
        g_registry.records    = nullptr;
        g_registry.count      = 0;
        g_registry.capacity   = 0;
        g_registry.incomplete = false;
      }
#endif // OSPRIM_TRACKING
    } // namespace

    namespace detail
    {
      bool is_initialized() noexcept
      {
        return g_lib.initialized;
      }

      uint64_t next_handle_id() noexcept
      {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
      }

      void* allocate(size_t size, size_t align) noexcept
      {
        return g_lib.alloc.alloc_fn(g_lib.alloc.user, size, align);
      }

      void deallocate(void* ptr, size_t size, size_t align) noexcept
      {
        g_lib.alloc.free_fn(g_lib.alloc.user, ptr, size, align);
      }

      void report(const diagnostic& d) noexcept
      {
        if (g_lib.on_diagnostic != nullptr)
          g_lib.on_diagnostic(g_lib.diagnostic_user, d);
      }

#if OSPRIM_TRACKING
      void track_open(
          const char* type, uint64_t native, std::source_location loc,
          const track_meta* meta) noexcept
      {
        if (!g_lib.tracking)
          return;
        bool duplicate = false;
        std::source_location previous{};
        {
          registry_lock lock;
          const size_t i = find(type, native);
          if (i != g_registry.count)
          {
            // The OS only reuses a value once it is closed: the previous
            // handle was closed without going through osprim.
            duplicate                        = true;
            previous                         = g_registry.records[i].created_at;
            g_registry.records[i].created_at = loc;
            g_registry.records[i].meta = meta != nullptr ? *meta : track_meta{};
          }
          else if (g_registry.count == g_registry.capacity && !grow())
            g_registry.incomplete = true;
          else
            g_registry.records[g_registry.count++] =
                record{type, native, loc, meta != nullptr ? *meta : track_meta{}, 1};
        }
        if (duplicate)
          report(
              diagnostic{
                  diagnostic_kind::DIAGNOSTIC_HANDLE_MISUSE, type, native, previous,
                  loc});
      }

      void track_close(
          const char* type, uint64_t native, std::source_location loc) noexcept
      {
        if (!g_lib.tracking)
          return;
        bool unknown = false;
        {
          registry_lock lock;
          const size_t i = find(type, native);
          if (i == g_registry.count)
            unknown = !g_registry.incomplete;
          else if (g_registry.records[i].count > 1)
            --g_registry.records[i].count; // a shared handle still opened elsewhere
          else
          {
            // Keep creation order: leaks are reported oldest first
            ::memmove(
                g_registry.records + i, g_registry.records + i + 1,
                (g_registry.count - i - 1) * sizeof(record));
            --g_registry.count;
          }
        }
        if (unknown)
          report(
              diagnostic{
                  diagnostic_kind::DIAGNOSTIC_HANDLE_MISUSE, type, native,
                  std::source_location{}, loc});
      }

      size_t tracked_count() noexcept
      {
        if (!g_lib.tracking)
          return 0;
        registry_lock lock;
        return g_registry.count;
      }

      void track_open_shared(
          const char* type, uint64_t native, std::source_location loc) noexcept
      {
        if (!g_lib.tracking)
          return;
        registry_lock lock;
        const size_t i = find(type, native);
        if (i != g_registry.count)
          ++g_registry.records[i].count;
        else if (g_registry.count == g_registry.capacity && !grow())
          g_registry.incomplete = true;
        else
          g_registry.records[g_registry.count++] =
              record{type, native, loc, track_meta{}, 1};
      }

      bool track_get(const char* type, uint64_t native, track_meta* out) noexcept
      {
        if (!g_lib.tracking)
          return false;
        registry_lock lock;
        const size_t i = find(type, native);
        if (i == g_registry.count)
          return false;
        *out = g_registry.records[i].meta;
        return true;
      }

      bool track_find(
          const char* type, track_match_fn match, const void* ctx, uint64_t* native,
          std::source_location* created_at) noexcept
      {
        if (!g_lib.tracking)
          return false;
        registry_lock lock;
        for (size_t i = 0; i < g_registry.count; ++i)
        {
          const record& r = g_registry.records[i];
          if (same_type(r.type, type) && match(r.meta, ctx))
          {
            *native     = r.native;
            *created_at = r.created_at;
            return true;
          }
        }
        return false;
      }
#endif // OSPRIM_TRACKING
    } // namespace detail

    status<initialize_result> initialize(const init_desc& desc) noexcept
    {
      OSPRIM_ASSERT(!g_lib.initialized, "osprim::initialize: already initialized");
      OSPRIM_ASSERT(
          (desc.alloc.alloc_fn == nullptr) == (desc.alloc.free_fn == nullptr),
          "osprim::initialize: alloc_fn and free_fn must be both set or both null");

      g_lib.on_diagnostic   = desc.on_diagnostic;
      g_lib.diagnostic_user = desc.diagnostic_user;
      g_lib.alloc           = desc.alloc;
      if (g_lib.alloc.alloc_fn == nullptr || g_lib.alloc.free_fn == nullptr)
        g_lib.alloc = allocator{nullptr, &default_alloc, &default_free};

#if OSPRIM_TRACKING
      g_lib.tracking = desc.track_handles;
      if (g_lib.tracking && !registry_initialize())
        return {
            initialize_result::INITIALIZE_OUT_OF_MEMORY,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#else
      g_lib.tracking = false;
#endif

      const status<initialize_result> clocks = detail::clock_initialize();
      if (!clocks.ok())
      {
#if OSPRIM_TRACKING
        if (g_lib.tracking)
          registry_deinitialize();
#endif
        return clocks;
      }

      detail::power_initialize();
      detail::topology_initialize();
      detail::file_initialize();

      g_lib.initialized = true;
      return {};
    }

    void deinitialize() noexcept
    {
      OSPRIM_ASSERT(g_lib.initialized, "osprim::deinitialize: not initialized");
#if OSPRIM_TRACKING
      if (g_lib.tracking)
        registry_deinitialize();
#endif
      detail::power_deinitialize();
      detail::file_deinitialize();
      g_lib = library_state{};
    }
  } // namespace v0_1
} // namespace osprim
