cmake_minimum_required(VERSION 3.16)
project(Verdad VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add cmake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# --- Dependencies ---

# Vendored submodules
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/fltk/CMakeLists.txt")
    message(FATAL_ERROR
        "FLTK submodule not found. Run: git submodule update --init --recursive")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/litehtml/CMakeLists.txt")
    message(FATAL_ERROR
        "litehtml submodule not found. Run: git submodule update --init --recursive")
endif()

# FLTK (vendored)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared FLTK libraries" FORCE)
set(FLTK_BUILD_TEST OFF CACHE BOOL "Build FLTK test programs" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "Build FLTK examples" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "Build FLUID" FORCE)
set(FLTK_BUILD_FLTK_OPTIONS OFF CACHE BOOL "Build fltk-options" FORCE)
set(FLTK_BUILD_FORMS OFF CACHE BOOL "Build the forms compatibility library" FORCE)
set(FLTK_BUILD_GL OFF CACHE BOOL "Build the OpenGL FLTK library" FORCE)
set(FLTK_OPTION_PRINT_SUPPORT OFF CACHE BOOL "Enable FLTK print support" FORCE)
set(FLTK_BACKEND_WAYLAND OFF CACHE BOOL "Enable FLTK Wayland backend" FORCE)
set(FLTK_USE_PANGO OFF CACHE BOOL "Use Pango for FLTK text rendering" FORCE)
set(FLTK_GRAPHICS_CAIRO OFF CACHE BOOL "Use Cairo for FLTK drawing" FORCE)
set(FLTK_USE_SYSTEM_LIBJPEG OFF CACHE BOOL "Use system libjpeg" FORCE)
set(FLTK_USE_SYSTEM_LIBPNG OFF CACHE BOOL "Use system libpng" FORCE)
set(FLTK_USE_SYSTEM_ZLIB OFF CACHE BOOL "Use system zlib" FORCE)
add_subdirectory(libs/fltk)

# SWORD library
find_package(SWORD REQUIRED)
if(NOT SWORD_FOUND)
    message(FATAL_ERROR "SWORD not found. Install libsword-dev.")
endif()

if(NOT WIN32)
    find_path(VERDAD_SWORD_SHARE_DIR
        NAMES locales.d
        PATHS
            ${SWORD_PREFIX}/share/sword
            /usr/share/sword
            /usr/local/share/sword
            /opt/homebrew/share/sword
    )

    find_file(VERDAD_SWORD_GLOBALS_CONF
        NAMES globals.conf
        PATH_SUFFIXES mods.d
        HINTS ${VERDAD_SWORD_SHARE_DIR}
    )
endif()

# SQLite3 (FTS5 search index)
find_package(SQLite3 REQUIRED)

# litehtml (built as submodule)
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "Disable litehtml tests")
add_subdirectory(libs/litehtml)

# Threads (all platforms)
find_package(Threads REQUIRED)

# --- Source files ---
set(SOURCES
    src/main.cpp
    src/app/VerdadApp.cpp
    src/app/PlatformPaths.cpp
    src/sword/SwordManager.cpp
    src/sword/SwordPaths.cpp
    src/search/SearchIndexer.cpp
    src/tags/TagManager.cpp
    src/ui/MainWindow.cpp
    src/ui/FilterableChoiceWidget.cpp
    src/ui/StyledTabs.cpp
    src/ui/ModuleManagerDialog.cpp
    src/ui/HtmlWidget.cpp
    src/ui/HtmlEditorWidget.cpp
    src/ui/LeftPane.cpp
    src/ui/BiblePane.cpp
    src/ui/RightPane.cpp
    src/ui/SearchPanel.cpp
    src/ui/ModulePanel.cpp
    src/ui/TagPanel.cpp
    src/ui/VerseContext.cpp
)

set(HEADERS
    src/app/VerdadApp.h
    src/app/PerfTrace.h
    src/app/PlatformPaths.h
    src/sword/SwordManager.h
    src/sword/SwordPaths.h
    src/search/SearchIndexer.h
    src/tags/TagManager.h
    src/ui/MainWindow.h
    src/ui/FilterableChoiceWidget.h
    src/ui/StyledTabs.h
    src/ui/ModuleManagerDialog.h
    src/ui/HtmlWidget.h
    src/ui/HtmlEditorWidget.h
    src/ui/LeftPane.h
    src/ui/BiblePane.h
    src/ui/RightPane.h
    src/ui/SearchPanel.h
    src/ui/ModulePanel.h
    src/ui/TagPanel.h
    src/ui/UiFontUtils.h
    src/ui/VerseReferenceSort.h
    src/ui/VerseListCopyMenu.h
    src/ui/VerseContext.h
)

