# Checks a SeleneWatch .zip package; run by CPack after it makes the packages
# (packaging/check_package.cmake), so the command line and Visual Studio get
# the same checks. Works in Windows PowerShell 5.1 and PowerShell 7.
#   - the expected program, Qt, runtime and license files are there, and the
#     Qt modules SeleneWatch does not use are not;
#   - every DLL a shipped binary imports is in the package or part of Windows;
#   - the packaged SeleneWatch.exe starts with only its own folder and Windows
#     on the path. Qt's offscreen platform plugin is added to a scratch copy
#     only, so no window opens; the program starts on its synthetic source, so
#     no camera is opened.
param(
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$Dumpbin,
    [Parameter(Mandatory)][string]$QtPrefix
)
$ErrorActionPreference = 'Stop'
$check = Join-Path (Split-Path $Zip -Parent) 'check'
if (Test-Path $check) { Remove-Item -Recurse -Force $check }
# .NET directly: Expand-Archive breaks in Windows PowerShell 5.1 started from
# PowerShell 7, which hands it the newer shell's module path.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $check)
try {
    $app = (Get-ChildItem $check -Directory | Select-Object -First 1).FullName

    $expected = 'SeleneWatch.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'platforms\qwindows.dll',
        'msvcp140.dll', 'vcruntime140.dll', 'LICENSE.txt', 'README.md', 'MANUAL.md', 'THIRD_PARTY_LICENSES.md',
        'licenses\Qt-LICENSE.txt', 'licenses\Qt-third-party-notices.txt'
    foreach ($file in $expected) {
        if (-not (Test-Path (Join-Path $app $file))) { throw "The package lacks $file" }
    }
    foreach ($file in 'Qt6Network.dll', 'Qt6Svg.dll') {
        if (Test-Path (Join-Path $app $file)) { throw "The package holds $file, which SeleneWatch does not use" }
    }
    if ((Test-Path (Join-Path $app 'ASICamera2.dll')) -ne (Test-Path (Join-Path $app 'licenses\ZWO-ASI-SDK.txt'))) {
        throw "ASICamera2.dll and its notice must ship together"
    }

    $system = Join-Path $env:SystemRoot 'System32'
    $missing = foreach ($binary in Get-ChildItem $app -Recurse -Include *.exe, *.dll) {
        foreach ($line in (& $Dumpbin /nologo /dependents $binary.FullName)) {
            if ($line -match '^\s+(\S+\.dll)\s*$') {
                $dll = $Matches[1]
                if ($dll -match '^(api|ext)-ms-') { continue }
                if (-not (Test-Path (Join-Path $app $dll)) -and -not (Test-Path (Join-Path $system $dll))) {
                    "$($binary.Name) needs $dll"
                }
            }
        }
    }
    if ($missing) { throw "Missing DLLs:`n$($missing -join "`n")" }

    Copy-Item (Join-Path $QtPrefix 'plugins\platforms\qoffscreen.dll') (Join-Path $app 'platforms')
    $saved = @{ PATH = $env:PATH; QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM; QT_PLUGIN_PATH = $env:QT_PLUGIN_PATH }
    try {
        $env:PATH = "$system;$env:SystemRoot"
        $env:QT_QPA_PLATFORM = 'offscreen'
        $env:QT_PLUGIN_PATH = $null
        $started = Start-Process (Join-Path $app 'SeleneWatch.exe') -PassThru -WorkingDirectory $app
    } finally {
        $env:PATH = $saved.PATH
        $env:QT_QPA_PLATFORM = $saved.QT_QPA_PLATFORM
        $env:QT_PLUGIN_PATH = $saved.QT_PLUGIN_PATH
    }
    Start-Sleep -Seconds 5
    if ($started.HasExited) { throw "The packaged SeleneWatch.exe exited at start (exit code $($started.ExitCode))" }
    Stop-Process -Id $started.Id
    $started.WaitForExit()
    "Package checked: files, DLL dependencies, and a start with its own files: $(Split-Path $Zip -Leaf)"
} finally {
    Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue
}
