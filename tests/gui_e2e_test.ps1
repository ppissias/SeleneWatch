<#
.SYNOPSIS
    Real Windows GUI end-to-end automated test for SeleneWatch using Windows UI Automation.
.DESCRIPTION
    Launches the real built SeleneWatch.exe visibly on Windows with isolated QSettings
    (SELENEWATCH_SETTINGS_NAME=SeleneWatchGuiE2ETest) and a fresh temporary output folder
    so the observer's real settings and output folder are never touched.

    Exercises:
      1. Synthetic Moon -> Preview zoom (100%, 200%, Fit) -> Preview blip (0 clips written)
         -> Save named Detection profile -> Monitoring bright flash + dark transit recorded on disk.
      2. Synthetic Sky -> Switch target to Sky -> Monitoring moving meteor streak + lightning recorded on disk.
      3. Synthetic Moon -> Time-lapse capture -> Stop & MP4 build -> Re-sharpen -> Rebuild video only.
      4. Real lunar recording (test_data\Moon_221449.avi, if present) -> Preview + blip -> Monitoring clip on disk
         -> Fast playback Time-lapse -> MP4 build.

    Saves screenshots of each stage into out\gui-e2e\ for visual verification.
#>
[CmdletBinding()]
param(
    [string]$ExePath = "",
    [string]$ScreenshotDir = "",
    [switch]$SkipRealData,
    [switch]$KeepOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Msg {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_KEYUP = 0x0101;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const int VK_RETURN = 0x0D;
    public const uint PW_RENDERFULLCONTENT = 0x00000002;
    public static void ClickScreenPoint(IntPtr hWnd, int sx, int sy) {
        POINT pt = new POINT { X = sx, Y = sy };
        ScreenToClient(hWnd, ref pt);
        IntPtr lParam = (IntPtr)((pt.Y << 16) | (pt.X & 0xFFFF));
        PostMessage(hWnd, WM_LBUTTONDOWN, (IntPtr)1, lParam);
        PostMessage(hWnd, WM_LBUTTONUP, IntPtr.Zero, lParam);
    }
    public static void SendReturn(IntPtr hWnd) {
        PostMessage(hWnd, WM_KEYDOWN, (IntPtr)VK_RETURN, IntPtr.Zero);
        PostMessage(hWnd, WM_KEYUP, (IntPtr)VK_RETURN, IntPtr.Zero);
    }
}
"@

$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not $ExePath) { $ExePath = Join-Path $RepoRoot "out\build\release\SeleneWatch.exe" }
if (-not $ScreenshotDir) { $ScreenshotDir = Join-Path $RepoRoot "out\gui-e2e" }
if (-not (Test-Path $ExePath)) {
    throw "SeleneWatch executable not found at: $ExePath. Build Release first."
}

$SettingsAppName = "SeleneWatchGuiE2ETest"
$RegBase = "HKCU:\Software\SeleneWatch\$SettingsAppName"
$TempOut = Join-Path $env:TEMP ("SeleneWatchGuiE2E_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $TempOut | Out-Null
New-Item -ItemType Directory -Force -Path $ScreenshotDir | Out-Null

function Set-QSetting([string]$SubKey, [string]$Name, [string]$Value) {
    $path = if ($SubKey) { Join-Path $RegBase $SubKey } else { $RegBase }
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    New-ItemProperty -Path $path -Name $Name -Value $Value -PropertyType String -Force | Out-Null
}

function Find-UiaElement {
    param(
        [System.Windows.Automation.AutomationElement]$Root,
        [string]$IdSuffix = "",
        [string]$Name = "",
        [System.Windows.Automation.ControlType]$ControlType = $null,
        [int]$TimeoutMs = 10000
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $cond = if ($ControlType) {
                New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $ControlType)
            } else {
                [System.Windows.Automation.Condition]::TrueCondition
            }
            $all = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
            foreach ($el in $all) {
                $id = $el.Current.AutomationId
                $nm = $el.Current.Name
                $idMatch = (-not $IdSuffix) -or ($id -eq $IdSuffix) -or ($id -like "*.$IdSuffix")
                $nameMatch = (-not $Name) -or ($nm -eq $Name) -or ($nm -like "*$Name*")
                if ($idMatch -and $nameMatch) {
                    return $el
                }
            }
        } catch {
            # Element tree may refresh mid-walk while Qt updates widgets.
        }
        Start-Sleep -Milliseconds 150
    }
    throw "Timed out waiting for UIA element (IdSuffix='$IdSuffix', Name='$Name')"
}