# --- Executable ---
add_executable(verdad ${SOURCES} ${HEADERS})

if(WIN32)
    set_target_properties(verdad PROPERTIES WIN32_EXECUTABLE TRUE)
    # Prevent windows.h from defining min/max macros that conflict with std::min/std::max
    target_compile_definitions(verdad PRIVATE NOMINMAX)
endif()

target_include_directories(verdad PRIVATE
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/src
    ${SWORD_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(verdad PRIVATE
    fltk::images
    ${SWORD_LIBRARIES}
    SQLite::SQLite3
    litehtml
    Threads::Threads
)

if(APPLE)
    target_link_libraries(verdad PRIVATE
        "-framework Cocoa"
        "-framework Carbon"
        "-framework ApplicationServices"
    )
elseif(WIN32)
    target_link_libraries(verdad PRIVATE shell32)
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/src/app/BuildConfig.h.in"
    "${CMAKE_BINARY_DIR}/generated/app/BuildConfig.h"
    @ONLY
)

if(UNIX AND NOT APPLE)
    configure_file(
        "${CMAKE_SOURCE_DIR}/data/verdad.desktop.in"
        "${CMAKE_BINARY_DIR}/verdad.desktop"
        @ONLY
    )

    configure_file(
        "${CMAKE_SOURCE_DIR}/data/io.github.mozmck.Verdad.metainfo.xml.in"
        "${CMAKE_BINARY_DIR}/io.github.mozmck.Verdad.metainfo.xml"
        @ONLY
    )
endif()

if(APPLE)
    configure_file(
        "${CMAKE_SOURCE_DIR}/data/Info.plist.in"
        "${CMAKE_BINARY_DIR}/Info.plist"
        @ONLY
    )
endif()

set(VERDAD_BUNDLE_NAME
    "verdad-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(VERDAD_BUNDLE_DIR "${CMAKE_BINARY_DIR}/${VERDAD_BUNDLE_NAME}")

# Copy runtime CSS asset into the build tree on every build invocation.
add_custom_target(copy_runtime_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/data"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/data/master.css"
            "${CMAKE_BINARY_DIR}/data/master.css"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/data/help.html"
            "${CMAKE_BINARY_DIR}/data/help.html"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/data/verdad_icon.png"
            "${CMAKE_BINARY_DIR}/data/verdad_icon.png"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/data/verdad_icon_128.png"
            "${CMAKE_BINARY_DIR}/data/verdad_icon_128.png"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/data/verdad_icon_24.png"
            "${CMAKE_BINARY_DIR}/data/verdad_icon_24.png"
    VERBATIM
)
add_dependencies(verdad copy_runtime_assets)

# Installation
install(TARGETS verdad DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    LICENSE
    data/master.css
    data/help.html
    data/verdad_icon.png
    data/verdad_icon_128.png
    data/verdad_icon_24.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/verdad)
install(DIRECTORY LICENSES
    DESTINATION ${CMAKE_INSTALL_DATADIR}/verdad)

if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_BINARY_DIR}/verdad.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES "${CMAKE_BINARY_DIR}/io.github.mozmck.Verdad.metainfo.xml"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES data/verdad_icon_128.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
        RENAME verdad.png)
    install(FILES data/verdad_icon_128.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps
        RENAME verdad.png)
    install(FILES data/verdad_icon_24.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/24x24/apps
        RENAME verdad.png)
endif()

