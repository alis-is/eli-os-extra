cmake_minimum_required (VERSION 2.6)
project (eli_proc_extra)

file(GLOB eli_proc_extra_sources ./src/**.c)
SET(eli_proc_extra ${eli_proc_extra_sources})

ADD_LIBRARY (eli_proc_extra ${eli_proc_extra})
TARGET_LINK_LIBRARIES (eli_proc_extra)