/*****************************************************************/ /**
 * @file   shared_plugin.cpp
 * @brief  A second module using the shared osprim (built with
 * OSPRIM_SHARED only): it must see the same library state as the test
 * executable (initialization, handle tracking, clocks).
 *********************************************************************/
#include <osprim/clock.hpp>
#include <osprim/file.hpp>

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #define OSPRIM_SHARED_PLUGIN_EXPORT __declspec(dllexport)
#else
  #define OSPRIM_SHARED_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C"
{
  /// @brief Opens (creates) `path` for reading and writing.
  /// @return The file_open_result
  OSPRIM_SHARED_PLUGIN_EXPORT int osprim_shared_plugin_open(
      const char* path, size_t size, osprim::file_handle* out)
  {
    osprim::file_open_desc desc;
    desc.access      = osprim::file_access::FILE_ACCESS_READ_WRITE;
    desc.disposition = osprim::file_disposition::FILE_DISPOSITION_OPEN_OR_CREATE;
    return static_cast<int>(osprim::file_open(path, size, desc, *out).result);
  }

  /// @brief Closes a file opened by any module.
  OSPRIM_SHARED_PLUGIN_EXPORT void osprim_shared_plugin_close(
      const osprim::file_handle* h)
  {
    osprim::file_close(*h);
  }

  /// @brief The awake clock, as this module reads it.
  OSPRIM_SHARED_PLUGIN_EXPORT int64_t osprim_shared_plugin_awake_ns()
  {
    return osprim::awake_now().ns;
  }
}
