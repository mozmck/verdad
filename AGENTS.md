# Repository Guidelines

## Project Structure & Module Organization
`src/` contains the application code. Major areas are `src/app/` for app startup and preferences, `src/ui/` for FLTK panes and widgets, `src/sword/` for SWORD integration and HTML generation, `src/search/` for SQLite-backed search, and `src/tags/` for verse tagging. Runtime styling lives in `data/master.css`. Reference notes and screenshots live in `doc/`. `cmake/` holds custom find modules, and `libs/litehtml/` is a vendored dependency that should only be changed when the issue is clearly inside litehtml.

## Local Upstream Sources
When work touches SWORD, BibleTime, or Xiphos behavior or APIs, check these local source trees before using the web. Only fall back to web lookup if the local tree is missing or clearly does not answer the question.

- Xiphos: `/home/moses/Projects/bible/xiphos`
- BibleTime: `/home/moses/Projects/bible/bibletime`
- SWORD: `/home/moses/Projects/bible/sword`

## Build, Test, and Development Commands
Configure a local build with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Build with:

```bash
cmake --build build -j$(nproc)
```

Run the app with:

```bash
./build/verdad
```

`data/master.css` is copied into `build/data/` automatically on each build. There is no dedicated test runner today; validation is primarily successful compilation plus manual UI checks.

## Coding Style & Naming Conventions
Use C++17 and follow the existing style: 4-space indentation, opening braces on the same line, and standard library includes grouped after project headers. Types and widgets use `PascalCase`, methods use `camelCase`, and member fields use a trailing underscore, for example `commentaryHtml_`. Keep small file-local helpers in anonymous namespaces. Match existing CSS naming such as `div.commentary-verse` and keep presentation changes in `data/master.css` when possible.

## HTML Rendering Notes
Commentary, dictionary, and general-book panes can receive legacy SWORD-rendered markup that does not map cleanly to normal HTML layout, especially self-closing `<p />`, repeated `<br />`, and empty spacer elements. Prefer normalizing that markup in `src/sword/SwordManager.cpp` and styling it in `data/master.css` before changing `libs/litehtml/`. In this codebase, subtle spacing tweaks such as small `em` margins or empty spacer blocks may have little or no visible effect in litehtml; if spacing is not responding, move quickly to structural separators and explicit pixel-based spacing. Only patch litehtml when there is a small, standards-valid repro showing an actual engine bug.

## Testing Guidelines
No `ctest` or unit-test suite is currently configured, and there is no coverage gate. For every change, at minimum:

1. Rebuild successfully with `cmake --build build`.
2. Smoke test the affected pane or workflow in `./build/verdad`.
3. For UI changes, verify Bible, commentary, and dictionary rendering if shared CSS or HTML generation was touched.

## Commit & Pull Request Guidelines
Recent history uses short imperative subjects in sentence case, usually ending with a period, for example `Improve text spacing and layout.` Keep commits focused and avoid mixing UI, search, and dependency updates without reason. Pull requests should include a concise summary, manual test notes, linked issues when applicable, and screenshots for visible UI changes.
