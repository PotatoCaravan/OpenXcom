# CLAUDE.md — operating guide for AI contributors

This repository is a fork of **OpenXcom Extended (OXCE) 8.6.1** — a C++17 / SDL 1.2 reimplementation
of *UFO: Enemy Unknown*. It is a testing ground for AI-driven engine work **and** AI-generated mod
content (rulesets, Y-Script, sprites). You are editing a real game engine that people run: changes
must compile and must not break mod loading.

**Read this before you touch anything, then keep it open while you work.**

---

## Golden rules

1. **Verify before you claim done.** Every change runs the verification ladder below.
   - C++ / engine change → at minimum `build-check.ps1` **and** `run-selftest.ps1` must pass.
   - `.rul` ruleset / Y-Script / mod change → `validate-mods.ps1` must pass.
   - Never report success on a change you have not built and run.

2. **Branch discipline.** The clean base branch for AI work is **`claude`**. Do work on a branch
   off `claude` (or on `claude` itself if the user says so) — **never** commit onto `oxce-plus`
   (that branch tracks upstream OXCE). One feature/fix per branch. This mirrors the repo's PR policy
   (`.github/pull_request_template.md`).

3. **Commit only when asked.** Don't commit or push unless the user requests it. When you do, use
   short imperative subjects (e.g. `Add headless mod validation`). End commit messages with the
   `Co-Authored-By` trailer.

4. **Never commit game data.** The copyrighted X-COM assets under `bin/UFO/`, `bin/TFTD/`, and any
   `bin/user/` saves must **never** be staged, committed, or pushed — the public repo is read-only.
   They are gitignored, linked in from outside the repo, and a pre-commit hook blocks them. Do not
   defeat these (`git add -f` on game data is forbidden). Only `bin/UFO/README.txt` and
   `bin/TFTD/README.txt` are tracked.

5. **Stay merge-friendly with upstream.** The user periodically merges upstream OXCE. Keep engine
   edits tiny and localized; prefer **new files**; mark every fork-added engine line with a
   `// [AI-MODS]` comment; never reformat upstream code. See `docs/UPSTREAM_SYNC.md`.

---

## The verification ladder

Run from the repo root in PowerShell. Each rung is fast and returns a non-zero exit code on failure.

| Rung | Command | Proves | Needs game data? |
|------|---------|--------|------------------|
| 1. Compiles | `./scripts/build-check.ps1` | the change builds (MSBuild, Release\|x64) | no |
| 2. Self-tests | `./scripts/run-selftest.ps1` | the binary starts; core routines behave | no |
| 3. Mods load | `./scripts/validate-mods.ps1 -Master xcom1` | rulesets + Y-Script load with strict validation | yes |
| 4. It runs (optional) | launch `bin/x64/Release/OpenXcom.exe` | end-to-end behaviour | yes |

One-time setup for rung 3/4 (creates gitignored junctions to your external game data + installs the
pre-commit guard): `./scripts/setup-gamedata.ps1`

Rungs 1–2 need nothing but the source and toolchain, so **always** run them. Run rung 3 for any
content/script change. See `docs/TESTING.md` for details, exit codes, and how to add a self-test.

The two headless modes are also usable directly:
`OpenXcom.exe --selftest` (exit 0 = all self-tests passed) and
`OpenXcom.exe -data <folder> -user <tmp> -cfg <tmp> -master xcom1 -validate` (exit 0 = clean,
1 = validation failure, 2 = no game data). Prefer the scripts — they handle the DLL path and output
capture for you.

---

## Building

- **Toolchain here:** VS Build Tools 2026 (MSBuild, located via `vswhere`), CMake 4.2.3, Git.
  Dependencies are vendored under `deps/` — no package install needed.
- **Do not use the `Release_XP` configuration** — it needs the VS2017 XP toolset, which isn't
  installed. Use `Release` (default) or `Debug`.
- Output: `bin/<Platform>/<Configuration>/OpenXcom.exe` (e.g. `bin/x64/Release/OpenXcom.exe`). The
  SDL runtime DLLs are copied to `bin/<Platform>/` (one level up from the exe).
- `build-check.ps1` flags: `-Configuration Debug`, `-Platform Win32`, `-Clean`, `-CMake`.
- Full details and the manual MSBuild/CMake commands: `docs/BUILDING.md`.

