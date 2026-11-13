# SeleneWatch

**Continuous lunar monitoring and automatic capture of transient phenomena.**

SeleneWatch is a native C++/Qt desktop application being developed to monitor the Moon through a telescope-mounted camera, identify potentially interesting short-lived events, and preserve the surrounding frames for later review.

The main motivation is to help observers detect and document **lunar transient phenomena**, particularly brief flashes that may be caused by meteoroid impacts. The design also covers transits, occultations, moving objects, and other unexpected events.

> **Project status: Feature-6 complete; Feature-9 implemented, with observing calibration still open.** Synthetic streams and SER/uncompressed AVI recordings use the same detector, 150-frame history, and asynchronous SER writer. The Detection panel controls a per-pixel noise trigger, including single-frame flashes, and shows its history and display overlays. Clips retain native pixel values, timestamps, test marks, detector settings, and review previews. Debug and Release tests cover the processing path and the actual window. Measurements and remaining data limitations are in [Feature-9 validation](docs/feature-9-validation.md). Display stretch, a shared RAM budget, camera adapters, and mount guiding follow.

## What SeleneWatch aims to do

- **Monitor continuously** using cameras from many vendors, at the highest resolution and frame rate the hardware allows.
- **Analyse every frame** to find candidate events while observation continues, without going blind while saving.
- **Keep a RAM ring buffer** sized in seconds, so frames from *before* a detection are still available.
- **Capture events automatically**, saving frames before and after a trigger. Predicted events (occultations, transits) are captured with continuous recording.
- **Preserve lossless evidence**: pixel values exactly as the camera delivered them, at 8, 12, or 16 bits, in SER files with per-frame UTC timestamps.
- **Measure sensitivity** by injecting known test signals into real data.
- **Hold the field** by locking on a lunar feature, showing its drift, and on request correcting the mount with small, logged guide pulses.
- **Support event review**, so observers inspect a short list of candidates instead of hours of recordings.
- **Develop and test without hardware** using synthetic streams, recorded SER or AVI files, and a simulated mount.

Automatic detections are **candidates for review**, not scientific confirmation of a lunar event. Confirming an impact flash normally needs an independent observation from another telescope or site. SeleneWatch records each candidate's timing and metadata so that comparison is possible.

## How it works

```text
Synthetic source / Recording (SER, AVI) / Cameras (ZWO, QHY, Player One, ...)
                              |
                       Frame acquisition
                 (a published frame is read-only
                  and shared, never copied)
                              |
              +---------------+---------------+
              |                               |
       RAM ring buffer                 Event detection
       (recent raw frames)        (on every frame, always)
              |                               |
              +---------------+---------------+
                              |
                       Event capture
                              |
                 Pre- and post-event frames
                     + metadata + log
                              |
                 Writer (separate thread)
                              |
             events/<UTC>/  and  candidates.txt

        Qt GUI: live preview with stretch, controls,
                status, and later an event browser

        Guide thread: feature lock, drift readout,
                and guide pulses to the mount
```

Acquisition never waits on the disk, the GUI, or the mount. The preview and the guider may skip frames; detection and recording may not.

## Evidence rules in brief

