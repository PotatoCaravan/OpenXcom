<#
.SYNOPSIS
    Makes the real X-COM game data available to the repo for --validate and for running the game,
    WITHOUT ever copying copyrighted files into a committable location.

.DESCRIPTION
    The engine needs the original UFO Defense / TFTD assets (GEODATA, MAPS, TERRAIN, ...) plus the
    repo's own version-matched rulesets (bin\standard, bin\common). This script creates directory
    JUNCTIONS from bin\UFO\<sub> and bin\TFTD\<sub> to the corresponding asset folders inside an
    installed copy of the game. Junctions are zero-copy, they leave the tracked bin\UFO\README.txt
    untouched, and everything under bin\UFO / bin\TFTD is gitignored - so nothing copyrighted can
    ever be committed or pushed.

    After running this once, use scripts\validate-mods.ps1 (which validates against bin\).

.PARAMETER SourceRoot
    Folder containing the installed UFO\ and TFTD\ data. Defaults to two directories above the
    repo (the installed engine that ships alongside this source tree).
.PARAMETER Force
    Recreate junctions even if they already exist.
.PARAMETER Remove
    Remove the junctions instead of creating them (leaves README.txt and the repo clean).

.EXAMPLE
    ./scripts/setup-gamedata.ps1
.EXAMPLE
    ./scripts/setup-gamedata.ps1 -SourceRoot "D:\Games\OpenXcom"
#>
[CmdletBinding()]
param(
    [string]$SourceRoot,
    [switch]$Force,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$repo = Get-RepoRoot
$bin = Join-Path $repo 'bin'

if (-not $SourceRoot) {
    $SourceRoot = (Resolve-Path (Join-Path $repo '..\..')).Path
}

function Test-IsJunction([string]$Path) {
    if (-not (Test-Path $Path)) { return $false }
    $item = Get-Item $Path -Force
    return [bool]($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}

function Link-Game([string]$Game) {
    $srcGame = Join-Path $SourceRoot $Game        # e.g. <install>\UFO
    $dstGame = Join-Path $bin $Game               # e.g. <repo>\bin\UFO
    if (-not (Test-Path $srcGame)) {
        Write-Host "SKIP $Game : source not found ($srcGame)" -ForegroundColor Yellow
        return
    }
    if (-not (Test-Path $dstGame)) {
        New-Item -ItemType Directory -Force -Path $dstGame | Out-Null
    }

    # Link each asset SUBDIRECTORY (GEODATA, MAPS, ...). Top-level files like README.txt are left
    # alone, so the repo's tracked bin\<Game>\README.txt is never disturbed.
    $subs = Get-ChildItem -Path $srcGame -Directory -ErrorAction SilentlyContinue
    if (-not $subs) {
        Write-Host "SKIP $Game : no asset subfolders in $srcGame" -ForegroundColor Yellow
        return
    }
    foreach ($sub in $subs) {
        $link = Join-Path $dstGame $sub.Name
        if ($Remove) {
            if (Test-IsJunction $link) { cmd /c rmdir "$link" | Out-Null; Write-Host "UNLINK bin\$Game\$($sub.Name)" -ForegroundColor DarkGray }
            continue
        }
        if (Test-Path $link) {
            if ((Test-IsJunction $link) -and -not $Force) {
                Write-Host "OK    bin\$Game\$($sub.Name) (already linked)" -ForegroundColor DarkGray
                continue
            }
            if (Test-IsJunction $link) { cmd /c rmdir "$link" | Out-Null }
            else {
                Write-Host "SKIP  bin\$Game\$($sub.Name) : real folder present, not overwriting" -ForegroundColor Yellow
                continue
            }
        }
        New-Item -ItemType Junction -Path $link -Target $sub.FullName | Out-Null
        Write-Host "LINK  bin\$Game\$($sub.Name) -> $($sub.FullName)" -ForegroundColor Green
    }
}

if ($Remove) {
    Write-Host "Removing game-data junctions under bin\UFO and bin\TFTD..." -ForegroundColor Cyan
}
else {
    Write-Host "Source install: $SourceRoot" -ForegroundColor Cyan
    Write-Host "Linking asset subfolders into bin\UFO and bin\TFTD (gitignored; README.txt preserved)." -ForegroundColor DarkGray
}
Link-Game 'UFO'
Link-Game 'TFTD'

# Install the pre-commit guard (blocks committing game data even with `git add -f`).
if (-not $Remove) {
    $current = (git -C $repo config --local --get core.hooksPath 2>$null)
    if ($current -ne 'scripts/git-hooks') {
        git -C $repo config --local core.hooksPath 'scripts/git-hooks' | Out-Null
        Write-Host "HOOK  installed pre-commit guard (core.hooksPath = scripts/git-hooks)" -ForegroundColor Green
    }
    else {
        Write-Host "HOOK  pre-commit guard already installed" -ForegroundColor DarkGray
    }
}

if (-not $Remove) {
    Write-Host ""
    Write-Host "Done. Validate with:  ./scripts/validate-mods.ps1 -Master xcom1" -ForegroundColor Green
    Write-Host "(Game data stays external via junctions and is gitignored - it can never be committed.)" -ForegroundColor DarkGray
}
