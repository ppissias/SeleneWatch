# SeleneWatch — Project Description and Architecture

> **Status:** Working architecture, revised November 2026 after Feature-4. It is not a frozen specification. Feature-level detail lives in [features.md](features.md) (open proposals) and [implemented features.md](implemented%20features.md) (history). This document holds the goals, the rules every feature must respect, and the overall structure. Where a feature conflicts with a rule here, either the feature changes or the rule is revised on purpose and the reason is written down.

## 1. Project Overview

**SeleneWatch** is a native desktop application for continuously observing the Moon using a camera attached to a telescope. Its goal is to help observers discover, capture, review, and document short-lived phenomena that would otherwise be easy to miss.

The application monitors a high-resolution, high-frame-rate image stream, analyses incoming frames in near real time, and automatically preserves the evidence around interesting events. The primary scientific motivation is the observation of **lunar transient phenomena**, especially brief flashes that may be caused by meteoroid impacts. The system should also be useful for recording other notable events, such as transits, occultations, unexpected moving objects, or image anomalies.

SeleneWatch aims to make prolonged observation practical: users start a session, let the system monitor continuously, and later inspect a manageable collection of candidate events rather than manually reviewing an entire recording.

## 2. Main Goals

- **Continuous acquisition:** Receive frames from cameras of many vendors, at the highest practical resolution and frame rate for the selected hardware and configuration.
- **Real-time monitoring:** Analyse every frame, with detection designed to keep pace with acquisition.
- **RAM-based history:** Keep a configurable ring buffer of recent frames in memory, so frames from *before* a detection are still available.
- **Event-triggered recording:** When a candidate is detected, preserve frames from before the trigger and continue for a configurable post-event interval.
- **Lossless evidence:** Write pixel values exactly as the source delivered them, at their native bit depth (8, 12, or 16 bits).
- **Timing fit for comparison:** Give every frame an index and a UTC time with a documented accuracy, so a candidate can be compared with another observer's record.
- **Measured sensitivity:** Inject known test signals into real data so the faintest detectable event of a session is a measured number, not a guess.
- **Multiple event types:** Start with flash detection and keep a path toward transits, occultations, and moving objects.
- **Reviewable evidence:** Save each candidate with the metadata needed to inspect, judge, and share it.
- **Usable desktop interface:** Live preview with display stretch, camera and session controls, detection status, and later an event browser.
- **Testability without hardware:** Run the same pipeline on synthetic streams and on recorded SER or AVI files.

An automatic detection is a **candidate**, not a verified lunar event. Confirming an impact flash normally requires an independent second observation (another telescope or another site) at the same time and place on the Moon. SeleneWatch's job is to make sure nothing real is lost and that each candidate carries enough information for that comparison.

## 3. Events of Interest and What They Imply

| Event | Typical duration | Frames at 25–60 fps | Predictable? | How it is captured |
| --- | --- | --- | --- | --- |
| Lunar impact flash | Tens of ms; rare bright ones longer | Often 1–3, rarely more than 5 | No | Triggered clip |
| Meteor in Earth's atmosphere crossing the field | 0.1–1 s | 3–60 | No | Triggered clip; also a false-positive class for flashes |
| Satellite or ISS transit | 0.3–3 s (ISS about 0.5 s near zenith) | 10–180 | Yes, from orbital predictions | Triggered clip or continuous recording |
| Aircraft, birds, bats | 0.1–5 s | 3–300 | No | Triggered clip; a false-positive class for flashes |
| Lunar occultation of a star (disappearance or reappearance) | The step itself is tens of ms; a baseline of seconds is needed around it | Hundreds to thousands | Yes | Continuous recording around the predicted time |
| Grazing occultation, planetary occultation | 1–3 minutes | Thousands | Yes | Continuous recording |

Consequences for the design:

1. **A triggered clip covers events up to a few seconds.** A transit is detected after it has entered the field, so the pre-trigger history must be seconds, not frames. Starting point: 2–5 s before and at least 3 s after the trigger. The ring buffer is therefore sized in seconds and capped by a RAM limit (Feature-5).
2. **A flash can occupy a single frame.** The trigger must be able to fire on one frame. A missed flash is not recoverable; an extra candidate costs one review (Feature-6).
3. **Predicted events are recorded on demand, not by trigger.** Minutes of data do not belong in a RAM ring. A continuous recording mode streams straight to disk (Section 7).
4. **The observer is also a detector.** A manual "save now" that keeps the current ring is the cheapest way to preserve something the observer saw and the detector did not.

