foreach(var WORK_DIR OUTPUT_FILE)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${var}")
    endif()
endforeach()

file(GLOB produced_appimages "${WORK_DIR}/*.AppImage")
list(LENGTH produced_appimages appimage_count)

if(NOT appimage_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one AppImage in ${WORK_DIR}, found ${appimage_count}")
endif()

list(GET produced_appimages 0 produced_appimage)
file(REMOVE "${OUTPUT_FILE}")
file(RENAME "${produced_appimage}" "${OUTPUT_FILE}")
message(STATUS "Created AppImage: ${OUTPUT_FILE}")