function Find-TopWindow {
    param(
        [int]$ProcessId,
        [string]$TitleContains = "",
        [System.Windows.Automation.AutomationElement]$OwnerWindow = $null,
        [int]$TimeoutMs = 15000
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $pidCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
    $winCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            if ($OwnerWindow) {
                $owned = $OwnerWindow.FindAll([System.Windows.Automation.TreeScope]::Children, $winCond)
                foreach ($w in $owned) {
                    if ((-not $TitleContains) -or ($w.Current.Name -like "*$TitleContains*")) {
                        return $w
                    }
                }
            }
            $wins = $desktop.FindAll([System.Windows.Automation.TreeScope]::Children, $pidCond)
            foreach ($w in $wins) {
                if ((-not $TitleContains) -or ($w.Current.Name -like "*$TitleContains*")) {
                    return $w
                }
            }
        } catch {}
        Start-Sleep -Milliseconds 200
    }
    throw "Timed out waiting for window (PID=$ProcessId, TitleContains='$TitleContains')"
}

function Invoke-UiaClick([System.Windows.Automation.AutomationElement]$El, [string]$Label = "") {
    $pat = $null
    if ($El.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) {
        $pat.Invoke()
        return
    }
    if ($El.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pat)) {
        $pat.Select()
        return
    }
    if ($El.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pat)) {
        $pat.Toggle()
        return
    }
    throw "Element '$Label' ($($El.Current.AutomationId)) supports neither Invoke, SelectionItem, nor Toggle"
}

function Set-UiaCheckBox([System.Windows.Automation.AutomationElement]$El, [bool]$Checked) {
    $pat = $null
    if (-not $El.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pat)) {
        throw "CheckBox $($El.Current.AutomationId) does not support TogglePattern"
    }
    $want = if ($Checked) { [System.Windows.Automation.ToggleState]::On } else { [System.Windows.Automation.ToggleState]::Off }
    if ($pat.Current.ToggleState -ne $want) {
        $pat.Toggle()
    }
}

function Get-NearestHwnd([System.Windows.Automation.AutomationElement]$El) {
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $cur = $El
    while ($cur) {
        $h = $cur.Current.NativeWindowHandle
        if ($h -ne 0) { return [IntPtr]$h }
        $cur = $walker.GetParent($cur)
    }
    return [IntPtr]::Zero
}

function Select-UiaComboItem {
    param(
        [System.Windows.Automation.AutomationElement]$Combo,
        [string]$ItemText,
        [int]$ProcessId
    )
    $ec = $null
    if ($Combo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$ec)) {
        $ec.Expand()
        Start-Sleep -Milliseconds 300
    }
    # Qt combo popups may be children of the combo or top-level popup windows for the same PID.
    $item = $null
    try {
        $item = Find-UiaElement -Root $Combo -Name $ItemText -ControlType ([System.Windows.Automation.ControlType]::ListItem) -TimeoutMs 1500
    } catch {
        $desktop = [System.Windows.Automation.AutomationElement]::RootElement
        $pidCond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
        $wins = $desktop.FindAll([System.Windows.Automation.TreeScope]::Children, $pidCond)
        foreach ($w in $wins) {
            try {
                $item = Find-UiaElement -Root $w -Name $ItemText -ControlType ([System.Windows.Automation.ControlType]::ListItem) -TimeoutMs 600
                if ($item) { break }
            } catch {}
        }
    }
    if (-not $item) {
        throw "Could not find combo box item containing '$ItemText'"
    }
    $hwnd = Get-NearestHwnd $item
    $rect = $item.Current.BoundingRectangle
    if ($hwnd -ne [IntPtr]::Zero -and $rect.Width -gt 0 -and $rect.Height -gt 0) {
        $cx = [int][Math]::Round($rect.X + $rect.Width / 2.0)
        $cy = [int][Math]::Round($rect.Y + $rect.Height / 2.0)
        [Win32Msg]::ClickScreenPoint($hwnd, $cx, $cy)
        Start-Sleep -Milliseconds 250
    } else {
        Invoke-UiaClick $item $ItemText
        Start-Sleep -Milliseconds 150
        if ($hwnd -ne [IntPtr]::Zero) { [Win32Msg]::SendReturn($hwnd) }
        Start-Sleep -Milliseconds 250
    }
}

function Wait-Until {
    param(
        [scriptblock]$Predicate,
        [string]$Description,
        [int]$TimeoutMs = 20000
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            if (& $Predicate) { return }
        } catch {}
        Start-Sleep -Milliseconds 200
    }
    throw "Timed out waiting for: $Description"
}

function Save-WindowScreenshot([System.Windows.Automation.AutomationElement]$Window, [string]$OutFile) {
    try {
        $rect = $Window.Current.BoundingRectangle
        $w = [int][Math]::Round($rect.Width)
        $h = [int][Math]::Round($rect.Height)
        if ($w -le 0 -or $h -le 0) { return }
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hwnd = [IntPtr]$Window.Current.NativeWindowHandle
        $ok = $false
        if ($hwnd -ne [IntPtr]::Zero) {
            $hdc = $g.GetHdc()
            try {
                $ok = [Win32Msg]::PrintWindow($hwnd, $hdc, [Win32Msg]::PW_RENDERFULLCONTENT)
            } finally {
                $g.ReleaseHdc($hdc)
            }
        }
        if (-not $ok) {
            $g.CopyFromScreen([int][Math]::Round($rect.X), [int][Math]::Round($rect.Y), 0, 0, $bmp.Size)
        }
        $bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose()
        $bmp.Dispose()
        Write-Host "  [Screenshot] $OutFile"
    } catch {
        Write-Warning "Could not capture screenshot $OutFile : $_"
    }
}

