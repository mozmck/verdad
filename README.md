# Verdad Bible Study

Verdad is a desktop Bible study application for CrossWire SWORD modules. It keeps Bible reading, commentary, dictionaries, general books, search, tags, and notes in one window.

## Features

- Multiple study tabs. Duplicate the current workspace, keep multiple passages open, and restore your tab set on the next launch.
- Three-pane study layout. The left pane handles modules, search, tags, and preview; the center pane shows Bible text; the right pane shows commentary, dictionaries, general books, and documents.
- Fast Bible search. Search by multi-word query, exact phrase, regex, or Strong's/lemma references, with canonical result ordering and a live preview pane.
- Search results workflow. Single-click a result to preview it, double-click to navigate the current study tab, or middle-click to open it in a new study tab.
- Bible reading tools. Jump by book, chapter, or typed reference; move chapter to chapter; switch between verse and paragraph mode; and compare up to seven Bible modules in parallel columns.
- Study marker controls. Toggle visible Strong's, morphology, footnote, and cross-reference markers without losing the underlying hover and context-menu data.
- Word study actions. Hovering a Bible word fills the lower-left preview pane, and right-clicking exposes word search, Strong's search, dictionary lookup, copy, and tagging actions.
- Synced right pane. Commentary follows the selected verse, the dictionary pane stays available at the bottom, and general books use the module table of contents directly.
- General books support. Pick a table of contents entry from the chooser, load one page at a time, and follow internal links without leaving the pane.
- Verse tagging. Create tags, rename or delete them, filter tags by name or verse reference, browse all tagged verses, and jump from a tag entry back into the Bible.
- Notes and editable content. The Documents tab opens and saves HTML study notes with lightweight rich-text editing, and writable commentary modules can be edited in place.
- Session persistence. Verdad remembers window geometry, splitter sizes, active study tab, Bible state, right-pane selections, open document path, and scroll positions.
- Built-in module management. Add local or remote SWORD sources, refresh them, and install or update modules from inside the app.
- Appearance and dictionary preferences. Choose UI font, text font and size, hover delay, default Greek and Hebrew Strong's dictionaries, and language-specific word dictionaries.

## Everyday workflow

The left pane combines the always-visible search box with three tabs: `Modules`, `Search`, and `Tags`. The preview area at the bottom is shared by search results, module metadata, word-hover preview content, and link previews from Bible, commentary, and general-book panes.

The center pane is the active Bible workspace. The right pane keeps commentary, general books, and the global Documents editor in top tabs, with the dictionary or lexicon pane docked below them. Scripture links inside commentary and general books can populate the preview pane or the Search tab, depending on whether a link resolves to one verse or many.

## SWORD modules and local data

Verdad is useful only after SWORD modules are installed. You can install them with your normal SWORD tooling or from `File > Module Manager...`. The module manager supports local sources and remote sources. Remote sources can reveal network activity, so use local sources when that matters for your situation.

Verdad stores user state in `~/.config/verdad/` on Linux:

- `preferences.conf` for appearance, layout, and restored session state
- `module_index.db` for the background SQLite search index
- `tags.db` for verse tags

## Build and run

Verdad currently builds as a C++17 desktop application with CMake.

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/verdad
```

Fresh single-config builds default to `Release` if you do not set `CMAKE_BUILD_TYPE` yourself. Use `Debug` only when you actually need it:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```

On a recent Debian/Ubuntu-based distribution, these packages are typically enough to build Verdad from source:

```bash
sudo apt install build-essential cmake pkg-config \
    git libsword-dev libsqlite3-dev \
    libx11-dev libxext-dev libxinerama-dev libxfixes-dev \
    libxcursor-dev libxrender-dev libxft-dev \
    libfontconfig-dev libfreetype-dev
```

To install and run a prebuilt Verdad binary without the compiler toolchain, install the runtime libraries instead:

```bash
sudo apt install libsword1.9.0 libsword-common libsqlite3-0 \
    libx11-6 libxext6 libxinerama1 libxfixes3 \
    libxcursor1 libxrender1 libxft2 libfontconfig1
```

FLTK 1.4.4 is vendored under `libs/fltk` and linked statically, so you do not need distro FLTK runtime or `-dev` packages. If you cloned the repository without submodules, run `git submodule update --init --recursive` before configuring.

On newer releases, the SWORD runtime package name may differ slightly from `libsword1.9.0`; if that exact package is unavailable, install the distro's current `libsword` runtime package instead. `litehtml` is vendored under `libs/litehtml`, so it is built with the project instead of being installed separately. `data/master.css` and `data/help.html` are copied into `build/data/` automatically during the build.

To build an AppImage on Linux:

```bash
cmake --build build --target appimage
```

On the first run, that target downloads `linuxdeploy` and the AppImage output plugin into `tools/appimage-tools/`, stages an `AppDir` under `build/appimage/`, bundles the current `libsword` along with the installed SWORD common data, and writes the final AppImage into `build/`. It does not bundle any SWORD modules. Install `libsword-common` as well if you want to use the `appimage` target.

To build the installer archive:

```bash
cmake --build build --target bundle
```

That target writes a `verdad-<version>-<system>-<arch>.tar.gz` archive into `build/`. The archive contains the built Verdad binary, its data files, licenses, and `install.sh` so it can be unpacked and installed under `~/.local` or `/usr/local`. Unlike the AppImage, it is not a fully self-contained runtime bundle.

There is no dedicated automated test suite yet. Validation is currently a successful build plus manual UI smoke testing in the running application.

## Install from source

Use the repo-level installer:

```bash
./install.sh
```

By default, `install.sh` installs for the current user under `~/.local`, and
that default can be accepted by pressing Enter. To install system-wide under
`/usr/local`, run it with `sudo`. The installer also prompts to add a desktop
launcher.

Useful options:

- `./install.sh --user` to force a per-user install
- `./install.sh --system` to force a system-wide install
- `./install.sh --prefix /some/prefix` to choose a different install prefix
- `./install.sh --yes` to accept defaults without prompts

## Technical overview

- UI: FLTK
- HTML rendering: `litehtml`
- Module access and XHTML generation: CrossWire SWORD
- Search indexing: SQLite FTS5 over Bible modules
- Tag storage: separate SQLite database from the search index
- Runtime styling and help content: [`data/master.css`](data/master.css) and [`data/help.html`](data/help.html)

Project layout:

- `src/app/` application startup, preferences, and session restore
- `src/ui/` FLTK panes, widgets, dialogs, and editors
- `src/sword/` SWORD integration, markup normalization, and rendered HTML generation
- `src/search/` SQLite-backed indexing and search
- `src/tags/` verse tagging and persistence
- `cmake/` custom CMake find modules
- `libs/litehtml/` vendored rendering dependency

## License

Except where a file or directory says otherwise, Verdad-authored code in this
repository is dual-licensed under `GPL-2.0-only OR Unlicense`. See
[LICENSE](LICENSE), [LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt), and
[LICENSES/Unlicense.txt](LICENSES/Unlicense.txt).

Third-party license texts and notices are bundled under
[LICENSES/README.md](LICENSES/README.md).

For distributed builds, the important practical constraint is CrossWire SWORD:
Verdad links against SWORD, so distributions that ship that combined program
need to comply with GPL-2.0 terms.
