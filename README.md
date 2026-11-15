# SeleneWatch

**Continuous lunar monitoring and automatic capture of transient phenomena.**

SeleneWatch is a native C++/Qt desktop application being developed to monitor the Moon through a telescope-mounted camera, identify potentially interesting short-lived events, and preserve the surrounding frames for later review.

The main motivation is to help observers detect and document **lunar transient phenomena**, particularly brief flashes that may be caused by meteoroid impacts. The design also covers transits, occultations, moving objects, and other unexpected events.

> **ZWO and QHY acquisition and manual save are implemented. QHY camera and lunar observing checks are pending.** Synthetic streams, SER/uncompressed AVI recordings and optional ZWO and QHY cameras use the same detector, time-based history, shared memory budget and asynchronous SER writer. Clips retain native pixels, timestamps, test marks, settings and review previews. The ASI120MM Mini passed RAW8/RAW16 bench checks. [ZWO validation](docs/zwo-validation.md)

The detector keeps learning outside active patches and records separate candidates during ongoing activity. **Save now** preserves the current history and following interval as a manual clip. Observing calibration and the earthshine display check remain partial. Registration remains experimental. [Reference and rearming](docs/reference-and-manual-validation.md), [observing calibration](docs/feature-9-validation.md), [earthshine display](docs/feature-5-validation.md), [registration experiment](docs/registration-experiment.md)

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

With the same Qt `PATH` and `QT_PLUGIN_PATH` used to launch the app, `ctest --test-dir out/build/debug --output-on-failure` runs the core, SER, detection, preview, display/capture, and window tests. The window tests use Qt's offscreen platform and an isolated settings namespace.

Camera SDKs stay outside the repository. Each adapter is optional in CMake: without an SDK path the application builds and runs on synthetic and recorded sources. On the development machine the **Debug-x64** and **Release-x64** presets set both SDK paths (the hidden `Camera-SDKs` preset in `CMakeUserPresets.json`), so a normal build from Visual Studio includes the ZWO and QHY adapters. On another machine, change those two paths, or remove `Camera-SDKs` from the presets' `inherits` to build without cameras.

The manual [camera soak tool](docs/camera-soak.md) saves periodic clips, flushes progress/memory/drop diagnostics, and stops at configured storage thresholds. The Mini completed [30 minutes in each raw format](docs/zwo-soak-validation.md), with zero SDK drops/refusals and all 420 sampled pixel hashes matched. These are bench checks; lunar observing acceptance remains open.

For a ZWO-only build outside the presets on Windows x64, configure a separate build after loading the MSVC/Qt environment:

```powershell
cmake -S . -B out/build/zwo-release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="$env:QTDIR" -DSELENEWATCH_ZWO_SDK_DIR="C:/dev/tools/SDKs/ASI SDK"
cmake --build out/build/zwo-release
ctest --test-dir out/build/zwo-release --output-on-failure
.\out\build\zwo-release\SeleneWatch.exe
```

Start with **Start preview**: frames reach the history, display and detector, while automatic capture, Save now and the candidate record are off. The Detection panel shows peak sigma relative to K, patch size/compactness limits, noise, persistence and **Would trigger** feedback. **Start monitoring** enables automatic event clips and Save now; **Stop monitoring** finishes the available clip and returns to Preview while acquisition continues. Queued evidence writes finish in the background. The banner always shows whether capture is ON or OFF. See [Preview and Monitoring](docs/preview-monitoring.md) for boundaries and validation.

Select **ZWO ASI camera** under Source. The Camera panel discovers devices and shows their reported controls beside the Detection panel. In Preview, exposure (ms), gain, offset, USB bandwidth and high-speed mode apply live when supported and writable; actual readbacks remain visible separately from requested values. Format, binning and ROI use **Apply format / ROI and restart Preview**. Camera settings are fixed during Monitoring; display and detection controls remain editable. Starting Monitoring reopens the camera and learns a new reference, excluding buffered tuning frames and history with uncertain settings. **Stop stream** closes the camera. The status bar shows SDK-reported drops; UTC is host arrival time, and drop locations are approximate because the SDK buffers frames. RAW16 keeps all delivered bits; the sensor's advertised ADC depth is separate metadata. The SDK DLL and its notice are copied into the build directory; release packaging must also satisfy [dependency notices](THIRD_PARTY_LICENSES.md).

