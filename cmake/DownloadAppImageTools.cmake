foreach(var
        VERDAD_APPIMAGE_TOOLS_DIR
        VERDAD_LINUXDEPLOY_URL
        VERDAD_LINUXDEPLOY_APPIMAGE
        VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL
        VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${var}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${VERDAD_APPIMAGE_TOOLS_DIR}")

function(download_appimage url output)
    if(EXISTS "${output}")
        message(STATUS "Using existing ${output}")
    else()
        message(STATUS "Downloading ${url}")
        file(DOWNLOAD "${url}" "${output}" SHOW_PROGRESS STATUS status TLS_VERIFY ON)
        list(GET status 0 code)
        list(GET status 1 text)
        if(NOT code EQUAL 0)
            file(REMOVE "${output}")
            message(FATAL_ERROR "Download failed for ${url}: ${text}")
        endif()
    endif()

    execute_process(
        COMMAND /bin/chmod 0755 "${output}"
        RESULT_VARIABLE chmod_result
    )
    if(NOT chmod_result EQUAL 0)
        message(FATAL_ERROR "Failed to mark ${output} executable")
    endif()
endfunction()

download_appimage("${VERDAD_LINUXDEPLOY_URL}" "${VERDAD_LINUXDEPLOY_APPIMAGE}")
download_appimage("${VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL}" "${VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE}")
