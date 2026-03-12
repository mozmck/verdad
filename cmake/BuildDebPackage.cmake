foreach(var
        WORK_DIR
        PACKAGE_ROOT
        OUTPUT_FILE
        PACKAGE_NAME
        PACKAGE_VERSION
        PACKAGE_ARCH
        PACKAGE_SECTION
        PACKAGE_PRIORITY
        PACKAGE_MAINTAINER
        PACKAGE_SUMMARY
        PACKAGE_DESCRIPTION
        DPKG_DEB_EXECUTABLE
        DPKG_SHLIBDEPS_EXECUTABLE)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${var}")
    endif()
endforeach()

if(NOT EXISTS "${PACKAGE_ROOT}/usr/bin/verdad")
    message(FATAL_ERROR "Missing staged executable: ${PACKAGE_ROOT}/usr/bin/verdad")
endif()

set(package_control_dir "${PACKAGE_ROOT}/DEBIAN")
set(debian_work_dir "${WORK_DIR}/debian")
set(debian_control "${debian_work_dir}/control")
set(debian_substvars "${debian_work_dir}/substvars")

file(MAKE_DIRECTORY "${package_control_dir}")
file(MAKE_DIRECTORY "${debian_work_dir}")

file(WRITE "${debian_control}" [=[Source: ]=] "${PACKAGE_NAME}\n"
    [=[Section: ]=] "${PACKAGE_SECTION}\n"
    [=[Priority: ]=] "${PACKAGE_PRIORITY}\n"
    [=[Maintainer: ]=] "${PACKAGE_MAINTAINER}\n"
    "Standards-Version: 4.7.0\n\n"
    [=[Package: ]=] "${PACKAGE_NAME}\n"
    [=[Architecture: ]=] "${PACKAGE_ARCH}\n"
    [=[Description: ]=] "${PACKAGE_SUMMARY}\n"
    " ${PACKAGE_DESCRIPTION}\n")
file(WRITE "${debian_substvars}" "")

execute_process(
    COMMAND ${DPKG_SHLIBDEPS_EXECUTABLE}
            -O
            -T${debian_substvars}
            ${PACKAGE_ROOT}/usr/bin/verdad
    WORKING_DIRECTORY ${WORK_DIR}
    RESULT_VARIABLE shlibdeps_result
    OUTPUT_VARIABLE shlibdeps_output
    ERROR_VARIABLE shlibdeps_error
)
if(NOT shlibdeps_result EQUAL 0)
    message(FATAL_ERROR
        "dpkg-shlibdeps failed with code ${shlibdeps_result}:\n${shlibdeps_error}")
endif()

string(REGEX MATCH "shlibs:Depends=([^\n\r]*)" _ "${shlibdeps_output}")
set(shlibs_depends "${CMAKE_MATCH_1}")
string(STRIP "${shlibs_depends}" shlibs_depends)

set(package_depends "")
if(NOT shlibs_depends STREQUAL "")
    set(package_depends "${shlibs_depends}")
endif()
if(DEFINED PACKAGE_EXTRA_DEPENDS AND NOT "${PACKAGE_EXTRA_DEPENDS}" STREQUAL "")
    if(package_depends STREQUAL "")
        set(package_depends "${PACKAGE_EXTRA_DEPENDS}")
    else()
        set(package_depends "${package_depends}, ${PACKAGE_EXTRA_DEPENDS}")
    endif()
endif()

file(GLOB_RECURSE installed_entries RELATIVE "${PACKAGE_ROOT}" "${PACKAGE_ROOT}/usr/*")
list(SORT installed_entries)

set(installed_size_bytes 0)
set(md5_lines "")
foreach(relative_path IN LISTS installed_entries)
    set(absolute_path "${PACKAGE_ROOT}/${relative_path}")
    if(IS_DIRECTORY "${absolute_path}")
        continue()
    endif()

    file(SIZE "${absolute_path}" file_size)
    math(EXPR installed_size_bytes "${installed_size_bytes} + ${file_size}")

    file(MD5 "${absolute_path}" file_md5)
    string(APPEND md5_lines "${file_md5}  ${relative_path}\n")
endforeach()
math(EXPR installed_size_kib "(${installed_size_bytes} + 1023) / 1024")

file(WRITE "${package_control_dir}/control" [=[Package: ]=] "${PACKAGE_NAME}\n"
    [=[Version: ]=] "${PACKAGE_VERSION}\n"
    [=[Section: ]=] "${PACKAGE_SECTION}\n"
    [=[Priority: ]=] "${PACKAGE_PRIORITY}\n"
    [=[Architecture: ]=] "${PACKAGE_ARCH}\n"
    [=[Maintainer: ]=] "${PACKAGE_MAINTAINER}\n")
if(NOT package_depends STREQUAL "")
    file(APPEND "${package_control_dir}/control" [=[Depends: ]=] "${package_depends}\n")
endif()
file(APPEND "${package_control_dir}/control"
    [=[Installed-Size: ]=] "${installed_size_kib}\n"
    [=[Description: ]=] "${PACKAGE_SUMMARY}\n"
    " ${PACKAGE_DESCRIPTION}\n")

file(WRITE "${package_control_dir}/md5sums" "${md5_lines}")

file(REMOVE "${OUTPUT_FILE}")
execute_process(
    COMMAND ${DPKG_DEB_EXECUTABLE} --root-owner-group --build ${PACKAGE_ROOT} ${OUTPUT_FILE}
    RESULT_VARIABLE dpkg_deb_result
    OUTPUT_VARIABLE dpkg_deb_output
    ERROR_VARIABLE dpkg_deb_error
)
if(NOT dpkg_deb_result EQUAL 0)
    message(FATAL_ERROR
        "dpkg-deb failed with code ${dpkg_deb_result}:\n${dpkg_deb_error}")
endif()

if(NOT dpkg_deb_output STREQUAL "")
    message(STATUS "${dpkg_deb_output}")
endif()
