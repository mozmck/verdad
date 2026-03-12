foreach(var APPDIR SWORD_SHARE_DIR SWORD_GLOBALS_CONF)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${var}")
    endif()
endforeach()

if(NOT EXISTS "${APPDIR}/usr/bin/verdad")
    message(FATAL_ERROR "Missing staged executable: ${APPDIR}/usr/bin/verdad")
endif()

if(NOT IS_DIRECTORY "${SWORD_SHARE_DIR}/locales.d")
    message(FATAL_ERROR
        "Missing SWORD locales directory under ${SWORD_SHARE_DIR}. Install libsword-common.")
endif()

if(NOT EXISTS "${SWORD_GLOBALS_CONF}")
    message(FATAL_ERROR
        "Missing SWORD globals.conf at ${SWORD_GLOBALS_CONF}. Install libsword-common.")
endif()

file(MAKE_DIRECTORY "${APPDIR}/usr/share/sword")
file(MAKE_DIRECTORY "${APPDIR}/usr/share/sword/mods.d")
file(COPY "${SWORD_SHARE_DIR}/locales.d" DESTINATION "${APPDIR}/usr/share/sword")
file(COPY "${SWORD_GLOBALS_CONF}" DESTINATION "${APPDIR}/usr/share/sword/mods.d")

set(desktop_file "${APPDIR}/usr/share/applications/verdad.desktop")
if(NOT EXISTS "${desktop_file}")
    message(FATAL_ERROR "Missing staged desktop file: ${desktop_file}")
endif()

file(READ "${desktop_file}" desktop_contents)
string(REGEX REPLACE "Exec=[^\n]*" "Exec=verdad" desktop_contents "${desktop_contents}")
string(REGEX REPLACE "TryExec=[^\n]*" "TryExec=verdad" desktop_contents "${desktop_contents}")
file(WRITE "${desktop_file}" "${desktop_contents}")