---

## Coding style (house style — match it exactly)

Enforced by `.clang-format`, `.editorconfig`, `.astylerc`; canonical reference:
<https://www.ufopaedia.org/index.php/Coding_Style_(OpenXcom)>.

- **Tabs** for indentation (C/C++). **Allman braces** (opening brace on its own line, even for
  `if`/`for`). **No column limit.**
- Types `PascalCase`; methods `camelCase()`; member variables `_camelCase` (leading underscore);
  enum values `UPPER_SNAKE`. Everything inside `namespace OpenXcom`.
- Doxygen `/** ... */` blocks on classes and methods. New headers start with `#pragma once`, then
  the GPLv3 header (copy it verbatim from an existing file).
- YAML / `.rul` files use **2-space** indentation (not tabs).
- Prefer forward declarations over includes in headers. Don't introduce non-standard string types
  (recent upstream work deliberately removed them).

---

## Repository map

| Path | What it is |
|------|-----------|
| `src/` | All C++ engine source. Subdirs: `Engine/` (core: Game, Options, Script, CrossPlatform, **Verify** — the AI-Mods test module), `Battlescape/`, `Geoscape/`, `Basescape/`, `Mod/` (ruleset classes), `Savegame/`, `Menu/`, `Interface/`, `Ufopaedia/`. Plus `OpenXcom.2010.sln` / `.vcxproj` and `main.cpp`. |
| `bin/standard/` | Bundled mods, including the masters `xcom1` (UFO) and `xcom2` (TFTD). **Version-matched to this engine** — validate against these. |
| `bin/common/` | Shared engine resources (Language, Palettes, Shaders, ...). |
| `bin/UFO/`, `bin/TFTD/` | Game-data folders. Only `README.txt` is tracked; real assets are linked in by `setup-gamedata.ps1` and gitignored. |
| `deps/`, `libs/` | Vendored dependencies (prebuilt SDL under `deps/`; `rapidyaml` + `miniz` sources under `libs/`). |
| `scripts/` | The verification suite (PowerShell) + the `git-hooks/` pre-commit guard. |
| `docs/` | `BUILDING.md`, `TESTING.md`, `UPSTREAM_SYNC.md`, `REST_AI.md` (the `rest-ai-server` branch's REST-controlled alien AI), plus Doxygen config. |
| `.github/workflows/` | CI (`ci.yml`). The old `nightly.test` is dormant. |

---

## Modding & validation notes

- A mod is a folder under `bin/standard/<id>/` (or a user mod) with a `metadata.yml` and `.rul`
  YAML rulesets (`items.rul`, `research.rul`, `armors.rul`, ...). Masters set `isMaster: true`.
- Ruleset validation runs **at load time**, centralized in `src/Mod/Mod.cpp`
  (`Mod::loadAll` → `Mod::checkForSoftError` → per-rule `afterLoad`), gated by
  `Options::oxceModValidationLevel`. `--validate` drives exactly this path headlessly at strict
  level and also fails if any error is logged (which catches Y-Script parse errors, since those are
  logged rather than thrown).
- Y-Script lives in `code:` blocks inside rulesets; the engine is `src/Engine/Script.cpp`. Scripts
  are validated when parsed during load — so `--validate` covers them.
- OXCE-specific ruleset features are documented in `Extended.txt`. The in-engine **Test Screen**
  (Ctrl+T in a debug build, or the Extended geoscape links menu) has extra asset/map checks
  (bad RMP nodes, MCD, palettes, unused map resources) — see `docs/TESTING.md`.

---

## Common pitfalls

- **Adding a `.cpp`/`.h` under `src/`?** Update **all three** build source lists or a build system
  breaks: `src/CMakeLists.txt`, `src/OpenXcom.2010.vcxproj`, and `src/OpenXcom.2010.vcxproj.filters`.
- Don't edit generated `git_version.h`.
- The Release exe is a GUI-subsystem app — its `stdout` isn't captured by a normal launcher; the
  headless modes write their report to the file named by `OXCE_VERIFY_OUT` (the scripts set this).
- Game data is **external**; a fresh `--validate` needs `setup-gamedata.ps1` to have run.
- Don't validate against the *installed* copy's `standard/` — it can be a different version. Validate
  against this repo's `bin/` (the scripts default to it).
