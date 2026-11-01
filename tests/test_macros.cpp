// The osprim headers must survive the macros of the system headers that
// engines include before them (min/max, ERROR, CLOCK_*, ...). Everything
// here only has to compile.

#if defined(_WIN32)
  // Deliberately without WIN32_LEAN_AND_MEAN or NOMINMAX: the worst case
  #include <windows.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <pthread.h>
  #include <sched.h>
  #include <signal.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <sys/types.h>
  #include <time.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach/mach_time.h>
  #elif defined(__linux__)
    #include <linux/futex.h>
    #include <sys/syscall.h>
  #endif
#endif

#include <osprim/osprim.hpp>

#include <doctest/doctest.h>

#include <initializer_list>

namespace
{
  int touch_every_enumerator() noexcept
  {
    using namespace osprim;
    int n = 0;
    for (native_error_kind k :
         {native_error_kind::NATIVE_ERROR_NONE,
          native_error_kind::NATIVE_ERROR_POSIX_ERRNO,
          native_error_kind::NATIVE_ERROR_WIN32,
          native_error_kind::NATIVE_ERROR_NT_STATUS,
          native_error_kind::NATIVE_ERROR_MACH_KERN})
      n += static_cast<int>(k);
    for (diagnostic_kind k :
         {diagnostic_kind::DIAGNOSTIC_HANDLE_LEAK,
          diagnostic_kind::DIAGNOSTIC_HANDLE_MISUSE})
      n += static_cast<int>(k);
    for (initialize_result r :
         {initialize_result::INITIALIZE_OK,
          initialize_result::INITIALIZE_OUT_OF_MEMORY,
          initialize_result::INITIALIZE_SYSTEM_ERROR})
      n += static_cast<int>(r);
    for (clock_id c :
         {clock_id::CLOCK_ID_AWAKE, clock_id::CLOCK_ID_CONTINUOUS,
          clock_id::CLOCK_ID_WALL, clock_id::CLOCK_ID_TICKS})
      n += static_cast<int>(c);
    for (tick_source t :
         {tick_source::TICK_SOURCE_AWAKE, tick_source::TICK_SOURCE_X86_TSC,
          tick_source::TICK_SOURCE_ARM64_CNTVCT})
      n += static_cast<int>(t);
    for (wait_result w : {wait_result::WAIT_WOKEN, wait_result::WAIT_TIMED_OUT})
      n += static_cast<int>(w);
    n += static_cast<int>(sizeof(wait_word_t));
    for (thread_state t :
         {thread_state::THREAD_STATE_STARTING, thread_state::THREAD_STATE_RUNNING,
          thread_state::THREAD_STATE_EXITED})
      n += static_cast<int>(t);
    for (thread_create_result r :
         {thread_create_result::THREAD_CREATE_OK,
          thread_create_result::THREAD_CREATE_OUT_OF_RESOURCES,
          thread_create_result::THREAD_CREATE_SYSTEM_ERROR})
      n += static_cast<int>(r);
    for (thread_join_result r :
         {thread_join_result::THREAD_JOIN_OK,
          thread_join_result::THREAD_JOIN_TIMED_OUT})
      n += static_cast<int>(r);
    for (set_thread_name_result r :
         {set_thread_name_result::SET_THREAD_NAME_OK,
          set_thread_name_result::SET_THREAD_NAME_SYSTEM_ERROR})
      n += static_cast<int>(r);
    for (thread_class c :
         {thread_class::THREAD_CLASS_BACKGROUND, thread_class::THREAD_CLASS_UTILITY,
          thread_class::THREAD_CLASS_NORMAL, thread_class::THREAD_CLASS_INTERACTIVE})
      n += static_cast<int>(c);
    for (set_thread_class_result r :
         {set_thread_class_result::SET_THREAD_CLASS_OK,
          set_thread_class_result::SET_THREAD_CLASS_DENIED,
          set_thread_class_result::SET_THREAD_CLASS_SYSTEM_ERROR})
      n += static_cast<int>(r);
    n += static_cast<int>(THREAD_NAME_MAX);
    n += static_cast<int>(timer_resolution_result::TIMER_RESOLUTION_OK)
         + static_cast<int>(timer_resolution_result::TIMER_RESOLUTION_SYSTEM_ERROR);
    for (activity_result r :
         {activity_result::ACTIVITY_OK, activity_result::ACTIVITY_UNSUPPORTED,
          activity_result::ACTIVITY_SYSTEM_ERROR})
      n += static_cast<int>(r);
    for (perf_session_result r :
         {perf_session_result::PERF_SESSION_OK,
          perf_session_result::PERF_SESSION_UNSUPPORTED,
          perf_session_result::PERF_SESSION_SYSTEM_ERROR})
      n += static_cast<int>(r);
    for (thermal_level l :
         {thermal_level::THERMAL_LEVEL_NOMINAL, thermal_level::THERMAL_LEVEL_FAIR,
          thermal_level::THERMAL_LEVEL_SERIOUS,
          thermal_level::THERMAL_LEVEL_CRITICAL})
      n += static_cast<int>(l);
    for (thermal_result r :
         {thermal_result::THERMAL_OK, thermal_result::THERMAL_UNSUPPORTED,
          thermal_result::THERMAL_UNAVAILABLE, thermal_result::THERMAL_SYSTEM_ERROR})
      n += static_cast<int>(r);
    n += static_cast<int>(ACTIVITY_REASON_MAX + PERF_SESSION_MAX_THREADS);
    n += FOREVER_NS == awake_instant::forever().ns;
    return n;
  }
} // namespace

TEST_CASE("headers: compile after the system headers")
{
  CHECK(touch_every_enumerator() > 0);
}
