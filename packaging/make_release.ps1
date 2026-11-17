# Makes a SeleneWatch release from the command line: refuses uncommitted
# changes, builds Release, runs every test, then builds the
# "Release-packages" preset, the same one Visual Studio offers, which makes
# the .zip and the installer in out\build\release\package and checks the
# .zip (packaging/check_package.ps1). Run it from the developer PowerShell
# (C:\dev\selenewatch-env.ps1 on the maintainer's machine) at the repository
# root:
#
#   packaging\make_release.ps1
#
# -AllowDirty makes a trial package from uncommitted changes; its revision
# ends in -dirty.
param([switch]$AllowDirty)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
function Step([string]$text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Run([string]$what, [scriptblock]$command) {
    & $command
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

if (-not $AllowDirty -and (git status --porcelain)) {
    throw "The working tree has changes. Commit them, or pass -AllowDirty for a trial package."
}

Step "Configure and build (Release)"
Run "configure" { cmake --preset Release-x64 }
Run "build" { cmake --build --preset Release-x64 }

Step "Tests"
Run "tests" { ctest --test-dir out/build/release --output-on-failure }

Step "Packages"
$packages = 'out\build\release\package'
Get-ChildItem $packages -Filter 'SeleneWatch-*' -File -ErrorAction SilentlyContinue | Remove-Item
Run "packaging" { cmake --build --preset Release-packages }
Get-ChildItem $packages -Filter 'SeleneWatch-*' -File | ForEach-Object { $_.FullName }
