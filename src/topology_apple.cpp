/*****************************************************************/ /**
 * @file   topology_apple.cpp
 * @brief  Apple topology and budget (no affinity).
 *
 * - Topology: sysctl. Performance levels (hw.nperflevels, hw.perflevelN.*:
 *   level 0 is the fastest) give the classes and their caches. Apple exposes
 *   no CPU identities: no CPU entries.
 * - Budget: hw.activecpu; no quotas.
 * - Affinity: none (QoS classes place threads): UNSUPPORTED.
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_APPLE
  #error "topology_apple.cpp is for Apple platforms only"
#endif

#include <stdio.h>
#include <sys/sysctl.h>
#include <sys/types.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      /// @brief An integer sysctl (32 or 64 bits).
      bool sysctl_u64(const char* name, uint64_t* out) noexcept
      {
        uint64_t v = 0; // little-endian: a 32-bit value fills the low half
        size_t len = sizeof(v);
        if (::sysctlbyname(name, &v, &len, nullptr, 0) != 0
            || (len != 4 && len != 8))
          return false;
        *out = v;
        return true;
      }

      uint64_t sysctl_or(const char* name, uint64_t fallback) noexcept
      {
        uint64_t v = 0;
        return sysctl_u64(name, &v) ? v : fallback;
      }

      uint32_t to_u32(uint64_t v) noexcept
      {
        return v > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(v);
      }

      uint16_t to_u16(uint64_t v) noexcept
      {
        return v > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(v);
      }

      constexpr status<thread_affinity_result> AFFINITY_UNSUPPORTED = {
          thread_affinity_result::THREAD_AFFINITY_UNSUPPORTED,
          native_error_kind::NATIVE_ERROR_NONE, 0};
    } // namespace

    namespace detail
    {
      void platform_topology_initialize() noexcept
      {
      }

      status<cpu_budget_result> platform_cpu_budget(cpu_budget* out) noexcept
      {
        out->available_cpus =
            to_u32(sysctl_or("hw.activecpu", sysctl_or("hw.logicalcpu", 1)));
        return {};
      }

      status<topology_result> platform_topology_query(
          topology_summary* out, cpu_desc*, size_t, size_t* count) noexcept
      {
        const uint64_t line = sysctl_or("hw.cachelinesize", CACHE_LINE_SIZE);
        out->logical_cpus   = to_u32(sysctl_or("hw.logicalcpu", 1));
        out->physical_cores = to_u32(sysctl_or("hw.physicalcpu", out->logical_cpus));
        out->packages       = to_u32(sysctl_or("hw.packages", 1));

        const uint64_t levels = sysctl_or("hw.nperflevels", 1);
        if (levels >= 2)
        {
          out->class_source = core_class_source::CORE_CLASS_SOURCE_REPORTED;
          out->class_count  = static_cast<uint32_t>(
              levels < MAX_CORE_CLASSES ? levels : MAX_CORE_CLASSES);
          char name[64];
          for (uint64_t i = 0; i < levels; ++i)
          {
            // Levels beyond MAX_CORE_CLASSES merge into the last class
            core_class_desc& c =
                out->classes[i < MAX_CORE_CLASSES ? i : MAX_CORE_CLASSES - 1];
            auto level = [&](const char* key, uint64_t fallback)
            {
              ::snprintf(
                  name, sizeof(name), "hw.perflevel%llu.%s",
                  static_cast<unsigned long long>(i), key);
              return sysctl_or(name, fallback);
            };
            const uint64_t logical  = level("logicalcpu", 0);
            const uint64_t physical = level("physicalcpu", logical);
            c.logical_cpus += to_u32(logical);
            c.physical_cores += to_u32(physical);
            if (i >= MAX_CORE_CLASSES)
              continue;
            const uint64_t per_core = physical != 0 ? logical / physical : 1;
            c.l1d                   = cache_desc{
                to_u32(level("l1dcachesize", 0)), to_u16(line), to_u16(per_core)};
            c.l2 = cache_desc{
                to_u32(level("l2cachesize", 0)), to_u16(line),
                to_u16(level("cpusperl2", 0))};
            c.l3 = cache_desc{
                to_u32(level("l3cachesize", 0)), to_u16(line),
                to_u16(level("cpusperl3", 0))};
            if (c.l3.size == 0)
              c.l3 = cache_desc{};
          }
        }
        else
        {
          out->class_source  = core_class_source::CORE_CLASS_SOURCE_UNIFORM;
          out->class_count   = 1;
          core_class_desc& c = out->classes[0];
          c.logical_cpus     = out->logical_cpus;
          c.physical_cores   = out->physical_cores;
          const uint64_t per_core =
              out->physical_cores != 0 ? out->logical_cpus / out->physical_cores : 1;
          c.l1d = cache_desc{
              to_u32(sysctl_or("hw.l1dcachesize", 0)), to_u16(line),
              to_u16(per_core)};
          c.l2 = cache_desc{to_u32(sysctl_or("hw.l2cachesize", 0)), to_u16(line), 0};
          c.l3 = cache_desc{to_u32(sysctl_or("hw.l3cachesize", 0)), to_u16(line), 0};
          if (c.l2.size == 0)
            c.l2 = cache_desc{};
          if (c.l3.size == 0)
            c.l3 = cache_desc{};
        }
        *count = 0; // Apple exposes no CPU identities
        return {};
      }

      status<thread_affinity_result> platform_preferred_cpus(
          const uint64_t*, size_t) noexcept
      {
        return AFFINITY_UNSUPPORTED;
      }

      status<thread_affinity_result> platform_allowed_cpus(
          const uint64_t*, size_t) noexcept
      {
        return AFFINITY_UNSUPPORTED;
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
