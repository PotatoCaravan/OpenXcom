# Syncing with upstream OXCE

This fork tracks **OpenXcom Extended (OXCE)** and periodically merges upstream changes. All
fork-added engine code is deliberately small, localized, and tagged `// [AI-MODS]` so merges stay
trivial. This document lists every touch-point.

## Branches

- **`oxce-plus`** — mirrors upstream OXCE. Keep it clean; merge upstream here.
- **`claude`** — the base branch for AI work (the verification suite + docs live here). Test-idea
  branches are forked off `claude`.

## Adding an upstream remote (one-time)

There is no upstream remote configured by default. Modern OXCE lives at Meridian's repo:

```bash
git remote add upstream https://github.com/MeridianOXC/OpenXcom.git
git fetch upstream
```

(The historical origin is `https://github.com/Yankes/OpenXcom` branch `OpenXcomExtended`; see
`Extended.txt`.)

## Merging upstream

```bash
git checkout oxce-plus
git fetch upstream
git merge upstream/master        # or the branch you track
# resolve conflicts (see touch-points below), then:
./scripts/build-check.ps1
./scripts/run-selftest.ps1
./scripts/validate-mods.ps1 -Master xcom1
git checkout claude
git merge oxce-plus              # bring upstream into the AI base branch
# re-run the verification ladder
```

## Fork touch-points (grep `[AI-MODS]`)

Everything the fork adds to the engine. To see the live list any time:

```bash
git grep -n "\[AI-MODS\]" -- src/
```

### New files (never conflict)

- `src/Engine/Verify.h`, `src/Engine/Verify.cpp` — the whole headless verification module
  (`--selftest` / `--validate` + the self-test registry).

### Edited upstream files (small, marked)

| File | Change |
|------|--------|
| `src/main.cpp` | `#include "Engine/Verify.h"`; two branches near the top of `main()` — `--selftest` before `Options::init`, `--validate` after it, each returning before a window is created. |
| `src/Engine/Options.cpp` | `loadArgs()` treats `-selftest`/`-validate` as valueless flags; two `-selftest`/`-validate` lines in `showHelp()`. |
| `src/Engine/CrossPlatform.h` | Declarations for `getLogErrorCount()` and `ensureConsoleOutput()`. |
| `src/Engine/CrossPlatform.cpp` | Extra includes (`<cstdio> <cstdint> <iostream>`, and `<io.h> <fcntl.h>` in the Win32 block); `logErrorCount` + `getLogErrorCount()`; `ensureConsoleOutput()`; a one-line increment in `log()`. |

### Build source lists (add the new file in all three)

- `src/CMakeLists.txt` — `Engine/Verify.cpp` in `engine_src`.
- `src/OpenXcom.2010.vcxproj` — `<ClCompile Include="Engine\Verify.cpp" />` + `<ClInclude Include="Engine\Verify.h" />`.
- `src/OpenXcom.2010.vcxproj.filters` — the same two entries under `<Filter>Engine</Filter>`.

### Non-engine additions (won't conflict with upstream engine merges)

- `CLAUDE.md`, `docs/BUILDING.md`, `docs/TESTING.md`, `docs/UPSTREAM_SYNC.md`
- `scripts/` (verification suite + `git-hooks/pre-commit`)
- `.claude/` (harness config), `.github/workflows/ci.yml`

## Conflict guidance

If upstream reworks the top of `main()`, `loadArgs`/`showHelp`, or the `CrossPlatform::log` area,
re-apply the small `[AI-MODS]` blocks by hand — they are self-contained. The `Verify` module itself
never conflicts. After any merge, the suite passing (build + selftest + validate) is the sign the
re-apply was correct.
