# Copilot Instructions for Verdad Bible Study

## Project Overview

Verdad is a desktop Bible study application written in **C++17**. It provides a three-pane layout similar to Xiphos and BibleTime, with full-text search, Strong's number hover tooltips, verse tagging, and bookmarking.

### Key Libraries

| Library | Purpose |
|---------|---------|
| [FLTK](https://www.fltk.org/) ≥ 1.3 | GUI toolkit (statically linked) |
| [litehtml](https://github.com/litehtml/litehtml) | XHTML/CSS rendering of Bible text |
| [SWORD](https://crosswire.org/sword/) ≥ 1.8 | Bible module access and FMT_XHTML rendering |

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**Dependencies (Debian/Ubuntu):**
```bash
sudo apt-get install -y \
    libfltk1.3-dev libsword-dev \
    libx11-dev libxft-dev libxrender-dev libfontconfig1-dev \
    libxinerama-dev libxcursor-dev libxfixes-dev \
    libpng-dev libjpeg-dev zlib1g-dev \
    cmake g++ pkg-config
```

There are no automated tests; build and manually run `./verdad` to verify changes.

## Architecture

```
src/
├── main.cpp                 # Entry point — constructs VerdadApp and calls run()
├── app/VerdadApp            # Singleton; owns all managers and the MainWindow
├── sword/SwordManager       # All SWORD library interactions (thread-safe via mutex)
├── tags/TagManager          # Verse tagging (persisted to ~/.config/verdad/)
├── bookmarks/BookmarkManager # Bookmark management (persisted to ~/.config/verdad/)
└── ui/
    ├── MainWindow           # Top-level Fl_Double_Window; owns the three panes
    ├── LeftPane             # Tabs: Modules, Search, Bookmarks, Tags
    ├── BiblePane            # Center pane: navigation bar + tabbed Bible text
    ├── RightPane            # Commentary and dictionary tabs
    ├── HtmlWidget           # FLTK widget wrapping litehtml for XHTML rendering
    ├── SearchPanel          # Search results list
    ├── ModulePanel          # Module tree view
    ├── BookmarkPanel        # Bookmark list
    ├── TagPanel             # Tag management
    ├── ToolTipWindow        # Floating word-info tooltip (Mag viewer)
    └── VerseContext         # Right-click context menu
```

User data is stored under `~/.config/verdad/`. The CSS for Bible text rendering lives in `data/master.css` and is installed to `share/verdad/`.

## Coding Conventions

- **Standard**: C++17; use `std::` types and algorithms.
- **Namespace**: All project code lives in the `verdad` namespace.
- **Include guards**: `#ifndef VERDAD_<SUBSYSTEM>_<CLASS>_H` / `#define ...` / `#endif`.
- **Doc comments**: Use `///` for public API (methods, classes, structs).
- **Member variables**: Private members use a trailing underscore (`app_`, `swordMgr_`).
- **Naming**:
  - Classes/structs: `PascalCase` (e.g., `SwordManager`, `ModuleInfo`)
  - Methods and local variables: `camelCase` (e.g., `getVerseText`, `moduleName`)
  - Constants/macros: `UPPER_SNAKE_CASE`
- **No copying**: Resource-owning classes delete the copy constructor and copy assignment operator.
- **Ownership**: Use `std::unique_ptr` for owned sub-objects; raw pointers for non-owning references.
- **FLTK callbacks**: Declare as `static void onXxx(Fl_Widget* w, void* data)` and cast `data` to the owning object.
- **Thread safety**: `SwordManager` uses `std::mutex`; do not call SWORD APIs from multiple threads without acquiring the lock.
- **Error handling**: Return `bool` for initialization routines; log errors to `std::cerr`.

## Common Patterns

### Adding a new UI panel

1. Create `src/ui/MyPanel.h` and `src/ui/MyPanel.cpp` following the style of existing panels (e.g., `SearchPanel`).
2. Inherit from the appropriate FLTK widget (`Fl_Group`, `Fl_Scroll`, etc.).
3. Add source files to `SOURCES` and `HEADERS` in `CMakeLists.txt`.
4. Instantiate and wire up in the relevant pane (`LeftPane`, `RightPane`, etc.).

### Adding a new SwordManager method

1. Declare the method in `SwordManager.h` with a `///` doc comment.
2. Implement in `SwordManager.cpp`; acquire `std::lock_guard<std::mutex> lock(mutex_)` before calling any SWORD API.
3. Use `getModule(name)` to retrieve a module; return an empty result if the module is not found.

### Persisting user data

Follow the pattern in `TagManager` or `BookmarkManager`: serialize to a JSON or simple text file inside `getConfigDir()`, and load in `initialize()`.
