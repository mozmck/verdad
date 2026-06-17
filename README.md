# Verdad Bible Study

Verdad is a desktop Bible study application built around CrossWire SWORD
modules. It combines Bible reading, parallel texts, search, commentary,
dictionaries, daily readings, imported documents, tags, and study notes in one
workspace.

Verdad is in active development. The project has been developed with extensive
AI assistance.

## Current features

### Study workspace

- Multiple study tabs with independent Bible, reference, paragraph or parallel
  mode, right-pane selections, splitter sizes, and scroll positions.
- A three-pane layout: modules, search, tags, and previews on the left; Bible
  text in the center; commentary, Daily, general books, Studypad, and dictionary
  content on the right.
- Chapter and reference navigation, paragraph mode, red-letter display, and up
  to seven parallel Bible columns.
- Controls for Strong's numbers, morphology, footnotes, and cross references.
- Word-hover previews and context actions for search, Strong's lookup,
  dictionaries, copying verses or selections, and verse tagging.
- Restored window geometry, study tabs, pane state, active Studypad, and scroll
  positions between sessions.

### Search and navigation

- Background SQLite FTS5 indexing of Bibles, commentaries, dictionaries,
  general books, and imported documents, with Bible-only, library-only, or
  combined search scopes.
- Multi-word, exact phrase, ECMAScript regex, and Strong's/lemma matching.
- Exact, spelling-assisted, synonym-assisted, and smart fuzzy search modes,
  with language-aware synonym expansion where supported.
- Canonically ordered results with contextual snippets and a shared preview
  pane.
- Single-click preview, double-click navigation in the current tab, and
  middle-click navigation in a new tab.
- Scripture links from commentary, general books, Studypad, daily content, and
  imported files can open previews or populate search results.

### Study resources and personal data

- Commentary follows the current verse, including navigation from commentary
  verse numbers and support for editing writable commentary modules.
- A persistent dictionary pane for Strong's lexicons and language-specific word
  dictionaries.
- Paged general-book navigation with a table-of-contents browser.
- A Daily workspace for SWORD devotionals, SWORD reading-plan modules, and
  editable reading plans with calendar navigation, completion tracking, and
  rescheduling.
- Verse tags with filtering, renaming, deletion, previews, and Bible markers.
- Studypad HTML notes with rich-text editing, detected scripture links, verse
  insertion, and optional ODT export through LibreOffice.
- PDF, plain-text, and Markdown import. Files or whole folders can be added to
  the library, optionally copied into Verdad's storage, indexed for search, and
  browsed like general-book modules.
- Settings export and import for preferences, tags, reading plans, and
  Studypads.
- A configurable user-data directory suitable for Syncthing or another file
  synchronization tool. Verdad detects external changes to tags, reading plans,
  and clean Studypad files and protects unsaved notes with conflict copies.

### Modules and offline language lookup

- A built-in SWORD module manager for local and remote repositories, module
  installation, and updates.
- Configurable Greek and Hebrew Strong's dictionaries and language-specific
  word dictionaries.
- Optional local-only English hover glosses from user-supplied WikDict SQLite
  databases. This is word lookup, not sentence translation.
- Optional Apertium-derived Spanish and German morphology resolves inflected
  forms to WikDict lemmas. Apertium is needed only to generate morphology
  databases, not while Verdad is running.

## SWORD modules

Verdad can start without SWORD modules, but Bible, commentary, dictionary,
general-book, devotional, and packaged reading-plan content requires installed
modules. Use `Tools > Module Manager...` or normal SWORD tools to install them.
Remote module sources use the network; local sources remain available for
offline installations.

## Offline dictionaries and morphology

By default, Verdad scans a `dictionaries` directory inside its platform config
directory. The recommended layout is:

```text
dictionaries/
  wikdict/
    es-en.sqlite3
    de-en.sqlite3
  morphology/
    es-morph-apertium.sqlite3
    es-morph-apertium.NOTICE
    de-morph-apertium.sqlite3
    de-morph-apertium.NOTICE
```

Flat WikDict files directly under `dictionaries/` are also supported. Dictionary
and morphology packs are user-managed data. They are not included in the
repository, packaged Verdad releases, or settings exports. Direct WikDict
lookups take precedence; morphology is used only when the surface form has no
direct gloss, and a derived lemma is shown only when the matching `*-en.sqlite3`
database supplies an English gloss.

To generate the complete Spanish morphology database without maintaining a
wordlist, install your distribution's `apertium-spa` package and run:

```bash
python3 tools/import_apertium_spanish_morphology.py
```

The importer locates `apertium-spa.spa.dix`, expands all finite single-token
paradigms, and writes the SQLite database and license notice to Verdad's default
morphology directory. The current full source produces about 1.8 million
analyses in a database of roughly 55 MB. See
[`doc/offline-spanish-morphology.md`](doc/offline-spanish-morphology.md) for
custom source paths, output locations, metadata, and alternate import modes.

To generate German morphology from the ignored local source checkout:

```bash
git clone --depth 1 \
  https://github.com/apertium/apertium-deu.git \
  tools/data/apertium-deu
python3 tools/import_apertium_german_morphology.py \
  --dictionary tools/data/apertium-deu/apertium-deu.deu.dix
```

