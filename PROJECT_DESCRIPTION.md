# SeleneWatch — Project Description and Architecture

> **Status:** Working architecture, revised November 2026 after Feature-4, after a design review that settled frame ownership and added mount guiding, and after Feature-7. It is not a frozen specification. Feature-level detail lives in [features.md](features.md) (open proposals) and [implemented features.md](implemented%20features.md) (history). This document holds the goals, the rules every feature must respect, and the overall structure. Where a feature conflicts with a rule here, either the feature changes or the rule is revised on purpose and the reason is written down.

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
- **A stable field:** Lock on a lunar feature, show how far it has drifted, and on request correct the mount with small guide pulses, so a long session stays on its target and the record shows every correction.
- **Usable desktop interface:** Live preview with display stretch, camera and session controls, detection status, and later an event browser.
- **Testability without hardware:** Run the same pipeline on synthetic streams and on recorded SER or AVI files, and close the guiding loop on a simulated mount.

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
2. **A flash can occupy a single frame.** The trigger must be able to fire on one frame. A missed flash is not recoverable; an extra candidate costs one review (Feature-9).
3. **Predicted events are recorded on demand, not by trigger.** Minutes of data do not belong in a RAM ring. A continuous recording mode streams straight to disk (Section 7).
4. **The observer is also a detector.** A manual "save now" that keeps the current ring is the cheapest way to preserve something the observer saw and the detector did not.
5. **The Moon does not stay in the field by itself.** A mount's lunar rate follows the mean motion in right ascension only. The motion in declination reaches about a quarter of an arcsecond per second, half the Moon's diameter in an hour, and mount errors add to it. A session of hours needs the drift measured and corrected, and every correction moves the image under the detector, so corrections are few, slow, and recorded (Feature-8).

## 4. Evidence Rules

These rules protect the scientific value of what SeleneWatch records. Every feature must respect them. Breaking one needs an explicit, recorded decision.

