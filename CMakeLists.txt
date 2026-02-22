cmake_minimum_required(VERSION 3.16)
project(Verdad VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add cmake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# --- Dependencies ---

# FLTK (static linking)
set(FLTK_SKIP_FLUID ON)
set(OPTION_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libs" FORCE)
find_package(FLTK REQUIRED)
if(NOT FLTK_FOUND)
    message(FATAL_ERROR "FLTK not found. Install FLTK development packages.")
endif()

# SWORD library
find_package(SWORD REQUIRED)
if(NOT SWORD_FOUND)
    message(FATAL_ERROR "SWORD not found. Install libsword-dev.")
endif()

# litehtml (built as submodule)
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "Disable litehtml tests" FORCE)
add_subdirectory(libs/litehtml)

# X11 and other system libraries (for Linux/FLTK)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_package(Threads REQUIRED)
endif()

# --- Source files ---
set(SOURCES
    src/main.cpp
    src/app/VerdadApp.cpp
    src/sword/SwordManager.cpp
    src/tags/TagManager.cpp
    src/bookmarks/BookmarkManager.cpp
    src/ui/MainWindow.cpp
    src/ui/HtmlWidget.cpp
    src/ui/LeftPane.cpp
    src/ui/BiblePane.cpp
    src/ui/RightPane.cpp
    src/ui/SearchPanel.cpp
    src/ui/ModulePanel.cpp
    src/ui/BookmarkPanel.cpp
    src/ui/TagPanel.cpp
    src/ui/ToolTipWindow.cpp
    src/ui/VerseContext.cpp
)

set(HEADERS
    src/app/VerdadApp.h
    src/sword/SwordManager.h
    src/tags/TagManager.h
    src/bookmarks/BookmarkManager.h
    src/ui/MainWindow.h
    src/ui/HtmlWidget.h
    src/ui/LeftPane.h
    src/ui/BiblePane.h
    src/ui/RightPane.h
    src/ui/SearchPanel.h
    src/ui/ModulePanel.h
    src/ui/BookmarkPanel.h
    src/ui/TagPanel.h
    src/ui/ToolTipWindow.h
    src/ui/VerseContext.h
)

# --- Executable ---
add_executable(verdad ${SOURCES} ${HEADERS})

target_include_directories(verdad PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${FLTK_INCLUDE_DIR}
    ${SWORD_INCLUDE_DIRS}
)

# Link libraries (FLTK statically)
target_link_libraries(verdad PRIVATE
    ${FLTK_LIBRARIES}
    ${SWORD_LIBRARIES}
    litehtml
)

if(UNIX AND NOT APPLE)
    target_link_libraries(verdad PRIVATE
        ${X11_LIBRARIES}
        Threads::Threads
        Xft Xrender fontconfig Xinerama Xcursor Xfixes
        png jpeg z dl m
    )
endif()

if(APPLE)
    target_link_libraries(verdad PRIVATE
        "-framework Cocoa"
        "-framework Carbon"
        "-framework ApplicationServices"
    )
endif()

# Installation
install(TARGETS verdad DESTINATION bin)
install(FILES data/master.css DESTINATION share/verdad)
