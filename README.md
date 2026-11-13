# SeleneWatch

**Continuous lunar monitoring and automatic capture of transient phenomena.**

SeleneWatch is a native C++/Qt desktop application being developed to monitor the Moon through a telescope-mounted camera, identify potentially interesting short-lived events, and preserve the surrounding frames for later review.

The main motivation is to help observers detect and document **lunar transient phenomena**, particularly brief flashes that may be caused by meteoroid impacts. The design also covers transits, occultations, moving objects, and other unexpected events.

> **Project status: Stage 5 done, Feature-6 next.** The app previews a synthetic lunar stream and keeps a 150-frame ring of shared, read-only frames. The detector runs on every frame, also while a clip is being collected or written. On **Inject flash** it writes the surrounding frames as lossless PNGs plus `event.txt` next to the executable, and every candidate, saved or not, gets a line in `events/candidates.txt`. Qt Test covers the frame pool, the ring, the detector, the capture rules, and the writer. Next comes Feature-6: playback of real recordings (SER and AVI) and SER clips. After that come a trigger measured on those recordings, display stretch, the ZWO and QHY camera adapters, and a feature lock with mount guiding.

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

`SeleneWatchTests` runs the automated checks. With the same Qt `PATH` and `QT_PLUGIN_PATH` used to launch the app, `ctest --test-dir out/build/debug` runs them from the project root.

Camera SDKs are not part of the repository. Each camera adapter is built only when its SDK is installed and enabled in CMake. Without any SDK, the app still builds and runs on synthetic and recorded sources.

## Roadmap

| Stage | Content | Feature | State |
| --- | --- | --- | --- |
| 1 | Bootstrap: Qt Widgets / CMake application, Git, conventions | — | Done |
| 2 | Preview and synthetic source | Feature-2 | Done |
| 3 | RAM ring buffer | Feature-3 | Done |
| 4 | First detector and event capture | Feature-4 | Done |
| 5 | An evidence-safe core: shared frames, detection on every frame, candidate log, lossless file path | Feature-7 | Done |
| 6 | Recordings in, SER out: SER and AVI playback, 16-bit frames, SER clips | Feature-6 | Next |
| 7 | A measured trigger: per-pixel noise, test injection, faint limit measured on real recordings | Feature-9 | Proposed |
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
