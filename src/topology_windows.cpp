/*****************************************************************/ /**
 * @file   topology_windows.cpp
 * @brief  Windows topology, budget and affinity.
 *
 * - Per CPU: CPU sets (GetSystemCpuSetInformation, Windows 10+): id, core,
 *   last-level cache, NUMA node, EfficiencyClass (higher: faster), and
 *   whether the set is reserved for another process. Resolved at run time:
 *   not every SDK declares them (MinGW).
 * - Packages and caches: GetLogicalProcessorInformationEx.
 * - Budget: the process's affinity over its processor groups (new threads
 *   get the process's affinity, whatever their creator's), minus CPU sets
 *   reserved for others; the job object's CPU rate cap.
 * - Affinity: soft with CPU sets (SetThreadSelectedCpuSets), hard with
 *   group affinity (one processor group).
 *
 * The OS buffers are read by their documented offsets (they hold
 * variable-sized entries, and SDKs disagree on the struct definitions).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "topology_windows.cpp is for Windows only"
#endif

#include "win32.hpp"

#include <string.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      using detail::cpu_record;

      using get_cpu_sets_fn = BOOL(WINAPI*)(void*, ULONG, PULONG, HANDLE, ULONG);
      using set_thread_cpu_sets_fn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);

      template<class F>
      F kernel32(const char* name) noexcept
      {
        const HMODULE m = ::GetModuleHandleW(L"kernel32.dll");
        const FARPROC p = m != nullptr ? ::GetProcAddress(m, name) : nullptr;
        return reinterpret_cast<F>(reinterpret_cast<void (*)(void)>(p));
      }

      template<class T>
      T load(const unsigned char* p) noexcept
      {
        T v;
        ::memcpy(&v, p, sizeof(T));
        return v;
      }

      // Both buffers are used under topology_lock
      alignas(8) unsigned char g_cpu_sets[TOPOLOGY_MAX_CPUS * 32];
      alignas(8) unsigned char g_relations[128 * 1024];

      // SYSTEM_CPU_SET_INFORMATION layout
      constexpr size_t CPU_SET_SIZE = 0; // DWORD
      constexpr size_t CPU_SET_TYPE =
          4; // CPU_SET_INFORMATION_TYPE, 0: CpuSetInformation
      constexpr size_t CPU_SET_ID         = 8;  // DWORD
      constexpr size_t CPU_SET_GROUP      = 12; // WORD
      constexpr size_t CPU_SET_LP         = 14; // BYTE LogicalProcessorIndex
      constexpr size_t CPU_SET_CORE       = 15; // BYTE CoreIndex
      constexpr size_t CPU_SET_LLC        = 16; // BYTE LastLevelCacheIndex
      constexpr size_t CPU_SET_NUMA       = 17; // BYTE NumaNodeIndex
      constexpr size_t CPU_SET_EFFICIENCY = 18; // BYTE EfficiencyClass
      constexpr size_t CPU_SET_FLAGS =
          19; // BYTE: Parked, Allocated, AllocatedToTargetProcess, RealTime
      constexpr uint8_t FLAG_ALLOCATED           = 0x2;
      constexpr uint8_t FLAG_ALLOCATED_TO_TARGET = 0x4;

      // SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX layout
      constexpr size_t REL_KIND   = 0; // LOGICAL_PROCESSOR_RELATIONSHIP
      constexpr size_t REL_SIZE   = 4; // DWORD
      constexpr size_t REL_UNION  = 8;
      constexpr DWORD REL_CACHE   = 2; // RelationCache
      constexpr DWORD REL_PACKAGE = 3; // RelationProcessorPackage
      // PROCESSOR_RELATIONSHIP: GroupCount (WORD) at 22, GROUP_AFFINITY[] at 24
      // CACHE_RELATIONSHIP: Level 0, LineSize 2, CacheSize 4, Type 8,
      // GroupCount (WORD, 0 before Windows 11) at 30, GROUP_AFFINITY[] at 32
      // GROUP_AFFINITY: KAFFINITY Mask at 0, WORD Group at 8, 16 bytes
      constexpr DWORD CACHE_UNIFIED = 0;
      constexpr DWORD CACHE_DATA    = 2;

      /// @brief Whether `location` (group << 8 | index) is in these group masks.
      bool in_masks(
          const unsigned char* masks, size_t count, uint64_t location) noexcept
      {
        const uint16_t group = static_cast<uint16_t>(location >> 8);
        const uint32_t index = static_cast<uint32_t>(location & 0xFF);
        for (size_t i = 0; i < count; ++i)
        {
          const uint64_t mask = load<uint64_t>(masks + i * 16);
          const uint16_t g    = load<uint16_t>(masks + i * 16 + 8);
          if (g == group && index < 64 && ((mask >> index) & 1u) != 0)
            return true;
        }
        return false;
      }

      size_t bits_in_masks(const unsigned char* masks, size_t count) noexcept
      {
        size_t n = 0;
        for (size_t i = 0; i < count; ++i)
        {
          uint64_t m = load<uint64_t>(masks + i * 16);
          while (m != 0)
          {
            m &= m - 1;
            ++n;
          }
        }
        return n;
      }

      /// @brief Fills the records from the live CPU sets, with availability.
      /// @return false on failure (GetLastError)
      bool load_records(size_t* n) noexcept
      {
        const auto get_cpu_sets =
            kernel32<get_cpu_sets_fn>("GetSystemCpuSetInformation");
        if (get_cpu_sets == nullptr)
        {
          ::SetLastError(ERROR_PROC_NOT_FOUND);
          return false;
        }
        ULONG len = 0;
        if (!get_cpu_sets(
                g_cpu_sets, sizeof(g_cpu_sets), &len, ::GetCurrentProcess(), 0))
          return false;

        // New threads get the process's affinity: its groups, and in its
        // primary group, its mask (0 when the process spans several groups)
        USHORT groups[64];
        USHORT group_count = 64;
        if (!::GetProcessGroupAffinity(::GetCurrentProcess(), &group_count, groups))
        {
          group_count = 1;
          groups[0]   = 0;
        }
        DWORD_PTR process_mask = 0, system_mask = 0;
        if (!::GetProcessAffinityMask(
                ::GetCurrentProcess(), &process_mask, &system_mask))
          process_mask = 0;
        GROUP_AFFINITY current{};
        ::GetThreadGroupAffinity(::GetCurrentThread(), &current);

        uint16_t allowed[64];
        for (size_t i = 0; i < group_count; ++i)
          allowed[i] = groups[i];
        *n = detail::windows_parse_cpu_sets(
            g_cpu_sets, len, allowed, group_count, current.Group, process_mask,
            detail::g_topology_scratch.records, TOPOLOGY_MAX_CPUS);
        return true;
      }

      status<topology_result> topology_error() noexcept
      {
        return {
            topology_result::TOPOLOGY_SYSTEM_ERROR,
            native_error_kind::NATIVE_ERROR_WIN32,
            static_cast<int32_t>(::GetLastError())};
      }

      status<thread_affinity_result> affinity_error() noexcept
      {
        return {
            thread_affinity_result::THREAD_AFFINITY_SYSTEM_ERROR,
            native_error_kind::NATIVE_ERROR_WIN32,
            static_cast<int32_t>(::GetLastError())};
      }

      constexpr status<thread_affinity_result> AFFINITY_INVALID = {
          thread_affinity_result::THREAD_AFFINITY_INVALID,
          native_error_kind::NATIVE_ERROR_NONE, 0};
    } // namespace

    namespace detail
    {
      size_t windows_parse_cpu_sets(
          const void* buffer, size_t size, const uint16_t* allowed_groups,
          size_t group_count, uint16_t primary_group, uint64_t primary_mask,
          cpu_record* records, size_t capacity) noexcept
      {
        const auto* base = static_cast<const unsigned char*>(buffer);
        size_t n         = 0;
        for (size_t off = 0; off + 20 <= size && n < capacity;)
        {
          const unsigned char* e = base + off;
          const uint32_t entry   = load<uint32_t>(e + CPU_SET_SIZE);
          if (entry < 20 || off + entry > size)
            break;
          off += entry;
          if (load<uint32_t>(e + CPU_SET_TYPE) != 0) // not CpuSetInformation
            continue;

          const uint16_t group = load<uint16_t>(e + CPU_SET_GROUP);
          const uint8_t lp     = e[CPU_SET_LP];
          const uint8_t flags  = e[CPU_SET_FLAGS];
          bool in_group        = false;
          for (size_t g = 0; g < group_count; ++g)
            in_group |= allowed_groups[g] == group;
          const bool in_mask  = group != primary_group || primary_mask == 0
                                || (lp < 64 && ((primary_mask >> lp) & 1u) != 0);
          const bool reserved = (flags & FLAG_ALLOCATED) != 0
                                && (flags & FLAG_ALLOCATED_TO_TARGET) == 0;

          cpu_record& r = records[n++];
          r             = cpu_record{};
          r.id          = load<uint32_t>(e + CPU_SET_ID);
          // The indices are group-relative
          r.core_key  = (uint64_t{group} << 16) | e[CPU_SET_CORE];
          r.llc_key   = (uint64_t{group} << 16) | e[CPU_SET_LLC];
          r.node_key  = (uint64_t{group} << 16) | e[CPU_SET_NUMA];
          r.rank      = e[CPU_SET_EFFICIENCY];
          r.location  = (uint64_t{group} << 8) | lp;
          r.available = in_group && in_mask && !reserved;
        }
        return n;
      }

      void platform_topology_initialize() noexcept
      {
      }

      status<cpu_budget_result> platform_cpu_budget(cpu_budget* out) noexcept
      {
        {
          topology_lock lock;
          size_t n = 0;
          if (!load_records(&n))
            return {
                cpu_budget_result::CPU_BUDGET_SYSTEM_ERROR,
                native_error_kind::NATIVE_ERROR_WIN32,
                static_cast<int32_t>(::GetLastError())};
          uint32_t available = 0;
          for (size_t i = 0; i < n; ++i)
            available += g_topology_scratch.records[i].available ? 1u : 0u;
          out->available_cpus = available;
        }

        // The job's CPU rate cap: 1/100 of a percent of all CPUs (10000: all)
        struct
        {
          DWORD flags;
          DWORD rate; // CpuRate, or MinRate | MaxRate << 16
        } info{};
        if (::QueryInformationJobObject(
                nullptr, JobObjectCpuRateControlInformation, &info, sizeof(info),
                nullptr))
        {
          constexpr DWORD ENABLE = 0x1, HARD_CAP = 0x4, MIN_MAX_RATE = 0x10;
          DWORD rate = 0;
          if ((info.flags & ENABLE) != 0 && (info.flags & HARD_CAP) != 0)
            rate = info.rate;
          else if ((info.flags & ENABLE) != 0 && (info.flags & MIN_MAX_RATE) != 0)
            rate = info.rate >> 16; // MaxRate
          if (rate != 0)
          {
            const uint64_t all   = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            const uint64_t milli = uint64_t{rate} * all / 10;
            out->quota_millicpus = static_cast<uint32_t>(milli == 0 ? 1 : milli);
            out->quota_period_ns = 0; // not exposed by Windows
          }
        }
        return {};
      }

      status<topology_result> platform_topology_query(
          topology_summary* out, cpu_desc* cpus, size_t capacity,
          size_t* count) noexcept
      {
        topology_lock lock;
        cpu_record* const r = g_topology_scratch.records;
        size_t n            = 0;
        if (!load_records(&n))
          return topology_error();

        // Packages, and the caches (best effort: unknown if refused)
        DWORD len = sizeof(g_relations);
        const bool relations =
            ::GetLogicalProcessorInformationEx(
                RelationAll,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    g_relations),
                &len)
            != FALSE;
        if (relations)
        {
          uint64_t package = 0;
          for (size_t off = 0; off + REL_UNION <= len;)
          {
            const unsigned char* e = g_relations + off;
            const uint32_t size    = load<uint32_t>(e + REL_SIZE);
            if (size < REL_UNION || off + size > len)
              break;
            off += size;
            if (load<uint32_t>(e + REL_KIND) != REL_PACKAGE)
              continue;
            const unsigned char* u = e + REL_UNION;
            const size_t groups    = load<uint16_t>(u + 22);
            for (size_t i = 0; i < n; ++i)
              if (in_masks(u + 24, groups, r[i].location))
                r[i].package_key = package;
            ++package;
          }
        }

        size_t first[MAX_CORE_CLASSES] = {};
        finish_topology(
            r, g_topology_scratch.classes, n,
            core_class_source::CORE_CLASS_SOURCE_REPORTED, out, cpus, capacity,
            first);

        if (relations)
        {
          for (size_t off = 0; off + REL_UNION <= len;)
          {
            const unsigned char* e = g_relations + off;
            const uint32_t size    = load<uint32_t>(e + REL_SIZE);
            if (size < REL_UNION || off + size > len)
              break;
            off += size;
            if (load<uint32_t>(e + REL_KIND) != REL_CACHE)
              continue;
            const unsigned char* u = e + REL_UNION;
            const uint8_t level    = u[0];
            const DWORD type       = load<uint32_t>(u + 8);
            if (type != CACHE_UNIFIED && type != CACHE_DATA)
              continue;
            size_t groups = load<uint16_t>(u + 30);
            if (groups == 0) // before Windows 11: one GROUP_AFFINITY
              groups = 1;
            cache_desc c{};
            c.size              = load<uint32_t>(u + 4);
            c.line_size         = load<uint16_t>(u + 2);
            const size_t shared = bits_in_masks(u + 32, groups);
            c.shared_by =
                static_cast<uint16_t>(shared > UINT16_MAX ? UINT16_MAX : shared);
            for (size_t k = 0; k < out->class_count; ++k)
            {
              if (!in_masks(u + 32, groups, r[first[k]].location))
                continue;
              core_class_desc& d = out->classes[k];
              if (level == 1 && d.l1d.size == 0)
                d.l1d = c;
              else if (level == 2 && d.l2.size == 0)
                d.l2 = c;
              else if (level == 3 && d.l3.size == 0)
                d.l3 = c;
            }
          }
        }
        *count = n;
        return {};
      }

      status<thread_affinity_result> platform_preferred_cpus(
          const uint64_t* ids, size_t count) noexcept
      {
        const auto set_cpu_sets =
            kernel32<set_thread_cpu_sets_fn>("SetThreadSelectedCpuSets");
        if (set_cpu_sets == nullptr)
          return {
              thread_affinity_result::THREAD_AFFINITY_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        if (count > TOPOLOGY_MAX_CPUS)
          return AFFINITY_INVALID;
        ULONG list[TOPOLOGY_MAX_CPUS];
        for (size_t i = 0; i < count; ++i)
        {
          if (ids[i] > UINT32_MAX)
            return AFFINITY_INVALID;
          list[i] = static_cast<ULONG>(ids[i]);
        }
        // An empty list removes the preference
        if (!set_cpu_sets(
                ::GetCurrentThread(), count != 0 ? list : nullptr,
                static_cast<ULONG>(count)))
        {
          if (::GetLastError() == ERROR_INVALID_PARAMETER)
            return {
                thread_affinity_result::THREAD_AFFINITY_INVALID,
                native_error_kind::NATIVE_ERROR_WIN32, ERROR_INVALID_PARAMETER};
          return affinity_error();
        }
        return {};
      }

      status<thread_affinity_result> platform_allowed_cpus(
          const uint64_t* ids, size_t count) noexcept
      {
        GROUP_AFFINITY ga{};
        if (count == 0)
        {
          // Back to the process's affinity in the primary group
          DWORD_PTR process_mask = 0, system_mask = 0;
          if (::GetProcessAffinityMask(
                  ::GetCurrentProcess(), &process_mask, &system_mask)
              && process_mask != 0)
            return ::SetThreadAffinityMask(::GetCurrentThread(), process_mask) != 0
                       ? status<thread_affinity_result>{}
                       : affinity_error();
          // The process spans several groups: all of the current group
          ::GetThreadGroupAffinity(::GetCurrentThread(), &ga);
          const DWORD active = ::GetActiveProcessorCount(ga.Group);
          ga.Mask = active >= 64 ? ~KAFFINITY{0} : ((KAFFINITY{1} << active) - 1);
        }
        else
        {
          topology_lock lock;
          size_t n = 0;
          if (!load_records(&n))
            return affinity_error();
          bool first = true;
          for (size_t i = 0; i < count; ++i)
          {
            const cpu_record* found = nullptr;
            for (size_t k = 0; k < n && found == nullptr; ++k)
              if (g_topology_scratch.records[k].id == ids[i])
                found = &g_topology_scratch.records[k];
            if (found == nullptr)
              return AFFINITY_INVALID;
            const WORD group = static_cast<WORD>(found->location >> 8);
            if (!first && group != ga.Group)
              return AFFINITY_INVALID; // one processor group only
            ga.Group = group;
            ga.Mask |= KAFFINITY{1} << (found->location & 0xFF);
            first = false;
          }
        }
        if (!::SetThreadGroupAffinity(::GetCurrentThread(), &ga, nullptr))
          return affinity_error();
        return {};
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
