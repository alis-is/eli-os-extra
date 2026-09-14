project(eli_os_extra)

file(GLOB eli_os_extra_sources ./src/*.c)
add_library(eli_os_extra ${eli_os_extra_sources})