1. **Raw pixels are not altered between the source and the evidence file.** Display stretch, debayering, registration, and detection work on copies or on derived data. The only exception is an injected test signal (rule 6). The code enforces this by ownership: a frame is read-only from the moment it is published (Section 6, Frame ownership).
2. **Samples are stored as delivered.** No rescaling and no truncation to 8 bits. A frame records its storage format and its number of significant bits. Some cameras deliver 12-bit data shifted into the top bits of a 16-bit word (ZWO's RAW16 does this); others put it in the low bits. The frame says which. A colour sensor's raw Bayer mosaic is stored as raw data with its pattern, and is debayered only for display.
3. **Evidence formats are lossless, and evidence is not written through the preview path.** SER holds clips and recordings. PNG (8- or 16-bit) is lossless and is acceptable for a few preview images and for today's Mono8 clips. JPEG and video codecs never hold evidence. The conversion that writes a file is separate from the conversion that feeds the screen. A preview shift or stretch must never be the code path that writes a frame to disk. The window has `frameToPreviewImage`; the writer has its own conversion beside the writer code (Feature-7).
4. **Every frame carries its index and its time.** The index comes from the source (the camera's frame counter where one exists). Each frame has a UTC time and a monotonic time. A gap in the index is a dropped frame, and gaps inside a clip are written into its metadata. A flash can fall into a gap; the record must say so.
5. **The detector never stops looking.** While a clip is being collected or written, frames are still analysed. A second candidate either extends the open clip or starts a new one. If memory or the writer cannot take it, it is still logged with its frame index, time, and measured values. Every candidate, saved or not, appears in the session's candidate log, so the record shows what was seen and what was kept.
6. **Test signals are marked.** Any frame changed by an injected signal is marked, per frame, and the clip says it contains a test. A clip without that mark is data.
7. **A written clip is not rewritten.** Review decisions (kept, discarded, classified) are stored beside the clip, not inside it. A write ends with an explicit completion marker, so a clip without one is known to be partial. A clip folder is never reused, even when the same recording is played twice.
8. **A clip describes itself.** Each clip records at least:
   - software version (Git commit) and source (camera model and serial, or file path);
   - frame size, storage format, significant bits, Bayer pattern if any;
   - exposure, gain, offset, binning, ROI, and frame-rate setting, where the source reports them;
   - per frame: index, UTC time, and any test mark; and dropped-frame gaps;
   - what the time means and where it came from (Section 5);
   - detector name, the parameters in force, the measured values that fired, and the onset and trigger frames;
   - the guiding state, the field offset, and the mount corrections inside the clip (rule 9);
   - observer site (latitude, longitude, height) and instrument, once they are configured.
9. **Mount corrections are recorded.** SeleneWatch moves the telescope only by guide pulses, which end by themselves. Each pulse is logged with its time, direction, and duration. A clip lists the pulses inside its span, and a candidate that fires during a pulse or its settle time says so. Such a candidate is still saved; the mark tells the reviewer that the image was moving. Guiding never changes pixel data.

## 5. Time Model

A candidate impact flash is only useful if its time can be compared with someone else's. Timing therefore needs a written meaning, not just a number.

- **Two clocks per frame.** `Frame` already carries `timestamp` (UTC from the system clock) and `steadyTimestamp` (monotonic). UTC is for the record. The monotonic clock measures intervals: frame jitter, and dropped frames when a source has no counter.
- **Each source defines what its time means.** For a camera, the host's arrival time is not the start of the exposure; readout and transfer add a delay that is roughly constant for one configuration. The source documents which instant it reports, and the clip records it. For a recording, the frame time is the time stored in the file, not the time it was played back. A looped playback is a test mode: it shifts each later lap by the loop's length, and its clips say so (Feature-6). For the synthetic source, it is the generation time.
- **Absolute accuracy depends on the PC clock.** Windows' default time synchronisation is not designed for millisecond accuracy, and an unsynchronised clock can drift far further. SeleneWatch does not set the clock. It records what it can find out about the clock's synchronisation and the frame-interval jitter it measures, so a reader knows how far to trust the time.
- **A precise time reference is a later option.** A GPS reference gives millisecond-level absolute time, which multi-site confirmation of short flashes ultimately needs. It can come from a PPS signal, a GPS-timed video inserter, or a camera with a built-in GPS receiver that stamps each exposure (QHY's GPS models do this). A camera adapter that has such a time reports it as the frame time and says so (see Camera support). It is not part of the current plan: the cameras in hand are not expected to have GPS, and the path cannot be tested without one that does. Nothing in the time model prevents it. Each source already states what its frame time means, and a GPS time would be one more answer to that question.

## 6. Architecture

The application is a **processing core** with a **Qt Widgets interface**. The core (sources, ring, detection, clip assembly, writing, guiding) does not depend on Qt UI classes and can be tested without a window.

```text
  SyntheticFrameSource   Recording (SER, AVI)  Cameras (ZWO, QHY, Player One, ...)
            |                     |                         |
            +---------------------+-------------------------+
                                  |
                     frame source (one narrow type,
                     introduced with Feature-6)
                                  |  source thread: one writable frame
                                  v
                     Session: test signal, then PUBLISH
                     (the frame is read-only from here on)
                                  |
          +-----------------------+---------------------------+
          |                       |                           |
     EventCapture          latest frame, one slot      latest frame, one slot
          |                       |                           |
   +------+----------+            v                           v
   |      |          |        GUI thread                 Guide thread
  ring  detector   clips      preview,                   FeatureLock, Guider
   |                 |        controls, status                |
   |                 v                                        v
   |            EventWriter  <---- guide log lines ----   mount adapter
   |          (writer thread)                           (ASCOM, simulated)
   |                 |
   +-- shared -------+--> events/<UTC stamp>/, candidates.txt, guiding.txt
       frames
```

Until Feature-6 introduces `Session`, `MainWindow` plays that part: its frame callback publishes the frame and calls the capture.

### Frame ownership

One rule covers every frame: **one writer, then many readers.**

- A source takes a writable frame from its `FramePool`, fills it, and hands it on as the only owner.
- The `Session` may add a test signal to it (Evidence rule 6). Then it publishes the frame as a `FramePtr`, a shared pointer to a constant `Frame`. From that point nobody can change it, which is how Evidence rule 1 is held in code and not only by convention.
- The ring, a clip, the preview, and the guider each hold pointers. Collecting a clip copies pointers, not pixels. Clips that overlap share frames.
- When the last pointer is dropped, the pixel buffer returns to the pool. A warm stream allocates no pixel buffer per frame.
- Live memory is the ring, plus the open clip, plus the clips waiting for the writer. Each of the three has its own cap, so the total is bounded and is shown.

This was chosen in Feature-7 over copying the ring at each trigger, and over protecting ring slots from overwrite. Its reasons are recorded there.

### Camera support

SeleneWatch is meant to work with many cameras, not one vendor. Each camera family is an **adapter** behind the same frame source type. Nothing outside an adapter depends on a vendor's types or behaviour.

- **Native SDKs.** Planetary and lunar cameras are driven through their vendors' SDKs, because those are the paths that deliver high frame rates. Generic still-image interfaces such as ASCOM are built around single exposures and are not suited to high-rate video. Likely families include ZWO ASI, QHYCCD, Player One, ToupTek-based cameras (sold under several brands), and industrial cameras (for example Basler or FLIR through their SDKs or GenICam). ZWO is the first and QHYCCD the second (Feature-10), because those are the cameras in hand; the order of the rest follows the hardware observers actually use.
- **Optional at build time.** Each adapter is built only when its SDK is present, behind its own CMake switch. Without any SDK the application still builds and runs on synthetic and recorded sources. SDKs are not copied into the repository. Each SDK's license and redistribution terms are checked and recorded in `THIRD_PARTY_LICENSES.md` before its adapter is added.
- **Adapters report what the camera can do.** Supported formats and bit depths, where the significant bits sit in a 16-bit word, whether there is a hardware frame counter, and what the frame time means (host arrival, exposure start, or a GPS-stamped exposure). Evidence rules 2 and 4 depend on those answers, so an adapter that cannot give one says so rather than guessing.
- **Mono and colour sensors are both supported.** A colour camera's raw Bayer mosaic is stored as it arrives, with its pattern (Evidence rule 2). It is debayered only for the screen. The trigger compares each pixel with its own history and runs on the mosaic unchanged. The feature lock bins a mosaic 2×2 before matching. Nothing else knows whether the sensor is mono or colour.
- **Adapters share behaviour, not code for its own sake.** A common base or helper appears when two adapters need the same logic.

### Threads and ownership

| Thread | Owns and runs | Must never |
| --- | --- | --- |
| Source thread | Acquisition, the test signal, publication, ring push, detection, clip assembly (`EventCapture`) | Wait on disk, on the GUI, or on the mount |
| Writer thread | All disk writes: finished clips, the candidate log, the guide log. Its own bounded queue | Touch the ring; block the thread that submits |
| GUI thread | Preview conversion, controls, status | Sit on the frame path; the preview takes the latest frame and skips the rest |
| Guide thread (Feature-8) | The lock template, shift measurements, the guiding decision, and the mount connection | Sit on the frame path; it takes the latest frame and skips the rest |

Each piece of mutable state has one owner. `EventCapture` is touched by the window only while the source thread is joined or not yet started. Threads meet in three places only: published frames, which are read-only; the one-slot mailboxes for the preview and the guider; and the writer's queue. Results travel to the window as notifications. Nothing on the frame path waits for the window or reads state the window keeps.

### Overload policy

- The ring takes every frame.
- Playback from a file analyses every frame; it may run slower than real time, never skip.
- Live, if detection cannot keep pace, the shortfall is counted and shown. Frames are still buffered and still recorded. Silent skipping is not allowed.
- If detection becomes too expensive for the source thread, it moves to its own thread behind a bounded queue. That move happens when a measurement shows the need, not before.
- The writer's queue is bounded. A clip that does not fit is dropped and its candidates are logged as not saved. The source thread never waits for the writer.
- One clip that fails to write is one failed clip, and the writer goes on. A writer that can no longer write the session's record is a session state the window keeps showing. Detection continues either way.

### Mount guiding

The guider measures where a locked lunar feature is, and corrects the mount when it has drifted out of a tolerance box (Feature-8). Three things constrain it.

- **It disturbs the detector, so it does little.** A correction moves the image under the detector's reference. Corrections are rare, slower than the seeing, and logged. Guiding holds the field; it does not sharpen the image.
- **It moves hardware, so it cannot run away.** Only guide pulses are sent. A pulse ends by itself, so a crash leaves nothing moving. No pulse is sent on a weak match, a lost lock, or a mount that is slewing, parked, or not tracking. Guiding that has no effect stops itself.
- **Each mount family is an adapter**, like a camera. ASCOM is the first, reached through late-bound COM with no code in the repository, and optional at build time. A simulated mount is the second implementation and the test seam.

The measured field shift is also what registration needs later. It is recorded from the start so that the detector's reference can use it when that is built.

### Scaling the evidence path

Until Feature-7 a trigger copied the whole ring. At 640×480 Mono8 that was 46 MB, and it was fine. At camera sizes it would not be:

| Frame and rate | Raw data rate | 5 s of history |
| --- | --- | --- |
| 640×480 Mono8, 30 fps | 9.2 MB/s | 46 MB |
| 1920×1080 Mono8, 60 fps | 124 MB/s | 0.62 GB |
| 1920×1080 Mono16, 60 fps | 249 MB/s | 1.24 GB |
| 3840×2160 Mono16, 30 fps | 498 MB/s | 2.49 GB |

Copying 1.2 GB doubles the memory and holds the ring lock for the whole copy, which stalls acquisition. Feature-7 removed the copy: frames are shared (Frame ownership, above), and a trigger copies pointers.

What remains is the writer's speed. Frames that wait for the writer stay in RAM, so a slow writer turns into memory. PNG encoding of large 16-bit frames is far slower than the data rates in the table. The SER writer of Feature-6 is what makes camera sizes practical, and the share of RAM given to waiting clips is set from its measured rate (Feature-5).

The same table bounds continuous recording. The upper rows need an SSD. The writer measures and shows its write rate. When the disk falls behind or fills up, recording stops cleanly and says so; it never corrupts a file or drops frames silently.

## 7. Recording Modes

| Mode | Started by | Contents | Output |
| --- | --- | --- | --- |
| Triggered clip | The detector | Pre-trigger ring history plus post-trigger frames | `events/<UTC stamp>/` |
| Manual save | The observer ("save now") | Current ring plus a short post interval | `events/<UTC stamp>/`, marked manual |
| Continuous recording | The observer (start/stop), for predicted events | Every frame until stopped | One SER file per recording |

Detection keeps running during a continuous recording. A candidate found during it is still written as a clip and also refers to the recording's frame index.

Alongside the clips, `events/candidates.txt` keeps one line per candidate for the session, saved or not, with its time, frames, measured values, and either its clip folder or the reason it was not saved (Evidence rule 5). Clips can overlap in time; that is accepted, because losing frames is worse than storing some twice. In memory the overlapping frames are shared; on disk they are written twice.

While a feature lock is held, `events/guiding.txt` keeps the measured field offset once a second and every guide pulse sent to the mount (Evidence rule 9).

Output goes next to the executable for now (a Feature-4 decision). A configurable location comes when there is a settings page for other reasons.

## 8. Intended Runtime Workflow

1. The observer selects a source (camera, recording, or synthetic) and configures the session: exposure, gain, ROI, trigger parameters, site.
2. SeleneWatch starts acquisition and keeps the latest frames in the ring.
3. Optionally, the observer locks on a lunar feature and watches the drift readout. With a mount connected and calibrated, guiding can be turned on to hold the field.
4. The detector analyses every frame and shows its noise, peak, and pass/fail state live.
5. Optionally, the observer injects a test signal to measure the session's faint limit.
6. A candidate starts a clip; the clip is collected and written without stopping acquisition or detection.
7. Predicted events are captured with continuous recording; anything the observer sees is kept with manual save.
8. Afterwards, the observer reviews, classifies, keeps, discards, or exports candidates.

## 9. Technology Choices

| Area | Choice | Notes |
| --- | --- | --- |
| Language | **C++20** | Native performance, memory control, camera SDK access. |
| Desktop GUI | **Qt 6 Widgets** | LGPLv3, linked dynamically. Qt 6 is required; there is no Qt 5 path. |
| Development environment | **Visual Studio 2026 + MSVC** | Built with `/W4 /permissive- /utf-8`. |
| Build system | **CMake + Ninja** | Presets in `CMakeUserPresets.json`. |
| Testing | **Qt Test + CTest** | `SeleneWatchTests`; synthetic frames and written-then-read files instead of hardware. |
| Image processing | **Plain C++ loops** | The flash trigger is a few passes over the pixels. OpenCV (Apache-2.0) is taken only when registration or tracking needs it, as argued in Feature-9. |
| Evidence files | **SER, written and read by SeleneWatch** | A small, documented format; no dependency needed. PNG through Qt for preview images. |
| Recorded input | **SER and uncompressed AVI, both with SeleneWatch's own readers** | Uncompressed AVI is lossless and is what planetary capture programs write. A compressed AVI is refused with its codec shown; a decoder through Windows Media Foundation is added only if the observer's files need one (Feature-6). |
| Cameras | **Native vendor SDKs, one adapter per family** | ZWO ASI first (ASICamera2 SDK), then QHYCCD, then other families. SDKs are closed or vendor-licensed, are not stored in the repository, and are optional at build time (Section 6, Camera support). |
| Mount | **ASCOM through late-bound COM, guide pulses only** | The driver is found at run time; no ASCOM code is copied or linked. Optional at build time. A simulated mount serves the tests. ASCOM Alpaca is the fallback if 32-bit in-process drivers prove common (Feature-8). |
| Feature lock | **Plain C++ loops** | Normalised cross-correlation of a small template in a small window, a few times a second. No library is needed for it. |
| Version control | **Git** | |

### SER details that affect correctness

- The header is 178 bytes, followed by the frames and an optional trailer of one 64-bit time per frame.
- Trailer times are UTC in 100 ns ticks since 0001-01-01 (the .NET epoch). This is not Windows `FILETIME`, which counts from 1601.
- The header's `LittleEndian` field is used inverted in practice: capture software writes 0 for little-endian data, and SER Player, PIPP, and Siril read it that way, although the specification's wording says the opposite. SeleneWatch writes 0 for its little-endian data and reads files the way those tools do.
- `PixelDepthPerPlane` is specified as the number of significant bits, but it cannot be relied on. The observer's FireCapture recordings store 12-bit data in the top bits of the word with `PixelDepthPerPlane` set to 16. The reader therefore inspects the low bits of the first frames of a 16-bit file and records what it finds. The writer's convention for 12-bit data is fixed after a SharpCap file and a file from each of the observer's cameras have been checked the same way (Evidence rule 2).
- Checked on the observer's own files (FireCapture 2.x, November 2026): `LittleEndian` is 0 and the data is little-endian, which confirms the inverted use; every file has a trailer in UT; and the header's local `DateTime` also holds UT, so only `DateTime_UTC` and the trailer are used for time.
- "12-bit data in the top bits" is nearly but not exactly true of the observer's 16-bit file. In about half its frames one sample has bit 3 set, a half-step between two 12-bit levels, probably from the camera's defect-pixel correction. The reader therefore records 13 significant bits. It records the bits the data uses, never fewer, so no later step can drop a bit that carries data (Feature-6, Part A).
- Every written file is checked by reading it back in the tests, and by opening it in SER Player, PIPP, or Siril before a release.

## 10. Key Engineering Considerations

- **Throughput and backpressure:** Acquisition, analysis, preview, and writing run at different speeds. Queues are bounded and overload is visible (Section 6).
- **Memory budget:** The ring is sized in seconds, and one RAM limit covers every live frame: the ring, the open clip, and the clips waiting for the writer. Pixel buffers are recycled through a pool; a warm stream does not allocate one per frame.
- **False positives:** Expect noise, hot pixels, cosmic-ray hits on the sensor, satellite glints, meteors, aircraft, birds, seeing, cloud, tracking drift, and the mount corrections themselves. The trigger measures noise per pixel from the frames themselves, because the sunlit limb and the earthshine differ by far more than any single threshold allows; test injection measures what is left.
- **Event integrity:** A trigger must not lose the frames that motivated it, and detection must not go blind while a clip is busy (Evidence rule 5). Tests cover overlapping candidates, long events, write failures, a full disk, and a stop in the middle of a clip.
- **Failure at thread boundaries:** An exception on the source thread is caught at the top of that thread, reported to the window, and ends that thread cleanly. Recovery is not attempted inside the per-frame loop. The writer catches per clip, so one bad clip does not end the session's record (Feature-7). A mount call that hangs stalls the guide thread only.
- **Mount safety:** Guiding is off until the observer turns it on. Only bounded guide pulses are sent, only on a trusted measurement, and only to a mount that reports it is tracking. Guiding that stops for a safety reason is not resumed without the observer (Feature-8).
- **Extensibility:** New sources and detectors are added without changes to the GUI's frame path. A shared interface is introduced when a second implementation exists, not before.
- **Observability:** Show the acquisition rate, buffer occupancy and the memory of all live frames, detector noise and peak, dropped frames, overload count, writer backlog, write rate, and the lock score, field offset, and guiding state.

No fixed resolution, frame rate, buffer duration, or performance guarantee is promised. Targets are set from measurements on real hardware.

## 11. Code Organization

The sources currently sit flat in the project root, with tests in `tests/`. That is appropriate for the present number of files.

A directory is introduced when several files share one job and the dependency direction is clear. The likely first split is `sources/` and `recording/` when Feature-6 adds the recording sources and the SER writer, followed by `cameras/` when Feature-10 adds the two camera adapters. A plausible later layout:

```text
SeleneWatch/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── ui/          # Qt windows, preview conversion and stretch, controls, review
│   ├── session/     # Session: the frame path from source to capture, preview, and guider
│   ├── sources/     # Synthetic and recording sources
│   ├── cameras/     # One adapter per camera family, each optional at build time
│   ├── capture/     # Ring buffer, event capture, clip assembly
│   ├── detection/   # Candidate detection algorithms
│   ├── guiding/     # Feature lock, guider, calibration
│   ├── mounts/      # One adapter per mount family, and the simulated mount
│   ├── recording/   # SER and metadata writing, continuous recording, session logs
│   └── core/        # Frame, FramePool, and other shared plain types
├── tests/
└── docs/
```

Dependencies point one way: `ui` depends on everything below it; nothing below depends on `ui` or on Qt GUI classes. `guiding` depends on `mounts` and `core`, and not on `capture` or `detection`; the detector and the capture do not know that a mount exists.

When the first directory is split off, everything below `ui` becomes one static library that the application and the tests both link. The build then enforces the boundary, and the tests stop listing the core sources a second time.

## 12. Development Stages

| Stage | Content | Feature | State |
| --- | --- | --- | --- |
| 1 | Bootstrap: Qt Widgets / CMake application, Git, conventions | — | Done |
| 2 | Preview and synthetic source, frame-rate diagnostics | Feature-2 | Done |
| 3 | RAM ring buffer and memory readout (the source interface was deferred until a second source existed) | Feature-3 | Done |
| 4 | First detector and event capture | Feature-4 | Done |
| 5 | **An evidence-safe core.** Shared frames, detection on every frame, a writer that owns its backlog, a candidate log, and a file conversion separate from the preview | Feature-7 | Done |
| 6 | **Recordings in, SER out.** The source type, `Session`, `StreamInfo`, Mono16 frames, playback of SER and uncompressed AVI recordings (whole or looped), the injected test signal, and the SER clip writer | Feature-6 | In progress |
| 7 | **A measured trigger.** Per-pixel noise, the sigma and size rules, and the faint limit measured on the observer's own recordings | Feature-9 | Proposed |
| 8 | **Seeing the faint side.** Display stretch, debayering for display, and a ring sized in seconds under one RAM limit | Feature-5 | Proposed |
| 9 | **First light.** The ZWO adapter, then the QHY adapter, camera controls, the dropped-frame record, and manual save | Feature-10 | Proposed |
| 10 | **A stable field.** (A) feature lock and drift readout; (B) the guiding loop on a simulated mount; (C) the ASCOM mount adapter | Feature-8 | Proposed |
| 11 | **The full session record.** Continuous recording, the time model, complete clip metadata | To be written | Planned |
| 12 | **Later.** More camera families as observers bring the hardware; registration, transit detection, event browser, export | To be written | Planned |

Each stage ends with something that can be used or measured, and one feature is one stage.

The order follows three rules.

1. **Safety before data.** Feature-7 came first because it removed two ways real data could be lost or damaged: the blind period (Evidence rule 5) and evidence written through the preview conversion (Evidence rule 3), which the 16-bit frames of Feature-6 would otherwise trigger. It also settled frame ownership, which every later stage builds on.
2. **Real data before hardware.** The observer has years of lunar recordings. Stages 6 and 7 run them through the path and measure the trigger on them. The trigger is the part of the design most likely to be wrong, so it meets real data before a camera adapter is written and before a night at the telescope depends on it. The SER reader and writer are built together in Stage 6, because each is the other's test.
3. **Nothing arrives before it can be used.** The camera comes after SER clips and the display stretch. Without the first, a 16-bit clip would be written as hundreds of slow PNG files. Without the second, the earthshine side cannot be seen on the screen. Guiding comes after the camera it needs.

What can move: Stage 8 moves ahead of Stage 7 if the recordings are too dark to work with under a fixed shift. Parts A and B of Stage 10 need no hardware and can run alongside Stages 7 to 9. Manual save can be built any time after Stage 5. The order of the remaining stages may change with hardware availability and measurements.

## 13. Open Decisions

- **Time reference:** PC clock with recorded synchronisation state only, or also a GPS reference (including GPS-stamping cameras), and when.
- **Overlapping candidates for long events:** Feature-7 extends the open clip up to a cap and starts another when the cap is reached. Whether transits need a different rule is decided with the transit detector.
- **Writer speed at camera scale:** frame ownership is decided (shared frames, Feature-7). How much RAM waiting clips may hold, and whether the SER writer keeps pace with the largest frames, is decided by measurement in Feature-5.
- **QHY SDK terms and frame bookkeeping:** the installed QHY SDK carries no license text, and whether its live mode exposes a frame counter or a dropped-frame count is not yet established. Both are settled before the QHY adapter is merged (Feature-10).
- **Guiding on real mounts:** whether the lock template needs refreshing over a session, whether pulses disturb the detector enough to justify tracking-rate offsets, and how to serve alt-azimuth mounts (Feature-8, Open decisions).
- **Field shift in the detector:** whether the shift measured by the feature lock should align the detector's reference, or whether per-pixel noise already absorbs slow drift. Decided on recordings, with registration.
- **Clip metadata format:** one machine-readable format that is still easy to read by eye (today `event.txt`; Feature-6 shows a proposed `event_summary.json`). Decided once, for all clips, before the full session record stage.
- **12-bit data in SER output:** as delivered (top-aligned) with `PixelDepthPerPlane = 16`, or another convention. Settled in Feature-6 by inspecting the observer's own recordings from the common capture programs.
- **Storage location** once there is a settings page.

## 14. Architecture Is Deliberately Revisable

This architecture is a working proposal, not a fixed contract. It will change through prototyping, measurement, observing experience, and scientific and technical feedback. We should be prepared to revise:

- component and directory boundaries;
- threading, queueing, ownership, and ring-buffer strategies;
- image-processing libraries;
- event detection and recording approaches;
- storage formats, metadata, and export mechanisms;
- GUI layout and workflow;
- implementation priorities and supported camera SDKs and mount interfaces.

The evidence rules in Section 4 change only by an explicit, recorded decision. Everything else is guided by **reliable evidence capture, simplicity, testability, maintainability, and measured performance**. Record important decisions and their trade-offs in the feature files as the project matures.

---

*SeleneWatch — project description, revised November 2026.*
