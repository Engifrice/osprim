/*****************************************************************/ /**
 * @file   win32.hpp
 * @brief  Internal: includes <windows.h> with the usual trimming.
 *********************************************************************/
#ifndef OSPRIM_SRC_WIN32_HPP
#define OSPRIM_SRC_WIN32_HPP

#include "platform.hpp"

#if OSPRIM_OS_WINDOWS
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

#endif // !OSPRIM_SRC_WIN32_HPP
