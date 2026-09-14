cmake_minimum_required(VERSION 3.13)
project(eli_signal_coordination_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()
if(WIN32)
  message(STATUS "Signal coordination regression requires POSIX")
  return()
endif()
get_filename_component(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
file(GLOB LUA_SOURCES "${DEPS}/lua/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|linit)\\.c$")
add_executable(signal_coordination signal_coordination.c ${LUA_SOURCES}
  "${DEPS}/eli-os-extra/src/los.c"
  "${DEPS}/eli-os-extra/src/lcwd.c"
  "${DEPS}/eli-extra-utils/src/lsleep.c"
  "${DEPS}/eli-proc-extra/src/execve_spawnp.c"
  "${DEPS}/eli-extra-utils/src/eruntime.c"
  "${DEPS}/eli-extra-utils/src/lerror.c")
target_include_directories(signal_coordination PRIVATE
  "${DEPS}/lua" "${DEPS}/c11threads"
  "${DEPS}/eli-extra-utils/src" "${DEPS}/eli-os-extra/src"
  "${DEPS}/eli-stream-extra/src" "${DEPS}/eli-pipe-extra/src")
target_compile_definitions(signal_coordination PRIVATE _DEFAULT_SOURCE LUA_USE_POSIX)
target_compile_options(signal_coordination PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(signal_coordination PRIVATE -Wl,-dead_strip)
else()
  target_link_options(signal_coordination PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(signal_coordination PRIVATE Threads::Threads m ${CMAKE_DL_LIBS})
add_test(NAME signal_coordination COMMAND signal_coordination)
set_tests_properties(signal_coordination PROPERTIES TIMEOUT 30)
