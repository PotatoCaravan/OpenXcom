# Building OpenXcom Extended (AI Mods fork)

Dependencies are **vendored** in `deps/` (prebuilt SDL 1.2 + friends for Windows) and `libs/`
(`rapidyaml`, `miniz` compiled from source), so on Windows there is nothing to install.

The fastest path is the compile-gate script:

```powershell
./scripts/build-check.ps1                       # Release | x64 (default)
./scripts/build-check.ps1 -Configuration Debug  # Debug | x64
./scripts/build-check.ps1 -Platform Win32       # Release | Win32
./scripts/build-check.ps1 -Clean                # clean first
./scripts/build-check.ps1 -CMake                # use the CMake path instead of MSBuild
```

It prints `BUILD OK -> <exe path>` and returns MSBuild's exit code. Output lands at
`bin/<Platform>/<Configuration>/OpenXcom.exe`; the SDL runtime DLLs are copied to `bin/<Platform>/`
(one level above the exe — the run scripts add that directory to the DLL search path for you).

## Toolchain on this machine

- **Visual Studio Build Tools 2026** — MSBuild at
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe`
  (not on `PATH`; `build-check.ps1` locates it via `vswhere`).
- **CMake 4.2.3**, **Git**. No Ninja, and no XP toolset.

> **Do not use the `Release_XP` configuration.** It targets the VS2017 `v141_xp` platform toolset,
> which isn't installed. Use `Release` or `Debug`. The other configurations use
> `$(DefaultPlatformToolset)`, which resolves to the installed toolset automatically.

## Manual MSBuild

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
& $msbuild src\OpenXcom.2010.sln /m /p:Configuration=Release /p:Platform=x64
```

## CMake (cross-platform)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

On Linux the equivalent needs the SDL 1.2 dev packages:
`libsdl1.2-dev libsdl-mixer1.2-dev libsdl-image1.2-dev libsdl-gfx1.2-dev` (plus `zlib`). The CMake
executable is named `openxcom` and lands in `build/bin`.

## Adding a source file

The three build systems each keep a **manual** source list. When you add a `.cpp`/`.h` under `src/`,
update all three or one build breaks:

- `src/CMakeLists.txt` — the relevant `set(..._src ...)` list.
- `src/OpenXcom.2010.vcxproj` — a `<ClCompile>` / `<ClInclude>` entry.
- `src/OpenXcom.2010.vcxproj.filters` — the same entry under its `<Filter>`.

(See how `Engine/Verify.cpp` / `Engine/Verify.h` are registered for a worked example.)

## Notes

- Warnings are not fatal (`FATAL_WARNING` is OFF), so warnings won't fail the build — but keep the
  code warning-clean.
- Don't edit the generated `git_version.h`.
- The Release exe is `/SUBSYSTEM:WINDOWS` (GUI); the Debug exe is console. This matters for how
  headless output is captured — see `docs/TESTING.md`.