The inspected German source produces about 338,000 normalized forms in a 15 MB
database. The generator records the upstream commit, source URL, dictionary
SHA-256, GPL-3.0 license, generator version, and generation time, and writes an
adjacent notice. See
[`doc/offline-german-morphology.md`](doc/offline-german-morphology.md).

## Local data

The default config directory is:

- Linux: `~/.config/verdad/`
- macOS: `~/Library/Application Support/Verdad/`
- Windows: `%APPDATA%\Verdad\`

Important files and directories include:

- `preferences.conf`: preferences, window layout, and restored session state
- `module_index.db`: generated search index
- `imports.db` and `imports/`: imported-document catalog and copied source files
- `dictionaries/`: user-managed WikDict and morphology databases
- `tags.db`: verse tags, stored in the selected user-data directory
- `reading_plans.db`: editable plans and completion state, stored in the
  selected user-data directory
- `studypad/`: study notes, stored in the selected user-data directory

SQLite databases can also create temporary `-wal`, `-shm`, or `-journal`
sidecar files while the application is running.

## Build, test, and run

Verdad is a C++17 CMake project. FLTK and litehtml are included as submodules.

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
./build/verdad
```

The automated tests currently cover the morphology provider, WikDict fallback
behavior, and Apertium morphology importer. UI and SWORD integration changes
still require a manual application smoke test.

For a debug build:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```

### Debian and Ubuntu build dependencies

```bash
sudo apt install build-essential cmake pkg-config git python3 \
    libsword-dev libsqlite3-dev \
    libx11-dev libxext-dev libxinerama-dev libxfixes-dev \
    libxcursor-dev libxrender-dev libxft-dev \
    libfontconfig-dev libfreetype-dev
```

FLTK 1.4.4 is built statically from `libs/fltk`, and litehtml is built from
`libs/litehtml`. `data/master.css` and `data/help.html` are copied into the
build tree automatically.

Linux packages and prebuilt bundles include the SWORD shared library and its
common runtime data. They still use the host system for SWORD's lower-level
dependencies and Verdad's other runtime libraries. On Debian and Ubuntu, a
typical runtime set is:

```bash
sudo apt install libsqlite3-0 \
    libx11-6 libxext6 libxinerama1 libxfixes3 \
    libxcursor1 libxrender1 libxft2 libfontconfig1
```

The generated Debian package declares the exact additional dependencies needed
by the bundled SWORD build on the packaging host.

## Packaging

Available CMake packaging targets depend on the host platform:

- Linux: `bundle`, `deb`, and `appimage`
- macOS: `app` and `dmg`
- Windows: `winzip` and `msi`

For example:

```bash
cmake --build build --target appimage
cmake --build build --target bundle
cmake --build build --target deb
```

The Linux AppImage, archive, and Debian package bundle the SWORD runtime and
common data but not Bible or other SWORD modules. Verdad still discovers
modules in the standard system and user SWORD locations. The private library is
installed under `lib/verdad`; Debian common data is kept under
`share/verdad/sword` to avoid colliding with `libsword-common`. Other
applications continue using the system SWORD library. GitHub Actions builds and
packages Linux, macOS, and Windows artifacts.

## Install on Linux

To install directly from a source build:

```bash
cmake --install build --prefix "$HOME/.local"
```

To use the standalone installer, build and unpack the prebuilt bundle, then run
`install.sh` from inside the unpacked directory:

```bash
cmake --build build --target bundle
tar -xzf build/verdad-*.tar.gz -C /tmp
/tmp/verdad-*/install.sh
```

The standalone installer uses `~/.local` by default. Run it with `sudo` for a
system-wide installation under `/usr/local`. Useful options:

- `./install.sh --user`
- `./install.sh --system`
- `./install.sh --prefix /some/prefix`
- `./install.sh --yes`

## Technical overview

- UI: FLTK
- HTML rendering: litehtml
- Module access and XHTML generation: CrossWire SWORD
- Search: SQLite FTS5
- Tags, reading plans, imports, and morphology: separate SQLite databases
- Runtime styling and help: [`data/master.css`](data/master.css) and
  [`data/help.html`](data/help.html)

Project layout:

- `src/app/`: startup, platform paths, preferences, and session restore
- `src/ui/`: FLTK panes, widgets, dialogs, editors, and daily workspace
- `src/sword/`: SWORD integration, markup normalization, and HTML generation
- `src/search/`: indexing, snippets, and search
- `src/tags/`: verse tagging and persistence
- `src/reading/`: editable reading plans and completion state
- `src/import/`: PDF, text, and Markdown library import
- `src/translation/`: WikDict lookup and optional morphology providers
- `tools/`: development and database-generation utilities
- `tests/`: C++ and Python automated tests
- `cmake/`: custom find modules and packaging helpers
- `libs/fltk/` and `libs/litehtml/`: vendored dependencies

## License

Except where a file or directory says otherwise, Verdad-authored code in this
repository is dual-licensed under `GPL-2.0-only OR Unlicense`. See
[LICENSE](LICENSE), [LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt), and
[LICENSES/Unlicense.txt](LICENSES/Unlicense.txt).

Third-party license texts and notices are listed in
[LICENSES/README.md](LICENSES/README.md). Distributed builds linking CrossWire
SWORD must comply with its GPL-2.0 terms. WikDict and generated Apertium data
retain their own source licenses and notices.
