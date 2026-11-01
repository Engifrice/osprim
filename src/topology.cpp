/*****************************************************************/ /**
 * @file   topology.cpp
 * @brief  Platform-independent topology code: the scratch storage and its
 * lock, classification, summary counts and dense indices.
 *********************************************************************/
#include "internal.hpp"

#include <atomic>

#if OSPRIM_OS_WINDOWS
  #include "win32.hpp"
#else
  #include <sched.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      std::atomic_flag g_topology_lock; // clear (C++20)

      /// @brief Number of distinct keys among records of class `c` (or all).
      template<class Key>
      uint32_t distinct(
          const detail::cpu_record* r, const uint8_t* cls, size_t n, int c,
          Key key) noexcept
      {
        uint32_t count = 0;
        for (size_t i = 0; i < n; ++i)
        {
          if (c >= 0 && cls[i] != c)
            continue;
          bool seen = false;
          for (size_t j = 0; j < i; ++j)
            if ((c < 0 || cls[j] == c) && key(r[j]) == key(r[i]))
            {
              seen = true;
              break;
            }
          if (!seen)
            ++count;
        }
        return count;
      }
    } // namespace

    namespace detail
    {
      topology_scratch g_topology_scratch{};

      topology_lock::topology_lock() noexcept
      {
        while (g_topology_lock.test_and_set(std::memory_order_acquire))
          while (g_topology_lock.test(std::memory_order_relaxed))
          {
#if OSPRIM_OS_WINDOWS
            ::SwitchToThread();
#else
            ::sched_yield();
#endif
          }
      }

      topology_lock::~topology_lock()
      {
        g_topology_lock.clear(std::memory_order_release);
      }

      void finish_topology(
          cpu_record* r, uint8_t* cls, size_t n, core_class_source multi_source,
          topology_summary* out, cpu_desc* cpus, size_t capacity,
          size_t first_of_class[MAX_CORE_CLASSES]) noexcept
      {
        // Sort by id (insertion sort: usually already sorted)
        for (size_t i = 1; i < n; ++i)
        {
          const cpu_record v = r[i];
          size_t j           = i;
          for (; j > 0 && r[j - 1].id > v.id; --j)
            r[j] = r[j - 1];
          r[j] = v;
        }

        // Distinct ranks, highest first
        uint64_t ranks[MAX_CORE_CLASSES];
        size_t rank_count = 0;
        uint64_t floor    = UINT64_MAX; // ranks already taken are above
        while (rank_count < MAX_CORE_CLASSES)
        {
          bool found    = false;
          uint64_t best = 0;
          for (size_t i = 0; i < n; ++i)
            if (r[i].rank < floor && (!found || r[i].rank > best))
            {
              best  = r[i].rank;
              found = true;
            }
          if (!found)
            break;
          ranks[rank_count++] = best;
          floor               = best;
        }
        if (rank_count == 0)
          rank_count = 1;

        for (size_t i = 0; i < n; ++i)
        {
          uint8_t c = static_cast<uint8_t>(
              rank_count - 1); // slower ranks merge into the last
          for (size_t k = 0; k < rank_count; ++k)
            if (r[i].rank == ranks[k])
            {
              c = static_cast<uint8_t>(k);
              break;
            }
          cls[i] = c;
        }

        *out              = topology_summary{};
        out->logical_cpus = static_cast<uint32_t>(n);
        out->physical_cores =
            distinct(r, cls, n, -1, [](const cpu_record& x) { return x.core_key; });
        out->packages = distinct(
            r, cls, n, -1, [](const cpu_record& x) { return x.package_key; });
        out->class_count  = static_cast<uint32_t>(rank_count);
        out->class_source = rank_count > 1
                                ? multi_source
                                : core_class_source::CORE_CLASS_SOURCE_UNIFORM;
        for (size_t k = 0; k < rank_count; ++k)
        {
          first_of_class[k] = 0;
          for (size_t i = 0; i < n; ++i)
            if (cls[i] == k)
            {
              first_of_class[k] = i;
              break;
            }
          uint32_t logical = 0;
          for (size_t i = 0; i < n; ++i)
            logical += cls[i] == k ? 1u : 0u;
          out->classes[k].logical_cpus   = logical;
          out->classes[k].physical_cores = distinct(
              r, cls, n, static_cast<int>(k),
              [](const cpu_record& x) { return x.core_key; });
        }

        if (cpus == nullptr)
          return;
        const size_t written = n < capacity ? n : capacity;
        for (size_t i = 0; i < written; ++i)
        {
          cpus[i]            = cpu_desc{};
          cpus[i].id         = r[i].id;
          cpus[i].core_class = cls[i];
          cpus[i].available  = r[i].available;
        }

        // Dense indices, in order of first appearance: one O(n^2) pass per key
        uint32_t* const index = g_topology_scratch.dense;
        auto densify          = [&](auto key, auto store)
        {
          uint32_t next = 0;
          for (size_t i = 0; i < written; ++i)
          {
            size_t j = 0;
            while (key(r[j]) != key(r[i]))
              ++j; // first record with this key (j <= i)
            index[i] = j == i ? next++ : index[j];
            store(cpus[i], index[i]);
          }
        };
        densify(
            [](const cpu_record& x) { return x.core_key; },
            [](cpu_desc& d, uint32_t v) { d.core = v; });
        densify(
            [](const cpu_record& x) { return x.package_key; },
            [](cpu_desc& d, uint32_t v) { d.package = v; });
        densify(
            [](const cpu_record& x) { return x.llc_key; },
            [](cpu_desc& d, uint32_t v) { d.last_level_cache = v; });
        densify(
            [](const cpu_record& x) { return x.node_key; },
            [](cpu_desc& d, uint32_t v) { d.numa_node = v; });
      }

      void topology_initialize() noexcept
      {
        platform_topology_initialize();
      }
    } // namespace detail

    status<cpu_budget_result> get_cpu_budget(cpu_budget* out) noexcept
    {
      OSPRIM_ASSERT(out != nullptr, "osprim::get_cpu_budget: null output");
      *out         = cpu_budget{};
      const auto s = detail::platform_cpu_budget(out);
      if (s.ok() && out->available_cpus == 0)
        out->available_cpus = 1;
      return s;
    }

    status<topology_result> topology_query(
        topology_summary* out, cpu_desc* cpus, size_t capacity,
        size_t* count) noexcept
    {
      OSPRIM_ASSERT(
          out != nullptr && count != nullptr, "osprim::topology_query: null output");
      OSPRIM_ASSERT(
          cpus != nullptr || capacity == 0,
          "osprim::topology_query: null array with a capacity");
      *out   = topology_summary{};
      *count = 0;
      return detail::platform_topology_query(out, cpus, capacity, count);
    }

    status<thread_affinity_result> set_current_thread_preferred_cpus(
        const uint64_t* ids, size_t count) noexcept
    {
      OSPRIM_ASSERT(
          ids != nullptr || count == 0,
          "osprim::set_current_thread_preferred_cpus: null ids");
      return detail::platform_preferred_cpus(ids, count);
    }

    status<thread_affinity_result> set_current_thread_allowed_cpus(
        const uint64_t* ids, size_t count) noexcept
    {
      OSPRIM_ASSERT(
          ids != nullptr || count == 0,
          "osprim::set_current_thread_allowed_cpus: null ids");
      return detail::platform_allowed_cpus(ids, count);
    }
  } // namespace v0_1
} // namespace osprim
