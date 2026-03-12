cmake_minimum_required(VERSION 3.16)
project(Verdad VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add cmake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# --- Dependencies ---

# FLTK - use fltk-config to get correct compile and link flags
find_program(FLTK_CONFIG_SCRIPT fltk-config)
if(NOT FLTK_CONFIG_SCRIPT)
    message(FATAL_ERROR "fltk-config not found. Install FLTK development packages.")
endif()

execute_process(COMMAND ${FLTK_CONFIG_SCRIPT} --use-images --ldflags
    OUTPUT_VARIABLE FLTK_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(FLTK_LDFLAGS)
execute_process(COMMAND ${FLTK_CONFIG_SCRIPT} --includedir
    OUTPUT_VARIABLE FLTK_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

# SWORD library
find_package(SWORD REQUIRED)
if(NOT SWORD_FOUND)
    message(FATAL_ERROR "SWORD not found. Install libsword-dev.")
endif()

# SQLite3 (FTS5 search index)
find_package(SQLite3 REQUIRED)

# litehtml (built as submodule)
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "Disable litehtml tests")
add_subdirectory(libs/litehtml)

# Threads (for Linux)
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
endif()

# --- Source files ---
set(SOURCES
    src/main.cpp
    src/app/VerdadApp.cpp
    src/sword/SwordManager.cpp
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
    src/sword/SwordManager.h
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
    src/ui/VerseContext.h
)

# --- Executable ---
add_executable(verdad ${SOURCES} ${HEADERS})

target_include_directories(verdad PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${FLTK_INCLUDE_DIR}
    ${SWORD_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(verdad PRIVATE
    ${FLTK_LDFLAGS}
    ${SWORD_LIBRARIES}
    SQLite::SQLite3
    litehtml
)

if(UNIX AND NOT APPLE)
    target_link_libraries(verdad PRIVATE
        Threads::Threads
    )
endif()

if(APPLE)
    target_link_libraries(verdad PRIVATE
        "-framework Cocoa"
        "-framework Carbon"
        "-framework ApplicationServices"
    )
endif()

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
    VERBATIM
)
add_dependencies(verdad copy_runtime_assets)

# Standalone FLTK repro for search-results browser redraw behavior.
add_executable(browser_redraw_test
    tools/browser_redraw_test.cpp
)

target_include_directories(browser_redraw_test PRIVATE
    ${FLTK_INCLUDE_DIR}
)

target_link_libraries(browser_redraw_test PRIVATE
    ${FLTK_LDFLAGS}
)

if(UNIX AND NOT APPLE)
    target_link_libraries(browser_redraw_test PRIVATE
        Threads::Threads
    )
endif()

if(APPLE)
    target_link_libraries(browser_redraw_test PRIVATE
        "-framework Cocoa"
        "-framework Carbon"
        "-framework ApplicationServices"
    )
endif()

# Installation
install(TARGETS verdad DESTINATION bin)
install(FILES
    LICENSE
    data/master.css
    data/help.html
    data/verdad_icon.png
    data/verdad_icon_128.png
    DESTINATION share/verdad)
install(DIRECTORY LICENSES
    DESTINATION share/verdad)
install(FILES data/verdad_icon_128.png
    DESTINATION share/pixmaps
    RENAME verdad.png)
install(FILES data/verdad_icon_128.png
    DESTINATION share/icons/hicolor/128x128/apps
    RENAME verdad.png)
