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
        'msvcp140.dll', 'vcruntime140.dll', 'opencv_world4140.dll', 'cfitsio.dll', 'ffmpeg.exe',
        'avcodec-63.dll', 'avdevice-63.dll', 'avfilter-12.dll', 'avformat-63.dll', 'avutil-61.dll',
        'swresample-7.dll', 'swscale-10.dll', 'LICENSE.txt', 'README.md', 'MANUAL.md', 'THIRD_PARTY_LICENSES.md',
        'ACKNOWLEDGEMENTS.md', 'icons\selenewatch-256.png', 'docs\images\readme-main-window.png',
        'docs\images\manual-camera-settings.png', 'docs\images\manual-display-histogram.png',
        'docs\images\manual-test-flash.png', 'docs\images\manual-sky-mask.png',
        'docs\images\manual-clip-overview.png', 'docs\images\manual-guiding.png',
        'docs\images\manual-output-folder.png', 'docs\images\manual-timelapse.png', 'docs\images\manual-resharpen.png',
        'docs\images\manual-site-instrument.png', 'docs\images\manual-darkening-transit.png',
        'docs\images\manual-transit-overview.png', 'docs\images\manual-sky-streak.jpg', 'docs\images\manual-sky-flash.jpg',
        'licenses\Qt-LICENSE.txt', 'licenses\Qt-third-party-notices.txt',
        'licenses\OpenCV-LICENSE.txt', 'licenses\CFITSIO-LICENSE.txt', 'licenses\FFmpeg-LICENSE.txt'
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
    # Settings of its own, so the observer's (a camera to connect at startup,
    # say) do not apply; removed afterwards.
    $settingsName = 'SeleneWatch package check'
    $saved = @{ PATH = $env:PATH; QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM; QT_PLUGIN_PATH = $env:QT_PLUGIN_PATH
        SELENEWATCH_SETTINGS_NAME = $env:SELENEWATCH_SETTINGS_NAME }
    try {
        $env:PATH = "$system;$env:SystemRoot"
        $env:QT_QPA_PLATFORM = 'offscreen'
        $env:QT_PLUGIN_PATH = $null
        $env:SELENEWATCH_SETTINGS_NAME = $settingsName
        $started = Start-Process (Join-Path $app 'SeleneWatch.exe') -PassThru -WorkingDirectory $app
    } finally {
        $env:PATH = $saved.PATH
        $env:QT_QPA_PLATFORM = $saved.QT_QPA_PLATFORM
        $env:QT_PLUGIN_PATH = $saved.QT_PLUGIN_PATH
        $env:SELENEWATCH_SETTINGS_NAME = $saved.SELENEWATCH_SETTINGS_NAME
    }
    Start-Sleep -Seconds 5
    if ($started.HasExited) { throw "The packaged SeleneWatch.exe exited at start (exit code $($started.ExitCode))" }
    Stop-Process -Id $started.Id
    $started.WaitForExit()
    Remove-Item "HKCU:\Software\SeleneWatch\$settingsName" -Recurse -ErrorAction SilentlyContinue
    "Package checked: files, DLL dependencies, and a start with its own files: $(Split-Path $Zip -Leaf)"
} finally {
    Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue
}
