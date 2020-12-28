cmake_minimum_required (VERSION 2.6)
project (eli_proc_extra_utils)

file(GLOB eli_proc_extra_utils_sources ./src/**.c)
SET(eli_proc_extra_utils ${eli_proc_extra_utils_sources})

ADD_LIBRARY (eli_proc_extra_utils ${eli_proc_extra_utils})
TARGET_LINK_LIBRARIES (eli_proc_extra_utils)