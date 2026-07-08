# Testing & verification

This fork adds a runnable, headless verification suite so an AI (or CI) can **prove** a change
instead of eyeballing it. Everything is exit-code driven and opens no window.

## The verification ladder

| Rung | Command | What it proves | Game data |
|------|---------|----------------|-----------|
| 1 | `./scripts/build-check.ps1` | it compiles (MSBuild Release\|x64) | no |
| 2 | `./scripts/run-selftest.ps1` | the binary starts; core C++ routines behave | no |
| 3 | `./scripts/validate-mods.ps1 -Master xcom1` | rulesets + Y-Script load under strict validation | yes |
| 4 | run `bin/x64/Release/OpenXcom.exe` | full end-to-end behaviour (manual) | yes |

Run **1 and 2 for every change** (they need nothing but source + toolchain). Run **3** for any
ruleset / Y-Script / mod change. Each rung returns non-zero on failure.

One-time setup for rungs 3–4 — links your external game data into `bin/` (gitignored) and installs
the pre-commit guard:

```powershell
./scripts/setup-gamedata.ps1
```

## Rung 1 — compile gate

`build-check.ps1` locates MSBuild via `vswhere`, builds `src/OpenXcom.2010.sln`, and returns
MSBuild's exit code. See `docs/BUILDING.md`.

## Rung 2 — self-tests (`--selftest`)

Fast, pure-C++ checks with no SDL, no options, no assets. `run-selftest.ps1` runs
`OpenXcom.exe --selftest`, prints a `PASS`/`FAIL` line per test and a `SELFTEST: X passed, Y failed`
summary, and exits 0 only if everything passed.

```
PASS collections_removeIf_int
PASS collections_removeIf_moveType
PASS fmath_vectors
SELFTEST: 3 passed, 0 failed
```

### Adding a self-test

Add an `OXC_SELFTEST` block in `src/Engine/Verify.cpp` (keep them in that file so the linker can't
drop the registrar). The body gets a `SelfTestContext& ctx`; use `ctx.check(cond, "message")` — it
records a failure and keeps going, and it works in Release builds (unlike `assert`).

```cpp
OXC_SELFTEST(my_feature_basics)
{
    ctx.check(computeThing(2) == 4, "computeThing(2) should be 4");
    ctx.check(computeThing(0) == 0, "computeThing(0) should be 0");
}
```

Then rebuild and run `./scripts/run-selftest.ps1`. Self-tests are the right home for pure logic
(math, containers, parsing helpers) — anything you can check without loading a game.

## Rung 3 — mod validation (`--validate`)

`validate-mods.ps1 -Master <xcom1|xcom2>` loads the master and its active mods **headlessly** with
strict validation and reports whether they load cleanly, using a throwaway temp folder for
`-user`/`-cfg` so your real saves and `options.cfg` are never touched.

```
VALIDATE: OK master=xcom1
VALIDATION PASSED (master=xcom1)
```

**Exit codes:** `0` clean · `1` validation failure (dangling reference, bad sprite/sound offset,
Y-Script parse error, ...) · `2` no game data / environment problem. On failure the script prints
the tail of `openxcom.log` from the temp folder; pass `-KeepTemp` to keep it for inspection.

What it does under the hood (`src/Engine/Verify.cpp::runModValidation`):
- forces `Options::oxceModValidationLevel = LOG_WARNING` **in memory only** (the game's shipping
  strictness — strict enough to catch real problems, not so strict it fails the base game on benign
  notices; the user's `options.cfg` is never rewritten);
- mutes audio, because headless has no audio device (otherwise every sound file logs a spurious
  "Audio device hasn't been opened" error);
- runs the same `Mod::loadAll()` the real game uses, then **fails if any error was logged** even
  when nothing threw — this is what catches Y-Script parse errors (they are logged, not thrown).

### Where the game data comes from

Copyrighted X-COM assets never live in the repo. `setup-gamedata.ps1` creates directory **junctions**
from `bin/UFO/<sub>` and `bin/TFTD/<sub>` to the asset folders inside an installed copy of the game
(auto-detected two directories above the repo, or pass `-SourceRoot`). This pairs the repo's own
**version-matched** `bin/standard` + `bin/common` rulesets with the real assets. Everything under
`bin/UFO` / `bin/TFTD` is gitignored, and the pre-commit hook blocks it — so nothing copyrighted can
be committed. Undo with `./scripts/setup-gamedata.ps1 -Remove`.

> Validate against the repo's `bin/` (the script default), **not** the installed copy's `standard/`,
> which may be a different version and produce misleading errors.

## Rung 4 — in-engine checks (manual, optional)

A debug build exposes the **Test Screen** (Ctrl+T, or the Extended geoscape links menu) with asset
and map validators that `--validate` doesn't cover: bad RMP route nodes, MCD map-data checks, palette
matching, script-tag dumps, and unused/missing map resources (`src/Menu/TestState.cpp`). Use it when
working on maps, terrains, or sprite palettes.

## Continuous integration

`.github/workflows/ci.yml` runs rungs 1–2 (build + `--selftest`) on Windows and Linux for every push
/ PR. Rung 3 is **not** in cloud CI because it needs copyrighted game data the runners can't have —
run it locally.
