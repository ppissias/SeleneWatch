# SeleneWatch

**Continuous lunar monitoring and automatic capture of transient phenomena.**

SeleneWatch is a native C++/Qt desktop application being developed to monitor the Moon through a telescope-mounted camera, identify potentially interesting short-lived events, and preserve the surrounding frames for later review.

The main motivation is to help observers detect and document **lunar transient phenomena**, particularly brief flashes that may be associated with impacts. The design also leaves room for investigating transits, occultations, moving objects, and other unexpected events.

> **Project status: early development.** The features below describe the intended direction, not functionality already implemented. The initial Qt/CMake application scaffold is the starting point.

## What SeleneWatch aims to do

- **Monitor continuously** using high-resolution, high-frame-rate camera input, within the limits of the available hardware.
- **Analyse incoming frames** to identify potential transient events while observation continues.
- **Maintain a configurable RAM ring buffer** so that frames from *before* a detection are still available.
- **Capture events automatically**, saving a configurable interval of frames before and after a trigger.
- **Preserve useful evidence**, including timestamps, relevant acquisition settings, and detection metadata.
- **Support event review**, so observers can inspect and classify candidates rather than search through hours of recordings.
- **Enable offline development and testing** using recorded sequences and synthetic/test frame sources, without requiring a camera.

Automatic detections are **candidates for review**, not scientific confirmation of a lunar event.

## Intended workflow

```text
Camera / Recorded Sequence / Synthetic Source
                    |
             Frame Acquisition
                    |
        +-----------+-----------+
        |                       |
   RAM Ring Buffer       Frame Processing
   (recent frames)       & Event Detection
        |                       |
        +-----------+-----------+
                    |
             Event Coordinator
                    |
          Pre- and Post-Event
           Frames + Metadata
                    |
              Event Recorder
                    |
             Review / Export

       Qt GUI: live preview, controls,
             status and events
```

Acquisition, analysis, preview, and saving should be separated where useful to avoid blocking the incoming frame stream.

## Technology stack

| Area | Initial direction |
| --- | --- |
| Language | C++20 |
| User interface | Qt 6 Widgets |
| Platform | Windows initially |
| IDE / compiler | Visual Studio 2026 / MSVC |
| Build system | CMake |
| Image processing | Native C++; OpenCV may be evaluated where useful |
| Testing | Automated tests with reproducible recorded or synthetic inputs |

The processing core should remain as independent of the GUI as practical, helping with performance testing, modularity, and future extensions.

## Getting started (development)

The project currently starts from a Qt Widgets / CMake application created in Visual Studio.

1. Install **Visual Studio** with the **Desktop development with C++** workload and CMake support.
2. Install a compatible **Qt 6** build for your MSVC toolchain, including **Qt Widgets**.
3. Open the SeleneWatch project in Visual Studio.
4. Select an **x64 Debug** configuration, configure CMake if prompted, and build.
5. Run the **SeleneWatch** executable from the IDE.

Exact setup details and camera dependencies will be documented as the project develops. Qt and MSVC versions must be compatible; a Qt build targeting one toolchain should not be assumed to work with every compiler configuration.

## Initial development roadmap

1. **Application bootstrap:** Buildable Qt Widgets app, CMake configuration, Git, and basic project conventions.
2. **Preview and simulation:** Image viewer and a recorded/synthetic frame source with frame-rate diagnostics.
3. **Acquisition and buffering:** Common frame-source API, bounded RAM ring buffer, and throughput measurements.
4. **First detector and recording:** Candidate flash detection plus reliable pre-/post-trigger evidence capture.
5. **Real camera integration:** Add and validate a supported camera SDK.
6. **Expanded capabilities:** Image registration, improved detection, event review, and export.

The order and details may change based on prototype results and available hardware.

## Scope

**In scope:** lunar image acquisition, software-based alignment/registration where needed, candidate-event detection, buffering, recording, diagnostics, and review.

**Not currently in scope:** telescope guiding or mount control. SeleneWatch is not intended to replace the telescope's guiding system.

## Architecture is expected to evolve

The architecture is an **initial proposal, not a fixed specification**. Modules, interfaces, threading and buffering strategies, GUI structure, algorithms, dependencies, and storage formats may be revised as development progresses. Decisions should be driven by **reliability, simplicity, maintainability, testability, and measured performance**.

For a fuller discussion of goals, component responsibilities, design trade-offs, and engineering considerations, see [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md).

---

*SeleneWatch is an early-stage project. Feedback, experiments, and design revisions are part of the development process.*