function Assert-True([bool]$Cond, [string]$Message) {
    if (-not $Cond) { throw "ASSERTION FAILED: $Message" }
}

$proc = $null
try {
    # Prepare isolated QSettings so we never affect the user's real preferences or output folder.
    if (Test-Path $RegBase) { Remove-Item -Path $RegBase -Recurse -Force }
    Set-QSetting "output" "folder" ($TempOut -replace '\\', '/')
    Set-QSetting "output" "legacyNoticeShown" "true"
    Set-QSetting "tips" "showAtStartup" "false"
    Set-QSetting "testing" "tools" "true"
    Set-QSetting "capture" "history" "0.5"
    Set-QSetting "capture" "post" "0.4"
    Set-QSetting "sky\capture" "history" "0.5"
    Set-QSetting "sky\capture" "post" "0.4"
    Set-QSetting "timelapse" "preset" "0"
    Set-QSetting "timelapse" "interval" "0.5"
    Set-QSetting "timelapse" "window" "0.3"
    Set-QSetting "timelapse" "keepPercent" "100"
    Set-QSetting "timelapse" "maxFrames" "50"
    Set-QSetting "timelapse" "stopKind" "0"
    Set-QSetting "timelapse" "writeMp4" "true"
    Set-QSetting "timelapse" "writeGif" "false"
    Set-QSetting "timelapse" "videoFps" "5"

    function Click-ElementAtFraction([System.Windows.Automation.AutomationElement]$El, [double]$XFrac, [double]$YFrac) {
        $hwnd = Get-NearestHwnd $El
        $r = $El.Current.BoundingRectangle
        $sx = [int][Math]::Round($r.X + $r.Width * $XFrac)
        $sy = [int][Math]::Round($r.Y + $r.Height * $YFrac)
        [Win32Msg]::ClickScreenPoint($hwnd, $sx, $sy)
    }

    function Select-DockTab([System.Windows.Automation.AutomationElement]$Win, [string]$TabName) {
        $tab = Find-UiaElement -Root $Win -Name $TabName -ControlType ([System.Windows.Automation.ControlType]::TabItem)
        $tabHwnd = Get-NearestHwnd $tab
        $tabRect = $tab.Current.BoundingRectangle
        if ($tabHwnd -ne [IntPtr]::Zero -and $tabRect.Width -gt 0 -and $tabRect.Height -gt 0) {
            [Win32Msg]::ClickScreenPoint($tabHwnd, [int][Math]::Round($tabRect.X + $tabRect.Width / 2.0), [int][Math]::Round($tabRect.Y + $tabRect.Height / 2.0))
        } else {
            Invoke-UiaClick $tab "$TabName tab"
        }
        Start-Sleep -Milliseconds 300
    }

    Write-Host "=== Launching SeleneWatch GUI E2E test ==="
    Write-Host "  Exe:     $ExePath"
    Write-Host "  TempOut: $TempOut"

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.WorkingDirectory = Split-Path $ExePath -Parent
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["SELENEWATCH_SETTINGS_NAME"] = $SettingsAppName
    $proc = [System.Diagnostics.Process]::Start($psi)

    $mainWin = Find-TopWindow -ProcessId $proc.Id -TitleContains "SeleneWatch" -TimeoutMs 15000
    Write-Host "  Main window ready: $($mainWin.Current.Name)"
    Start-Sleep -Milliseconds 500

    $sourceCombo = Find-UiaElement -Root $mainWin -IdSuffix "sourceComboBox"
    $startBtn = Find-UiaElement -Root $mainWin -IdSuffix "startButton"
    $monitorBtn = Find-UiaElement -Root $mainWin -IdSuffix "monitoringButton"
    $stopBtn = Find-UiaElement -Root $mainWin -IdSuffix "stopButton"
    $injectBtn = Find-UiaElement -Root $mainWin -IdSuffix "injectBlipButton"
    $blipKindCombo = Find-UiaElement -Root $mainWin -IdSuffix "blipKindComboBox"
    $previewEl = Find-UiaElement -Root $mainWin -IdSuffix "previewWidget"

    # --- Scenario 1: Moon Preview, Zoom (100%/200%/Fit), Detection Profile, Bright + Darkening Clips ---
    Write-Host "`n[1/4] Synthetic Moon: Preview Zoom + Detection Profile + Bright & Darkening Clips..."
    Select-DockTab $mainWin "Detection"
    Select-UiaComboItem -Combo $sourceCombo -ItemText "Synthetic" -ProcessId $proc.Id
    Wait-Until { $startBtn.Current.IsEnabled } "Start button enabled"
    Invoke-UiaClick $startBtn "Start synthetic Moon preview"
    Wait-Until { $stopBtn.Current.IsEnabled } "Moon preview running"
    Start-Sleep -Milliseconds 700

    # Test Preview Zoom buttons: 100%, 200%, Fit.
    $zoom100 = Find-UiaElement -Root $mainWin -IdSuffix "previewZoom100"
    $zoom200 = Find-UiaElement -Root $mainWin -IdSuffix "previewZoom200"
    $zoomFit = Find-UiaElement -Root $mainWin -IdSuffix "previewZoomFit"
    $zoomLbl = Find-UiaElement -Root $mainWin -IdSuffix "previewZoomLabel"
    Invoke-UiaClick $zoom100 "Zoom 100%"
    Wait-Until { $zoomLbl.Current.Name -eq "100%" } "Zoom readout 100%"
    Invoke-UiaClick $zoom200 "Zoom 200%"
    Wait-Until { $zoomLbl.Current.Name -eq "200%" } "Zoom readout 200%"
    Save-WindowScreenshot $mainWin (Join-Path $ScreenshotDir "01_moon_zoom200.png")
    Invoke-UiaClick $zoomFit "Zoom Fit"
    Wait-Until { $zoomLbl.Current.Name -like "Fit*" } "Zoom readout Fit"

    # Click lunar disk center and inject bright blip in Preview (must NOT write any clips).
    Click-ElementAtFraction $previewEl 0.5 0.5
    Wait-Until { $injectBtn.Current.IsEnabled } "Inject blip enabled after clicking preview"
    Invoke-UiaClick $injectBtn "Inject Moon preview blip"
    Start-Sleep -Milliseconds 600
    $moonEventsDir = Join-Path $TempOut "events\moon"
    $moonBeforeMon = if (Test-Path $moonEventsDir) { @(Get-ChildItem -Path $moonEventsDir -Directory).Count } else { 0 }
    Assert-True ($moonBeforeMon -eq 0) "Preview must not write clips, found $moonBeforeMon"

    # Save a named Moon detection profile via the Save... dialog (async PostMessage click so modal exec doesn't block UIA Invoke).
    $saveProfBtn = Find-UiaElement -Root $mainWin -IdSuffix "detectionSaveProfile"
    Click-ElementAtFraction $saveProfBtn 0.5 0.5
    $profDlg = Find-TopWindow -ProcessId $proc.Id -TitleContains "Save detection profile" -OwnerWindow $mainWin -TimeoutMs 8000
    $profEdit = Find-UiaElement -Root $profDlg -IdSuffix "detectionProfileNameEdit" -ControlType ([System.Windows.Automation.ControlType]::Edit)
    $valPat = $null
    if ($profEdit.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$valPat)) {
        $valPat.SetValue("Moon E2E Profile")
    }
    $saveDlgBtn = Find-UiaElement -Root $profDlg -Name "Save" -ControlType ([System.Windows.Automation.ControlType]::Button)
    Invoke-UiaClick $saveDlgBtn "Confirm Save detection profile"
    Start-Sleep -Milliseconds 300
    $profCombo = Find-UiaElement -Root $mainWin -IdSuffix "detectionProfileCombo"
    Write-Host "  Saved profile active in combo: $($profCombo.Current.Name)"

    # Start Monitoring and inject a bright flash -> verify clip on disk.
    Invoke-UiaClick $monitorBtn "Start Moon monitoring"
    Wait-Until { $monitorBtn.Current.Name -like "*Stop monitoring*" } "Moon monitoring active"
    Start-Sleep -Milliseconds 700
    Wait-Until { $injectBtn.Current.IsEnabled } "Inject blip ready for Moon bright flash"
    Invoke-UiaClick $injectBtn "Inject Moon bright flash"
    Wait-Until {
        (Test-Path $moonEventsDir) -and (@(Get-ChildItem -Path $moonEventsDir -Directory).Count -ge 1)
    } "Moon bright flash clip written on disk" -TimeoutMs 15000

    # Enable darkening and inject a Dark transit across the sunlit Moon -> verify second clip on disk.
    $darkChk = Find-UiaElement -Root $mainWin -IdSuffix "darkeningTrigger"
    Set-UiaCheckBox $darkChk $true
    Click-ElementAtFraction $previewEl 0.42 0.5
    Select-UiaComboItem -Combo $blipKindCombo -ItemText "Dark transit" -ProcessId $proc.Id
    Wait-Until { $injectBtn.Current.IsEnabled } "Inject blip ready for Dark transit"
    $moonCountBeforeDark = @(Get-ChildItem -Path $moonEventsDir -Directory).Count
    Invoke-UiaClick $injectBtn "Inject Moon dark transit"
    Wait-Until {
        @(Get-ChildItem -Path $moonEventsDir -Directory).Count -gt $moonCountBeforeDark
    } "Moon dark transit clip written on disk" -TimeoutMs 15000
    Start-Sleep -Milliseconds 600

    Invoke-UiaClick $stopBtn "Stop Moon stream"
    Wait-Until { $startBtn.Current.IsEnabled } "Moon stream stopped"

    $moonDirs = @(Get-ChildItem -Path $moonEventsDir -Directory)
    Assert-True ($moonDirs.Count -ge 2) "Expected >= 2 Moon event clips, found $($moonDirs.Count)"
    $sawMoonDark = $false
    foreach ($d in $moonDirs) {
        Wait-Until { Test-Path (Join-Path $d.FullName "04_event_preview.gif") } "Moon preview GIF written" -TimeoutMs 10000
        $evTxt = Get-Content (Join-Path $d.FullName "event.txt") -Raw
        Assert-True ($evTxt -match "contains_test_signal: yes") "Moon clip missing contains_test_signal: yes"
        if ($evTxt -match "kind=darkening") { $sawMoonDark = $true }
    }
    Assert-True $sawMoonDark "Expected Moon darkening clip in events\moon"
    Write-Host "  Moon clips verified on disk: $($moonDirs.Count) (bright + darkening)"

    # --- Scenario 2: Synthetic Sky Watch (Meteor streak + Lightning on disk) ---
    Write-Host "`n[2/4] Synthetic Sky Watch: Meteor streak + Lightning recorded on disk..."
    $targetCombo = Find-UiaElement -Root $mainWin -IdSuffix "targetChoice"
    Select-UiaComboItem -Combo $targetCombo -ItemText "The sky" -ProcessId $proc.Id
    Start-Sleep -Milliseconds 300

    Invoke-UiaClick $startBtn "Start synthetic Sky preview"
    Wait-Until { $stopBtn.Current.IsEnabled } "Sky preview running"
    Start-Sleep -Milliseconds 700
    Save-WindowScreenshot $mainWin (Join-Path $ScreenshotDir "02_synthetic_sky.png")

    Invoke-UiaClick $monitorBtn "Start Sky monitoring"
    Wait-Until { $monitorBtn.Current.Name -like "*Stop monitoring*" } "Sky monitoring active"
    Start-Sleep -Milliseconds 800

    $skyEventsDir = Join-Path $TempOut "events\sky"
    Click-ElementAtFraction $previewEl 0.25 0.25
    Select-UiaComboItem -Combo $blipKindCombo -ItemText "Moving streak" -ProcessId $proc.Id
    Wait-Until { $injectBtn.Current.IsEnabled } "Inject blip ready for Sky meteor streak"
    Invoke-UiaClick $injectBtn "Inject Sky meteor streak"
    Wait-Until {
        (Test-Path $skyEventsDir) -and (@(Get-ChildItem -Path $skyEventsDir -Directory).Count -ge 1)
    } "Sky meteor clip written on disk" -TimeoutMs 15000

    Select-UiaComboItem -Combo $blipKindCombo -ItemText "Lightning" -ProcessId $proc.Id
    Wait-Until { $injectBtn.Current.IsEnabled } "Inject blip ready for Sky lightning"
    $skyCountBeforeLightning = @(Get-ChildItem -Path $skyEventsDir -Directory).Count
    Invoke-UiaClick $injectBtn "Inject Sky lightning"
    Wait-Until {
        @(Get-ChildItem -Path $skyEventsDir -Directory).Count -gt $skyCountBeforeLightning
    } "Sky lightning clip written on disk" -TimeoutMs 15000
    Start-Sleep -Milliseconds 600

    Invoke-UiaClick $stopBtn "Stop Sky stream"
    Wait-Until { $startBtn.Current.IsEnabled } "Sky stream stopped"

    $skyDirs = @(Get-ChildItem -Path $skyEventsDir -Directory)
    Assert-True ($skyDirs.Count -ge 2) "Expected >= 2 Sky event clips, found $($skyDirs.Count)"
    $sawSkyMeteor = $false
    $sawSkyLightning = $false
    foreach ($d in $skyDirs) {
        Wait-Until { Test-Path (Join-Path $d.FullName "04_event_preview.gif") } "Sky preview GIF written" -TimeoutMs 10000
        $evTxt = Get-Content (Join-Path $d.FullName "event.txt") -Raw
        Assert-True ($evTxt -match "mode: sky") "Sky clip missing mode: sky"
        if ($evTxt -match "moving_streak|suggested_kind: meteor") { $sawSkyMeteor = $true }
        if ($evTxt -match "kind=brightening|suggested_kind: lightning") { $sawSkyLightning = $true }
    }
    Assert-True $sawSkyMeteor "Expected Sky meteor clip in events\sky"
    Assert-True $sawSkyLightning "Expected Sky lightning clip in events\sky"
    Write-Host "  Sky clips verified on disk: $($skyDirs.Count) (meteor + lightning)"

    # Switch back to Moon for Time-lapse scenarios.
    Select-UiaComboItem -Combo $targetCombo -ItemText "The Moon" -ProcessId $proc.Id
    Select-UiaComboItem -Combo $blipKindCombo -ItemText "Bright flash" -ProcessId $proc.Id
    Start-Sleep -Milliseconds 300

    # --- Scenario 3: Synthetic Time-lapse + Re-sharpen + Non-destructive Rebuild ---
    Write-Host "`n[3/4] Synthetic Time-lapse + Re-sharpen + Rebuild..."
    Wait-Until { $startBtn.Current.IsEnabled } "Start button enabled"
    Invoke-UiaClick $startBtn "Start"
    Wait-Until { $stopBtn.Current.IsEnabled } "Stream started (Stop enabled)"

    Select-DockTab $mainWin "Time-lapse"

    $tlStartBtn = Find-UiaElement -Root $mainWin -IdSuffix "timelapseStartButton"
    Wait-Until { $tlStartBtn.Current.IsEnabled } "Start time-lapse button enabled"
    Invoke-UiaClick $tlStartBtn "Start time-lapse"
    Wait-Until { $tlStartBtn.Current.Name -like "*Stop*" } "Time-lapse started (button changed to Stop)" -TimeoutMs 8000

    $tlStats = Find-UiaElement -Root $mainWin -IdSuffix "timelapseStats"
    $tlTiming = Find-UiaElement -Root $mainWin -IdSuffix "timelapseTiming"
    try {
        Wait-Until { $tlStats.Current.Name -match "Stacks:\s*[2-9]" } "At least 2 synthetic stacks completed" -TimeoutMs 25000
    } catch {
        Write-Host "  [Debug] tlStartBtn='$($tlStartBtn.Current.Name)' tlTiming='$($tlTiming.Current.Name)' tlStats='$($tlStats.Current.Name)'"
        throw
    }
    Write-Host "  Live telemetry: $($tlStats.Current.Name)"
    Save-WindowScreenshot $mainWin (Join-Path $ScreenshotDir "03_timelapse_running.png")

    Invoke-UiaClick $tlStartBtn "Stop time-lapse"
    Wait-Until {
        ($tlTiming.Current.Name -like "Finished (*") -and ($tlStartBtn.Current.Name -like "*Start*") -and $tlStartBtn.Current.IsEnabled
    } "Time-lapse finished and returned to Start state" -TimeoutMs 25000
    Write-Host "  Completed: $($tlTiming.Current.Name)"

    # Verify session outputs on disk.
    $tlRoot = Join-Path $TempOut "timelapses"
    $sessions = @(Get-ChildItem -Path $tlRoot -Directory | Sort-Object Name)
    Assert-True ($sessions.Count -eq 1) "Expected 1 synthetic session directory, found $($sessions.Count)"
    $sessionDir = $sessions[0].FullName
    Assert-True (Test-Path (Join-Path $sessionDir "timelapse.mp4")) "Missing timelapse.mp4"
    Assert-True (Test-Path (Join-Path $sessionDir "timelapse.txt")) "Missing timelapse.txt"
    Assert-True (Test-Path (Join-Path $sessionDir "sharpening_profile.json")) "Missing sharpening_profile.json"
    $profileJson = Get-Content (Join-Path $sessionDir "sharpening_profile.json") -Raw
    Assert-True ($profileJson -match '"commonOverlap"') "sharpening_profile.json missing persisted commonOverlap"
    $stackFits = @(Get-ChildItem (Join-Path $sessionDir "stacks") -Filter "*.fits")
    Assert-True ($stackFits.Count -ge 2) "Expected >=2 unsharpened stacks, found $($stackFits.Count)"
    $origMp4Time = (Get-Item (Join-Path $sessionDir "timelapse.mp4")).LastWriteTimeUtc

    # Open Re-sharpen dialog, apply sharpening, and test non-destructive Rebuild video only.
    $resharpBtn = Find-UiaElement -Root $mainWin -IdSuffix "timelapseResharpenButton"
    Wait-Until { $resharpBtn.Current.IsEnabled } "Re-sharpen button enabled"
    Invoke-UiaClick $resharpBtn "Re-sharpen"

    $dlg = Find-TopWindow -ProcessId $proc.Id -TitleContains "Re-sharpen" -OwnerWindow $mainWin -TimeoutMs 10000
    Start-Sleep -Milliseconds 400
    Save-WindowScreenshot $dlg (Join-Path $ScreenshotDir "04_resharpen_dialog.png")

    $applyBtn = Find-UiaElement -Root $dlg -IdSuffix "resharpenApply"
    $statusLbl = Find-UiaElement -Root $dlg -IdSuffix "resharpenStatus"
    Invoke-UiaClick $applyBtn "Apply to all"
    Wait-Until { $statusLbl.Current.Name -like "Done:*" } "Re-sharpen finished" -TimeoutMs 25000
    Write-Host "  Re-sharpen: $($statusLbl.Current.Name)"
    $reDirs = @(Get-ChildItem -Path $sessionDir -Directory -Filter "sharpened_*")
    Assert-True ($reDirs.Count -eq 1) "Expected 1 sharpened_* output folder"
    Assert-True (Test-Path (Join-Path $reDirs[0].FullName "timelapse.mp4")) "Missing re-sharpened timelapse.mp4"

    # Toggle Rebuild video only and verify non-destructive video_* output without overwriting original MP4.
    $rebuildChk = Find-UiaElement -Root $dlg -IdSuffix "resharpenRebuildOnly"
    Set-UiaCheckBox $rebuildChk $true
    Start-Sleep -Milliseconds 300
    Invoke-UiaClick $applyBtn "Apply Rebuild video only"
    Wait-Until {
        ($statusLbl.Current.Name -like "Done:*") -and ($statusLbl.Current.Name -match "video_")
    } "Rebuild video only finished into video_* folder" -TimeoutMs 25000
    Write-Host "  Rebuild video only: $($statusLbl.Current.Name)"
    $vidDirs = @(Get-ChildItem -Path $sessionDir -Directory -Filter "video_*")
    Assert-True ($vidDirs.Count -eq 1) "Expected 1 non-destructive video_* folder"
    Assert-True (Test-Path (Join-Path $vidDirs[0].FullName "timelapse.mp4")) "Missing rebuilt timelapse.mp4"
    Assert-True (((Get-Item (Join-Path $sessionDir "timelapse.mp4")).LastWriteTimeUtc) -eq $origMp4Time) "Original timelapse.mp4 was unexpectedly overwritten!"

    $closeDlgBtn = Find-UiaElement -Root $dlg -Name "Close" -ControlType ([System.Windows.Automation.ControlType]::Button)
    Invoke-UiaClick $closeDlgBtn "Close Re-sharpen"
    Start-Sleep -Milliseconds 300

    Invoke-UiaClick $stopBtn "Stop synthetic stream"
    Wait-Until { $startBtn.Current.IsEnabled } "Synthetic stream stopped"

    # --- Scenario 4: Real Lunar Recording (Blip Detection + Time-lapse when local test_data/ is present) ---
    $realMoon = Join-Path $RepoRoot "test_data\Moon_221449.avi"
    if ((-not $SkipRealData) -and (Test-Path $realMoon)) {
        Write-Host "`n[4/4] Real Lunar Recording: Blip Injection + Time-lapse ($realMoon)..."
        Select-UiaComboItem -Combo $sourceCombo -ItemText "Recording" -ProcessId $proc.Id

        $openFileBtn = Find-UiaElement -Root $mainWin -IdSuffix "openFileButton"
        Wait-Until { $openFileBtn.Current.IsEnabled } "Open file button enabled"
        Invoke-UiaClick $openFileBtn "Open recording"

        $fileDlg = Find-TopWindow -ProcessId $proc.Id -TitleContains "Open recording" -OwnerWindow $mainWin -TimeoutMs 10000
        Start-Sleep -Milliseconds 300
        # Standard Windows Open dialog File name edit box has AutomationId 1148.
        $fileEdit = Find-UiaElement -Root $fileDlg -IdSuffix "1148" -ControlType ([System.Windows.Automation.ControlType]::Edit) -TimeoutMs 5000
        $valPat = $null
        if ($fileEdit.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$valPat)) {
            $valPat.SetValue($realMoon)
        } else {
            throw "File dialog edit box does not support ValuePattern"
        }
        Start-Sleep -Milliseconds 250
        # Press Enter on the native File Open dialog edit box (and fallback to clicking Open button).
        $editHwnd = Get-NearestHwnd $fileEdit
        if ($editHwnd -ne [IntPtr]::Zero) {
            [Win32Msg]::SendReturn($editHwnd)
        } else {
            $openConfirm = Find-UiaElement -Root $fileDlg -IdSuffix "1" -ControlType ([System.Windows.Automation.ControlType]::Button) -TimeoutMs 5000
            $ocHwnd = Get-NearestHwnd $openConfirm
            $ocRect = $openConfirm.Current.BoundingRectangle
            [Win32Msg]::ClickScreenPoint($ocHwnd, [int][Math]::Round($ocRect.X + $ocRect.Width / 2.0), [int][Math]::Round($ocRect.Y + $ocRect.Height / 2.0))
        }

        # Wait until index read finishes (fileLabel stops saying "reading its index").
        $fileLabel = Find-UiaElement -Root $mainWin -IdSuffix "fileLabel"
        Wait-Until {
            ($fileLabel.Current.Name -like "*Moon_221449.avi*") -and ($fileLabel.Current.Name -notlike "*reading its index*")
        } "Recording index loaded" -TimeoutMs 15000
        Write-Host "  Loaded recording: $($fileLabel.Current.Name)"

        # First run Monitoring on the real lunar AVI with an injected bright flash and verify it records on disk.
        $speedCombo = Find-UiaElement -Root $mainWin -IdSuffix "speedComboBox"
        Select-UiaComboItem -Combo $speedCombo -ItemText "Real time" -ProcessId $proc.Id
        Invoke-UiaClick $startBtn "Start real lunar AVI preview"
        Wait-Until { $stopBtn.Current.IsEnabled } "Real lunar AVI running"
        Start-Sleep -Milliseconds 800
        Invoke-UiaClick $monitorBtn "Start real lunar AVI monitoring"
        Wait-Until { $monitorBtn.Current.Name -like "*Stop monitoring*" } "Real lunar AVI monitoring active"
        Start-Sleep -Milliseconds 700
        Click-ElementAtFraction $previewEl 0.5 0.5
        Wait-Until { $injectBtn.Current.IsEnabled } "Inject blip ready on real lunar AVI"
        $moonCountBeforeReal = @(Get-ChildItem -Path $moonEventsDir -Directory).Count
        Invoke-UiaClick $injectBtn "Inject blip on real lunar AVI"
        Wait-Until {
            @(Get-ChildItem -Path $moonEventsDir -Directory).Count -gt $moonCountBeforeReal
        } "Real lunar AVI blip clip written on disk" -TimeoutMs 15000
        Write-Host "  Real lunar AVI blip clip recorded on disk."
        Invoke-UiaClick $stopBtn "Stop real lunar AVI monitoring"
        Wait-Until { $startBtn.Current.IsEnabled } "Real lunar AVI stopped"

        Select-UiaComboItem -Combo $speedCombo -ItemText "Fast" -ProcessId $proc.Id

        # Start Time-lapse directly from Stopped (verifies auto-start from frame 0 on real Moon AVI!).
        Wait-Until { $tlStartBtn.Current.IsEnabled } "Start time-lapse enabled for recording" -TimeoutMs 5000
        Invoke-UiaClick $tlStartBtn "Start real lunar time-lapse"
        Wait-Until {
            @(Get-ChildItem -Path $tlRoot -Directory).Count -ge 2
        } "Second (real lunar) session directory created" -TimeoutMs 15000
        Wait-Until {
            ($tlTiming.Current.Name -like "Finished (*") -or ($tlStats.Current.Name -match "Stacks:\s*[2-9]")
        } "Real lunar time-lapse produced stacks" -TimeoutMs 45000
        Write-Host "  Real lunar telemetry: $($tlStats.Current.Name)"
        Save-WindowScreenshot $mainWin (Join-Path $ScreenshotDir "05_real_moon_timelapse.png")

        if ($tlStartBtn.Current.IsEnabled -and ($tlStartBtn.Current.Name -like "*Stop*")) {
            Invoke-UiaClick $tlStartBtn "Stop real lunar time-lapse"
        }
        Wait-Until {
            ($tlTiming.Current.Name -like "Finished (*") -and ($tlStartBtn.Current.Name -like "*Start*") -and $tlStartBtn.Current.IsEnabled
        } "Real lunar time-lapse finalized" -TimeoutMs 30000
        Write-Host "  Real lunar completed: $($tlTiming.Current.Name)"

        $sessions2 = @(Get-ChildItem -Path $tlRoot -Directory | Sort-Object Name)
        Assert-True ($sessions2.Count -eq 2) "Expected 2 session directories after real lunar run, found $($sessions2.Count)"
        $realSession = $sessions2[1].FullName
        Assert-True (Test-Path (Join-Path $realSession "timelapse.mp4")) "Missing real lunar timelapse.mp4"
        $realFits = @(Get-ChildItem (Join-Path $realSession "stacks") -Filter "*.fits")
        Assert-True ($realFits.Count -ge 1) "Expected >=1 real lunar FITS stack"

        if ($stopBtn.Current.IsEnabled) {
            Invoke-UiaClick $stopBtn "Stop real lunar stream"
        }
    } else {
        Write-Host "`n[2/2] Skipped real-recording scenario (test_data\Moon_221449.avi not present or -SkipRealData)."
    }

    # Close MainWindow cleanly.
    $winPat = $null
    if ($mainWin.TryGetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern, [ref]$winPat)) {
        $winPat.Close()
    }
    if (-not $proc.WaitForExit(8000)) {
        throw "SeleneWatch.exe did not exit cleanly within 8 seconds after Close"
    }
    Assert-True ($proc.ExitCode -eq 0) "SeleneWatch.exe exited with non-zero code $($proc.ExitCode)"

    Write-Host "`n=== GUI E2E TEST PASSED ===" -ForegroundColor Green
    Write-Host "Screenshots saved to: $ScreenshotDir"
}
finally {
    if ($proc -and -not $proc.HasExited) {
        try { $proc.Kill() } catch {}
    }
    if (Test-Path $RegBase) {
        Remove-Item -Path $RegBase -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ((-not $KeepOutput) -and (Test-Path $TempOut)) {
        Remove-Item -Path $TempOut -Recurse -Force -ErrorAction SilentlyContinue
    }
}