The full list is in [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md#4-evidence-rules). In short:

- Raw pixels are never altered between the camera and the evidence file. Stretching is for the screen only.
- Samples are stored as delivered, with their bit depth recorded. Evidence is always lossless.
- Every frame keeps its index and UTC time, and dropped frames are recorded.
- The detector never stops looking. Every candidate is logged, even one that could not be saved.
- Injected test signals are marked. Written clips are never rewritten.
- The telescope is moved only by guide pulses, and every pulse is recorded.

## Technology stack

| Area | Direction |
| --- | --- |
| Language | C++20 |
| User interface | Qt 6 Widgets (LGPLv3, dynamically linked) |
| Platform | Windows first |
| IDE / compiler | Visual Studio 2026 / MSVC |
| Build system | CMake + Ninja |
| Testing | Qt Test + CTest |
| Evidence format | SER, read and written by SeleneWatch; PNG for preview images |
| Recorded input | SER and uncompressed AVI, both read by SeleneWatch |
| Cameras | Native vendor SDKs, one adapter per camera family, each optional at build time. ZWO ASI first, then QHYCCD; Player One, ToupTek-based, and industrial cameras to follow. |
| Mount | ASCOM through late-bound COM, guide pulses only, optional at build time; a simulated mount for tests |
| Image processing | Plain C++ for the trigger; OpenCV only if registration or tracking needs it |

The processing core does not depend on Qt UI classes, so it can be tested and measured without a window.

## Getting started (development)

1. Install **Visual Studio** with the **Desktop development with C++** workload and CMake support.
2. Install a **Qt 6** build for your MSVC toolchain, including **Qt Widgets**.
3. Point `QTDIR` in `CMakeUserPresets.json` at that Qt kit. Machine-specific paths and commands are in [local environment.txt](local%20environment.txt).
4. Open the project in Visual Studio, select **Debug-x64**, configure, and build.
5. Run **SeleneWatch** from the IDE.

With the same Qt `PATH` and `QT_PLUGIN_PATH` used to launch the app, `ctest --test-dir out/build/debug --output-on-failure` runs the core, SER, detection, preview, and window tests. The window tests use Qt's offscreen platform and an isolated settings namespace.

Camera SDKs are not part of the repository. Each camera adapter is built only when its SDK is installed and enabled in CMake. Without any SDK, the app still builds and runs on synthetic and recorded sources.

To try a test flash, start a source, click the preview to select a pixel, set **FWHM**, **Peak**, and **Duration** under **Test flash**, then press **Inject blip**. Peak is an addition in native sample units (ADU), including for 12-bit data stored in a 16-bit word. The readout also gives the requested peak in local noise units. A saved clip marks each injected frame and records its settings. The synthetic source's **Fixed flash** remains available as a capture check.

Use the **Detection** dock to adjust K (sigma), minimum and maximum patch size, and whether one frame may trigger. Changes reach the next frame; Stop and closing the app remember the values. Green slider marks show the current measurement and amber marks the recent maximum. The history covers ten seconds of source time. **Sigma map** shows positive changes relative to local noise, with pixels at or above K in red; **Noise map** shows the noise in ADU. The overlays affect only the display. Closing the dock leaves detection running; reopen it through **View → Detection**.

Start by measuring a recording with no injection, then lower the injected amplitude until recovery stops. A peak above K alone is insufficient: enough neighbouring pixels must also pass. Saturation and motion at bright edges can limit recovery. The defaults (6 sigma, 8–400 pixels) are starting values, not a calibrated observing prescription. Each event folder contains `event.ser`, `event.txt`, `01_baseline.png`, `02_event_peak.png`, and `03_diff_annotated.png`; the PNGs are display previews. A final `complete: yes` line identifies a finished clip. The candidate log records a pending entry before the writer starts the clip and its outcome afterwards; open clips and queued work still in RAM can be lost in a process crash.

## Roadmap

| Stage | Content | Feature | State |
| --- | --- | --- | --- |
| 1 | Bootstrap: Qt Widgets / CMake application, Git, conventions | — | Done |
| 2 | Preview and synthetic source | Feature-2 | Done |
| 3 | RAM ring buffer | Feature-3 | Done |
| 4 | First detector and event capture | Feature-4 | Done |
| 5 | An evidence-safe core: shared frames, detection on every frame, candidate log, lossless file path | Feature-7 | Done |
| 6 | Recordings in, SER out: SER and AVI playback (whole or looped), 16-bit frames, test injection, SER clips | Feature-6 | Done |
| 7 | A measured trigger: per-pixel noise, controls, overlays, and recording calibration | Feature-9 | Implemented; field calibration partial |
| 8 | Seeing the faint side: display stretch, ring sized in seconds | Feature-5 | Proposed |
| 9 | First light: ZWO and QHY camera adapters, manual save | Feature-10 | Proposed |
| 10 | A stable field: feature lock, drift readout, mount guiding (simulated mount, then ASCOM) | Feature-8 | Proposed |
| 11 | The full session record: continuous recording, time model, complete clip metadata | — | Planned |
| 12 | More camera families; registration, transit detection, event browser, export | — | Planned |

Feature details are in [features.md](features.md) (open) and [implemented features.md](implemented%20features.md) (history). The order may change with prototype results and available hardware.

## Scope

**In scope:** lunar image acquisition from multiple camera families, software registration where needed, candidate detection, buffering, lossless recording, diagnostics, review, and holding the field on a locked lunar feature with small guide pulses.

**Not in scope:** pointing the telescope. SeleneWatch does not slew, sync, park, or change the mount's tracking; the observer puts the Moon in the field, and guiding only keeps it there.

## Architecture is expected to evolve

The architecture is a **working proposal, not a fixed specification**. Modules, threading, buffering, GUI structure, algorithms, dependencies, and storage formats may change as development progresses. The evidence rules change only by an explicit, recorded decision. Everything else is driven by **reliable evidence capture, simplicity, maintainability, testability, and measured performance**.

For goals, evidence rules, the time model, component responsibilities, and open decisions, see [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md).

---

*SeleneWatch is an early-stage project. Feedback, experiments, and design revisions are part of the development process.*
