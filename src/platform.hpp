/*****************************************************************/ /**
 * @file   platform.hpp
 * @brief  Internal: the public platform macros, plus a check that the
 * sources selected by the build system match the compiler's target.
 *
 * OSPRIM_PLATFORM_ID must match the OSPRIM_CMAKE_PLATFORM definition
 * passed by CMake: 1 Linux, 2 Android, 3 Windows, 4 Apple.
 *********************************************************************/
#ifndef OSPRIM_SRC_PLATFORM_HPP
#define OSPRIM_SRC_PLATFORM_HPP

#include <osprim/platform.hpp>

#if OSPRIM_OS_LINUX
  #define OSPRIM_PLATFORM_ID 1
#elif OSPRIM_OS_ANDROID
  #define OSPRIM_PLATFORM_ID 2
#elif OSPRIM_OS_WINDOWS
  #define OSPRIM_PLATFORM_ID 3
#elif OSPRIM_OS_APPLE
  #define OSPRIM_PLATFORM_ID 4
#endif

#if defined(OSPRIM_CMAKE_PLATFORM) && OSPRIM_CMAKE_PLATFORM != OSPRIM_PLATFORM_ID
  #error \
      "osprim: the build system selected sources for a different platform than the compiler targets"
#endif

#endif // !OSPRIM_SRC_PLATFORM_HPP