## 4. Evidence Rules

These rules protect the scientific value of what SeleneWatch records. Every feature must respect them. Breaking one needs an explicit, recorded decision.

1. **Raw pixels are not altered between the source and the evidence file.** Display stretch, debayering, registration, and detection work on copies or on derived data. The only exception is an injected test signal (rule 6).
2. **Samples are stored as delivered.** No rescaling and no truncation to 8 bits. A frame records its storage format and its number of significant bits. Some cameras deliver 12-bit data shifted into the top bits of a 16-bit word (ZWO's RAW16 does this); others put it in the low bits. The frame says which. A colour sensor's raw Bayer mosaic is stored as raw data with its pattern, and is debayered only for display.
3. **Evidence formats are lossless, and evidence is not written through the preview path.** SER holds clips and recordings. PNG (8- or 16-bit) is lossless and is acceptable for a few preview images and for today's Mono8 clips. JPEG and video codecs never hold evidence. The conversion that writes a file is separate from the conversion that feeds the screen. A preview shift or stretch must never be the code path that writes a frame to disk. (Today `frameToQImage` serves both. Feature-7 splits it before the first frame wider than 8 bits is written.)
4. **Every frame carries its index and its time.** The index comes from the source (the camera's frame counter where one exists). Each frame has a UTC time and a monotonic time. A gap in the index is a dropped frame, and gaps inside a clip are written into its metadata. A flash can fall into a gap; the record must say so.
5. **The detector never stops looking.** While a clip is being collected or written, frames are still analysed. A second candidate either extends the open clip or starts a new one. If memory or the writer cannot take it, it is still logged with its frame index, time, and measured values. Every candidate, saved or not, appears in the session's candidate log, so the record shows what was seen and what was kept. (Feature-4 skips detection while a clip is busy. Feature-7 fixes that.)
6. **Test signals are marked.** Any frame changed by an injected signal is marked, per frame, and the clip says it contains a test. A clip without that mark is data.
7. **A written clip is not rewritten.** Review decisions (kept, discarded, classified) are stored beside the clip, not inside it. A write ends with an explicit completion marker, so a clip without one is known to be partial.
8. **A clip describes itself.** Each clip records at least:
   - software version (Git commit) and source (camera model and serial, or file path);
   - frame size, storage format, significant bits, Bayer pattern if any;
   - exposure, gain, offset, binning, ROI, and frame-rate setting, where the source reports them;
   - per frame: index, UTC time, and any test mark; and dropped-frame gaps;
   - what the time means and where it came from (Section 5);
   - detector name, the parameters in force, the measured values that fired, and the onset and trigger frames;
   - observer site (latitude, longitude, height) and instrument, once they are configured.

## 5. Time Model

A candidate impact flash is only useful if its time can be compared with someone else's. Timing therefore needs a written meaning, not just a number.

- **Two clocks per frame.** `Frame` already carries `timestamp` (UTC from the system clock) and `steadyTimestamp` (monotonic). UTC is for the record. The monotonic clock measures intervals: frame jitter, and dropped frames when a source has no counter.
- **Each source defines what its time means.** For a camera, the host's arrival time is not the start of the exposure; readout and transfer add a delay that is roughly constant for one configuration. The source documents which instant it reports, and the clip records it. For a recording, the frame time is the time stored in the file, not the time it was played back. For the synthetic source, it is the generation time.
- **Absolute accuracy depends on the PC clock.** Windows' default time synchronisation is not designed for millisecond accuracy, and an unsynchronised clock can drift far further. SeleneWatch does not set the clock. It records what it can find out about the clock's synchronisation and the frame-interval jitter it measures, so a reader knows how far to trust the time.
- **A precise time reference is a later option.** A GPS reference gives millisecond-level absolute time, which multi-site confirmation of short flashes ultimately needs. It can come from a PPS signal, a GPS-timed video inserter, or a camera with a built-in GPS receiver that stamps each exposure (QHY's GPS models do this). A camera adapter that has such a time reports it as the frame time and says so (see Camera support). It is not part of the current plan, but nothing in the time model should prevent it.

## 6. Architecture

The application is a **processing core** with a **Qt Widgets interface**. The core (sources, ring, detection, clip assembly, writing) does not depend on Qt UI classes and can be tested without a window.

```text
  SyntheticFrameSource   Recording (SER / AVI)   Cameras (ZWO, QHY, Player One, ...)
            |                      |                          |
            +----------------------+--------------------------+
                                   |
                      frame source (one narrow type,
                      introduced with Feature-6)
                                   |  source thread
                                   v
                             EventCapture
             +---------------------+----------------------+
             |                     |                      |
      FrameRingBuffer        FlashDetector         clips in progress
     (recent raw frames)  (reference, noise,              |
                           trigger rules)                 |
                                                          v
                                                     EventWriter
                                                   (writer thread)
                                                          |
                                         events/<UTC stamp>/, candidates.txt

  GUI thread: MainWindow takes the latest frame for preview (may drop
  frames), shows counters and status, and holds the controls.
```

### Camera support

SeleneWatch is meant to work with many cameras, not one vendor. Each camera family is an **adapter** behind the same frame source type. Nothing outside an adapter depends on a vendor's types or behaviour.

- **Native SDKs.** Planetary and lunar cameras are driven through their vendors' SDKs, because those are the paths that deliver high frame rates. Generic still-image interfaces such as ASCOM are built around single exposures and are not suited to high-rate video. Likely families include ZWO ASI, QHYCCD, Player One, ToupTek-based cameras (sold under several brands), and industrial cameras (for example Basler or FLIR through their SDKs or GenICam). ZWO is the first (Feature-6); the order of the rest follows the hardware observers actually use.
- **Optional at build time.** Each adapter is built only when its SDK is present, behind its own CMake switch. Without any SDK the application still builds and runs on synthetic and recorded sources. SDKs are not copied into the repository. Each SDK's license and redistribution terms are checked and recorded in `THIRD_PARTY_LICENSES.md` before its adapter is added.
- **Adapters report what the camera can do.** Supported formats and bit depths, where the significant bits sit in a 16-bit word, whether there is a hardware frame counter, and what the frame time means (host arrival, exposure start, or a GPS-stamped exposure). Evidence rules 2 and 4 depend on those answers, so an adapter that cannot give one says so rather than guessing.
- **Adapters share behaviour, not code for its own sake.** A common base or helper appears when two adapters need the same logic.

### Threads and ownership

| Thread | Owns and runs | Must never |
| --- | --- | --- |
| Source thread | Acquisition, ring push, detection, clip assembly (`EventCapture`) | Wait on disk or on the GUI |
| Writer thread | All disk writes: finished clips and the candidate log | Touch the ring |
| GUI thread | Preview conversion, controls, status | Sit on the frame path; the preview drops frames instead |

Each piece of mutable state has one owner. `EventCapture` is touched by the window only while the source thread is joined or not yet started.

### Overload policy

- The ring takes every frame.
- Playback from a file analyses every frame; it may run slower than real time, never skip.
- Live, if detection cannot keep pace, the shortfall is counted and shown. Frames are still buffered and still recorded. Silent skipping is not allowed.
- If detection becomes too expensive for the source thread, it moves to its own thread behind a bounded queue. That move happens when a measurement shows the need, not before.

### Scaling the evidence path

Feature-4 copies the whole ring when a candidate fires. At 640×480 Mono8 that is 46 MB, and it is fine. At camera sizes it is not:

| Frame and rate | Raw data rate | 5 s of history |
| --- | --- | --- |
| 640×480 Mono8, 30 fps | 9.2 MB/s | 46 MB |
| 1920×1080 Mono8, 60 fps | 124 MB/s | 0.62 GB |
| 1920×1080 Mono16, 60 fps | 249 MB/s | 1.24 GB |
| 3840×2160 Mono16, 30 fps | 498 MB/s | 2.49 GB |

Copying 1.2 GB doubles the memory and holds the ring lock for the whole copy, which stalls acquisition. Before a camera at that size is monitored, the trigger path must stop duplicating the ring: for example, pre-trigger frames handed to the writer in order while their slots are protected from overwrite. The choice is made by measurement, together with the SER writer (Feature-5).

The same table bounds continuous recording. The upper rows need an SSD. The writer measures and shows its write rate. When the disk falls behind or fills up, recording stops cleanly and says so; it never corrupts a file or drops frames silently.

## 7. Recording Modes

| Mode | Started by | Contents | Output |
| --- | --- | --- | --- |
| Triggered clip | The detector | Pre-trigger ring history plus post-trigger frames | `events/<UTC stamp>/` |
| Manual save | The observer ("save now") | Current ring plus a short post interval | `events/<UTC stamp>/`, marked manual |
| Continuous recording | The observer (start/stop), for predicted events | Every frame until stopped | One SER file per recording |

Detection keeps running during a continuous recording. A candidate found during it is still written as a clip and also refers to the recording's frame index.

Alongside the clips, `events/candidates.txt` keeps one line per candidate for the session, saved or not, with its time, frames, measured values, and either its clip folder or the reason it was not saved (Evidence rule 5). Clips can overlap in time; that is accepted, because losing frames is worse than storing some twice.

Output goes next to the executable for now (a Feature-4 decision). A configurable location comes when there is a settings page for other reasons.

## 8. Intended Runtime Workflow

1. The observer selects a source (camera, recording, or synthetic) and configures the session: exposure, gain, ROI, trigger parameters, site.
2. SeleneWatch starts acquisition and keeps the latest frames in the ring.
3. The detector analyses every frame and shows its noise, peak, and pass/fail state live.
4. Optionally, the observer injects a test signal to measure the session's faint limit.
5. A candidate starts a clip; the clip is collected and written without stopping acquisition or detection.
6. Predicted events are captured with continuous recording; anything the observer sees is kept with manual save.
7. Afterwards, the observer reviews, classifies, keeps, discards, or exports candidates.

## 9. Technology Choices

| Area | Choice | Notes |
| --- | --- | --- |
| Language | **C++20** | Native performance, memory control, camera SDK access. |
| Desktop GUI | **Qt 6 Widgets** | LGPLv3, linked dynamically. Qt 6 is required; there is no Qt 5 path. |
| Development environment | **Visual Studio 2026 + MSVC** | Built with `/W4 /permissive- /utf-8`. |
| Build system | **CMake + Ninja** | Presets in `CMakeUserPresets.json`. |
| Testing | **Qt Test + CTest** | `SeleneWatchTests`; synthetic frames and written-then-read files instead of hardware. |
| Image processing | **Plain C++ loops** | The flash trigger is a few passes over the pixels. OpenCV (Apache-2.0) is taken only when registration or tracking needs it, as argued in Feature-6. |
| Evidence files | **SER, written and read by SeleneWatch** | A small, documented format; no dependency needed. PNG through Qt for preview images. |
| Recorded input | **SER (own reader), AVI via Windows Media Foundation** | Media Foundation ships with Windows. |
| Cameras | **Native vendor SDKs, one adapter per family** | ZWO ASI first (ASICamera2 SDK), then other families. SDKs are closed or vendor-licensed, are not stored in the repository, and are optional at build time (Section 6, Camera support). |
| Version control | **Git** | |

### SER details that affect correctness

- The header is 178 bytes, followed by the frames and an optional trailer of one 64-bit time per frame.
- Trailer times are UTC in 100 ns ticks since 0001-01-01 (the .NET epoch). This is not Windows `FILETIME`, which counts from 1601.
- The header's `LittleEndian` field is used inverted in practice: capture software writes 0 for little-endian data, and SER Player, PIPP, and Siril read it that way, although the specification's wording says the opposite. SeleneWatch writes 0 for its little-endian data and reads files the way those tools do.
- `PixelDepthPerPlane` records significant bits. How the common tools treat 12-bit data stored in the top bits of a 16-bit word must be checked before the writer's convention is fixed (Evidence rule 2).
- Every written file is checked by reading it back in the tests, and by opening it in SER Player, PIPP, or Siril before a release.

## 10. Key Engineering Considerations

- **Throughput and backpressure:** Acquisition, analysis, preview, and writing run at different speeds. Queues are bounded and overload is visible (Section 6).
- **Memory budget:** The ring is sized in seconds and capped by a RAM limit. Allocation happens up front; a warm ring does not allocate per frame.
- **False positives:** Expect noise, hot pixels, cosmic-ray hits on the sensor, satellite glints, meteors, aircraft, birds, seeing, cloud, and tracking drift. The trigger measures noise from the frames themselves; hot pixels are learned; test injection measures what is left.
- **Event integrity:** A trigger must not lose the frames that motivated it, and detection must not go blind while a clip is busy (Evidence rule 5). Tests cover overlapping candidates, long events, write failures, a full disk, and a stop in the middle of a clip.
- **Failure at thread boundaries:** An exception on the source or writer thread is caught at the top of that thread, reported to the window, and ends that thread cleanly. Recovery is not attempted inside the per-frame loop.
- **Extensibility:** New sources and detectors are added without changes to the GUI's frame path. A shared interface is introduced when a second implementation exists, not before.
- **Observability:** Show the acquisition rate, buffer occupancy and memory, detector noise and peak, dropped frames, overload count, writer backlog, and write rate.

No fixed resolution, frame rate, buffer duration, or performance guarantee is promised. Targets are set from measurements on real hardware.

## 11. Code Organization

The sources currently sit flat in the project root, with tests in `tests/`. That is appropriate for the present number of files.

A directory is introduced when several files share one job and the dependency direction is clear. The likely first split is `sources/` when Feature-6 adds the recording source and the first camera adapter, followed by `cameras/` once a second camera family arrives. A plausible later layout:

```text
SeleneWatch/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── ui/          # Qt windows, preview conversion and stretch, controls, review
│   ├── sources/     # Synthetic and recording sources
│   ├── cameras/     # One adapter per camera family, each optional at build time
│   ├── capture/     # Ring buffer, event capture, clip assembly
│   ├── detection/   # Candidate detection algorithms
│   ├── recording/   # SER and metadata writing, continuous recording
│   └── core/        # Frame and other shared plain types
├── tests/
└── docs/
```

Dependencies point one way: `ui` depends on everything below it; nothing below depends on `ui` or on Qt GUI classes.

## 12. Development Stages

| Stage | Content | Feature | State |
| --- | --- | --- | --- |
| 1 | Bootstrap: Qt Widgets / CMake application, Git, conventions | — | Done |
| 2 | Preview and synthetic source, frame-rate diagnostics | Feature-2 | Done |
| 3 | RAM ring buffer and memory readout (the source interface was deferred until a second source existed) | Feature-3 | Done |
| 4 | First detector and event capture | Feature-4 | Done |
| 5 | Evidence safety: detection on every frame, a candidate log, and a file conversion separate from the preview | Feature-7 | Next |
| 6 | Real inputs and a measurable trigger: SER and AVI playback, the first camera adapter (ZWO), Mono16 frames, noise-based trigger, injected test signal | Feature-6 | Proposed |
| 7 | High-bit-depth display and SER evidence: display stretch, ring sized in seconds, SER clips, an evidence path that scales | Feature-5 | Proposed |
| 8 | More camera adapters (QHY, Player One, ToupTek-based, industrial), each behind its own SDK switch | To be written | Planned |
| 9 | Session record and timing: time model, complete clip metadata, manual save, continuous recording | To be written | Planned |
| 10 | Registration, transit detection, event browser, export | To be written | Planned |

Feature-7 comes first because it removes two ways real data could be lost or damaged: the blind period (Evidence rule 5) and evidence written through the preview conversion (Evidence rule 3), which Feature-6's 16-bit frames would otherwise trigger. Stage 8 can start as soon as the frame source type from Feature-6 exists, and can run alongside the others. The order of the remaining stages may change with hardware availability and measurements.

## 13. Open Decisions

- **Time reference:** PC clock with recorded synchronisation state only, or also a GPS reference (including GPS-stamping cameras), and when.
- **Overlapping candidates for long events:** Feature-7 extends the open clip up to a cap and starts another when the cap is reached. Whether transits need a different rule is decided with the transit detector.
- **Evidence path at camera scale:** protected ring slots handed to the writer, or another scheme; decided by measurement.
- **Clip metadata format:** one machine-readable format that is still easy to read by eye (today `event.txt`; Feature-5 proposes JSON).
- **12-bit data in SER output:** as delivered (top-aligned) with `PixelDepthPerPlane = 16`, or another convention, after checking the common tools.
- **Storage location** once there is a settings page.

## 14. Architecture Is Deliberately Revisable

This architecture is a working proposal, not a fixed contract. It will change through prototyping, measurement, observing experience, and scientific and technical feedback. We should be prepared to revise:

- component and directory boundaries;
- threading, queueing, ownership, and ring-buffer strategies;
- image-processing libraries;
- event detection and recording approaches;
- storage formats, metadata, and export mechanisms;
- GUI layout and workflow;
- implementation priorities and supported camera SDKs.

The evidence rules in Section 4 change only by an explicit, recorded decision. Everything else is guided by **reliable evidence capture, simplicity, testability, maintainability, and measured performance**. Record important decisions and their trade-offs in the feature files as the project matures.

---

*SeleneWatch — project description, revised November 2026.*
