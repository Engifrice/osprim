cmake_minimum_required(VERSION 3.26)

project(osprim
  VERSION 0.1.0
  DESCRIPTION "Low-level, unopinionated operating system primitives for game engines"
  LANGUAGES CXX
)

option(OSPRIM_BUILD_TESTS "Build the osprim tests" ${PROJECT_IS_TOP_LEVEL})
option(OSPRIM_INSTALL "Generate the install target" ${PROJECT_IS_TOP_LEVEL})
option(OSPRIM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(OSPRIM_TRACKING "AUTO" CACHE STRING
  "Compile handle tracking in: AUTO (Debug configurations only), ON or OFF")
set_property(CACHE OSPRIM_TRACKING PROPERTY STRINGS AUTO ON OFF)
set(OSPRIM_USER_CONFIG "" CACHE STRING
  "Header included first by every osprim header (overrides OSPRIM_ASSERT)")

include(GNUInstallDirs)

#####################################################################
# Target platform
#
# OSPRIM_CMAKE_PLATFORM is checked against the compiler target in
# src/platform.hpp: 1 Linux, 2 Android, 3 Windows, 4 Apple.
#####################################################################

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _osprim_n_arch)
  if(_osprim_n_arch GREATER 1)
    message(FATAL_ERROR
      "osprim: multi-architecture Apple builds are not supported "
      "(CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}), build each architecture separately")
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(_osprim_platform 2)
  set(_osprim_platform_sources src/clock_linux.cpp src/wait_linux.cpp src/thread_posix.cpp src/power_linux.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_osprim_platform 1)
  set(_osprim_platform_sources src/clock_linux.cpp src/wait_linux.cpp src/thread_posix.cpp src/power_linux.cpp)
elseif(WIN32)
  set(_osprim_platform 3)
  set(_osprim_platform_sources src/clock_windows.cpp src/wait_windows.cpp src/thread_windows.cpp src/power_windows.cpp)
elseif(APPLE)
  set(_osprim_platform 4)
  set(_osprim_platform_sources src/clock_apple.cpp src/wait_apple.cpp src/thread_posix.cpp src/power_apple.cpp)
else()
  message(FATAL_ERROR "osprim: unsupported system '${CMAKE_SYSTEM_NAME}'")
endif()

message(STATUS "osprim: ${CMAKE_SYSTEM_NAME}, sources: ${_osprim_platform_sources}")

#####################################################################
# Library
#####################################################################

add_library(osprim STATIC
  include/osprim/base.hpp
  include/osprim/clock.hpp
  include/osprim/wait.hpp
  include/osprim/thread.hpp
  include/osprim/power.hpp
  include/osprim/osprim.hpp
  src/platform.hpp
  src/internal.hpp
  src/win32.hpp
  src/base.cpp
  src/clock.cpp
  src/thread.cpp
  src/power.cpp
  ${_osprim_platform_sources}
)
add_library(osprim::osprim ALIAS osprim)

target_include_directories(osprim
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(osprim PUBLIC cxx_std_20)
target_compile_definitions(osprim PRIVATE OSPRIM_CMAKE_PLATFORM=${_osprim_platform})
if(WIN32)
  # synchronization: WaitOnAddress, WakeByAddress*. winmm: timeBeginPeriod.
  target_link_libraries(osprim PUBLIC synchronization winmm)
elseif(APPLE)
  # Foundation and the Objective-C runtime: NSProcessInfo activities and
  # thermal state. Blocks: the thermal notification handler.
  target_link_libraries(osprim PUBLIC "-framework Foundation" objc)
  target_compile_options(osprim PRIVATE -fblocks)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
  # dlopen of libandroid.so (performance hints, thermal)
  target_link_libraries(osprim PUBLIC ${CMAKE_DL_LIBS})
endif()

# Handle tracking changes the library's behavior, not its interface, but
# the headers expose osprim::tracking_compiled: PUBLIC keeps both in sync.
string(TOUPPER "${OSPRIM_TRACKING}" _osprim_tracking)
if(_osprim_tracking STREQUAL "AUTO")
  target_compile_definitions(osprim PUBLIC "OSPRIM_TRACKING=$<IF:$<CONFIG:Debug>,1,0>")
elseif(_osprim_tracking STREQUAL "ON")
  target_compile_definitions(osprim PUBLIC OSPRIM_TRACKING=1)
elseif(_osprim_tracking STREQUAL "OFF")
  target_compile_definitions(osprim PUBLIC OSPRIM_TRACKING=0)
else()
  message(FATAL_ERROR "osprim: OSPRIM_TRACKING must be AUTO, ON or OFF (got '${OSPRIM_TRACKING}')")
endif()

# The user configuration must be seen by the library and its users alike
if(OSPRIM_USER_CONFIG)
  target_compile_definitions(osprim PUBLIC "OSPRIM_USER_CONFIG=\"${OSPRIM_USER_CONFIG}\"")
endif()

set_target_properties(osprim PROPERTIES
  # the static library is meant to be linked into shared libraries too
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  CXX_EXTENSIONS OFF
)

# The library itself never throws and uses no RTTI. _HAS_EXCEPTIONS is left
# alone on MSVC: it changes the standard library headers, and mixing values
# across translation units of one program is an ODR violation.
if(MSVC)
  target_compile_options(osprim PRIVATE /W4 /permissive- /EHs-c- /GR-)
  if(OSPRIM_WARNINGS_AS_ERRORS)
    target_compile_options(osprim PRIVATE /WX)
  endif()
else()
  target_compile_options(osprim PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -fno-exceptions -fno-rtti)
  if(OSPRIM_WARNINGS_AS_ERRORS)
    target_compile_options(osprim PRIVATE -Werror)
  endif()
endif()

#####################################################################
# Install
#####################################################################

if(OSPRIM_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS osprim
    EXPORT osprimTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT osprimTargets
    NAMESPACE osprim::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osprim
  )
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/osprimConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/osprimConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osprim
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/osprimConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    # 0.x: minor versions break compatibility (and change the inline namespace)
    COMPATIBILITY SameMinorVersion
  )
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/osprimConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/osprimConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osprim
  )
endif()

#####################################################################
# Tests
#####################################################################

if(OSPRIM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
