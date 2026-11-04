if(NOT TARGET doctest::doctest)
  include(FetchContent)
  # doctest declares its options with an old policy level: only cache
  # variables are honored.
  set(DOCTEST_WITH_TESTS OFF CACHE BOOL "Build doctest tests")
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "Skip doctest install rules")
  # doctest 2.4.11 declares cmake_minimum_required(VERSION 3.0), which
  # CMake 4 rejects unless a minimum policy version is provided.
  if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  endif()
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(doctest)
endif()

find_package(Threads REQUIRED)

if(OSPRIM_SHARED)
  # The internal functions the main suite drives (tracking registry, tick
  # conversion, parsers) are not exported from the shared library: the
  # shared build runs its own tests, through the public API only (the
  # static builds run the main suite).
  add_library(osprim_shared_plugin SHARED shared_plugin.cpp)
  target_link_libraries(osprim_shared_plugin PRIVATE osprim::osprim)
  set_target_properties(osprim_shared_plugin PROPERTIES CXX_EXTENSIONS OFF)

  add_executable(osprim_tests main.cpp support.hpp files.hpp test_shared.cpp)
  target_link_libraries(osprim_tests PRIVATE
    osprim::osprim osprim_shared_plugin doctest::doctest Threads::Threads)
  target_compile_features(osprim_tests PRIVATE cxx_std_20)
  set_target_properties(osprim_tests PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(osprim_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(osprim_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(WIN32)
    # Windows finds DLLs next to the executable: copy osprim and the plugin there
    add_custom_command(TARGET osprim_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:osprim_tests> $<TARGET_FILE_DIR:osprim_tests>
      COMMAND_EXPAND_LISTS
    )
  endif()
  return()
endif()

# The shared library the dynamic library tests load. Named as DYNLIB_PREFIX
# and DYNLIB_SUFFIX say (MinGW would otherwise prefix "lib" on Windows).
add_library(osprim_test_plugin SHARED plugin.cpp)
set_target_properties(osprim_test_plugin PROPERTIES CXX_EXTENSIONS OFF)
if(WIN32)
  set_target_properties(osprim_test_plugin PROPERTIES PREFIX "")
  if(MINGW)
    # No libgcc DLL needed to load it
    target_link_options(osprim_test_plugin PRIVATE -static-libgcc)
  endif()
endif()

add_executable(osprim_tests
  main.cpp
  support.hpp
  files.hpp
  user_config.hpp
  test_base.cpp
  test_clock.cpp
  test_wait.cpp
  test_thread.cpp
  test_power.cpp
  test_topology.cpp
  test_file.cpp
  test_region.cpp
  test_path.cpp
  test_dynlib.cpp
  test_macros.cpp
)
target_link_libraries(osprim_tests PRIVATE osprim::osprim doctest::doctest Threads::Threads)
add_dependencies(osprim_tests osprim_test_plugin)
target_compile_definitions(osprim_tests PRIVATE "OSPRIM_TEST_PLUGIN_PATH=\"$<TARGET_FILE:osprim_test_plugin>\"")
# The tests reach into src/internal.hpp to drive the tracking registry
# and the tick conversion directly.
target_include_directories(osprim_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(osprim_tests PRIVATE cxx_std_20)
set_target_properties(osprim_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(osprim_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(osprim_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
