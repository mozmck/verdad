# FindSWORD.cmake
# Find the SWORD Bible library
#
# This sets:
#   SWORD_FOUND        - True if SWORD was found
#   SWORD_INCLUDE_DIRS - SWORD include directories
#   SWORD_LIBRARIES    - SWORD libraries to link
#   SWORD_PREFIX       - Prefix where SWORD is installed

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_SWORD QUIET sword)
endif()

find_path(SWORD_INCLUDE_DIRS
    NAMES swmgr.h
    PATH_SUFFIXES sword
    HINTS
        ${PC_SWORD_INCLUDEDIR}
        ${PC_SWORD_INCLUDE_DIRS}
        ${CMAKE_PREFIX_PATH}/include
        ENV SWORD_DIR
    PATHS
        /usr/include
        /usr/local/include
        /opt/homebrew/include
)

# Determine static vs shared preference per platform
if(WIN32 OR APPLE)
    set(_sword_names sword sword_static)
else()
    set(_sword_names sword)
endif()

find_library(SWORD_LIBRARIES
    NAMES ${_sword_names}
    HINTS
        ${PC_SWORD_LIBDIR}
        ${PC_SWORD_LIBRARY_DIRS}
        ${CMAKE_PREFIX_PATH}/lib
        ENV SWORD_DIR
    PATHS
        /usr/lib
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu
        /opt/homebrew/lib
)

# Try to determine the SWORD prefix from the include path
if(SWORD_INCLUDE_DIRS)
    get_filename_component(_sword_inc_parent "${SWORD_INCLUDE_DIRS}" DIRECTORY)
    get_filename_component(SWORD_PREFIX "${_sword_inc_parent}" DIRECTORY)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SWORD
    REQUIRED_VARS SWORD_LIBRARIES SWORD_INCLUDE_DIRS
)

mark_as_advanced(SWORD_INCLUDE_DIRS SWORD_LIBRARIES SWORD_PREFIX)
