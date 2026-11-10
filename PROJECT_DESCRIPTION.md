# SeleneWatch — Project Description and Initial Architecture

> **Status:** Initial project vision and proposed architecture. This document is a starting point, not a frozen specification. Features, component boundaries, technologies, and implementation details may be revised as development, testing, and performance measurements reveal better approaches.

## 1. Project Overview

**SeleneWatch** is a native desktop application for continuously observing the Moon using a camera attached to a telescope. Its goal is to help observers discover, capture, review, and document short-lived phenomena that would otherwise be easy to miss.

The application is intended to monitor a high-resolution, high-frame-rate image stream, analyse incoming frames in near real time, and automatically preserve the evidence around interesting events. The primary scientific motivation is the observation of **lunar transient phenomena**, including brief flashes that may be associated with impacts. The system should also be useful for recording other notable events, such as transits, occultations, unexpected moving objects, or image anomalies.

SeleneWatch aims to make prolonged observation practical: users start a session, let the system monitor continuously, and later inspect a manageable collection of candidate events rather than manually reviewing an entire recording.

## 2. Main Goals

- **Continuous acquisition:** Receive frames from supported cameras at the highest practical resolution and frame rate for the selected hardware and configuration.
- **Real-time monitoring:** Process the live stream continuously, with detection designed to keep pace with acquisition where feasible.
- **RAM-based history:** Maintain a configurable circular (ring) buffer of recent frames in memory. Its useful duration depends on frame size, frame rate, available RAM, and buffer strategy.
- **Event-triggered recording:** When a potential event is detected, preserve frames from **before** the trigger and continue recording for a configurable **post-event** interval.
- **Multiple event types:** Support initial transient/flash detection and an extensible path toward transit, occultation, moving-object, and more general anomaly detection.
- **Reviewable evidence:** Save candidate-event clips together with useful metadata (such as timestamps, acquisition settings, detection information, and relevant frame context), so events can be inspected and shared.
- **Usable desktop interface:** Provide live preview, camera/session controls, detection status, candidate-event review, and configuration without hiding the core workflow behind technical complexity.
- **Testability without hardware:** Support prerecorded videos or image sequences as virtual camera inputs, plus synthetic streams and injected test events.

The emphasis is on **capturing potentially interesting evidence reliably**. An automatic detection is a *candidate*, not a scientifically verified lunar event.

## 3. Scope and Working Assumptions

### In scope

- A native **Windows** desktop application, initially targeting **C++20** and **Qt 6 Widgets**.
- Camera input through suitable native camera SDKs, with actual cameras and SDKs selected and validated during development.
- A source-independent pipeline that can also operate on recorded files and synthetic frames.
- Software-based frame registration/alignment as needed to compensate for lunar motion or drift in the image.
- An efficient memory buffer and event-driven saving mechanism.
- Detection, review, and export of candidate events.
- Logging, diagnostics, and performance measurement (including dropped frames and pipeline throughput).

### Not currently in scope

- **Telescope guiding or mount control.** Software image registration may be used, but an active guiding subsystem is not part of the current plan.
- A Java-based application or a FireCapture plugin as the primary implementation.
- Dependence on a paid/proprietary external application to provide the core pipeline.
- Automated scientific confirmation of all reported events; human inspection and, where possible, external corroboration remain important.

Cross-platform support may be investigated later, but the first target is Windows.

## 4. Proposed High-Level Architecture

The application is organized around a **performance-sensitive processing core** with a **Qt-based user interface**. Wherever practical, acquisition, buffering, detection, and recording should be usable independently of the GUI.

```text
  Physical Camera       Video / Image Files       Synthetic / Test Source
        |                        |                         |
        +------------------------+-------------------------+
                                 |
                         Frame Source API
                                 |
                      Frame + Timestamp + Metadata
                                 |
                         Acquisition Pipeline
                                 |
                      +----------+-----------+
                      |                      |
               RAM Ring Buffer       Processing / Analysis
             (recent raw frames)      (registration, filters,
                      |               temporal comparisons)
                      |                      |
                      |                Event Detectors
                      |              (flash, transit,
                      |               occultation, ...)
                      |                      |
                      +----------+-----------+
                                 |
                          Event Coordinator
                                 |
                     Pre-event + Post-event Frames
                                 |
                     Recorder + Metadata / Export
                                 |
                       Event Files and Review

                 Qt GUI: preview, controls, status,
                     settings, event browsing
```

The diagram describes **logical responsibilities**, not necessarily a single linear thread or a finalized implementation. In particular, preview, processing, and disk writing should not be allowed to unnecessarily block camera acquisition.

### Major components

| Component | Responsibility |
| --- | --- |
| **Frame source / camera adapter** | Presents real cameras, video files, and synthetic sources through a common interface. |
| **Acquisition** | Receives frames, timestamps them, attaches relevant metadata, and passes them onward with minimal overhead. |
| **Ring buffer** | Keeps a bounded recent history in RAM for pre-trigger recording. Buffer capacity and memory policy must be configurable and measured. |
| **Registration / preprocessing** | Optionally aligns successive lunar images and prepares frames or derived data for analysis. |
| **Detection engine** | Runs independently testable algorithms for flashes and other event categories; emits candidate events with supporting evidence or scores. |
| **Event coordinator** | Handles triggers, event duration, pre/post-frame collection, overlapping or repeated triggers, and candidate metadata. |
| **Recorder / exporter** | Writes event clips and metadata without stalling the live acquisition path. Storage formats (for example, video, SER, or FITS-related exports) will be evaluated rather than prematurely fixed. |
| **Qt desktop UI** | Displays the live feed, acquisition settings, processing status, event list, review tools, and configuration. |
| **Diagnostics and tests** | Measures latency, throughput, RAM usage, frame loss, detection behavior, and recording integrity. |

