/*****************************************************************/ /**
 * @file   support.hpp
 * @brief  Test helpers: an osprim session that records diagnostics.
 *********************************************************************/
#ifndef OSPRIM_TESTS_SUPPORT_HPP
#define OSPRIM_TESTS_SUPPORT_HPP

#include <osprim/base.hpp>

#include <doctest/doctest.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

namespace osprim_test
{
  /// @brief Prints an informational line (measurements, detected hardware).
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((format(printf, 1, 2)))
#endif
  inline void note(const char* fmt, ...) noexcept
  {
    va_list args;
    va_start(args, fmt);
    ::fputs("  [note] ", stdout);
    ::vfprintf(stdout, fmt, args);
    ::fputc('\n', stdout);
    ::fflush(stdout);
    va_end(args);
  }

  /// @brief Collects the diagnostics reported by osprim.
  struct diagnostic_log
  {
    static constexpr size_t CAPACITY = 64;
    osprim::diagnostic entries[CAPACITY];
    size_t count = 0;

    static void callback(void* user, const osprim::diagnostic& d) noexcept
    {
      auto* log = static_cast<diagnostic_log*>(user);
      if (log->count < CAPACITY)
        log->entries[log->count] = d;
      ++log->count;
    }
  };

  /// @brief Initializes osprim for the duration of a test. By default,
  /// requires that no diagnostic was reported when it ends.
  class session
  {
  public:
    explicit session(osprim::init_desc desc = {})
    {
      desc.on_diagnostic   = &diagnostic_log::callback;
      desc.diagnostic_user = &log;
      const auto s         = osprim::initialize(desc);
      REQUIRE(s.ok());
      active = true;
    }

    ~session()
    {
      if (active)
      {
        osprim::deinitialize();
        CHECK(log.count == 0);
      }
    }

    /// @brief Deinitializes now, so that the test can inspect the
    /// diagnostics reported at deinitialization.
    const diagnostic_log& end() noexcept
    {
      if (active)
        osprim::deinitialize();
      active = false;
      return log;
    }

    session(const session&)            = delete;
    session& operator=(const session&) = delete;

    diagnostic_log log;

  private:
    bool active = false;
  };
} // namespace osprim_test

#endif // !OSPRIM_TESTS_SUPPORT_HPP
