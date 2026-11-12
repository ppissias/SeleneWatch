# SeleneWatch features

Open proposals. What is already in the program is in [implemented features.md](implemented%20features.md). When moving completed features to `implemented features.md`, append them in full detail without removing or condensing specifications, review notes, or design decisions.

## Feature-7 — Evidence safety: no blind detector, and a lossless file path

Two faults in the Feature-4 code break evidence rules 3 and 5 of [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md#4-evidence-rules). Neither shows on the synthetic source today. Both would lose or damage real data. This step fixes them before Feature-6 brings recordings, cameras, and 16-bit frames.

### Problem 1 — the detector is blind while a clip is busy

[`EventCapture::onFrame`](event_capture.cpp#L21-L47) calls `observe` only when no clip is being collected and no write is in flight. Otherwise it calls `advanceReference`, which moves the reference and never tests the frame. The blind period is the post-event frames (30) plus the time the writer needs. At 640×480 that is about a second plus a few hundred milliseconds of PNG encoding. With a camera and a ring of several seconds it is several seconds per candidate. A second flash in that period is neither saved nor logged, and nothing records that the detector was not looking.

### Fix 1 — detect on every frame

- The detector runs on every frame. The busy path no longer calls `advanceReference` in place of `observe`.
- **A new candidate during collection joins the open clip.** A new candidate is a new onset after at least one quiet frame; frames that keep firing in the same run are the same candidate. The clip records each trigger (onset index, trigger index, time, measured values), and the post-event count restarts from the newest trigger.
- **A clip has a length cap.** After its first trigger a clip collects at most `kMaxPostEventFrames` frames (starting value 300, about 10 s at 30 fps). At the cap the clip is handed off. If the detector is still firing, a new clip starts at once from the ring. Its history overlaps the previous clip; that is accepted.
- **A candidate during a write starts a new clip.** The write lockout goes. Each waiting clip holds a copy of the ring, so the number of clips handed to the writer and not yet written is capped at `kMaxClipsInFlight` (starting value 2). A candidate beyond the cap is not saved, but it is logged as "not saved: writer busy" with its measured values.
- **Every candidate is logged.** `events/candidates.txt` gets one line per trigger, saved or not: UTC time, onset and trigger index, measured values, and either the clip folder or the reason it was not saved. The writer thread appends the lines: for a saved clip, after the clip is written, so the folder is known; for an unsaved candidate, when it arrives. The source thread still never touches the disk. `EventWriter`'s single queue keeps clips and log lines in order.
- `event.txt` lists every trigger in its clip.
- **Inject flash** stays enabled while the stream runs, so two quick flashes can be tested by hand.

### Problem 2 — files are written through the preview conversion

`EventWriter` saves each frame with `frameToQImage`, the same function the window uses for the preview. For Mono8 and RGB888 the two needs happen to agree. Feature-6 adds Mono16 and says its preview is "a fixed shift so the image is visible". If that shift goes into the shared function, every saved clip silently becomes 8-bit.

### Fix 2 — two conversions, two contracts

- `frameToQImage` is renamed `frameToPreviewImage` and belongs to the window. It may shift, stretch, or invert, and it returns an 8-bit image.
- The writer gets its own conversion, next to the writer code, which returns the stored samples unchanged: Mono8 as `Format_Grayscale8`, RGB888 as `Format_RGB888`, and Mono16 as `Format_Grayscale16` once Feature-6 adds it. A format it cannot write losslessly is an error reported like any other write failure, never a fallback to 8 bits.
- The two functions are nearly identical today. The duplication is intended: they have different contracts, and they diverge with Feature-6 (preview shift) and Feature-5 (stretch, SER).
- When clips move to SER (Feature-5), the SER frame write takes over the writer's conversion. The few preview PNGs in the event folder may then use the preview conversion, because they are not evidence.

### Done when

1. A test feeds two flashes ten frames apart: one clip with two triggers, and the post-event frames counted from the second.
2. A test feeds a flash while a write is in flight: a second clip is produced.
3. A test with `kMaxClipsInFlight` writes outstanding: the next candidate produces a "not saved: writer busy" log line and no clip.
4. A test with a trigger that never stops: the clip is handed off at the cap and a new clip starts.
5. A Mono8 clip written to PNG and read back has the same pixel values, bit for bit. When Feature-6 adds Mono16, the same test runs on a Mono16 frame with values above 255 and with the low bits set.
6. By hand: two quick presses of **Inject flash** while streaming are both saved, and `candidates.txt` lists both.

### Out of this step

- A trigger path that does not copy the whole ring. That is the scaling problem in [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md#scaling-the-evidence-path), decided together with the SER writer.
- Merging rules tuned for long transits.
- An event browser.

### Order

Before Feature-6. The conversion split must exist before the first Mono16 frame is written. The detection change must exist before a recording or a camera is monitored.

---

## Feature-6 — Selectable sources, an injected blip, and a measured trigger

The detector today fires when two frames rise by more than 60 gray levels across 120 pixels of the synthetic disk. Those numbers match the painted test image. They do not match a recording or a camera: the Moon is not centred, the noise depends on gain and sky, and a published lunar impact flash is often one frame. The trigger has to be set from the frames being watched, and the same trigger has to run on the synthetic source, on a recording, and on a ZWO camera.

### Selectable source

Three sources now do the same job, so one narrow source type is justified. It starts, stops, and delivers frames on a worker thread, with the same error report the synthetic source already has. The window holds the one that is selected. There is no plugin registry.

- **Synthetic.** The source that exists. Its fixed flash button remains.
- **Video file.** The user picks a `.ser` or `.avi` recording. SER is read by SeleneWatch. AVI is decoded with Windows Media Foundation, which is already on the machine. A file Media Foundation cannot decode is reported on the status bar, and the stream does not start.
- **ZWO ASI.** Video-mode capture through the ASICamera2 SDK. The SDK is closed and is not copied into the repository. CMake builds this source only when the SDK is present. Without it, the app still builds and the other two sources still run. The SDK's own license notice is recorded in `THIRD_PARTY_LICENSES.md` when this source is added. ZWO is the first camera adapter, not the only one. Other vendors follow as adapters of the same source type, each behind its own CMake switch (see [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md#camera-support)). Nothing outside the adapter may depend on ZWO types or behaviour.

The capture path does not care which source produced the frame. Preview, the ring, the detector, and the writer stay where they are.

Playback adds a speed control: real time, or as fast as the detector can run. The fast speed is how a recording is searched. The preview may drop frames, as it already does. The detector may not.

`Frame` gains `PixelFormat::Mono16`, stored as little-endian `uint16_t` in the existing byte buffer. SER bit depths of 8 and 16, and the ASI raw image, use that. A 12-bit ASI frame is stored as Mono16 with its real range. The preview of a 16-bit frame is a fixed shift so the image is visible. The stretch sliders stay in Feature-5.

### Injected blip

A blip is a disk painted onto the live frames for a chosen number of frames. It is how a recording becomes a measurement. "Inject flash" on the synthetic source is the same idea with fixed numbers. The blip works on every source.

The user sets the centre by clicking the preview, and sets the radius in pixels, the amplitude in gray levels of the frame's own format, and the duration in frames. The status line shows that amplitude again as a multiple of the current noise, so a value of 200 ADU on the bright limb and on the earthshine is not mistaken for the same event.

The painted frames are what the detector sees and what the clip stores. `event.txt` records that the blip was injected and records its centre, radius, amplitude, and duration. A clip from a blip is a test. A clip with no blip line is a candidate from the data.

### The trigger

The trigger is four numbers, applied to the difference against a held reference. They are editable while the stream runs, and the values that fired are copied onto the clip.

- **Reference.** The mean of the last 8 quiet frames. It stays frozen while a candidate is open, so a bright frame is not compared with the previous bright frame. Eight frames is a named constant.
- **Noise.** A robust scale of that difference, estimated from a subsampled grid so the whole frame is not sorted. The scale updates every frame and is shown live.
- **Sigma.** A pixel counts when its rise exceeds K times that noise. K is the sensitivity. The starting value is 6.
- **Size.** The counting pixels must form a compact patch: the count lies between a minimum and a maximum, and their bounding box is not much larger than the count. The minimum rejects a hot pixel. The maximum and the box reject a cloud, a bump of the telescope, or scattered noise. Starting values: minimum 8 pixels, maximum 400, box area at most 4 times the count.
- **Persistence.** One frame fires when its peak is at least K times the noise and the patch passes the size test. Two consecutive frames fire at the same K. The one-frame path is on by default, because a missed one-frame flash is not recoverable. If a recording shows that one-frame candidates are mostly noise, the user turns that path off and only the two-frame rule remains.

Hot pixels are learned, not listed by hand. A position that exceeds K on more than a third of the frames in a few seconds is ignored afterwards. The list is capped. It resets when the stream starts.

Every frame the status bar shows the noise, the peak in units of that noise, the patch size, and whether the frame passed. That line is the sensitivity meter. There is no separate filter stack.

### OpenCV

The project description lists OpenCV as something to consider, not as a requirement. It is the usual library for the later alignment work: a whole-frame shift when the mount drifts, and optical flow or a Kalman filter when a transit has to be followed. Feature-5 already leaves both of those out.

The flash trigger does not use them. The difference, the noise scale, the threshold, and the compact-patch test are plain loops over the pixels. A blur, a morphology pass, or a background-subtractor model would hide the same four numbers behind another library, and a background model treats lunar drift as the event. Connected components are not worth the dependency either: the bounding box of the pixels that passed the threshold is the clump test, and it is one pass.

OpenCV comes back only if a real recording shows that the noise floor is mount drift, the limb moving between frames, rather than photon noise and hot pixels. The measurement is the meter on that recording. If the ordinary peak is a bright rim that slides a few pixels a frame, the fix is a shift of the whole frame before the difference, and that is a later feature. A small FFT is enough for that one shift. OpenCV is the library to take if that feature also needs optical flow and feature matching. It is Apache-2.0, which the project can link dynamically. It is not added on speculation.

### How the numbers are chosen

The recording is the calibration source.

1. Play a recording of the lunar conditions that matter, with the blip off. Read the meter. K and the minimum size have to sit above the ordinary peaks. If the ordinary patch is already large, the hot-pixel rule or the box test is wrong, and raising K will not fix it.
2. Place a small blip on the dark limb. Step the amplitude down until the trigger stops. That amplitude, in noise units, is the faint limit for those conditions.
3. Repeat on the bright limb. The same amplitude in gray levels is a smaller multiple of the noise there. The gray-level number will differ. The sigma number is the one that should agree.
4. Inject a blip of one frame at the amplitude from step 2. If it must be kept, leave the one-frame path on and raise K only until a run with no blip writes no folder.
5. Leave the minimum size above a hot pixel and below the blip. Leave the maximum size above the blip and below a frame that changed across the disk.

Those four numbers are the session's trigger. They are stored on every clip from that run. They are not compiled in, and they are not claimed to be universal.

### Done when

1. The window can run the synthetic source or a chosen SER file through the same capture path. An AVI that Media Foundation can decode plays the same way. One that it cannot decode is reported and does not start.
2. While a recording plays, clicking the preview sets the blip centre. Radius, amplitude, and duration are editable. Injecting the blip paints it for that many frames.
3. With the blip off, a recording whose ordinary peaks sit under the trigger writes no folder. The same recording with a blip above the trigger writes one folder. `event.txt` names the sigma, the size limits, the measured peak, and the blip.
4. Changing K or the size limits affects the next frame without a restart.
5. The fast playback speed runs the detector over the file without waiting for real time.
6. With the ASI SDK configured and a camera connected, that camera is a third source and its frames reach the same detector. With no SDK, the app builds and the other sources run.
7. Tests cover the sigma rule on a constructed noisy frame: a compact patch at the threshold fires, a patch just below it does not, a single hot pixel does not, and a change spread across the frame does not. A SER file written by the test is read back with the same pixels and timestamps.

### Out of this step

- Display stretch, a duration-based ring, and writing the event as SER. Those remain Feature-5. Clips are still PNG plus `event.txt`.
- Limb fitting, registration, optical flow, and a separate transit detector. OpenCV is not a dependency of this feature.
- An event browser.
- A control that picks K by itself.

### Order

The SER reader, the Mono16 frame, the blip, and the sigma trigger come first, because they are what makes the numbers measurable. AVI follows. The ZWO source follows those, behind the CMake switch. The synthetic source stays, and its fixed flash remains a check that the new trigger still catches the painted spot.

---

## Feature-5 — Multi-bit-depth pipeline, display stretch, and lossless SER event recording

Reading a SER file, storing a Mono16 frame, and setting the live trigger are Feature-6. This feature keeps the display-stretch controls, the duration-based ring, and writing the event as SER.

Astronomical cameras provide 8-bit, 12-bit, or 16-bit raw pixel streams. Faint lunar phenomena (such as impact flashes on the unilluminated limb or earthshine) occupy the lower bit ranges, while bright illuminated terrain occupies the high end. This feature introduces native 8/12/16-bit data pipelines, a non-destructive display stretch engine for live preview, a configurable time-based ring buffer, and event recording to the standard astronomical SER (`.ser`) container accompanied by key preview images and metadata.

### 1. Event duration research and buffer sizing

To determine the ideal ring buffer capacity and recording durations, events observable against the Moon are categorized by their physics and duration:

| Event Type | Typical Duration | Frame Count (30–60 fps) | Detection & Capture Strategy |
| :--- | :--- | :--- | :--- |
| **Lunar Impact Flash** | 20 ms – 150 ms (rarely up to 1–2 s) | 1 – 5 frames | Sudden point-source brightening on earthshine or night side. Fast onset; 1–2 frame trigger. |
| **Atmospheric Meteor Transit** | 0.1 s – 0.8 s | 3 – 45 frames | Fast luminous streak crossing the lunar disk. |
| **ISS / Tiangong Transit** | 0.4 s – 1.5 s (up to ~2.5 s at low elevations) | 15 – 150 frames | Rapid silhouette crossing. At 400 km orbital altitude (~7.6 km/s), transit across 31' Moon takes ~0.5 s near zenith. |
| **LEO Satellite Transit** (Starlink, etc.) | 0.5 s – 3.0 s | 15 – 180 frames | Fast silhouette or specular solar glint. |
| **Commercial Aircraft Transit** | 0.5 s – 4.0 s | 15 – 240 frames | Silhouette, exhaust turbulence, and contrails. Distant or slow aircraft can take up to 4–5 s. |
| **Birds / Bats** | 0.1 s – 1.0 s | 3 – 60 frames | Fast silhouette with rapidly shifting geometry. |
| **Impact Ejecta / Vapor Plume** | 1.0 s – 8.0 s | 30 – 480 frames | Expanding dust/vapor cloud scattering sunlight or obscuring surface features. |
| **Stellar Occultation** (Ingress / Egress) | < 50 ms (diffraction limit) | Instantaneous edge | Step-function disappearance/reappearance. Requires 5–10 s window for baseline calibration. |
| **Grazing Stellar Occultation** | 1 – 3 minutes | 1,800 – 10,000 frames | Star blinks multiple times through lunar topography. (Planned ephemeris event). |
| **Planetary Occultation** (e.g. Jupiter, Mars) | 60 – 120 s (ingress alone) | 1,800 – 7,200 frames | Extended planetary disc occultation. (Planned ephemeris event). |

#### Capture window conclusions:
- **Transient / Unplanned Events (Flashes, Meteors, Transits, Plumes):** An overall capture window of **5 to 10 seconds** is optimal. A configurable pre-trigger history of **2 to 5 seconds** guarantees recording the object's entry into the field of view even if detection occurs midway across the disk. A post-trigger duration of **3 to 5 seconds** captures the transit exit or plume dissipation.
- **Extended / Ephemeris Events (Occultations):** Long multi-minute events are best served by continuous manual or scheduled streaming to SER, rather than maintaining an impractically large RAM ring buffer.

### 2. Multi-bit-depth data representation

Extend `PixelFormat` and `Frame` to support high-dynamic-range sensor inputs:
- `PixelFormat::Mono8` (1 byte per pixel, 0–255)
- `PixelFormat::Mono12` (stored in memory as unpacked 16-bit little-endian `uint16_t`, 0–4095, 2 bytes per pixel)
- `PixelFormat::Mono16` (stored in memory as 16-bit little-endian `uint16_t`, 0–65535, 2 bytes per pixel)
- `PixelFormat::RGB888` (3 bytes per pixel)

#### Memory budgeting and buffer sizing:
For a 10-second ring buffer:
- **640 × 480 Mono16 @ 60 fps:** ~368 MB
- **1920 × 1080 Mono16 @ 30 fps:** ~1.24 GB
- **1920 × 1080 Mono16 @ 60 fps:** ~2.48 GB
- **3840 × 2160 (4K) Mono16 @ 30 fps:** ~4.98 GB

The ring buffer capacity must no longer be a hard-coded constant (150 frames). It will be configured by **target duration in seconds** (e.g. default 3–5 seconds) clamped by a **user-configurable maximum RAM ceiling** (e.g. default 2.0 GB).

### 3. Non-destructive display stretch pipeline

Standard monitors and Qt GUI widgets (`QImage`) display 8 bits per channel. If 12-bit or 16-bit data is simply shifted or clamped, faint features on the dark limb and earthshine appear completely black.

The display stretch converts high-bit-depth frames into an 8-bit `QImage` (`Format_Grayscale8` or `Format_RGB888`) strictly for GUI rendering:
- **Data integrity rule:** The raw frame data stored in `Frame`, buffered in `FrameRingBuffer`, and recorded to disk remains **100% linear, untouched, and unclipped**.
- **Display modes:**
  - *High-bit shift / Linear:* Fast bit-shift (`>> 4` or `>> 8`), zero CPU overhead, optimal for bright illuminated lunar maria and highlands.
  - *Window / Level (Min/Max):* Sliders for Black Point ($B$) and White Point ($W$) mapping linearly to 0–255.
  - *Auto-stretch (MTF / Percentile):* Automatically samples frame median and dispersion (e.g. median ± $k \times \text{MAD}$ or 0.1% / 99.9% percentiles) to automatically reveal earthshine and faint details.
  - *Invert mode:* Inverts brightness for high-contrast tracking of dark transit silhouettes.

### 4. Lossless SER container storage (`SerWriter`)

Instead of writing hundreds of separate PNG files—which costs CPU time for DEFLATE compression, scatters one event over many files, and carries no standard astronomical metadata—candidate clips will be saved in the astronomical standard **SER v3 format** (`.ser`). PNG itself is lossless and supports 16-bit grayscale, so it remains acceptable for preview images.

#### Advantages:
- **Zero compression overhead:** Streams sequential raw pixel blocks directly to disk. Writing hundreds of frames takes fractions of a second on NVMe/SSD storage without stalling acquisition.
- **Native 8, 12, and 16-bit support:** Standardized by `PixelDepth` (8, 12, or 16) and `ColorID` in the SER header.
- **Per-frame 64-bit UTC timestamps:** SER v3 appends a trailer with one 64-bit UTC timestamp per frame, in 100 ns ticks since 0001-01-01 (the .NET epoch, not Windows `FILETIME`, which counts from 1601). See also the SER notes in [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) on the inverted `LittleEndian` flag and 12-bit alignment.
- **Ecosystem compatibility:** Natively supported by SER Player, PIPP, AutoStakkert!, Siril, RegiStax, and Python astropy pipelines.

### 5. Event folder structure and preview assets

To allow immediate visual inspection of detections without opening an external SER player, each event directory will contain:

```text
events/
└── 20261111-170522-123Z/
    ├── event.ser               # Lossless raw SER v3 clip with trailer timestamps
    ├── event_summary.json      # Structured metadata (trigger time, onset frame, SNR, ROI, parameters)
    ├── 01_baseline.png         # Reference frame immediately prior to event onset
    ├── 02_event_peak.png       # Peak detection frame (maximum flash intensity or transit silhouette)
    └── 03_diff_annotated.png   # Differential image with ROI bounding box / trajectory annotation
```

### Done when

1. `SyntheticFrameSource` can produce Mono8, Mono12, and Mono16 test frames.
2. The preview window displays 12-bit and 16-bit feeds with interactive stretch controls (Min/Max sliders and Auto-stretch).
3. The ring buffer can be configured by duration in seconds with memory use tracked and capped against a RAM limit.
4. An injected flash or transit triggers clip capture, and the background writer outputs `event.ser`, `event_summary.json`, and the three key preview PNGs.
5. The generated `event.ser` opens reliably in SER Player or PIPP with valid frame counts, bit depth, and timestamps.

### Out of this step

- Hardware camera SDK integration (Feature-6).
- Advanced automated transit / silhouette tracking detectors (optical flow / Kalman filtering).
- Lunar limb sub-pixel registration.
