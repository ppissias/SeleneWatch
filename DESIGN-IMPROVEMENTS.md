# Design improvements

This file lists design, refactoring and code-quality work for SeleneWatch: changes to how the code is organised, not to what the application does. New or changed functionality is in [features.md](features.md). The principles, design rules and patterns these items serve are in [DEVELOPER.md](DEVELOPER.md#1-principles-and-design-rules); the architecture is in [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md).

Every item is a proposal until the maintainer decides on it. Refactorings keep behaviour, saved settings and file formats; a change to any of them needs an explicit decision. Check each item with the full CTest suite, and with the real-window E2E test when it touches the window.

When an item is completed, move its whole section, unchanged, to [Completed](#completed) at the end of this file and add its outcome and checks below it; describe the resulting design in DEVELOPER.md or PROJECT_DESCRIPTION.md. Like `implemented features.md`, the completed part is an append-only record.

## Pending work

| ID | Change | Size |
| --- | --- | --- |
| [Design-3](#design-3-one-flow-for-the-camera-panels-that-load-the-observers-dll) | One flow for the camera panels that load the observer's DLL | Medium; deferred until a fourth such camera family |

### Design-3. One flow for the camera panels that load the observer's DLL

**Deferred by the maintainer (2026-11-22): not now; revisit when a fourth camera family that loads the observer's DLL is added.** `QhyCameraPanel`, `PlayerOneCameraPanel` and `ToupTekCameraPanel` (170–210 lines each) build the same widgets and run the same refresh, inspect, read-back and enable flow; Player One and ToupTek differ in about 50 lines once the names are swapped. A change to that flow must be made three times (principle 5).

**Possible change.** A base panel for families whose DLL the observer installs, holding the widgets and the flow, with each family supplying its listing, inspection, settings and read-back text (the template method pattern, which DEVELOPER.md does not list yet). **Against it:** the three families' profile and settings types differ, so the base needs either per-family hooks for nearly every step or a template; the ZWO panel would stay outside it; and the window tests check each panel's controls by name. Worth deciding when a fourth family that loads its DLL is added, or when the flow next changes.

## Completed

### Design-2. Source folders by job

**Decided by the maintainer (2026-11-22).** About 60 headers and sources sit at the repository root beside the documents, mixing the processing core, the window and its widgets. PROJECT_DESCRIPTION.md §11 sketched a target layout without adopting it.

**Change.** All code moves under `src/`, one folder per job, with `src/` as the include root, so `#include "cameras/zwo_api.h"` and the other existing folder paths stay as they are. The folders, from the bottom up:

| Folder | Job |
| --- | --- |
| `core/` | The frame and its pool, the memory budget, the test-signal mark, stream and site descriptions, what is watched (`Target`, `Watch`, `CaptureSettings`) |
| `display/` | Converting frames to images for the screen and the clip overviews (`preview_image`, `display_settings`) |
| `detection/` | `FlashDetector`, its settings, results, mask and each watch's defaults |
| `sources/` | `FrameSource`, the synthetic and recorded sources (unchanged) |
| `cameras/` | One adapter per camera family (unchanged) |
| `capture/` | The ring of recent frames, clip assembly (`EventCapture`) and the clip |
| `recording/` | Readers and writers of SER, AVI and the text records, the nightly report (unchanged) |
| `sky/` | Sun and Moon times at the site, the exposure follower, site presets |
| `autotune/` | Auto-tune's decisions and its measuring watches |
| `mounts/` | The `Mount` boundary, `SimulatedMount`, `AscomMount` (moved out of `guiding/`) |
| `guiding/` | Feature lock, calibration, the guider |
| `timelapse/` | Lunar and planetary time-lapses, the sky time-lapse, video export (unchanged) |
| `settings/` | What is saved between launches: named detection profiles, each watch's settings (Design-1) |
| `session/` | `Session`, the frame path from source to capture, preview and guider; test-signal injection, the clock check, the end of a session |
| `ui/`, `ui/cameras/`, `ui/widgets/` | The window, its docks and dialogs; the camera family panels; small reusable widgets |
| `app/` | `main.cpp` and the Windows resource script |

Dependencies point one way: `ui` and `app` may use everything; `session` uses the processing folders; nothing below `ui` includes a window header. **A test reads every `#include` under `src/` and fails on a dependency that is not allowed**, so the directions in the table stay true. `tests/`, `tools/`, `docs/` and `packaging/` stay at the root.

**Checks.** A pure move: `git mv` and include paths only, no other code change, so history follows each file. The full CTest suite on Debug, the Release build and package, and the new dependency test.

**Outcome (2026-11-22).** Done in two commits. The first is the pure move: `git mv` of 185 files into the folders above, the include paths rewritten from the new root, test helpers included from their own folder, and the CMake lists; no other code change. The second:

- **A cycle the move made visible.** `recording/event_writer.cpp` used `timelapse/video_exporter` for the event preview GIF, while `timelapse` uses `recording`. The FFmpeg encoding that is not about time-lapses (`findFfmpegExecutable`, `Mp4Stream`, `encodeStreamWithFfmpeg`, `encodeImagesWithFfmpeg`, `encodeEventPreviewGif`) moved, unchanged, to `recording/ffmpeg_video`; `timelapse/video_exporter` keeps the time-lapse video and re-sharpen. `guiding` uses `recording` (the guide log goes through the event writer), which the table allows.
- **The `dependency_rules` test** reads every `#include` under `src/` and fails on an include its table does not allow, on a folder missing from the table, and on Qt Widgets outside `ui/` and `app/`. A deliberately wrong include (`core/blip.h` including `ui/mainwindow.h`) made it fail with that line named.
- **The table** is in DEVELOPER.md, *Code organization*, with each folder's allowed dependencies; PROJECT_DESCRIPTION.md §11 shows the layout.
- `tools/` and `tests/` stay at the root; the probes include each other as `tools/…` from the repository root.

**Checks run.** Debug build of every target after each commit; the full CTest suite on Debug, 24 of 24 suites passed (the 23 before and `dependency_rules`). **Not run:** the Release build and package, and the real-window E2E test. The Release build in `out\build\release` is the one used for observing, so it was left for the maintainer to rebuild.

**Checks run later (2026-11-22), after Design-1 and Design-3's deferral (commit 0efff9a).** `packaging\make_release.ps1` on a clean tree: the Release build of every target, the full CTest suite on Release (25 of 25 passed, including the offscreen `ui` and `preview` window suites), and both packages (`.zip` and `.msi`) with their checks. The real-window E2E test (`tests\gui_e2e_test.ps1`) on the Release build: GUI E2E TEST PASSED, all four scenarios including the real lunar AVI; its screenshots were checked.

### Design-1. One owner for a watch's saved settings, and the code review's findings

**Decided by the maintainer (2026-11-22).** A code review on 22 November 2026, against the principles in DEVELOPER.md, found these. Each keeps behaviour and every saved setting that an observer can have.

1. **A watch's saved settings have no single owner** (principle 2). Where each watch's settings live in QSettings is decided in five places: `detectionKey`, `captureKey`, `followKey` and `savedWatch` in `mainwindow.cpp`; `watchKey` in `detection_profiles.cpp`; the `watchName(…) + "/autotune/longestExposureMs"` key in `mainwindow_autotune.cpp`; and the `skySlow/timelapse/…` literals in `setupDetectionPanel`. The window also reads and writes every field itself (`savedDetectionSettings`, `saveDetectionSettings`, `savedCaptureSettings`, `saveCaptureSettings`, `loadSkyExtras`, `saveSkyExtras`), about 150 lines of persistence in a 2,900-line window.
   **Change:** a `WatchSettings` module in the core library (no Widgets) owns the key scheme and loads and saves, per watch, its detection settings, capture settings, exposure follower, time-lapse and auto-tune longest exposure, and the watch chosen last. The window asks it and hands the values to the panels and the session. A test checks each watch's keys round-trip and that the keys an observer already has are read unchanged.
2. **The detection settings' field list is written four times** (principle 5): QSettings load and save in the window, and JSON to and from in the named profiles. A field added to one and not the others would be lost silently.
   **Change:** one field list, used by both the watch's saved settings and the named profiles.
3. **The slow sky's auto-tune longest exposure is saved under `sky_slow/`**, while its other settings are under `skySlow/`. One watch, two prefixes.
   **Change:** `skySlow/autotune/longestExposureMs`. No release has the slow sky yet (Feature-35, 22 November 2026), so no observer has the old key and no compatibility path is needed (principle 3).
4. **`MainWindow` keeps the target twice** (principle 2): `target_` is always `watchTarget(watch_)`, set in two places beside it.
   **Change:** keep only the watch; the target is derived from it.
5. **The exposure follower's and the slow sky's time-lapse settings are declared in a window header** (`SkyExtras` and `WatchTimelapse` in `detection_panel.h`), though the core's saved settings and the session use them.
   **Change:** declare them with the watch's saved settings, in the core library.
6. **The guider has its own copy of `utcText`** (principle 5), identical to the one in `recording/metadata_text`, which the guide log's other times use.
   **Change:** the guider uses the shared one.

**Considered and not proposed.** Splitting `MainWindow` into more files by size (the "Avoid" list); a settings framework over QSettings; moving the time-lapse panel's own saved settings out of the panel (it is their only reader and writer).

**Checks.** The full CTest suite; the window test that each watch keeps its own settings; a new round-trip test of every watch's keys.

**Outcome (2026-11-22).** All six done, in one commit after Design-2:

1. **`settings/watch_settings`** (in the core library) owns the keys of each watch's saved settings and loads and saves them: the watch chosen last, detection settings (with the sky's mask), capture settings, the exposure follower, the slow sky's time-lapse, auto-tune's longest exposure, and the named-profiles group. The keys are derived from one group per watch (the Moon's flashes at the root, `moonShadows/`, `sky/`, `skySlow/`), with the shares written out once: both Moon watches keep the Moon's capture settings, and a Moon watch reads the fast sky's follower. The window's helpers became `saveDetectionPanel`, `saveCapturePanel`, `saveSkyExtrasPanel` and `showSkyExtras`, which move values between the panels and the module; `mainwindow.cpp` lost about 150 lines.
2. **One field list** (`detectionFields`, `detectionFromFields`) serves the watch's settings and the named profiles. The profiles' JSON keeps the same keys and values.
3. The slow sky's longest exposure is `skySlow/autotune/longestExposureMs`.
4. `MainWindow` keeps only `watch_`; `target()` derives the target.
5. `SkyExtras` and `WatchTimelapse` are declared in `settings/watch_settings.h`; the Detection panel includes it.
6. The guider uses `utcText` from `recording/metadata_text`.

**Checks run.** Debug build of every target; the full CTest suite on Debug, 25 of 25 passed. The new `watch_settings` suite runs each test in the registry (the program's default store) and in a portable `.ini` (values come back as text): every watch's settings round-trip and stay apart; the keys earlier versions wrote are read unchanged; named profiles store the same fields and read back as saved; invalid values read as the watch's defaults. The window test that each watch keeps its own settings passed unchanged. **Not run:** the Release build and the real-window E2E test.

**Checks run later (2026-11-22), after Design-1 and Design-3's deferral (commit 0efff9a).** `packaging\make_release.ps1` on a clean tree: the Release build of every target, the full CTest suite on Release (25 of 25 passed, including the offscreen `ui` and `preview` window suites), and both packages (`.zip` and `.msi`) with their checks. The real-window E2E test (`tests\gui_e2e_test.ps1`) on the Release build: GUI E2E TEST PASSED, all four scenarios including the real lunar AVI; its screenshots were checked.