# ============================================================================
# Linux packaging targets (bundle, deb, AppImage)
# ============================================================================
if(UNIX AND NOT APPLE)
    set(VERDAD_BUNDLE_ARCHIVE "${CMAKE_BINARY_DIR}/${VERDAD_BUNDLE_NAME}.tar.gz")

    add_custom_command(
        OUTPUT "${VERDAD_BUNDLE_ARCHIVE}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${VERDAD_BUNDLE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${VERDAD_BUNDLE_ARCHIVE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VERDAD_BUNDLE_DIR}/bin"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VERDAD_BUNDLE_DIR}/share/verdad"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VERDAD_BUNDLE_DIR}/share/metainfo"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:verdad>"
                "${VERDAD_BUNDLE_DIR}/bin/verdad"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE"
                "${VERDAD_BUNDLE_DIR}/share/verdad/LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/master.css"
                "${VERDAD_BUNDLE_DIR}/share/verdad/master.css"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/help.html"
                "${VERDAD_BUNDLE_DIR}/share/verdad/help.html"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon.png"
                "${VERDAD_BUNDLE_DIR}/share/verdad/verdad_icon.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon_128.png"
                "${VERDAD_BUNDLE_DIR}/share/verdad/verdad_icon_128.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon_24.png"
                "${VERDAD_BUNDLE_DIR}/share/verdad/verdad_icon_24.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/io.github.mozmck.Verdad.metainfo.xml"
                "${VERDAD_BUNDLE_DIR}/share/metainfo/io.github.mozmck.Verdad.metainfo.xml"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/LICENSES"
                "${VERDAD_BUNDLE_DIR}/share/verdad/LICENSES"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/install.sh"
                "${VERDAD_BUNDLE_DIR}/install.sh"
        COMMAND /bin/chmod 0755 "${VERDAD_BUNDLE_DIR}/bin/verdad"
        COMMAND /bin/chmod 0755 "${VERDAD_BUNDLE_DIR}/install.sh"
        COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}"
                ${CMAKE_COMMAND} -E tar "cfvz" "${VERDAD_BUNDLE_ARCHIVE}"
                "${VERDAD_BUNDLE_NAME}"
        DEPENDS verdad copy_runtime_assets "${CMAKE_SOURCE_DIR}/install.sh"
        VERBATIM
    )

    add_custom_target(bundle
        DEPENDS "${VERDAD_BUNDLE_ARCHIVE}"
    )
    find_program(VERDAD_DPKG_EXECUTABLE NAMES dpkg)
    find_program(VERDAD_DPKG_DEB_EXECUTABLE NAMES dpkg-deb)
    find_program(VERDAD_DPKG_SHLIBDEPS_EXECUTABLE NAMES dpkg-shlibdeps)

    if(VERDAD_DPKG_EXECUTABLE AND VERDAD_DPKG_DEB_EXECUTABLE
       AND VERDAD_DPKG_SHLIBDEPS_EXECUTABLE)
        execute_process(
            COMMAND ${VERDAD_DPKG_EXECUTABLE} --print-architecture
            OUTPUT_VARIABLE VERDAD_DEB_ARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(NOT VERDAD_DEB_ARCH)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
                set(VERDAD_DEB_ARCH "amd64")
            else()
                set(VERDAD_DEB_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
            endif()
        endif()

        set(VERDAD_DEB_WORK_DIR "${CMAKE_BINARY_DIR}/deb")
        set(VERDAD_DEB_PACKAGE_ROOT "${VERDAD_DEB_WORK_DIR}/pkgroot")
        set(VERDAD_DEB_OUTPUT
            "${CMAKE_BINARY_DIR}/verdad_${PROJECT_VERSION}_${VERDAD_DEB_ARCH}.deb")

        add_custom_target(deb
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${VERDAD_DEB_WORK_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${VERDAD_DEB_WORK_DIR}
            COMMAND ${CMAKE_COMMAND} -E env
                    DESTDIR=${VERDAD_DEB_PACKAGE_ROOT}
                    ${CMAKE_COMMAND}
                    -DCMAKE_INSTALL_PREFIX=/usr
                    -DCMAKE_INSTALL_CONFIG_NAME=${CMAKE_BUILD_TYPE}
                    -DCMAKE_INSTALL_LOCAL_ONLY=1
                    -P ${CMAKE_BINARY_DIR}/cmake_install.cmake
            COMMAND ${CMAKE_COMMAND}
                    -DWORK_DIR=${VERDAD_DEB_WORK_DIR}
                    -DPACKAGE_ROOT=${VERDAD_DEB_PACKAGE_ROOT}
                    -DOUTPUT_FILE=${VERDAD_DEB_OUTPUT}
                    -DPACKAGE_NAME=verdad
                    -DPACKAGE_VERSION=${PROJECT_VERSION}
                    -DPACKAGE_ARCH=${VERDAD_DEB_ARCH}
                    -DPACKAGE_SECTION=education
                    -DPACKAGE_PRIORITY=optional
                    -DPACKAGE_MAINTAINER=Verdad\ maintainers
                    -DPACKAGE_SUMMARY=Desktop\ Bible\ study\ application\ for\ CrossWire\ SWORD\ modules
                    -DPACKAGE_DESCRIPTION=Verdad\ is\ a\ desktop\ Bible\ study\ application\ for\ CrossWire\ SWORD\ modules.
                    -DPACKAGE_EXTRA_DEPENDS=libsword-common
                    -DDPKG_DEB_EXECUTABLE=${VERDAD_DPKG_DEB_EXECUTABLE}
                    -DDPKG_SHLIBDEPS_EXECUTABLE=${VERDAD_DPKG_SHLIBDEPS_EXECUTABLE}
                    -P ${CMAKE_SOURCE_DIR}/cmake/BuildDebPackage.cmake
            DEPENDS verdad copy_runtime_assets
            USES_TERMINAL
            VERBATIM
        )
    else()
        message(STATUS
            "Debian packaging target disabled; dpkg, dpkg-deb, or dpkg-shlibdeps not found.")
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
        set(VERDAD_APPIMAGE_ARCH "x86_64")
    else()
        set(VERDAD_APPIMAGE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(VERDAD_APPIMAGE_TOOLS_DIR "${CMAKE_SOURCE_DIR}/tools/appimage-tools")
    set(VERDAD_LINUXDEPLOY_APPIMAGE
        "${VERDAD_APPIMAGE_TOOLS_DIR}/linuxdeploy-${VERDAD_APPIMAGE_ARCH}.AppImage")
    set(VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE
        "${VERDAD_APPIMAGE_TOOLS_DIR}/linuxdeploy-plugin-appimage-${VERDAD_APPIMAGE_ARCH}.AppImage")
    set(VERDAD_LINUXDEPLOY_URL
        "https://github.com/linuxdeploy/linuxdeploy/releases/latest/download/linuxdeploy-${VERDAD_APPIMAGE_ARCH}.AppImage"
        CACHE STRING "Download URL for linuxdeploy")
    set(VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL
        "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/latest/download/linuxdeploy-plugin-appimage-${VERDAD_APPIMAGE_ARCH}.AppImage"
        CACHE STRING "Download URL for the linuxdeploy AppImage plugin")

    set(VERDAD_APPIMAGE_WORK_DIR "${CMAKE_BINARY_DIR}/appimage")
    set(VERDAD_APPIMAGE_APPDIR "${VERDAD_APPIMAGE_WORK_DIR}/AppDir")
    set(VERDAD_APPIMAGE_OUTPUT
        "${CMAKE_BINARY_DIR}/verdad-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${VERDAD_APPIMAGE_ARCH}.AppImage")

    add_custom_target(appimage
        COMMAND ${CMAKE_COMMAND}
                -DVERDAD_APPIMAGE_TOOLS_DIR=${VERDAD_APPIMAGE_TOOLS_DIR}
                -DVERDAD_LINUXDEPLOY_URL=${VERDAD_LINUXDEPLOY_URL}
                -DVERDAD_LINUXDEPLOY_APPIMAGE=${VERDAD_LINUXDEPLOY_APPIMAGE}
                -DVERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL=${VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL}
                -DVERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE=${VERDAD_LINUXDEPLOY_PLUGIN_APPIMAGE}
                -P ${CMAKE_SOURCE_DIR}/cmake/DownloadAppImageTools.cmake
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${VERDAD_APPIMAGE_WORK_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VERDAD_APPIMAGE_WORK_DIR}
        COMMAND ${CMAKE_COMMAND} -E env
                DESTDIR=${VERDAD_APPIMAGE_APPDIR}
                ${CMAKE_COMMAND}
                -DCMAKE_INSTALL_PREFIX=/usr
                -DCMAKE_INSTALL_CONFIG_NAME=${CMAKE_BUILD_TYPE}
                -DCMAKE_INSTALL_LOCAL_ONLY=1
                -P ${CMAKE_BINARY_DIR}/cmake_install.cmake
        COMMAND ${CMAKE_COMMAND}
                -DAPPDIR=${VERDAD_APPIMAGE_APPDIR}
                -DSWORD_SHARE_DIR=${VERDAD_SWORD_SHARE_DIR}
                -DSWORD_GLOBALS_CONF=${VERDAD_SWORD_GLOBALS_CONF}
                -P ${CMAKE_SOURCE_DIR}/cmake/PrepareAppImage.cmake
        COMMAND ${CMAKE_COMMAND} -E chdir ${VERDAD_APPIMAGE_WORK_DIR}
                ${CMAKE_COMMAND} -E env
                APPIMAGE_EXTRACT_AND_RUN=1
                ARCH=${VERDAD_APPIMAGE_ARCH}
                ${VERDAD_LINUXDEPLOY_APPIMAGE}
                --appdir ${VERDAD_APPIMAGE_APPDIR}
                --desktop-file ${VERDAD_APPIMAGE_APPDIR}/usr/share/applications/verdad.desktop
                --icon-file ${VERDAD_APPIMAGE_APPDIR}/usr/share/icons/hicolor/128x128/apps/verdad.png
                --executable ${VERDAD_APPIMAGE_APPDIR}/usr/bin/verdad
                --library ${SWORD_LIBRARIES}
                --output appimage
        COMMAND ${CMAKE_COMMAND}
                -DWORK_DIR=${VERDAD_APPIMAGE_WORK_DIR}
                -DOUTPUT_FILE=${VERDAD_APPIMAGE_OUTPUT}
                -P ${CMAKE_SOURCE_DIR}/cmake/RenameAppImage.cmake
        DEPENDS verdad copy_runtime_assets
        USES_TERMINAL
        VERBATIM
    )
endif()

# ============================================================================
# macOS packaging target (app bundle)
# ============================================================================
if(APPLE)
    set(VERDAD_APP_BUNDLE "${CMAKE_BINARY_DIR}/Verdad.app")
    set(VERDAD_APP_CONTENTS "${VERDAD_APP_BUNDLE}/Contents")
    set(VERDAD_APP_RESOURCES "${VERDAD_APP_CONTENTS}/Resources")
    set(VERDAD_APP_MACOS "${VERDAD_APP_CONTENTS}/MacOS")

    add_custom_target(app
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${VERDAD_APP_BUNDLE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VERDAD_APP_MACOS}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VERDAD_APP_RESOURCES}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:verdad>"
                "${VERDAD_APP_MACOS}/verdad"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/Info.plist"
                "${VERDAD_APP_CONTENTS}/Info.plist"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/master.css"
                "${VERDAD_APP_RESOURCES}/master.css"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/help.html"
                "${VERDAD_APP_RESOURCES}/help.html"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon.png"
                "${VERDAD_APP_RESOURCES}/verdad_icon.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon_128.png"
                "${VERDAD_APP_RESOURCES}/verdad_icon_128.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon_24.png"
                "${VERDAD_APP_RESOURCES}/verdad_icon_24.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE"
                "${VERDAD_APP_RESOURCES}/LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/LICENSES"
                "${VERDAD_APP_RESOURCES}/LICENSES"
        DEPENDS verdad copy_runtime_assets
        COMMENT "Assembling Verdad.app bundle"
        VERBATIM
    )

    set(VERDAD_DMG_OUTPUT "${CMAKE_BINARY_DIR}/${VERDAD_BUNDLE_NAME}.dmg")
    add_custom_target(dmg
        COMMAND ${CMAKE_COMMAND} -E rm -f "${VERDAD_DMG_OUTPUT}"
        COMMAND hdiutil create -volname "Verdad"
                -srcfolder "${VERDAD_APP_BUNDLE}"
                -ov -format UDZO
                "${VERDAD_DMG_OUTPUT}"
        DEPENDS app
        COMMENT "Creating ${VERDAD_BUNDLE_NAME}.dmg"
        VERBATIM
    )
endif()

# ============================================================================
# Windows packaging target (zip)
# ============================================================================
if(WIN32)
    set(VERDAD_WIN_DIR "${VERDAD_BUNDLE_DIR}")
    set(VERDAD_WIN_ZIP "${CMAKE_BINARY_DIR}/${VERDAD_BUNDLE_NAME}.zip")

    add_custom_target(winzip
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${VERDAD_WIN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VERDAD_WIN_DIR}/data"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:verdad>"
                "${VERDAD_WIN_DIR}/verdad.exe"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/master.css"
                "${VERDAD_WIN_DIR}/data/master.css"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/help.html"
                "${VERDAD_WIN_DIR}/data/help.html"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon.png"
                "${VERDAD_WIN_DIR}/data/verdad_icon.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon_128.png"
                "${VERDAD_WIN_DIR}/data/verdad_icon_128.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/verdad_icon_24.png"
                "${VERDAD_WIN_DIR}/data/verdad_icon_24.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE"
                "${VERDAD_WIN_DIR}/LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/LICENSES"
                "${VERDAD_WIN_DIR}/LICENSES"
        COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}"
                ${CMAKE_COMMAND} -E tar "cf" "${VERDAD_WIN_ZIP}" --format=zip
                "${VERDAD_BUNDLE_NAME}"
        DEPENDS verdad copy_runtime_assets
        COMMENT "Creating ${VERDAD_BUNDLE_NAME}.zip"
        VERBATIM
    )
endif()