For QHY on Windows x64, set `SELENEWATCH_QHY_SDK_DIR` to the vendor SDK root when configuring the build. Install QHY's Windows All-In-One driver and SDK, then select **QHY camera** under Source. SeleneWatch discovers the installed x64 DLL on demand. **Choose QHY DLL…** saves a path for a custom installation. **Use automatic discovery** clears that override. QHY supports live tuning in Preview and fixes acquisition settings during Monitoring. The adapter uses bin 1 and reports SDK losses as unknown. [Build and validation instructions](docs/qhy-development.md), [QHY downloads](https://www.qhyccd.com/download/)

To try a test flash, start Preview, click the image to select a pixel, set **FWHM**, **Peak**, and **Duration** under **Test flash**, then press **Inject blip**. Preview shows whether it would trigger; start Monitoring to save test events. Peak is an addition in native sample units (ADU), including for 12-bit data stored in a 16-bit word. The readout also gives the requested peak in local noise units. A saved clip marks each injected frame and records its settings. The synthetic source's **Fixed flash** remains available as a capture check.

Use **View → Detection** to adjust K (sigma), patch size limits, maximum box-to-patch area ratio, and one- or two-frame confirmation. Expand **Background and noise** for the reference learning time and minimum noise in native ADU. Sliders make broad changes and the number fields allow precise values. The panel reports the selected patch's measured values and the reason it failed a check. Changes reach the next frame, and the app remembers the values on Stop or close. Green marks show current measurements and amber marks show recent highs. The sigma history covers ten seconds of source time. **Sigma map** highlights pixels at or above K in red. **Noise map** shows the local noise in ADU.

Start by measuring a recording with no injection, then lower the injected amplitude until recovery stops. A peak above K alone is insufficient: enough neighbouring pixels must also pass. Saturation and motion at bright edges can limit recovery. The defaults (6 sigma, 8–400 pixels) are starting values, not a calibrated observing prescription. Each event folder contains `event.ser`, `event.txt`, `01_baseline.png`, `02_event_peak.png`, and `03_diff_annotated.png`; the PNGs are display previews. A final `complete: yes` line identifies a finished clip. The candidate log records a pending entry before the writer starts the clip and its outcome afterwards; open clips and queued work still in RAM can be lost in a process crash.

**View → Histogram** opens a plot of the original samples below the live or recorded preview. It shows the observed ADU range and the percentages at zero and at the highest code in the stream's declared range. **Fit range** zooms to the observed values. **Log counts** shows bins with few samples. The graph uses native samples before stretch, inversion or colour conversion. For colour frames it combines the raw Bayer samples or the three stored RGB channels.

The **Display and capture** tab offers Linear, Black/white levels, and Auto-stretch, plus inversion and Bayer colour. Level controls use native ADU and work while stopped. Choose pre-event history, quiet tail, maximum duration after a trigger, and the image/analysis RAM ceiling before Start. Defaults are 5 s, 3 s, 10 s, and 2 GiB. Playback speed does not change these source-time windows. The effective history may be shorter under the byte allowance, and clip metadata records why collection ended. The readout includes shared pixels, detector buffers, maps, and previews; Qt caches, container metadata, and codec workspaces are additional to that ceiling. A full writer logs refused work, and a required allocation failure stops acquisition visibly while preserving the available clip.

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
| 8 | Seeing the faint side: display stretch, time windows, shared image/analysis RAM limit | Feature-5 | Implemented; real-earthshine display check pending |
| 9 | First light: ZWO and QHY camera adapters, manual save | Feature-10 | Software implemented. QHY hardware and observing checks pending. |
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
