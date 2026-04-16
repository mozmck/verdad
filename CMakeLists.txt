cmake_minimum_required(VERSION 3.16)
project(Verdad VERSION 0.6.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# On Windows with MSVC, align Verdad, FLTK, litehtml, and vcpkg static
# dependencies on the static CRT (/MT). FLTK otherwise resets itself to /MD
# inside its own CMake options.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL
        "Use the static MSVC runtime for FLTK" FORCE)
endif()

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

# SQLite3 (FTS5 search index)
# vcpkg installs sqlite3 under the 'unofficial-sqlite3' cmake package name.
if(WIN32)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    if(NOT TARGET SQLite::SQLite3)
        add_library(SQLite::SQLite3 ALIAS unofficial::sqlite3::sqlite3)
    endif()
else()
    find_package(SQLite3 REQUIRED)
endif()

# litehtml (built as submodule)
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "Disable litehtml tests")
add_subdirectory(libs/litehtml)
target_include_directories(litehtml PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Threads (all platforms)
find_package(Threads REQUIRED)

# --- Source files ---
set(SOURCES
    src/main.cpp
    src/app/VerdadApp.cpp
    src/app/PlatformPaths.cpp
    src/import/ImportedModuleManager.cpp
    src/sword/SwordManager.cpp
    src/sword/SwordPaths.cpp
    src/search/SearchIndexer.cpp
    src/search/SmartSearch.cpp
    src/reading/ReadingPlanGenerator.cpp
    src/reading/ReadingPlanManager.cpp
    src/reading/ReadingPlanUtils.cpp
    src/tags/TagManager.cpp
    src/ui/MainWindow.cpp
    src/ui/BibleBookChoice.cpp
    src/ui/FilterableChoiceWidget.cpp
    src/ui/WrappingChoice.cpp
    src/ui/WrappingInputChoice.cpp
    src/ui/StyledTabs.cpp
    src/ui/ModuleManagerDialog.cpp
    src/ui/HtmlWidget.cpp
    src/ui/HtmlEditorWidget.cpp
    src/ui/MonthCalendarWidget.cpp
    src/ui/ReadingPlanEditorDialog.cpp
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
    src/import/ImportedModuleManager.h
    src/sword/SwordManager.h
    src/sword/SwordPaths.h
    src/search/SearchIndexer.h
    src/search/SmartSearch.h
    src/reading/DateUtils.h
    src/reading/ReadingPlanGenerator.h
    src/reading/ReadingPlanManager.h
    src/reading/ReadingPlanUtils.h
    src/tags/TagManager.h
    src/ui/MainWindow.h
    src/ui/BibleBookChoice.h
    src/ui/FilterableChoiceWidget.h
    src/ui/WrappingChoice.h
    src/ui/WrappingInputChoice.h
    src/ui/StyledTabs.h
    src/ui/ModuleManagerDialog.h
    src/ui/HtmlWidget.h
    src/ui/HtmlEditorWidget.h
    src/ui/MonthCalendarWidget.h
    src/ui/ReadingPlanEditorDialog.h
    src/ui/LeftPane.h
    src/ui/BiblePane.h
    src/ui/DailyWorkspaceState.h
    src/ui/RightPane.h
    src/ui/SearchPanel.h
    src/ui/ModulePanel.h
    src/ui/TagPanel.h
    src/ui/ChoicePopupUtils.h
    src/ui/UiFontUtils.h
    src/ui/VerseReferenceSort.h
    src/ui/VerseListCopyMenu.h
    src/ui/VerseContext.h
)

# --- Executable ---
add_executable(verdad ${SOURCES} ${HEADERS})

add_executable(verdad-bench
    src/bench_main.cpp
    src/app/PlatformPaths.cpp
    src/import/ImportedModuleManager.cpp
    src/sword/SwordManager.cpp
    src/sword/SwordPaths.cpp
    src/ui/HtmlWidget.cpp
)

if(WIN32)
    set_target_properties(verdad PROPERTIES WIN32_EXECUTABLE TRUE)
    # Prevent windows.h from defining min/max macros that conflict with std::min/std::max
    target_compile_definitions(verdad PRIVATE NOMINMAX)
    target_compile_definitions(verdad-bench PRIVATE NOMINMAX)
    # Static SWORD on Windows has unresolved refs to curl, zlib, and bzip2;
    # link them explicitly via the vcpkg-provided static packages.
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(BZip2 REQUIRED)
    target_link_libraries(verdad PRIVATE CURL::libcurl ZLIB::ZLIB BZip2::BZip2)
    target_link_libraries(verdad-bench PRIVATE CURL::libcurl ZLIB::ZLIB BZip2::BZip2)
endif()

target_include_directories(verdad PRIVATE
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/src
    ${SWORD_INCLUDE_DIRS}
)

target_include_directories(verdad-bench PRIVATE
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

target_link_libraries(verdad-bench PRIVATE
    fltk::images
    ${SWORD_LIBRARIES}
    SQLite::SQLite3
    litehtml
    Threads::Threads
)

target_compile_definitions(verdad-bench PRIVATE VERDAD_NO_APP_FONT_LOOKUP)

if(APPLE)
    target_link_libraries(verdad PRIVATE
        "-framework Cocoa"
        "-framework Carbon"
        "-framework ApplicationServices"
        # SWORD static library has external references to these macOS system libraries.
        z bz2 curl
    )
    target_link_libraries(verdad-bench PRIVATE
        "-framework Cocoa"
        "-framework Carbon"
        "-framework ApplicationServices"
        z bz2 curl
    )
elseif(WIN32)
    target_link_libraries(verdad PRIVATE shell32)
    target_link_libraries(verdad-bench PRIVATE shell32)
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
if(WIN32)
    install(TARGETS verdad
        RUNTIME DESTINATION .)
    install(FILES
        LICENSE
        DESTINATION .)
    install(FILES
        data/master.css
        data/help.html
        data/verdad_icon.png
        data/verdad_icon_128.png
        data/verdad_icon_24.png
        DESTINATION data)
    install(DIRECTORY LICENSES
        DESTINATION .)
    if(IS_DIRECTORY "${VERDAD_SWORD_SHARE_DIR}")
        install(DIRECTORY "${VERDAD_SWORD_SHARE_DIR}/"
            DESTINATION sword)
    endif()
else()
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
endif()

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
    set(VERDAD_WIN_MSI "${CMAKE_BINARY_DIR}/${VERDAD_BUNDLE_NAME}.msi")
    set(VERDAD_WIN_LICENSE_TXT "${CMAKE_BINARY_DIR}/LICENSE.txt")

    configure_file(
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${VERDAD_WIN_LICENSE_TXT}"
        COPYONLY
    )

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

    set(CPACK_PACKAGE_NAME "Verdad")
    set(CPACK_PACKAGE_VENDOR "Verdad maintainers")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Desktop Bible study application for CrossWire SWORD modules")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Verdad")
    set(CPACK_PACKAGE_FILE_NAME "${VERDAD_BUNDLE_NAME}")
    set(CPACK_PACKAGE_EXECUTABLES "verdad;Verdad")
    set(CPACK_RESOURCE_FILE_LICENSE "${VERDAD_WIN_LICENSE_TXT}")
    set(CPACK_WIX_UPGRADE_GUID "985E9F89-FBF0-4450-A40E-30D2D9920B97")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Verdad")
    set(CPACK_WIX_VERSION 3)
    include(CPack)

    add_custom_target(msi
        COMMAND ${CMAKE_COMMAND} -E rm -f "${VERDAD_WIN_MSI}"
        COMMAND ${CMAKE_CPACK_COMMAND} -G WIX -C $<CONFIG>
                --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
        DEPENDS verdad copy_runtime_assets
        COMMENT "Creating ${VERDAD_BUNDLE_NAME}.msi"
        VERBATIM
    )
endif()
