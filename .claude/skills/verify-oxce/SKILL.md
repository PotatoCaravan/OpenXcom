---
name: verify-oxce
description: Verify a change to this OpenXcom Extended (AI Mods) repo by running the verification ladder - compile gate, headless self-tests, and (for ruleset/script/mod changes) headless mod validation. Use after any C++ or .rul / Y-Script change, before claiming it works.
---

# Verify an OXCE change

Run the verification ladder from the repo root (PowerShell). Stop and report at the first failure.

1. **Compile gate** — always:
   ```powershell
   ./scripts/build-check.ps1
   ```
   Must print `BUILD OK` and exit 0.

2. **Self-tests** — always:
   ```powershell
   ./scripts/run-selftest.ps1
   ```
   Must print `SELF-TESTS PASSED` and exit 0.

3. **Mod validation** — only if the change touches rulesets (`.rul`), Y-Script, mods, or anything
   affecting mod loading:
   ```powershell
   ./scripts/validate-mods.ps1 -Master xcom1
   ```
   (Also run `-Master xcom2` if the change could affect TFTD.) Must print `VALIDATE: OK` and exit 0.
   If it reports "no game assets", run `./scripts/setup-gamedata.ps1` once first.

## Interpreting results

- Any non-zero exit = the change is **not** done. Read the printed output / the tailed
  `openxcom.log` and fix the root cause.
- Scope the run to what changed: docs-only or script-only changes don't need the C++ build; a
  pure ruleset change still needs rung 3.
- Details, exit-code meanings, and how to add a self-test: `docs/TESTING.md`.

Report exactly which rungs ran and their results. Do not claim success for a rung you did not run.
