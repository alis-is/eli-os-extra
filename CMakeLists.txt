project (eli_os_extra)

file(GLOB eli_proc_extra_utils_sources ./src/**.c)
set(eli_os_extra ${eli_proc_extra_utils_sources})

add_library(eli_os_extra ${eli_os_extra})
target_link_libraries(eli_os_extra)