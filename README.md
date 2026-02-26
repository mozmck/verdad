# Verdad Bible Study

A desktop Bible study application built with C++17, featuring:

- **FLTK** (statically linked) for the user interface
- **litehtml** for XHTML rendering of Bible text
- **SWORD** (CrossWire) library for Bible module access with FMT_XHTML rendering

## Features

### Three-Pane Layout (similar to Xiphos)

- **Left Pane** — Tabs for:
  - **Modules**: Tree view of all installed SWORD modules (Bibles, commentaries, dictionaries)
  - **Search**: Full-text search with multi-word, phrase, and regex modes; results list with preview
  - **Tags**: User-defined verse tags with verse lists per tag
  - Always-visible search box at top
  - Preview area at bottom for displaying selected search result text

- **Center Pane** — Bible text display:
  - Navigation bar with book/chapter selectors and reference input
  - Tabbed browsing of multiple Bible modules
  - Parallel Bible view (side-by-side columns)
  - Previous/next chapter navigation

- **Right Pane** — Commentary and dictionary:
  - Commentary tab with module selector
  - Dictionary/lexicon tab with module selector
  - Auto-updates commentary when navigating Bible text

### Additional Features (similar to BibleTime)

- **Word Hover Tooltip**: Hovering over words shows Strong's numbers, definitions, and morphology in a floating tooltip (like BibleTime's MAG viewer)
- **Right-Click Context Menu**:
  - Search for underlying Strong's number
  - Look up word in dictionary/lexicon
  - Add/remove verse tags
  - Copy verse reference or word
- **Parallel Bible View**: Display multiple Bible translations side-by-side
- **Verse Tagging**: Create custom tags, apply to any verse, browse all verses by tag

## Dependencies

### Required Libraries

| Library | Purpose | Notes |
|---------|---------|-------|
| [FLTK](https://www.fltk.org/) ≥ 1.3 | GUI toolkit | Linked statically |
| [litehtml](https://github.com/nicehash/nicehash-litehtml) | HTML/CSS renderer | |
| [SWORD](https://crosswire.org/sword/) ≥ 1.8 | Bible module access | |
| X11 / Xft / fontconfig | Display (Linux) | |

### Installing Dependencies

**Debian/Ubuntu:**
```bash
sudo apt-get install -y \
    libfltk1.3-dev \
    libsword-dev \
    libx11-dev libxft-dev libxrender-dev libfontconfig1-dev \
    libxinerama-dev libxcursor-dev libxfixes-dev \
    libpng-dev libjpeg-dev zlib1g-dev \
    cmake g++ pkg-config
```

For litehtml, you may need to build from source:
```bash
git clone https://github.com/nicehash/nicehash-litehtml.git
cd nicehash-litehtml
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make && sudo make install
```

**Fedora:**
```bash
sudo dnf install -y \
    fltk-devel fltk-static \
    sword-devel \
    libX11-devel libXft-devel libXrender-devel fontconfig-devel \
    libXinerama-devel libXcursor-devel libXfixes-devel \
    libpng-devel libjpeg-devel zlib-devel \
    cmake gcc-c++ pkgconfig
```

### Installing SWORD Modules

After installing the SWORD library, install Bible modules:
```bash
# Install the module manager
sudo apt-get install -y libsword-utils

# List available modules
installmgr -s          # List sources
installmgr -r          # Refresh remote sources
installmgr -l          # List available modules

# Install modules (examples)
installmgr -i KJV      # King James Version
installmgr -i ESV2011  # English Standard Version
installmgr -i MHC      # Matthew Henry Commentary
installmgr -i StrongsGreek
installmgr -i StrongsHebrew
installmgr -i Robinson # Robinson morphology codes
```

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Build Options

```bash
# Debug build
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Custom install prefix
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local

# Specify FLTK path if not found automatically
cmake .. -DFLTK_DIR=/path/to/fltk
```

### Install

```bash
sudo make install
```

## Running

```bash
./verdad
```

The application will:
1. Initialize the SWORD library and detect installed modules
2. Load user preferences and tags from `~/.config/verdad/`
3. Display the main three-pane window

## Project Structure

```
verdad/
├── CMakeLists.txt              # Build configuration
├── cmake/
│   └── FindSWORD.cmake         # CMake module to find SWORD library
├── data/
│   └── master.css              # Default CSS for Bible text rendering
└── src/
    ├── main.cpp                # Application entry point
    ├── app/
    │   ├── VerdadApp.h         # Main application class
    │   └── VerdadApp.cpp
    ├── sword/
    │   ├── SwordManager.h      # SWORD library wrapper
    │   └── SwordManager.cpp
    ├── tags/
    │   ├── TagManager.h        # Verse tagging system
    │   └── TagManager.cpp
    └── ui/
        ├── MainWindow.h/cpp    # Main window with three-pane layout
        ├── HtmlWidget.h/cpp    # FLTK widget using litehtml for XHTML
        ├── LeftPane.h/cpp      # Left pane (modules/search/tags)
        ├── BiblePane.h/cpp     # Center pane (Bible text with tabs)
        ├── RightPane.h/cpp     # Right pane (commentary/dictionary)
        ├── SearchPanel.h/cpp   # Search results panel
        ├── ModulePanel.h/cpp   # Module tree panel
        ├── TagPanel.h/cpp      # Tag management panel
        ├── ToolTipWindow.h/cpp # Floating tooltip for word info
        └── VerseContext.h/cpp  # Right-click context menu
```

## License

See [LICENSE](LICENSE) for details.
