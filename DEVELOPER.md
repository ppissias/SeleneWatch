# SeleneWatch Developer Guide

This guide covers the principles the code follows, the design rules and patterns that come from them, building SeleneWatch from source, running automated tests and manual hardware probes, the code layout, and packaging a release. It is for developers and coding agents. The principles guide changes; the other sections describe the current code. Pending design and refactoring work is in [DESIGN-IMPROVEMENTS.md](DESIGN-IMPROVEMENTS.md), and pending functionality in [features.md](features.md).

The user-facing feature name is **Time-lapse** or **lunar and planetary time-lapses**. Introductions may describe planetary rotation and changing lunar shadows; technical instructions describe frame selection, alignment, and stacking directly.

- For what the application does, see [README.md](README.md).
- For operating instructions at the telescope, see [MANUAL.md](MANUAL.md).
- For the architecture, the evidence rules, the time model and thread ownership, read [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) before changing the codebase. The rules below link to its sections rather than repeat them.
- For licensing and feature tracking, see [AGENTS.md](AGENTS.md).
- For concrete paths, tool versions, connected hardware, and local archive data on the maintainer's workstation, see [local environment.txt](local%20environment.txt).

## Contents

1. [Principles and design rules](#1-principles-and-design-rules)
2. [Design patterns in use](#2-design-patterns-in-use)
3. [How to add](#3-how-to-add)
4. [Working rules](#4-working-rules), including [Writing the user documentation](#writing-the-user-documentation)
5. [Technology stack](#5-technology-stack)
6. [Prerequisites](#6-prerequisites)
7. [Configuring and building](#7-configuring-and-building)
8. [Camera SDK and mount build options](#8-camera-sdk-and-mount-build-options)
9. [Running tests and manual probes](#9-running-tests-and-manual-probes)
10. [Code organization and extension boundaries](#10-code-organization-and-extension-boundaries)
11. [Building a release](#11-building-a-release)

---

## 1. Principles and design rules

Scientific correctness and preservation of valid results constrain every design change: the [evidence rules](PROJECT_DESCRIPTION.md#4-evidence-rules) change only by an explicit, recorded decision. Within that constraint, the maintainer's goals for the code, in order:

1. **Readable code.** A reader should understand a class from its name, its header comment, and its fields. Prefer plain, direct code over clever generality.
2. **A clear application design with clear boundaries.** Each piece of mutable state has one authoritative owner. Each directory has one job, and its dependencies point one way.
3. **No abstraction or engineering without a real need.** An interface normally needs two implementations or a test seam that is actually used; an explicit listener or ownership boundary can also justify one. A layer needs a responsibility of its own. A compatibility path needs a caller that still uses it. Remove unused machinery after identifying the contracts it protects. Fewer classes or files alone do not make a design simpler.
4. **A few chosen design patterns, applied consistently.** They are listed in [Design patterns in use](#2-design-patterns-in-use), with when to use each. Reach for one of them before inventing a structure; consistency does not justify an unnecessary abstraction.
5. **One implementation of each behaviour.** Code that must give the same result is written once and called from every place that needs it: the preview's "Would trigger" and auto-tune measure with the detector's own code, not a copy. A second implementation is acceptable when it has a reason of its own, such as bounded memory, speed or a different measurement; it then gives that reason in its header comment, and a test pins the two together where they must agree. Share the step, not a framework.

The design rules that follow from them, each with what checks it: the build or a test where one exists, otherwise review. The tests check selected boundaries and outcomes; they do not prove thread confinement or every ownership rule.

| Rule | Checked by |
| --- | --- |
| **The evidence rules hold.** A frame is stored as it arrived, with what is known and unknown about its time and loss ([§4](PROJECT_DESCRIPTION.md#4-evidence-rules)). | Review; the SER, session-record and camera tests check what each file records |
| **A published frame is never changed:** one writer, then many readers ([Frame ownership](PROJECT_DESCRIPTION.md#frame-ownership)). | The compiler: `FramePtr` points to a `const Frame` |
| **Nothing on the frame path waits** for the disk, the window or the mount ([Threads and ownership](PROJECT_DESCRIPTION.md#threads-and-ownership)). | The overload, writer-failure and full-disk tests; otherwise review |
| **Overload is counted and shown, never skipped silently** ([Overload policy](PROJECT_DESCRIPTION.md#overload-policy)). | The capture and overload tests |
| **One owner for each piece of mutable state,** and a worker uses settings copied when it takes them (each trigger keeps the values it used). | The detection and session tests; otherwise review |
| **A watch's saved settings are read and written only through `settings/watch_settings`,** the one place that knows their QSettings keys; the detection settings' fields are listed once there, for the watch and for named profiles. | The `watch_settings` test, in the registry and in a portable `.ini`, including the keys earlier versions wrote |
| **The processing core has no Qt Widgets.** Everything below the window builds and tests without one. | The build: `selenewatch_core` links only Qt Core and Qt Gui; the `dependency_rules` test |
| **Each folder under `src/` has one job, and its dependencies point one way** ([Code organization](#10-code-organization-and-extension-boundaries)). | The `dependency_rules` test |
| **Vendor SDK headers stay in one `*_sdk.cpp`** behind the family's `*_api.h`. | Review, and the builds without SDKs |
| **The mount is sent only guide pulses,** and a stop of tracking at the end of a session the observer chose; nothing SeleneWatch sends can leave it moving ([Mount guiding](PROJECT_DESCRIPTION.md#mount-guiding)). | The guiding tests with `SimulatedMount` |
| **Collaborators are passed in and listeners are explicit.** No singletons, service locator, global event bus or mutable static application state. | Review |
| **Evidence is written as it arrives; derived files are replaced only when complete.** SER files, the fragmented MP4 and the text journals grow frame by frame and line by line, so a crash or power cut keeps what was captured. A report or summary that is rewritten (`report.html`) is written beside and swapped in (`QSaveFile`). | The SER, time-lapse (an unclosed MP4 still plays) and report tests |
| **Every output names the program that wrote it** (`software:` in `event.txt`, `recording.txt` and `timelapse.txt`). | The session-record tests |
| **Logic is testable without the window.** A decision with several steps is a pure function the window or a worker carries out. | The core suites run without a window; the `ui` suite drives the real window offscreen |
| **Tests never touch the observer's settings or output.** | The `ui` suite uses its own settings name and temporary folders; the E2E script sets `SELENEWATCH_SETTINGS_NAME` |
| **One implementation of each behaviour** (principle 5). | Review. The Preview's "Would trigger" readout is the live detector's own summary, and auto-tune's watches run `FlashDetector` itself |

**Avoid:** splitting a class or file by line count; replacing a large window with an equally large controller; a generic event bus or making every object observable; moving detection off the source thread before a measurement shows the need; copying pixel buffers to hand frames on; an interface before its second implementation; keeping large image buffers for later convenience; a compatibility path for settings nobody has saved.

## 2. Design patterns in use

Reach for one of these before inventing a structure.

| Pattern | Use it when | Where |
| --- | --- | --- |
| Adapter behind a narrow API, with a fake | Code talks to hardware or a vendor SDK | `ZwoApi`, `QhyApi`, `PlayerOneApi`, `ToupTekApi` (each with a `Fake…Api` in `tests/`); `Mount` with `SimulatedMount` and `AscomMount`; `FrameSource` |
| Family panel | The window must handle a camera family without naming it | `CameraFamilyPanel`: Source entry, discovery, requested settings, the source it makes, readbacks and live controls |
| Publish once, read-only after | Data crosses threads | `FramePtr` (`std::shared_ptr<const Frame>`) from a `FramePool`; a clip copies pointers, not pixels |
| Newest value wins | A slow reader follows a fast stream | `LatestFrameSlot` for the preview; the guider's one-slot mailbox |
| Bounded queue to one worker | Slow work (disk, encoding) must not hold up the frame path | `EventWriter`, `RecordingWriter`, `SkyTimelapse` (two frames); what does not fit is counted and reported |
| Explicit notifications | The core tells the window something | `Session::Notifications`: named callbacks, queued to the window's thread |
| Status snapshot | The window shows a worker's state | `status()` returns a value struct taken under the worker's lock: `RecordingWriter::Status`, `SkyTimelapse::Status`, `GuiderStatus`, `TuneWatchStatus`, `TimelapseStatus` |
| Pure decision function | A procedure of several steps decides its next move | `nextCameraStep` and `refineGain` (auto-tune), `decideSkyExposure` (the exposure follower), the guide step, `sky_schedule`: the window or a worker carries the step out, and tests run the decision without a camera |
| Settings of a watch | A value is saved per watch | `settings/watch_settings`: one function pair per kind of setting (`savedDetectionSettings` / `saveDetectionSettings`, …); the window asks it and hands the values to the panels and the session |
| Settings copied at admission | A worker uses settings the observer can change | The detector's settings are copied before each frame, a trigger keeps the values it used, a time-lapse keeps its interval |
| Plain-text record of every output | Anything is saved | `event.txt`, `recording.txt`, `timelapse.txt` as `key: value` lines; `session.txt` as `<UTC> <event> key=value` lines, written through `metadata_text` |
| Vendor header isolation | A vendor SDK is used | Its header is included in one `*_sdk.cpp`; the rest of the code sees the family's `*_api.h` |

## 3. How to add

Short recipes for the common additions. Each follows a pattern above.

**A camera family.**
1. Write the family's `*_api.h` boundary and its `*_sdk.cpp`, the only file that includes the vendor header; load an observer-installed DLL through `cameras/vendor_runtime`.
2. Write its `FrameSource` in `cameras/`, reporting what the camera can and cannot say about time, loss and sample packing ([Camera support](PROJECT_DESCRIPTION.md#camera-support)).
3. Write a `Fake…Api` in `tests/` and a test suite against it; the tests never open hardware.
4. Write its `CameraFamilyPanel` and register it with `addCameraFamily`; the window names no family.
5. Add the CMake option, [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), [Section 8](#8-camera-sdk-and-mount-build-options) and MANUAL.md.

**A detection setting.**
1. Add the field to `DetectionSettings` with its default in `target_defaults.h`, and its check in `valid()`.
2. Use it in `FlashDetector`; a trigger records the value it used.
3. Add it to the one field list in `settings/watch_settings.cpp` (`detectionFields` and `detectionFromFields`), which saves it with the watch and in named profiles; give it a control in the Detection panel.
4. Test it in the detection suite, and describe it in MANUAL.md.

**A watch** (something to look for, such as the slow sky of Feature-35).
1. Add it to `Watch` with its target, name and defaults (`target_defaults.h`).
2. Give it its group of saved settings in `settings/watch_settings.cpp` (and say there which settings it shares), its named-profiles group, and its entry in the Detection panel's list.
3. Record it on `monitoring_start` in `session.txt`, and extend the window test that checks each watch keeps its own settings.

**An output file or a `session.txt` line.**
1. Write it through the writer that owns that folder, never from the window or the frame path.
2. Use the plain-text record: `key: value` lines, the `software:` line, UTC from `metadata_text`.
3. List it in MANUAL.md, and check it in a session-record or capture test.

## 4. Working rules

These apply to everyone, including coding agents.

- **Never overwrite the observer's saved settings or output.** Tests use their own settings name and temporary folders. An observing run may be live from `out\build\release`: check before building or running anything there.
- **Discuss a fix before implementing it,** and show its effect on real data or a real camera where it touches detection or capture.
- **Separate refactoring from behaviour changes.** A refactoring keeps behaviour, saved settings and file formats; a change to them needs an explicit decision and its own commit.
- **Document every new type** with a header comment covering its responsibility, and any ownership, units or threading contract; update it when behaviour changes.
- **Keep documentation current in the same change:** behaviour in MANUAL.md, design in this guide or PROJECT_DESCRIPTION.md, pending work only in features.md or DESIGN-IMPROVEMENTS.md.
- **Diagnostic programs and test output** go in a scratch folder or the ignored `out\`, never in tracked files. Manual probes that are kept belong in `tools/`.
- **Commits:** descriptive messages with the validation in the body; stage only your own changes; never put private details or the observer's location in a commit message, a screenshot or a tracked file.

### Writing the user documentation

The README, the MANUAL and the website are read by observers, most of them not programmers. They should be able to read them with pleasure and understand them the first time.

- **Lead with what the observer can do,** in one order, the same in the README, the MANUAL and the website: watching the sky, lunar and planetary time-lapses, shadows crossing the Moon, lunar impact flashes. For each: what it is for, how it works in a few sentences, the steps, what the result looks like, and a table of its controls with their starting values.
- **Plain words.** Short sentences, one idea to a paragraph, everyday words before technical ones. Explain a term the first time it appears ("K: how far above its own noise a pixel must rise"), and prefer "the frames from just before and after" to "the pre-event ring buffer". Name each control exactly as the window shows it, in bold.
- **Say what was measured, and no more.** No selling words ("powerful", "seamless"). Give numbers with their scope ("on two recordings, 23 of 24 test crossings"). A detection is a candidate, never a discovery. Say what has been tested and what still waits for a real night or a real camera.
- **The README is brief, the MANUAL complete.** The README says what SeleneWatch does, shows each use with one picture, lists the other features in a line each, and says how to get it and how far it has been tested. Defaults, limits, every control and the files belong in the MANUAL, which the README links to.
- **Every picture shows the feature and says what it is.** Prefer a real capture. Without one, make test data through the application's own path (the probe's `inject-clip`). Say so in the caption: *Test data*, what was added, and that no real event was recorded. Label sample data as sample data. A caption says what to look at.
- **Nothing private or incidental.** No recording file names or paths in the text, the captions or the pictures; remove an overview's source line before using it. Output file names only where the observer needs them, in the MANUAL's chapter on the saved files. Never the site's position.
- **Keep it true as the code changes.** A change of behaviour updates the MANUAL in the same change. A fact (a default, a limit, a measurement) is stated once in detail and linked from elsewhere, not repeated.
- **Keep the links working.** The window opens MANUAL sections by their heading's anchor (the startup tips, checked by the `ui` suite), and the README and the website link to them too. When a heading changes, update all three, and check that every image exists and every anchor resolves. Use plain Markdown in the MANUAL, which the window shows with its own viewer: no HTML layout.

---

## 5. Technology stack

| Area | Choice |
| --- | --- |
| Language | C++20 (`/W4 /permissive- /utf-8` on MSVC) |
| User interface | Qt 6 Widgets (`Core`, `Gui`, `Widgets`; dynamically linked under LGPLv3); UI-only Qt `Network` for release checks |
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

## 6. Prerequisites

1. **Visual Studio** with the **Desktop development with C++** workload and CMake support.
2. **Qt 6** built for your **MSVC 64-bit** toolchain, including **Qt Widgets** (do not use a MinGW Qt kit).
3. **Ninja** (supplied with Visual Studio or Qt Tools).
4. **Optional camera SDKs** (kept outside the repository; see [Section 8](#8-camera-sdk-and-mount-build-options)). Without any camera SDK configured, SeleneWatch still builds and runs with the Synthetic source and SER/AVI recordings.
5. **Optional WiX Toolset 3** binaries (`bin\candle.exe`, `bin\light.exe`), required only to generate the `.msi` installer during release packaging.

---

## 7. Configuring and building

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
  -DCMAKE_PREFIX_PATH="$env:QTDIR" -DSELENEWATCH_ZWO_SDK_DIR="C:/dev/tools/SDKs/ASI SDK" `
  -DSELENEWATCH_OPENCV_DIR="C:/dev/tools/opencv/build" `
  -DSELENEWATCH_CFITSIO_DIR="C:/dev/tools/cfitsio" `
  -DSELENEWATCH_FFMPEG_DIR="C:/dev/tools/ffmpeg"
cmake --build out/build/zwo-release
ctest --test-dir out/build/zwo-release --output-on-failure
.\out\build\zwo-release\SeleneWatch.exe
```

---

## 8. Camera SDK and mount build options

Camera SDKs stay outside the repository. Each adapter is enabled when its CMake cache variable is non-empty:

| CMake variable | Expected layout | Build & runtime behavior |
| --- | --- | --- |
| `SELENEWATCH_ZWO_SDK_DIR` | `include/ASICamera2.h`, `lib/x64/ASICamera2.lib`, `lib/x64/ASICamera2.dll`, `license.txt` | Links against the import library and copies `ASICamera2.dll` plus `licenses/ZWO-ASI-SDK.txt` into the build directory and release packages. |
| `SELENEWATCH_QHY_SDK_DIR` | `include/qhyccd.h` | Compiles against headers only; discovers and loads the user-installed x64 `qhyccd.dll` on demand at run time (`cameras/vendor_runtime`). |
| `SELENEWATCH_PLAYERONE_SDK_DIR` | `include/PlayerOneCamera.h` | Compiles against headers only; loads `PlayerOneCamera.dll` on demand (user choice, program folder, or N.I.N.A. / 64-bit SharpCap installs). |
| `SELENEWATCH_TOUPTEK_SDK_DIR` | `inc/toupcam.h` | Compiles against headers (`TOUPCAM_NOIMPORTS`); loads `toupcam.dll` on demand (user choice, program folder, or N.I.N.A. / 64-bit SharpCap / ToupTek ASCOM driver installs). |
| `SELENEWATCH_OPENCV_DIR` | `OpenCVConfig.cmake` (or `x64/vc16/lib/OpenCVConfig.cmake`), `x64/vc16/bin/opencv_world4140[d].dll` | Required for time-lapse demosaicing, phase-correlation/ECC alignment, stacking, and sharpening; copies the runtime DLL into build and release outputs. |
| `SELENEWATCH_CFITSIO_DIR` | `include/fitsio.h`, `lib/cmake/cfitsio/cfitsioConfig.cmake`, `bin/cfitsio.dll` | Required for 32-bit float FITS reading/writing (`CFITSIO::cfitsio`); copies `cfitsio.dll` into build and release outputs. |
| `SELENEWATCH_FFMPEG_DIR` | `bin/ffmpeg.exe` + LGPL shared DLLs, `LICENSE.txt` | Bundles external LGPLv3 `ffmpeg.exe` + shared DLLs for time-lapse MP4 (`libopenh264`) and GIF export. |
| `SELENEWATCH_WITH_ASCOM` | Boolean (default `ON` on Windows) | Links `ole32` and `oleaut32` for late-bound COM against the installed ASCOM Platform. Requires no ASCOM SDK at build time. |
| `SELENEWATCH_WIX_ROOT` | Folder holding `bin/candle.exe` | Enables `.msi` generation in Release packaging (also reads `$ENV{WIX}`). Without it, packaging creates only the `.zip`. |

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for the license analysis explaining why `ASICamera2.dll` ships with builds while QHY, Player One, and ToupTek DLLs remain user-installed runtimes.

---

## 9. Running tests and manual probes

### Monitoring folders and nightly reports

The Session allocates a unique watch directory on arming. The event writer puts journals at its root and clip folders under the clips directory. The recorder uses the watch's recordings directory during Monitoring and the output root's recordings directory during Preview. Report finalization follows both writers' queued work. Monitoring boundaries in session.txt use host UTC, with elapsed time measured on the steady clock. Candidate timestamps use source UTC, including during replay.

To regenerate a report without starting acquisition, run `SeleneWatch.exe --nightly-report "<watch folder>"` with the usual Qt runtime. The generator reads the journal and sidecars and atomically replaces report.html. Legacy clips can be grouped under clips with their original candidates.txt at the watch root. Missing stop times appear as unknown coverage. It groups each clip again from `event.txt` with `recording/clip_groups` (Feature-46), the code the overview uses when the clip is written, and puts consecutive clips of one event together. Its **Open SER** and **Show in folder** links become `selenewatch:` links, which Windows hands to `SeleneWatch.exe --open-link "<link>"`: it checks the file with `recording/report_links` (known types, inside a watch folder), opens it with its program or in Explorer, and exits. The program registers that link for the current user at each start, except under `SELENEWATCH_SETTINGS_NAME`. [Report generator](src/recording/nightly_report.cpp)

### Automated suite (`CTest`)

With Qt's `bin` directory on `PATH` and `QT_PLUGIN_PATH` set:

```powershell
ctest --test-dir out/build/debug --output-on-failure
```

- [CMakeLists.txt](CMakeLists.txt) registers Qt Test suites for the source folders' dependency directions (`dependency_rules`), each watch's saved settings (`watch_settings`), core/session/ring/memory (`core`, `display_capture`, `manual_capture`, `session_record`), file formats (`ser`), detection (`detection`, `overview`, `sky`, `darkening`, `sky_extras`, `registration`, `auto_tune`), time-lapse (`timelapse`), guiding (`guiding`), camera boundaries and runtime discovery (`zwo`, `qhy`, `playerone`, `touptek`, `vendor_runtime`, `camera_probe`), GUI widgets (`preview`, `ui`), and the status page for a phone (`remote`; its server listens on the loopback only, so no firewall asks).
- `manual_capture` checks that a Moon time-lapse saves a transit in an idle gap, delays the next burst for its clip, and finishes an open clip at Stop. `timelapse` covers grace expiry and cadence. `ui` covers live Preview watch changes, settings, and the Moon-only transit controls.
- Camera tests run against deterministic boundary implementations (`FakeZwoApi`, `FakeQhyApi`, `FakePlayerOneApi`, `FakeToupTekApi`) and never open physical hardware.
- Window and offscreen rendering tests set `QT_QPA_PLATFORM=offscreen` and isolate QSettings under a test namespace.
- CTest invokes Qt Test targets with `-o -,tap` so test output reaches stdout on Windows even when launched without an attached console.
- **Optional real archive data (`test_data/`, gitignored):** Placing real `.avi` / `.ser` recordings and matching FireCapture `.txt` logs (`Moon_221449.avi`, `Mars_222831.avi`, `Moon_171311.ser`) into `test_data/` at the repository root enables `TimelapseTest::realArchiveRecordingsIfPresent()`; when `test_data/` is absent, the test skips cleanly via `QSKIP`.
- **Real Windows GUI end-to-end test (`tests/gui_e2e_test.ps1`):** Launches the real built `out\build\release\SeleneWatch.exe` visibly on Windows using Windows UI Automation with isolated settings (`SELENEWATCH_SETTINGS_NAME=SeleneWatchGuiE2ETest`) and a temporary output folder, exercising Moon Preview zoom + profile save + bright/darkening clips on disk, Sky Watch meteor/lightning clips on disk, Time-lapse + Re-sharpen + non-destructive video rebuild, and real lunar AVI playback, saving verification screenshots to `out\gui-e2e\`.

### Which tests to run, and when

There are two levels of GUI testing:

| Level | What it is | Runs | When |
| --- | --- | --- | --- |
| **Offscreen UI suite** (`ui`, `preview` in CTest) | Qt Test drives the real `MainWindow`, docks and dialogs without showing them (`QT_QPA_PLATFORM=offscreen`): saved settings, confirmations (raw SER), closing during a time-lapse, Re-sharpen, profiles, zoom, Moon/Sky clips written on disk | Every `ctest` run, and `packaging\make_release.ps1` | After every change |
| **Real Windows GUI E2E** (`tests/gui_e2e_test.ps1`) | The built Release `SeleneWatch.exe` on the real desktop, driven through Windows UI Automation | By hand; takes a few minutes and opens a visible window (do not use the mouse or keyboard while it runs) | After changes to the window, docks, dialogs, threads, time-lapse or packaging, and **before every release** |

```powershell
cmake --build --preset Release-x64
# Add -SkipRealData when test_data\Moon_221449.avi is not present.
powershell -ExecutionPolicy Bypass -File tests\gui_e2e_test.ps1
```

The E2E script prints `GUI E2E TEST PASSED` and leaves screenshots in `out\gui-e2e\`; look at them, because a screenshot can show a layout problem that no assertion checks. Its settings and output folder are temporary, so your own settings are not touched.

### Manual CLI probes

These targets are built alongside the application and are never executed by CTest:

| Target | Purpose | Docs / notes |
| --- | --- | --- |
| `SeleneWatchProbe` | Runs detection, rearming, overview, darkening, lock, registration, and ASCOM/simulated guiding against explicit recordings or synthetic loops (`guide-sim`, `ascom`, etc.). `iss INPUT OUTPUT.json LENGTH[,LENGTH...]` counts ISS crossings at their real speed with the shadows watch's two-frame default and with a one-frame trigger (Feature-48). `inject-clip INPUT OUTPUT_ROOT CONFIG.json` plays a recording through the application's own Monitoring path with one marked test signal, for documentation pictures of test data. | Path must point to an explicit file/output directory; see [local environment.txt](local%20environment.txt) and `tools/`. |
| `SeleneWatchCameraProbe` | ZWO hardware inspection, bench capture, and unattended soak testing (`inspect`, `bench`, `soak`). | [docs/camera-soak.md](docs/camera-soak.md), [docs/zwo-validation.md](docs/zwo-validation.md) |
| `SeleneWatchQhyProbe` | QHY runtime version check, inspection, and bench/soak tool (`version`, `inspect`, `bench`, `soak`). | [docs/qhy-development.md](docs/qhy-development.md) |
| `SeleneWatchRuntimeProbe` | Checks dynamic runtime discovery/version without opening a camera: `SeleneWatchRuntimeProbe qhy\|playerone\|touptek [DLL]`. | Used to verify QHY, Player One, and ToupTek runtime DLL loading. |

---

## 10. Code organization and extension boundaries

### Build targets and directories

- `selenewatch_core` (static library): everything under `src/` except `remote/`, `ui/` and `app/`. Depends on `Qt6::Core` and `Qt6::Gui` only.
- `selenewatch_remote` (static library): `src/remote/`, the status page for a phone (Feature-47). Depends on `Qt6::Core`, `Qt6::Gui` and `Qt6::Network`, and on `selenewatch_qrcodegen`, Project Nayuki's MIT QR code generator, compiled unmodified from `third_party/qrcodegen/`.
- `selenewatch_ui` (static library): `src/ui/`: `MainWindow`, preview widget, docks, dialogs (including `AcknowledgementsDialog` and `AboutDialog` with embedded `LICENSE` and logos), and camera family panels.
- `SeleneWatch` (executable): `src/app/`: the entry point (`main.cpp`) and the Windows resource script (`selenewatch.rc`).

All application code is under `src/`, one folder per job (Design-2). `src/` is the include root, so an include names the folder: `#include "cameras/zwo_api.h"`. Paths in this guide such as `cameras/vendor_runtime` are under `src/`. The folders, from the bottom up; each uses only the folders listed for it:

| Folder | Job | May use |
| --- | --- | --- |
| `core/` | `Frame`, `FramePool`, `MemoryBudget`, the test-signal mark (`blip.h`), stream and site descriptions, what is watched (`Target`, `Watch`, `CaptureSettings`) | — |
| `display/` | Frames to images for the screen and the clip overviews (`preview_image`, `display_settings`) | core |
| `detection/` | `FlashDetector`, its settings, results and mask, each watch's defaults (`target_defaults.h`) | core |
| `capture/` | `FrameRingBuffer`, clip assembly (`EventCapture`), the clip (`event_clip.h`) | core, detection |
| `recording/` | SER/AVI readers, FireCapture `.txt` log reader, SER event & continuous writers, `output_paths`, `metadata_text`, `frame_timing`, `clip_overview`, `clip_groups` (a clip's measured group and events), the nightly report and its `report_links`, FFmpeg encoding (`ffmpeg_video`) | core, capture, detection, display |
| `sources/` | `FrameSource` interface, `SyntheticFrameSource`, `RecordingSource` | core, recording |
| `cameras/` | Per-vendor SDK wrappers (`*_sdk.cpp`), sources (`*_source.cpp`), `vendor_runtime` DLL discovery, and shared `raw_camera_layout.h` | core, sources |
| `sky/` | Sun and Moon times at the site (`sky_schedule`), the exposure follower (`sky_exposure`), site presets | core, detection |
| `autotune/` | Auto-tune's decisions (`auto_tune`) and its measuring watches (`auto_tune_watch`) | core, detection, sky |
| `mounts/` | The `Mount` boundary, `SimulatedMount`, `AscomMount` | core |
| `guiding/` | `FeatureLock`, `LockTracker`, `Calibrator`, `Guider`, `GuideController` | core, mounts, recording |
| `timelapse/` | `TimelapseSession`, CFITSIO 32-bit float FITS I/O (`fits_io`), OpenCV burst stacking/alignment/sharpening (`stack_processor`), the time-lapse video and re-sharpen (`video_exporter`), `timelapse_settings`, the slow sky's `SkyTimelapse` | core, display, guiding, recording |
| `settings/` | What is saved between launches: each watch's settings and the watch chosen last (`watch_settings`, the only code that knows their QSettings keys), named detection profiles | core, detection |
| `session/` | `Session` (the frame path from source to capture, preview and guider), test-signal injection, the clock check, the end of a session | all of the above |
| `remote/` | The read-only status page for a phone (Feature-47): the snapshot, its JSON and page, and the answer to a request (`status_page`), the server on Qt Network (`status_server`), this computer's addresses and the QR code (`phone_link`). The window fills the snapshot from what it shows | — |
| `ui/widgets/` | Small reusable widgets (`ElidedLabel`, `FlowLayout`, `ReadoutLabel`, `WheelGuard`, control help) | core |
| `ui/cameras/` | `CameraFamilyPanel` and one panel per family, the shared settings editor, the runtime chooser | core, sources, cameras, ui/widgets |
| `ui/` | `MainWindow` (with `mainwindow_autotune.cpp`, `mainwindow_session_end.cpp` and `mainwindow_remote.cpp`), its docks and dialogs | everything except app |
| `app/` | `main.cpp` and `selenewatch.rc` | core, recording, session, ui |

The `dependency_rules` test reads every `#include` under `src/` and fails on one the table does not allow, on a folder that is not in it, and on Qt Widgets outside `ui/` and `app/`; its table and this one change together. Outside `src/`:

| Directory | Job |
| --- | --- |
| `packaging/` | CPack/WiX rules, post-build `.zip` verification script, Qt notice generator, release script |
| `tests/` | Automated Qt Test suites and deterministic vendor fakes |
| `tools/` | Manual probe executables and experimental prototypes (such as `translation_estimator`); they include each other as `tools/…` |
| `docs/` | Feature validation reports, soak tool documentation, and machine-readable benchmark JSON files |
| `icons/` | The program's icons |

### Patterns and third-party obligations

The patterns to follow when adding code (frame ownership, camera family panels, vendor header isolation and the others) are in [Design patterns in use](#2-design-patterns-in-use). Preserve BSD-2-Clause licensing for SeleneWatch code, keep Qt 6 dynamically linked, keep un-redistributable vendor SDKs out of the repository and release packages, and update [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) whenever a dependency changes ([AGENTS.md](AGENTS.md)).

---

## 11. Building a release

### Versioning

Releases are named `YYYY.MM-NN`: the year, the month, and the release number within that month (for example `2026.11-01` is the first release of November 2026). Set `SELENEWATCH_RELEASE` once at the top of [CMakeLists.txt](CMakeLists.txt). From there CMake generates `selenewatch_version.h` (with the Git revision), populating:

- the window title and **Help → About SeleneWatch**;
- the executable's Windows file properties (`src/app/selenewatch.rc`);
- the `software:` line in every `event.txt` and `recording.txt`;
- the CPack package filenames.

Tag the release commit `vYYYY.MM-NN` (e.g. `v2026.11-01`).

Publish a **stable GitHub Release**, marked as the latest release, for that tag; pushing a tag alone does not publish a release. Feature-20 checks `https://api.github.com/repos/ppissias/SeleneWatch/releases/latest` once after normal startup. It compares the year, month, and monthly release number with the compiled `SELENEWATCH_RELEASE`; both `vYYYY.MM-NN` and `YYYY.MM-NN` tags are accepted. Drafts, prereleases, malformed versions, equal/older versions, and failed checks produce no notice. A newer version shows a button at the top-right corner of the menu bar, opening the fixed SeleneWatch releases page.

`release_checker.h/.cpp` owns this asynchronous check, its four-second overall deadline, and a bounded response. It belongs to the UI library, with no network dependency added to the processing core. Qt handles HTTPS certificate verification; release packages use the Windows Schannel TLS backend. No GitHub credentials or observing data are sent. Isolated package/GUI test launches (`SELENEWATCH_SETTINGS_NAME`) skip the startup request. The `release_check` CTest suite supplies fake Qt network replies for version, HTTP, malformed-data, response-size, and timeout checks; the `ui` suite checks the top-right button and browser destination without opening a browser.

### What a release contains

The Release build's `package` target invokes CPack using [packaging/package.cmake](packaging/package.cmake) to produce two packages in `out\build\release\package`:

| Package | Behavior |
| --- | --- |
| `SeleneWatch-<version>-win64.zip` | Unpacks into a `SeleneWatch-<version>-win64` folder and runs directly from there. |
| `SeleneWatch-<version>-win64.msi` | Installs per-user (no administrator rights) in `%LOCALAPPDATA%\SeleneWatch` with a Start-menu entry. Newer installers upgrade older ones while preserving the `events` folder on upgrade and uninstall. |

Both packages contain:

- `SeleneWatch.exe`
- Required Qt DLLs and plugins collected by `windeployqt`, including `Qt6Network.dll` and `tls/qschannelbackend.dll` for HTTPS release checks (excluding Qt Svg, OpenSSL/certificate-only TLS backends, translations, and software OpenGL)
- Visual C++ runtime DLLs (`InstallRequiredSystemLibraries`; Universal CRT comes from Windows 10/11)
- `ASICamera2.dll` (when configured with `SELENEWATCH_ZWO_SDK_DIR`)
- `opencv_world4140.dll`, `cfitsio.dll`, and external LGPLv3 `ffmpeg.exe` + FFmpeg shared DLLs (`avcodec-63.dll`, `avdevice-63.dll`, `avfilter-12.dll`, `avformat-63.dll`, `avutil-61.dll`, `swresample-7.dll`, `swscale-10.dll`)
- `LICENSE.txt`, `README.md`, `MANUAL.md`, `CAMERA_SETTINGS_GUIDE.md`, `THIRD_PARTY_LICENSES.md`
- `icons/selenewatch-256.png` and `docs/images/` (logo and documentation screenshots referenced by `README.md` and `MANUAL.md`)
- `licenses/`: Qt's LGPLv3/GPLv3 license text (`Qt-LICENSE.txt`), generated third-party notices for shipped Qt libraries (`Qt-third-party-notices.txt`), `ZWO-ASI-SDK.txt` (when built with ZWO), `OpenCV-LICENSE.txt`, `CFITSIO-LICENSE.txt`, and `FFmpeg-LICENSE.txt`

Tests, probes, and QHY / Player One / ToupTek vendor files are never shipped. Debug builds have no `package` target.

### Packaging commands and automated verification

Building the `Release-packages` preset creates both packages and immediately runs [packaging/check_package.cmake](packaging/check_package.cmake) / [packaging/check_package.ps1](packaging/check_package.ps1), failing the build if any check fails:

- expected binaries, licenses, docs, `Qt6Network.dll`, and `tls/qschannelbackend.dll` are present in the `.zip`; unused Qt Svg and OpenSSL/certificate-only TLS backends are absent;
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
2. Run `packaging\make_release.ps1` (it runs the full CTest suite, including the offscreen UI tests).
3. Run the real Windows GUI E2E test, `tests\gui_e2e_test.ps1`, on the Release build with `test_data\` present, and check its screenshots (see [Which tests to run, and when](#which-tests-to-run-and-when)).
4. Install the generated `.msi` once on a machine, launch SeleneWatch from the Start menu, and verify **Help → About SeleneWatch** and **Help → Acknowledgements**.
5. Tag the release commit (`git tag vYYYY.MM-NN`) and publish both packages with their SHA-256 checksums.

When upgrading Qt, regenerate the shipped Qt third-party notices from the new Qt documentation directory and commit the updated [packaging/licenses/Qt-third-party-notices.txt](packaging/licenses/Qt-third-party-notices.txt):

```powershell
powershell -File packaging\make_qt_notices.ps1 -QtDocs C:\dev\tools\Qt\Docs\Qt-<version> -QtVersion <version>
```