## 5. Intended Runtime Workflow

1. The observer selects a physical camera or test source and configures the observing session.
2. SeleneWatch starts continuous acquisition and keeps the latest frames in the RAM ring buffer.
3. The analysis pipeline examines incoming frames, using registration and preprocessing where useful.
4. One or more detectors identify a **candidate event**.
5. The event coordinator preserves relevant pre-trigger frames from the ring buffer and collects a configurable amount of post-trigger footage.
6. The recorder saves the clip and associated metadata; live acquisition continues.
7. The user reviews, classifies, keeps, discards, or exports candidates.

The exact buffering, trigger-merging, recording, and retention policies will be designed and validated against real workloads.

## 6. Initial Technology Choices

| Area | Initial choice | Notes |
| --- | --- | --- |
| Language | **C++20** | Native performance, memory control, and access to camera SDKs. |
| Desktop GUI | **Qt 6 Widgets** | Main window, preview, controls, settings, event review. |
| Development environment | **Visual Studio 2026 + MSVC** | Primary Windows development and debugging setup. |
| Build system | **CMake** | IDE-independent build configuration and testing integration. |
| Image processing | **C++ with OpenCV considered** | Adopt selectively after evaluating algorithms and performance; not a requirement for every component. |
| Testing | **Automated C++ tests + reproducible recorded/synthetic sources** | Specific test framework to be selected. |
| Version control | **Git** | Project source, documentation, and controlled evolution. |

The underlying processing core should avoid unnecessary dependencies on Qt UI classes. Threading, queues, data ownership, and memory strategies will be chosen based on measured behavior, not assumptions.

## 7. Key Engineering Considerations

- **Throughput and backpressure:** Acquisition, analysis, preview, and recording may run at different speeds. Define bounded queues and explicit policies for overload and dropped frames.
- **Memory budget:** High resolution and high FPS can consume RAM rapidly. Estimate and measure ring-buffer duration and consider optimized storage or frame representations when needed.
- **Timestamp accuracy:** Preserve the best available acquisition time information and document clock limitations; this is important for interpreting brief events.
- **False positives:** Expect atmospheric seeing, noise, hot pixels, clouds, tracking drift, compression artefacts, and moving foreground objects. Support review and gradual detector improvement.
- **Event integrity:** A trigger must not silently lose the frames that motivated it. Design tests for overlaps, long events, recording failures, and session interruptions.
- **Extensibility:** New frame sources and detectors should be addable without major changes to the GUI or other subsystems.
- **Observability:** Expose useful metrics such as actual FPS, processing latency, buffer occupancy, memory use, and dropped-frame counts.

No fixed resolution, FPS, buffer duration, or performance guarantee is assumed at this stage. Targets will be established through hardware-specific benchmarks.

## 8. Suggested Initial Code Organization

```text
SeleneWatch/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── ui/             # Qt windows, preview, controls, review
│   ├── camera/         # Frame sources and camera SDK adapters
│   ├── acquisition/    # Frame delivery, timing, session control
│   ├── buffer/         # RAM ring buffer and memory policies
│   ├── processing/     # Registration and preprocessing
│   ├── detection/      # Candidate detection algorithms
│   ├── recording/      # Event collection, storage, export
│   └── core/           # Shared types and application services
├── tests/              # Unit, integration, synthetic-event tests
├── docs/               # Design notes and experiments
└── README.md
```

This is a **suggested future layout**, not a requirement to reorganize the newly created Qt template immediately. Directories and modules should be introduced when the code justifies them.

## 9. Proposed Development Stages

1. **Bootstrap:** Qt Widgets / CMake application starts, builds, and runs reliably; establish Git and basic project conventions.
2. **Preview and test source:** Display frames from video/image sequences and synthetic sources; expose FPS and essential status information.
3. **Acquisition and memory prototype:** Implement the frame-source abstraction and ring buffer; benchmark throughput, memory, and dropped frames.
4. **First detector and event capture:** Implement a simple, interpretable flash/candidate detector and reliably save pre/post-event evidence.
5. **Real camera integration:** Add and validate one supported native camera SDK, then optimize based on measured end-to-end performance.
6. **Registration, richer detection, and review:** Improve alignment, false-positive handling, additional event types, event browsing, and export.

The ordering may change as hardware availability, integration risks, and testing results become clearer.

## 10. Architecture Is Deliberately Revisable

**This architecture is a working proposal, not a fixed contract.** The project will evolve through prototyping, measurements, user experience, and scientific/technical feedback. We should be prepared to revise:

- component and directory boundaries;
- threading, queueing, ownership, and ring-buffer strategies;
- which image-processing libraries are used;
- event detection and recording approaches;
- storage formats, metadata, and export mechanisms;
- GUI layout and workflow;
- implementation priorities and supported camera SDKs.

Changes should be guided by **simplicity, reliable evidence capture, testability, maintainability, and measured performance**. Record important architectural decisions and their trade-offs as the project matures, rather than treating this first design as immutable.

---

*SeleneWatch — initial project description, November 2026.*
