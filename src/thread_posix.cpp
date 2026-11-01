/*****************************************************************/ /**
 * @file   thread_posix.cpp
 * @brief  Linux, Android and Apple threads (pthreads), plus the calling
 * thread settings of each.
 *
 * - id: gettid (Linux, Android), pthread_threadid_np (Apple).
 * - class: per-thread nice through setpriority(PRIO_PROCESS, tid) (Linux,
 *   Android), QoS class (Apple).
 * - sleep: absolute clock_nanosleep on CLOCK_MONOTONIC (Linux, Android),
 *   mach_wait_until (Apple): both on the awake clock itself.
 *********************************************************************/
#include "internal.hpp"

#if !(OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID || OSPRIM_OS_APPLE)
  #error "thread_posix.cpp is for Linux, Android and Apple only"
#endif

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if OSPRIM_OS_APPLE
  #include <mach/mach_time.h>
  #include <pthread/qos.h>
#else
  #include <sys/resource.h>
  #include <sys/syscall.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      static_assert(sizeof(pthread_t) <= sizeof(uint64_t));

      uint64_t to_native(pthread_t t) noexcept
      {
        uint64_t v = 0;
        ::memcpy(&v, &t, sizeof(t));
        return v;
      }

      pthread_t from_native(uint64_t v) noexcept
      {
        pthread_t t;
        ::memcpy(&t, &v, sizeof(t));
        return t;
      }

      void* trampoline(void* ctx) noexcept
      {
        detail::run_thread(static_cast<thread_context*>(ctx));
        return nullptr;
      }

      size_t round_up(size_t v, size_t to) noexcept
      {
        return (v + to - 1) / to * to;
      }

      status<thread_create_result> posix_error(
          thread_create_result r, int code) noexcept
      {
        return {
            r, native_error_kind::NATIVE_ERROR_POSIX_ERRNO,
            static_cast<int32_t>(code)};
      }
    } // namespace

    namespace detail
    {
      status<thread_create_result> platform_thread_create(
          thread_context& ctx, size_t stack_size) noexcept
      {
        pthread_attr_t attr;
        int rc = ::pthread_attr_init(&attr);
        if (rc != 0)
          return posix_error(thread_create_result::THREAD_CREATE_SYSTEM_ERROR, rc);
        if (stack_size != 0)
        {
          const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
          size_t size       = round_up(stack_size, page);
          const size_t min  = round_up(static_cast<size_t>(PTHREAD_STACK_MIN), page);
          rc = ::pthread_attr_setstacksize(&attr, size < min ? min : size);
          if (rc != 0)
          {
            ::pthread_attr_destroy(&attr);
            return posix_error(thread_create_result::THREAD_CREATE_SYSTEM_ERROR, rc);
          }
        }
        pthread_t t;
        rc = ::pthread_create(&t, &attr, &trampoline, &ctx);
        ::pthread_attr_destroy(&attr);
        if (rc == EAGAIN)
          return posix_error(
              thread_create_result::THREAD_CREATE_OUT_OF_RESOURCES, rc);
        if (rc != 0)
          return posix_error(thread_create_result::THREAD_CREATE_SYSTEM_ERROR, rc);
        ctx.native = to_native(t);
        return {};
      }

      void platform_thread_join(uint64_t native) noexcept
      {
        ::pthread_join(from_native(native), nullptr);
      }

      void platform_thread_detach(uint64_t native) noexcept
      {
        ::pthread_detach(from_native(native));
      }
    } // namespace detail

    uint64_t current_thread_id() noexcept
    {
#if OSPRIM_OS_APPLE
      uint64_t id = 0;
      ::pthread_threadid_np(nullptr, &id);
      return id;
#else
      return static_cast<uint64_t>(::syscall(SYS_gettid));
#endif
    }

    status<set_thread_name_result> set_current_thread_name(const char* name) noexcept
    {
      OSPRIM_ASSERT(name != nullptr, "osprim::set_current_thread_name: null name");
      OSPRIM_ASSERT(
          ::strlen(name) <= THREAD_NAME_MAX,
          "osprim::set_current_thread_name: longer than THREAD_NAME_MAX bytes");
#if OSPRIM_OS_APPLE
      const int rc = ::pthread_setname_np(name);
#else
      const int rc = ::pthread_setname_np(::pthread_self(), name);
#endif
      if (rc != 0)
        return {
            set_thread_name_result::SET_THREAD_NAME_SYSTEM_ERROR,
            native_error_kind::NATIVE_ERROR_POSIX_ERRNO, static_cast<int32_t>(rc)};
      return {};
    }

    status<set_thread_class_result> set_current_thread_class(thread_class c) noexcept
    {
      OSPRIM_ASSERT(
          static_cast<size_t>(c)
              <= static_cast<size_t>(thread_class::THREAD_CLASS_INTERACTIVE),
          "osprim::set_current_thread_class: invalid class");
#if OSPRIM_OS_APPLE
      constexpr qos_class_t QOS[4] = {
          QOS_CLASS_BACKGROUND, QOS_CLASS_UTILITY, QOS_CLASS_USER_INITIATED,
          QOS_CLASS_USER_INTERACTIVE};
      const int err =
          ::pthread_set_qos_class_self_np(QOS[static_cast<size_t>(c)], 0);
#else
      constexpr int NICE[4] = {10, 5, 0, -6};
      // With a tid, PRIO_PROCESS targets that thread only (Linux semantics)
      const int err = ::setpriority(
                          PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)),
                          NICE[static_cast<size_t>(c)])
                              == 0
                          ? 0
                          : errno;
#endif
      if (err == 0)
        return {};
      return {
          err == EPERM || err == EACCES
              ? set_thread_class_result::SET_THREAD_CLASS_DENIED
              : set_thread_class_result::SET_THREAD_CLASS_SYSTEM_ERROR,
          native_error_kind::NATIVE_ERROR_POSIX_ERRNO, static_cast<int32_t>(err)};
    }

    void yield_thread() noexcept
    {
      ::sched_yield();
    }

    void sleep_until(awake_instant deadline) noexcept
    {
      OSPRIM_ASSERT(
          !deadline.is_forever(), "osprim::sleep_until: cannot sleep forever");
      if (awake_now() >= deadline)
        return;
#if OSPRIM_OS_APPLE
      uint64_t ticks = UINT64_MAX;
      detail::awake_ns_to_mach_ceil(deadline.ns, &ticks);
      // KERN_ABORTED on interruption: sleep again
      while (awake_now() < deadline)
        ::mach_wait_until(ticks);
#else
      struct timespec ts;
      ts.tv_sec  = static_cast<time_t>(deadline.ns / 1000000000);
      ts.tv_nsec = static_cast<long>(deadline.ns % 1000000000);
      // Absolute: an interrupted sleep resumes against the same deadline
      while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr)
             == EINTR)
      {
      }
#endif
    }
  } // namespace v0_1
} // namespace osprim
