# SeleneWatch Developer Guide

This guide covers building SeleneWatch from source, running automated tests and manual hardware probes, understanding the code layout, and packaging a release.

- For what the application does, see [README.md](README.md).
- For operating instructions at the telescope, see [MANUAL.md](MANUAL.md).
- For architectural rules, the time model, thread ownership, and scientific evidence constraints, read [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) and [AGENTS.md](AGENTS.md) before changing the codebase.
- For concrete paths, tool versions, connected hardware, and local archive data on the maintainer's workstation, see [local environment.txt](local%20environment.txt).

## Contents

1. [Technology stack](#1-technology-stack)
2. [Prerequisites](#2-prerequisites)
3. [Configuring and building](#3-configuring-and-building)
4. [Camera SDK and mount build options](#4-camera-sdk-and-mount-build-options)
5. [Running tests and manual probes](#5-running-tests-and-manual-probes)
6. [Code organization and extension boundaries](#6-code-organization-and-extension-boundaries)
7. [Building a release](#7-building-a-release)

---

## 1. Technology stack

| Area | Choice |
| --- | --- |
| Language | C++20 (`/W4 /permissive- /utf-8` on MSVC) |
| User interface | Qt 6 Widgets (`Core`, `Gui`, `Widgets`; dynamically linked under LGPLv3) |
| Platform | Windows x64 |
| IDE / compiler | Visual Studio (2026 / MSVC v14.51 toolset) |
| Build system | CMake + Ninja (`CMakePresets.json` + `CMakeUserPresets.json`) |
| Testing | Qt Test + CTest (`Qt6::Test`, offscreen platform for window/UI tests) |
| Evidence & recorded input | Built-in SER reader/writer, uncompressed AVI reader, Qt PNG previews |
| Cameras | Optional per-family vendor SDK adapters (`cameras/`) behind `CameraFamilyPanel` |
| Mount | Late-bound Windows COM against installed ASCOM Platform (`SELENEWATCH_WITH_ASCOM`); simulated mount for tests |
| Image processing | Plain C++ loops for detection and feature lock (OpenCV deferred unless registration/tracking requires it) |

The processing core (`selenewatch_core`) links only `Qt6::Core` and `Qt6::Gui`, with no dependency on Qt Widgets, so it builds and tests without a window.

---

## 2. Prerequisites

1. **Visual Studio** with the **Desktop development with C++** workload and CMake support.
2. **Qt 6** built for your **MSVC 64-bit** toolchain, including **Qt Widgets** (do not use a MinGW Qt kit).
3. **Ninja** (supplied with Visual Studio or Qt Tools).
4. **Optional camera SDKs** (kept outside the repository; see [Section 4](#4-camera-sdk-and-mount-build-options)). Without any camera SDK configured, SeleneWatch still builds and runs with the Synthetic source and SER/AVI recordings.
5. **Optional WiX Toolset 3** binaries (`bin\candle.exe`, `bin\light.exe`), required only to generate the `.msi` installer during release packaging.

---

## 3. Configuring and building

### Using CMake presets (Visual Studio or CLI)

[CMakePresets.json](CMakePresets.json) expects `QTDIR` in the environment, and [CMakeUserPresets.json](CMakeUserPresets.json) is tracked in Git as a reference setup providing:

- **Configure presets:** `Debug-x64` (`out/build/debug`) and `Release-x64` (`out/build/release`).
- **Build presets:** `Debug-x64`, `Release-x64`, and `Release-packages`.
- **Hidden helper presets:** `Qt-Default` (sets `QTDIR`), `Camera-SDKs` (sets all four `SELENEWATCH_*_SDK_DIR` paths), and `Release-Tools` (sets `SELENEWATCH_WIX_ROOT`).

On another machine, update those paths in `CMakeUserPresets.json` (or remove `Camera-SDKs` / `Release-Tools` from the presets' `inherits` list to build without external SDKs or WiX). Machine-specific paths and the maintainer's environment script are documented in [local environment.txt](local%20environment.txt).

**In Visual Studio:**

1. Open the repository folder.
2. Select **Debug (x64)** (or **Release (x64)**) as the configure preset.
3. Configure, build, and run **SeleneWatch**.

**From a developer PowerShell** (with MSVC, Qt, CMake, and Ninja on `PATH`):

```powershell
cmake --preset Debug-x64
cmake --build --preset Debug-x64
.\out\build\debug\SeleneWatch.exe
```

When launching the non-packaged `SeleneWatch.exe` outside the IDE, ensure `%QTDIR%\bin` is on `PATH` and `QT_PLUGIN_PATH` points to `%QTDIR%\plugins`.

### Standalone CLI configuration (without presets)

You can configure any subset of options in a separate build directory. For example, a ZWO-only Release build on Windows x64:

```powershell
cmake -S . -B out/build/zwo-release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="$env:QTDIR" -DSELENEWATCH_ZWO_SDK_DIR="C:/dev/tools/SDKs/ASI SDK"
cmake --build out/build/zwo-release
ctest --test-dir out/build/zwo-release --output-on-failure
.\out\build\zwo-release\SeleneWatch.exe
```

---

## 4. Camera SDK and mount build options

Camera SDKs stay outside the repository. Each adapter is enabled when its CMake cache variable is non-empty:

| CMake variable | Expected layout | Build & runtime behavior |
| --- | --- | --- |
| `SELENEWATCH_ZWO_SDK_DIR` | `include/ASICamera2.h`, `lib/x64/ASICamera2.lib`, `lib/x64/ASICamera2.dll`, `license.txt` | Links against the import library and copies `ASICamera2.dll` plus `licenses/ZWO-ASI-SDK.txt` into the build directory and release packages. |
| `SELENEWATCH_QHY_SDK_DIR` | `include/qhyccd.h` | Compiles against headers only; discovers and loads the user-installed x64 `qhyccd.dll` on demand at run time (`cameras/vendor_runtime`). |
| `SELENEWATCH_PLAYERONE_SDK_DIR` | `include/PlayerOneCamera.h` | Compiles against headers only; loads `PlayerOneCamera.dll` on demand (user choice, program folder, or N.I.N.A. / 64-bit SharpCap installs). |
| `SELENEWATCH_TOUPTEK_SDK_DIR` | `inc/toupcam.h` | Compiles against headers (`TOUPCAM_NOIMPORTS`); loads `toupcam.dll` on demand (user choice, program folder, or N.I.N.A. / 64-bit SharpCap / ToupTek ASCOM driver installs). |
| `SELENEWATCH_WITH_ASCOM` | Boolean (default `ON` on Windows) | Links `ole32` and `oleaut32` for late-bound COM against the installed ASCOM Platform. Requires no ASCOM SDK at build time. |
| `SELENEWATCH_WIX_ROOT` | Folder holding `bin/candle.exe` | Enables `.msi` generation in Release packaging (also reads `$ENV{WIX}`). Without it, packaging creates only the `.zip`. |

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for the license analysis explaining why `ASICamera2.dll` ships with builds while QHY, Player One, and ToupTek DLLs remain user-installed runtimes.

---

## 5. Running tests and manual probes

### Automated suite (`CTest`)

With Qt's `bin` directory on `PATH` and `QT_PLUGIN_PATH` set:

```powershell
ctest --test-dir out/build/debug --output-on-failure
```

- [CMakeLists.txt](CMakeLists.txt) registers Qt Test suites for core/session/ring/memory (`core`, `display_capture`, `manual_capture`, `session_record`), file formats (`ser`), detection (`detection`, `overview`, `sky`, `darkening`, `sky_extras`, `registration`), guiding (`guiding`), camera boundaries and runtime discovery (`zwo`, `qhy`, `playerone`, `touptek`, `vendor_runtime`, `camera_probe`), and GUI widgets (`preview`, `ui`).
- Camera tests run against deterministic boundary implementations (`FakeZwoApi`, `FakeQhyApi`, `FakePlayerOneApi`, `FakeToupTekApi`) and never open physical hardware.
- Window and offscreen rendering tests set `QT_QPA_PLATFORM=offscreen` and isolate QSettings under a test namespace.
- CTest invokes Qt Test targets with `-o -,tap` so test output reaches stdout on Windows even when launched without an attached console.

### Manual CLI probes

These targets are built alongside the application and are never executed by CTest:

| Target | Purpose | Docs / notes |
| --- | --- | --- |
| `SeleneWatchProbe` | Runs detection, rearming, overview, darkening, lock, registration, and ASCOM/simulated guiding against explicit recordings or synthetic loops (`guide-sim`, `ascom`, etc.). | Path must point to an explicit file/output directory; see [local environment.txt](local%20environment.txt) and `tools/`. |
| `SeleneWatchCameraProbe` | ZWO hardware inspection, bench capture, and unattended soak testing (`inspect`, `bench`, `soak`). | [docs/camera-soak.md](docs/camera-soak.md), [docs/zwo-validation.md](docs/zwo-validation.md) |
| `SeleneWatchQhyProbe` | QHY runtime version check, inspection, and bench/soak tool (`version`, `inspect`, `bench`, `soak`). | [docs/qhy-development.md](docs/qhy-development.md) |
| `SeleneWatchRuntimeProbe` | Checks dynamic runtime discovery/version without opening a camera: `SeleneWatchRuntimeProbe qhy\|playerone\|touptek [DLL]`. | Used to verify QHY, Player One, and ToupTek runtime DLL loading. |

---

## 6. Code organization and extension boundaries

### Build targets and directories

- `selenewatch_core` (static library): acquisition, ring buffer, `FlashDetector`, `EventCapture`, `Session`, format readers/writers, sky schedule/exposure, feature lock, guider, and mount adapters. Depends on `Qt6::Core` and `Qt6::Gui` only.
- `selenewatch_ui` (static library): `MainWindow`, preview widget, docks, settings dialogs, and embedded [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) resource.
- `SeleneWatch` (executable): desktop entry point (`main.cpp`) + Windows resource metadata (`selenewatch.rc`).

| Directory | Job |
| --- | --- |
| Root (`*.cpp`, `*.h`, `*.ui`) | Application core (`Session`, `FlashDetector`, `EventCapture`, `FramePool`, `FrameRingBuffer`, `MemoryBudget`) and Qt Widgets UI |
| `sources/` | `FrameSource` interface, `SyntheticFrameSource`, `RecordingSource` |
| `cameras/` | Per-vendor SDK wrappers (`*_sdk.cpp`), sources (`*_source.cpp`), `vendor_runtime` DLL discovery, and shared `raw_camera_layout.h` |
| `guiding/` | `FeatureLock`, `LockTracker`, `Calibrator`, `Guider`, `GuideController`, `Mount` interface, `SimulatedMount`, `AscomMount` |
| `recording/` | SER/AVI readers, FireCapture `.txt` log reader, SER event & continuous writers, `metadata_text`, `frame_timing`, `clip_overview` |
| `packaging/` | CPack/WiX rules, post-build `.zip` verification script, Qt notice generator, release script |
| `tests/` | Automated Qt Test suites and deterministic vendor fakes |
| `tools/` | Manual probe executables and experimental prototypes (such as `translation_estimator`) |
| `docs/` | Feature validation reports, soak tool documentation, and machine-readable benchmark JSON files |

### Key patterns when adding code

- **Frame ownership:** Sources write into a pooled `Frame` and `Session` publishes a `FramePtr` (`std::shared_ptr<const Frame>`). Downstream consumers never mutate pixel buffers.
- **Camera family panels:** The main window interacts with camera families solely through `CameraFamilyPanel` (`camera_family_panel.h`)—covering Source selection, device discovery, requested settings, source creation, live readbacks, and preview controls—so adding a new family does not require vendor-specific branches in `MainWindow`.
- **Vendor header isolation:** Keep third-party SDK headers confined to a single `*_sdk.cpp` translation unit behind a clean `*_api.h` boundary so tests stay deterministic and vendor preprocessor quirks do not leak.
- **Third-party obligations:** Preserve BSD-2-Clause licensing for SeleneWatch code, keep Qt 6 dynamically linked, keep un-redistributable vendor SDKs out of the repository and release packages, and update [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) whenever a dependency changes.

---

## 7. Building a release

### Versioning

Releases are named `YYYY.MM-NN`: the year, the month, and the release number within that month (for example `2026.11-01` is the first release of November 2026). Set `SELENEWATCH_RELEASE` once at the top of [CMakeLists.txt](CMakeLists.txt). From there CMake generates `selenewatch_version.h` (with the Git revision), populating:

- the window title and **Help → About SeleneWatch**;
- the executable's Windows file properties (`selenewatch.rc`);
- the `software:` line in every `event.txt` and `recording.txt`;
- the CPack package filenames.

Tag the release commit `vYYYY.MM-NN` (e.g. `v2026.11-01`).

### What a release contains

The Release build's `package` target invokes CPack using [packaging/package.cmake](packaging/package.cmake) to produce two packages in `out\build\release\package`:

| Package | Behavior |
| --- | --- |
| `SeleneWatch-<version>-win64.zip` | Unpacks into a `SeleneWatch-<version>-win64` folder and runs directly from there. |
| `SeleneWatch-<version>-win64.msi` | Installs per-user (no administrator rights) in `%LOCALAPPDATA%\SeleneWatch` with a Start-menu entry. Newer installers upgrade older ones while preserving the `events` folder on upgrade and uninstall. |

Both packages contain:

- `SeleneWatch.exe`
- Required Qt DLLs and plugins collected by `windeployqt` (explicitly excluding Qt Network, Qt Svg, translations, and software OpenGL)
- Visual C++ runtime DLLs (`InstallRequiredSystemLibraries`; Universal CRT comes from Windows 10/11)
- `ASICamera2.dll` (when configured with `SELENEWATCH_ZWO_SDK_DIR`)
- `LICENSE.txt`, `README.md`, `MANUAL.md`, `THIRD_PARTY_LICENSES.md`, `ACKNOWLEDGEMENTS.md`
- `icons/selenewatch-256.png` and `docs/images/` (logo and documentation screenshots referenced by `README.md` and `MANUAL.md`)
- `licenses/`: Qt's LGPLv3/GPLv3 license text (`Qt-LICENSE.txt`), generated third-party notices for shipped Qt libraries (`Qt-third-party-notices.txt`), and `ZWO-ASI-SDK.txt` (when built with ZWO)

Tests, probes, and QHY / Player One / ToupTek vendor files are never shipped. Debug builds have no `package` target.

### Packaging commands and automated verification

Building the `Release-packages` preset creates both packages and immediately runs [packaging/check_package.cmake](packaging/check_package.cmake) / [packaging/check_package.ps1](packaging/check_package.ps1), failing the build if any check fails:

- expected binaries, licenses, and docs are present in the `.zip`, and `Qt6Network.dll` / `Qt6Svg.dll` are absent;
- every DLL imported by any shipped `.exe`/`.dll` (`dumpbin /dependents`) is in the package or Windows `System32`;
- the packaged `SeleneWatch.exe` launches cleanly on Qt's offscreen platform with only its own folder and Windows directories on `PATH`.

**From Visual Studio:** select **Release (x64)** as configure preset and **Release: .zip and installer** (`Release-packages`) as build preset, then choose **Build → Build All** (switch the build preset back to **Debug (x64)** or **Release (x64)** for normal work).

**From developer PowerShell:**

```powershell
cmake --preset Release-x64
cmake --build --preset Release-packages
```

**Using the release script** ([packaging/make_release.ps1](packaging/make_release.ps1)), which refuses a dirty Git working tree, configures Release, runs the full `ctest` suite, and builds `Release-packages` (pass `-AllowDirty` for a trial `-dirty` package):

```powershell
packaging\make_release.ps1
```

### Release checklist

1. Update `SELENEWATCH_RELEASE` in [CMakeLists.txt](CMakeLists.txt), verify feature status in [README.md](README.md) / [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) / [MANUAL.md](MANUAL.md), and commit.
2. Run `packaging\make_release.ps1`.
3. Install the generated `.msi` once on a machine, launch SeleneWatch from the Start menu, and verify **Help → About SeleneWatch** and **Help → Acknowledgements**.
4. Tag the release commit (`git tag vYYYY.MM-NN`) and publish both packages with their SHA-256 checksums.

When upgrading Qt, regenerate the shipped Qt third-party notices from the new Qt documentation directory and commit the updated [packaging/licenses/Qt-third-party-notices.txt](packaging/licenses/Qt-third-party-notices.txt):

```powershell
powershell -File packaging\make_qt_notices.ps1 -QtDocs C:\dev\tools\Qt\Docs\Qt-<version> -QtVersion <version>
```
