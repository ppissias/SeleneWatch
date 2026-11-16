# SeleneWatch features

Open proposals. What is already in the program is in [implemented features.md](implemented%20features.md). When moving completed features to `implemented features.md`, append them in full detail without removing or condensing specifications, review notes, or design decisions.

## Feature-9 — A measured trigger

> **14 November 2026: implementation complete; acceptance item 7 partly open.** The controls, detector, overlays, evidence, and automated/window checks are built. Recording measurements below cover mare, terminator, bright limb, crater rim, and RGB terrain. Earthshine was not identified in the survey, and the bright-limb comparison is saturation-limited. The full feature remains here until that observing calibration is completed; its proposal and review history are preserved below.

The detector today fires when two frames rise by more than 60 gray levels across 120 pixels of the synthetic disk. Those numbers match the painted test image. They do not match a recording or a camera: the Moon is not centred, the noise depends on gain and sky, and a published lunar impact flash is often one frame. The trigger has to be set from the frames being watched, and the same trigger has to run on the synthetic source, on a recording, and on a camera.

This is the step where the design is most likely to be wrong, because it is the first one judged by real lunar data. It comes before any camera work for that reason. Feature-6 supplies the recordings.

### Injected blip

The blip moved to Feature-6, Part C, on 13 November 2026. Its full description is in [implemented features.md](implemented%20features.md). The observer can test with it as soon as recordings play, and this feature finds its calibration tool ready.

What this feature adds to it: the status line shows the blip's amplitude as a multiple of the noise at its centre, once the trigger below measures that noise.

### The trigger

The trigger is four numbers, applied to the difference against a held reference. They are editable while the stream runs, and the values that fired are copied onto the clip.

- **Reference.** A running mean per pixel over the recent quiet frames, with a time constant of about two seconds (a named constant). It stays frozen while a candidate is open, so a bright frame is not compared with the previous bright frame.
- **Noise.** A running variance per pixel, kept beside the mean and frozen with it. Noise on the Moon is not one number. Photon noise and seeing make the sunlit limb and every sharp edge far noisier than the earthshine, and the earthshine is where impact flashes are found. One scale for the whole frame would either fire on the bright part or be deaf on the dark part. The per-pixel value has a floor: a robust scale of the whole difference, estimated from a subsampled grid so the frame is not sorted. The floor keeps a saturated or clipped pixel, whose own variance is zero, from firing on nothing. The floor updates every frame and is the "noise" number shown live.
- **Sigma.** A pixel counts when its rise exceeds K times its own noise. K is the sensitivity. The starting value is 6.
- **Size.** The counting pixels must form a compact patch: the count lies between a minimum and a maximum, and their bounding box is not much larger than the count. The minimum rejects a hot pixel. The maximum and the box reject a cloud, a bump of the telescope, or scattered noise. Starting values: minimum 8 pixels, maximum 400, box area at most 4 times the count.
- **Persistence.** One frame fires when its peak is at least K times the noise and the patch passes the size test. Two consecutive frames fire at the same K. The one-frame path is on by default, because a missed one-frame flash is not recoverable. If a recording shows that one-frame candidates are mostly noise, the user turns that path off and only the two-frame rule remains.

Hot pixels need no list of their own at first. A steady hot pixel does not rise. One that flickers has a large variance of its own, and the minimum size rejects a single pixel in any case. A learned list (a position that exceeds K on more than a third of the frames in a few seconds is ignored afterwards) is added only if a recording shows hot pixels that still fire.

The mean and the variance are two float buffers the size of the frame, updated in the same pass that tests the pixels. The cost is measured on the largest frame in use before the camera stage; the overload policy in the project description applies if it does not keep pace.

Every frame the detection panel (below) shows the noise floor, the peak in units of its own noise, the patch size, and whether the frame passed. That is the sensitivity meter. There is no separate filter stack. *(The first draft put this meter in a status-bar line. The panel replaced it on 13 November 2026.)*

### The detection panel

*Added on 13 November 2026, from a discussion with the observer.* Setting a trigger from numbers in a status line is hard. The observer needs to see, while the image plays, how close the ordinary frames come to firing. The panel is where the trigger is set and watched.

```
Detection ───────────────────────────────────────────────────
 Sensitivity K  [·····│·······▲·····┃········]  6.0σ
                     now 3.1σ   ▲ highest in last 10 s: 4.7σ
 Min size       [··▲┃························]  8 px   now 3 px
 Max size       [·▲··························┃]  400 px
 One-frame trigger [x]
 Peak σ, last 10 s:  ▁▂▁▃▂▁▅▂▁▁▂▃▁▁▂ ┄┄┄ K
 Noise floor 2.3 ADU   peak at (812, 433)   frame: quiet
 Overlay:  ( ) none   (•) σ map   ( ) noise map
```

- **A slider per setting, with the live value on it.** The slider for K carries the frame's peak in units of its own noise; the size sliders carry the current patch size. A value read at 30 frames per second flickers too fast to judge, so each slider also holds the highest value of the last few seconds. The distance between that mark and the setting answers "how close am I to a detection".
- **A short history.** A strip of the peak per frame over the last seconds, with K drawn across it. The ordinary noise shows as low grass, the margin above it is visible, and a blip shows as a spike.
- **Overlays on the preview.** A σ map colours each pixel by its rise in units of its own noise and marks the pixels over K. A noise map shows each pixel's noise, which is the threshold in gray levels divided by K: high on the sunlit limb and at edges, low on the earthshine. A box marks the peak.
- **A panel, not a mode.** The same detector runs whether the panel is open or not, and what the panel shows is what the detector does. It can stay open during a real session, where it shows cloud or a falling transparency as rising noise.
- **The rules it keeps.** The detector runs on the source thread and owns its buffers. It sends a small per-frame summary, and the overlay at preview rate, through a one-slot mailbox like the preview's; the window never reads the detector's state. Settings travel the other way and take effect from the next frame. The overlays are drawn for the screen only and never touch a stored frame (Evidence rules 1 and 3). The values in force when a candidate fires are copied onto its clip.
- **Settings are remembered between runs.** The panel is the first settings page of the program, so the last trigger values are kept and restored at the next start.
- **Not for the Feature-4 detector.** Its two numbers (60 gray levels, 120 pixels) are replaced by this feature, so the panel is built with the new trigger and not before.

### One set of numbers for the whole disk

The observer asked whether the bright part of the Moon and the earthshine need settings of their own. A single threshold in gray levels cannot serve both: one value either fires on the sunlit limb or is deaf on the earthshine. This trigger is built around that problem. Each pixel learns its own noise, and K counts in units of that noise, so one K gives every pixel its own threshold in gray levels, high where the image is noisy and low where it is quiet. The noise map overlay shows this directly.

Settings per region are therefore not built now, for three reasons.

- **The calibration tests the question.** Steps 2 and 3 of "How the numbers are chosen" measure the faint limit on the earthshine and on the bright limb in units of noise. If the two agree, one K serves the whole disk. If they differ, that measurement is the case for regions, and it is recorded under item 7 of "Done when".
- **The Moon moves under a drawn region.** It drifts through the field over a session, and the terminator and the earthshine move with it. A region drawn on the image would need to follow the feature lock of Feature-8. The per-pixel noise follows the drift by itself, within its time constant of about two seconds.
- **The likely weak spot is the edges, not the halves.** At the limb and the terminator, seeing moves the edges between frames. That noise has heavier tails than the per-pixel variance assumes, so false candidates may gather along edges at a K that is quiet elsewhere. If a recording shows that, the remedy is a rule tied to strong gradients, or registration. A hand-drawn region is not.

One kind of region may still earn its place later: a mask that watches only the dark side. Flashes cannot be seen against the sunlit surface anyway, so the mask would cut false candidates and work. It would have to follow the Moon's drift and be recorded on every clip. It is added only if recordings show the need.

### OpenCV

The project description lists OpenCV as something to consider, not as a requirement. It is the usual library for the later alignment work: a whole-frame shift when the mount drifts, and optical flow or a Kalman filter when a transit has to be followed. Feature-5 already leaves both of those out.

The flash trigger does not use them. The difference, the noise scale, the threshold, and the compact-patch test are plain loops over the pixels. A blur, a morphology pass, or a background-subtractor model would hide the same four numbers behind another library, and a background model treats lunar drift as the event. Connected components are not worth the dependency either: the bounding box of the pixels that passed the threshold is the clump test, and it is one pass.

OpenCV comes back only if a real recording shows that the noise floor is mount drift, the limb moving between frames, rather than photon noise and hot pixels. The measurement is the meter on that recording. If the ordinary peak is a bright rim that slides a few pixels a frame, the fix is a shift of the whole frame before the difference, and that is a later feature. A small FFT is enough for that one shift. OpenCV is the library to take if that feature also needs optical flow and feature matching. It is Apache-2.0, which the project can link dynamically. It is not added on speculation.

### How the numbers are chosen

The recording is the calibration source.

1. Play a recording of the lunar conditions that matter, with the blip off. Read the meter, looped over a few seconds if that helps (Feature-6, Playback). K and the minimum size have to sit above the ordinary peaks. If the ordinary patch is already large, the noise estimate or the box test is wrong, and raising K will not fix it.
2. Place a small blip on the dark limb. Step the amplitude down until the trigger stops. That amplitude, in noise units, is the faint limit for those conditions.
3. Repeat on the bright limb. The same amplitude in gray levels is a smaller multiple of the noise there. The gray-level number will differ. The sigma number is the one that should agree.
4. Inject a blip of one frame at the amplitude from step 2. If it must be kept, leave the one-frame path on and raise K only until a run with no blip writes no folder.
5. Leave the minimum size above a hot pixel and below the blip. Leave the maximum size above the blip and below a frame that changed across the disk.

Those four numbers are the session's trigger. They are stored on every clip from that run. They are not compiled in, and they are not claimed to be universal.

### Colour frames

The trigger works on one value per pixel. For an RGB frame it reads the mean of the three channels, and for a raw Bayer mosaic it reads the mosaic as it is. Both are derived views; the stored frame is not touched (Evidence rule 1).

### Done when

1. While a recording plays, the detection panel shows the blip's amplitude as a multiple of the noise at its centre. *(This item first described the blip's controls. Those moved to Feature-6 with the blip, as its item 13.)*
2. With the blip off, a recording whose ordinary peaks sit under the trigger writes no folder. The same recording with a blip above the trigger writes one folder. `event.txt` names the sigma, the size limits, the measured peak, and the blip, and the folder holds the annotated difference image.
3. Changing K or the size limits affects the next frame without a restart.
4. Tests cover the sigma rule on a constructed noisy frame: a compact patch at the threshold fires, a patch just below it does not, a single hot pixel does not, and a change spread across the frame does not. On a frame with a quiet half and a noisy half, the same blip in gray levels fires on the quiet half only, and the same blip in units of noise fires on both.
5. The synthetic source's fixed flash still fires the new trigger, and its moving marker still does not.
6. The cost of the trigger per frame is measured on the largest recording in hand and written down here.
7. By hand, on the observer's own recordings of at least three conditions (earthshine, the sunlit limb, the terminator): the steps under "How the numbers are chosen" are carried out, and the faint limit and the false-candidate rate found for each are written down here. If the ordinary peaks on those recordings come from drift or seeing at edges and not from noise, that finding is recorded and decides whether registration moves forward. Whether the faint limit in units of noise agrees between the earthshine and the bright limb is written down too, and decides whether settings per region are needed.
8. The detection panel shows each setting as a slider carrying the live value and the highest value of the last few seconds, the peak history with K across it, and the σ map and noise map overlays. Moving a slider changes the next frame.
9. A test shows that opening the panel or switching an overlay changes no stored frame and no detection result.
10. The trigger values in force at Stop are restored at the next start, and a clip records the values that fired, not the values shown later.

### Implementation and validation review — 14 November 2026

The implementation is in `FlashDetector`, `DetectionPanel`, `Session`, `EventCapture`, and the recording writer. It uses native ADU for Mono8/Mono16, channel-mean RGB, and the unchanged Bayer mosaic. Threshold comparison is inclusive. The reference learns cumulatively at startup, then at the named two-second exponential rate; the first frame initializes it. The noise floor is at least 0.5 ADU and uses the median absolute deviation on a bounded 4096-point grid.

Two original implementation choices above were revised on purpose. Detection and quiet reference updates use two passes: the global decision must be known before changing any reference sample. A sparse 8-connected patch search replaces the single global bounding box, because a regression showed that one unrelated hot/moving pixel could veto a real compact flash. This uses plain C++, with no OpenCV dependency, and rejects dense changes before the bounded sparse work. The strongest passing patch is selected, or the strongest rejected patch when none passes. The synthetic pacing marker was reduced to five pixels, below the default minimum; the fixed flash keeps radius eight. Detection has no synthetic-disk mask or other source-specific rule.

The dock implements the sliders and exact controls, current and recent markers, ten seconds of source-time history, selected-centre noise units, and full-resolution sigma/noise maps. Settings reach the next frame and are restored at the next launch. Frame and immutable measurements travel together at preview rate (also on triggers); maps use the actual pre-update noise. Closing the dock leaves every-frame detection running. Tests compare bytes and decisions across overlay modes and exercise the actual window with the dock hidden during injection.

Each trigger records its original K, size limits, persistence mode, normalized peak, noise floor, and patch coordinates. The strongest passing frame keeps its actual difference from the held reference for `03_diff_annotated.png`. This derived preview is separate from the raw SER; its frame and settings are recorded explicitly. Test markers survive a standalone SER replay, including an injection occurring after its first frame. The writer releases the difference buffer when it refuses a clip. The software-version header now refreshes on every build, so source edits and commits without reconfiguration cannot retain an earlier clean version in the metadata; an isolated Git-repository check covers both and verifies that unchanged headers are not rewritten.

All five CTest targets pass in Debug and Release, including threshold/size/noisy-half checks, native low/high-aligned Mono16, the fixed synthetic flash and nonfiring marker, next-frame settings, persistence, EOF/stop and loop boundaries, original trigger settings after later changes, and actual-window overlay/injection/settings checks. Window testing also found and fixed a dock visibility callback during destruction. No new dependency or license change was introduced.

Measured on an AMD Ryzen 9 9955HX in Release, the largest archive frame (1280×1024 RGB) takes **5.050 ms mean / 5.130 ms 95th percentile** without a map; sigma/noise maps made every frame take **5.666 / 5.635 ms mean**. The 1296×964 Mono8 case takes **3.577 ms mean / 3.666 ms 95th percentile**, or **4.230 / 4.276 ms mean** with maps. These exclude reading, GUI colouring, and disk writing. Both are below the recordings' nominal 100 / 55.6 ms periods; a separate detection thread is not justified by these measurements. Full model/map/difference memory is additional to the frame-pool readout and remains part of Feature-5's RAM-budget work.

The real-recording controls expose a sensitivity cost. At K=6, the roughly thirteen-second prefixes produced 8 mare, 10 terminator/limb, 21 crater-rim, and 2 RGB background candidates (36.15, 45.19, 96.61, and 8.89 candidates/minute over those short prefixes). They are unclassified candidates, not a verified false-impact rate. Tested quiet K values were 18, 24, 12, and 24 respectively. Changing K can change reference freezing, so candidate counts are not necessarily monotonic in K.

With FWHM five, one- and three-frame blips had these tested miss-to-recovery brackets: mare **22.5σ → 27σ (36.01 → 43.21 ADU)** at K=18; terminator **30σ → 36σ (150.98 → 181.18 ADU)** at K=24; crater rim **15σ → 18σ (131.59 → 157.91 ADU)** at K=12. These are single-position/time trials, not recovery probabilities or universal faint limits. The bright limb and RGB recover only after saturation in the chosen trials, so their requested amplitudes do not establish an unclipped faint limit. A real mare control writes zero folders without injection and exactly one complete 180-frame SER with a one-frame blip; Siril 1.4.4 independently reads its format, timestamps, and test marker.

The archive first-frame survey opened 240 lunar-named recordings and did not identify a suitable earthshine exposure. Black sky or clipped shadows are not substitutes. Annotated ordinary-frame differences show crater rims and the limb, consistent with motion/seeing against the held reference. **A registration experiment should move ahead of camera deployment**; whether global drift or local seeing dominates still needs measurement. **No region settings UI is justified yet:** earthshine versus bright-limb agreement is unanswered, and the available sunlit result is saturation-limited.

Acceptance items **1–6 and 8–10 are implemented and checked**. Item **7 is partial**: real-terrain rates and recovery brackets are recorded, but suitable earthshine/unclipped comparison data and the final one-K conclusion are still needed. The detailed methods, per-condition coordinates, timing meanings, saturation limits, runtime table, and reproduction commands are in [docs/feature-9-validation.md](docs/feature-9-validation.md); the measured data subset is [docs/feature-9-calibration.json](docs/feature-9-calibration.json). Full local QA artifacts are under ignored `out/qa-feature9/`. This incomplete calibration is why Feature-9 has not been moved into the append-only implemented history.

### Registration follow-up review — 14 November 2026

The planned whole-frame experiment now exists in the manual probe, with a tested native-word integer translator and four recording comparisons. It is linked into the probe/tests only. Cropped and aligned trials use the same 64-pixel interior and the same injected lunar target; a full-field control identifies cropping effects. All 856 shifts pass the prototype's internal checks, with mean estimation costs of 5.35–5.98 ms. No archive/evidence pixels or dependencies changed.

Candidate counts alone were misleading. At K=6 the aligned mare falls from eight cropped candidates to one, but 238/240 frames pass in one long firing run; the rim similarly has 239/240 passing frames. RGB remains active for 135/136 frames in both cropped and aligned modes. Passing occupancy and longest-run metrics are therefore now recorded alongside counts. At the previously quiet K values, all controls are quiet; the mare and terminator have unchanged tested recovery brackets, while the rim/RGB aligned recoveries are saturation-limited. Short, single-position trials establish neither a long-session rate nor an unclipped sensitivity improvement.

**Registration stays experimental.** Investigate startup, held references, and rearming during sustained edge/seeing activity before production integration, then repeat longer controls and independent injections at several positions/times with suitable unclipped earthshine/bright-limb data. The evidence does not yet separate residual seeing from reference holding or integer registration error. Feature-9 acceptance item 7 remains partial. Methods, all comparison tables, limits, and reproduction are in [docs/registration-experiment.md](docs/registration-experiment.md), with [docs/registration-measurements.json](docs/registration-measurements.json) retaining the measured cases. Earlier proposal and validation text above remain historical records.

### Reference and rearming implementation review — 14 November 2026

The sustained-activity follow-up reproduces and fixes three faults: one global run could hide a separate new patch, unrelated successive patches could confirm each other in two-frame mode, and an open clip prevented quiet background learning. The original whole-frame freeze rule is revised deliberately: passing patch boxes plus one pixel are held, while background elsewhere keeps learning through collection. Separate patches have independent persistence; adjacent/overlapping patches inherit state across merges/splits, and source-index gaps break pending confirmation. All new candidate patches, including several in one frame, reach the writer's records. A measured difference is copied before the explicit reference-update phase. Raw published pixels remain unchanged.

The panel and trigger records now expose active patch count, longest active duration, protected pixels, and model learning frames. A constructed 359.9-second TEST run recovers all 98 independent one-frame flashes beside a continuously stronger patch, with the expected 99 total new candidates and no duplicate for that persistent patch. Regression tests also cover merge/split identity and pre-update snapshots. The shared budget now charges the one-byte-per-pixel protection mask.

Four complete 500-frame recordings were checked with five independent one-frame injections each, at K=6 and the earlier quiet comparison K. At K=6 the matched counts are 4/5 mare, 4/5 terminator, 5/5 rim, and 5/5 RGB; several recover while other activity is already firing. Background updates on all 499 post-initialization frames. This does not establish universal recovery or a quiet default: higher-K recovery is position-dependent, some first-opportunity flashes are missed, and rim/RGB examples include saturation. The revised common-crop experiment reduces aligned passing occupancy from 238/240 to 15/240 on the mare, 226/240 to 27/240 on the terminator, and 239/240 to 15/240 on the rim; RGB remains 135/136. New candidate totals count independent patch runs and cannot be compared directly with older global-run totals. Registration stays experimental; acceptance item 7 remains partial.

Methods, runtime, unchanged historical measurements, scientific limitations, and validation are in [docs/reference-and-manual-validation.md](docs/reference-and-manual-validation.md), with complete trial summaries in [docs/reference-rearming-measurements.json](docs/reference-rearming-measurements.json). A connected/overlapping transient inside an already active patch remains unresolved by spatial persistence; no blind warm-up interval or automatic threshold tuning was added.

### Live threshold controls and rejection feedback — 15 November 2026

The observer can adjust six numeric settings with sliders and exact entry, plus the existing one- or two-frame confirmation choice. The maximum bounding-box area per above-K pixel defaults to 4.0. The **Background and noise** section exposes the reference time constant (2.0 s) and the minimum noise floor (0.50 ADU). Their values apply at the next frame and are remembered with the other detector settings. The time constant controls future reference updates. Each trigger, candidate record and measured clip peak carries its own settings.

The live panel distinguishes a peak below K, a patch outside the pixel bounds, a box-to-patch ratio above its limit, and a frame that exceeds the bounded patch search. It also shows the pending 1 / 2 frames state when two-frame confirmation is selected. The measure and measure-batch probes accept `maxPatchBoxRatio`, `referenceTimeConstantSeconds` and `minNoiseAdu` for replay calibration. Constructed tests show the corresponding changes in patch acceptance, noise-normalised response and background learning. Field calibration remains open. [Preview and Monitoring](docs/preview-monitoring.md)

### Out of this step

- Limb fitting, registration, optical flow, and a separate transit detector. OpenCV is not a dependency of this feature.
- A control that picks K by itself.
- Settings per region of the disk, and a dark-side mask. Each waits for a recording that shows the need (see "One set of numbers for the whole disk").
- An event browser.

### Order

After Feature-6, which supplies the recordings, the `Session`, and the blip. Before Feature-10, so that the trigger has been through real data before a night at the telescope depends on it.

---

## Feature-5 — Display stretch and a ring sized in seconds

> **14 November 2026: implemented; real-earthshine display acceptance remains open.** Native-bit stretch, Bayer display, source-time capture windows, a shared image/analysis byte budget, writer admission/rate reporting, and window checks are built. The constructed faint-recording check passes; no suitable real earthshine sequence was identified. The proposal and implementation notes stay together here until that field check is available.

Storing a Mono16 frame and reading and writing SER are Feature-6. The trigger is Feature-9. This feature keeps the display-stretch controls and the duration-based ring under one RAM limit. Both are needed before a camera is used: without a stretch the earthshine side of a 16-bit image is black on the screen and cannot be pointed at or focused, and a ring counted in frames means a different length of history at every frame rate. Both can be built and tested on 16-bit recordings, with no hardware.

Astronomical cameras provide 8-bit, 12-bit, or 16-bit raw pixel streams. Faint lunar phenomena (such as impact flashes on the unilluminated limb or earthshine) occupy the lower bit ranges, while bright illuminated terrain occupies the high end. This feature introduces a non-destructive display stretch engine for live preview and a configurable time-based ring buffer.

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

The storage formats are the ones Feature-6 defines:
- `PixelFormat::Mono8` (1 byte per pixel, 0–255)
- `PixelFormat::Mono16` (stored in memory as 16-bit little-endian `uint16_t`, 2 bytes per pixel)
- `PixelFormat::RGB888` (3 bytes per pixel)

There is no `Mono12` format. Twelve-bit data is a Mono16 frame that records 12 significant bits and whether they sit at the top or the bottom of the word, stored exactly as the source delivered it (Evidence rule 2). The display stretch and the SER header read those two fields.

#### Memory budgeting and buffer sizing:
For a 10-second ring buffer:
- **640 × 480 Mono16 @ 60 fps:** ~368 MB
- **1920 × 1080 Mono16 @ 30 fps:** ~1.24 GB
- **1920 × 1080 Mono16 @ 60 fps:** ~2.48 GB
- **3840 × 2160 (4K) Mono16 @ 30 fps:** ~4.98 GB

The ring buffer capacity must no longer be a hard-coded constant (150 frames). It will be configured by **target duration in seconds** (e.g. default 3–5 seconds) clamped by a **user-configurable maximum RAM ceiling** (e.g. default 2.0 GB).

The ceiling covers every live frame, not only the ring. Since Feature-7 frames are shared, and the frames held by the open clip and by clips waiting for the writer count against the same limit. The writer's queue limit, counted in frames in Feature-7, becomes a share of this ceiling in bytes. How large that share must be depends on how fast the SER writer drains it, and that is measured here.

### 3. Non-destructive display stretch pipeline

Standard monitors and Qt GUI widgets (`QImage`) display 8 bits per channel. If 12-bit or 16-bit data is simply shifted or clamped, faint features on the dark limb and earthshine appear completely black.

The display stretch converts high-bit-depth frames into an 8-bit `QImage` (`Format_Grayscale8` or `Format_RGB888`) strictly for GUI rendering:
- **Data integrity rule:** The raw frame data stored in `Frame`, buffered in `FrameRingBuffer`, and recorded to disk remains **100% linear, untouched, and unclipped**.
- **Display modes:**
  - *High-bit shift / Linear:* Fast bit-shift (`>> 4` or `>> 8`), zero CPU overhead, optimal for bright illuminated lunar maria and highlands.
  - *Window / Level (Min/Max):* Sliders for Black Point ($B$) and White Point ($W$) mapping linearly to 0–255.
  - *Auto-stretch (MTF / Percentile):* Automatically samples frame median and dispersion (e.g. median ± $k \times \text{MAD}$ or 0.1% / 99.9% percentiles) to automatically reveal earthshine and faint details.
  - *Invert mode:* Inverts brightness for high-contrast tracking of dark transit silhouettes.

### 4. Colour display

A raw Bayer mosaic is debayered for the screen only, with a simple bilinear interpolation in plain loops. The stored frame keeps the mosaic and its pattern (Evidence rule 2).

### Done when

1. `SyntheticFrameSource` can produce Mono8 and Mono16 test frames, the latter with 12 or 16 significant bits.
2. The preview window displays 12-bit and 16-bit feeds with interactive stretch controls (Min/Max sliders and Auto-stretch). On a 16-bit recording of the earthshine side, the auto-stretch shows surface detail that the fixed shift shows as black.
3. The ring buffer can be configured by duration in seconds with memory use tracked and capped against a RAM limit. The limit covers the ring, the open clip, and the clips waiting for the writer.
4. The SER writer's rate is measured on the largest frames in hand and shown as the write rate. The share of the RAM limit given to waiting clips is set from it.
5. A test shows that no stretch setting changes a stored frame or a written clip.

### Implementation and validation review — 14 November 2026

The Display and capture dock is tabbed beside Detection. Linear/high-bit, native-ADU black/white sliders and exact controls, percentile/midtones Auto-stretch, inversion, and bilinear Bayer colour are display-only. Changes also redraw a stopped recording from its retained raw frame. Settings persist. Capture controls apply at Start; synthetic input adds low/high Mono12, Mono16, and RGGB12 with a deliberately faint test half-disk.

History, quiet tail, and maximum post-trigger collection now use source seconds, defaulting to 5, 3, and 10. Playback speed does not change the window. Long events continue at time or RAM boundaries. Each clip records requested durations, available history, byte allowances, and its collection ending. Completed files and a completed requested tail are distinct. A RAM ending is visible in the window's counter/message.

One shared `MemoryBudget` follows numeric image/analysis allocations across pools, detector/sparse arrays, maps, differences, and display images, including old sources still held by the writer. Shared pixels count once, copies separately, and capacity/reallocation peaks/spares count. This is not a process-RSS ceiling: metadata, bounded diagnostic histories, indices, Qt caches, codec workspaces and allocator overhead are additional. A layout-dependent working reserve precedes the history/open-clip/writer shares; actual admission remains authoritative. The collection allowance leaves room for its difference and one incoming frame. Writer admission counts active plus queued bytes. Refused clips/continuations are logged. Optional-map refusal leaves detection unchanged; a required allocation failure stops acquisition visibly and hands off the available clip with the failure recorded. The complete ownership and admission policy is in [docs/feature-5-validation.md](docs/feature-5-validation.md).

Ten new core cases cover native low/high 12/16-bit contrast and evidence invariance, actual synthetic native layouts/faint detail, all Bayer patterns/edges, shared/copy/image lifetimes, optional-map refusal, time/byte-limited history, tails at different rates, long-event continuation, forced allocation failure, and RAM splitting with its difference. The actual window plays and stretches a generated 12-bit SER during a one-frame injection, checks unmodified saved frames and the original file byte-for-byte, restretches after Stop, and restores settings. All 800 frames of the pressure sequence are analysed even when a capped writer refuses a continuation. Qt offscreen rendering is used; no manual native-desktop check is claimed. Siril independently opens new Mono8/RGB/Mono16 samples; profiler/measurement output cannot name the input itself or its hardlink.

Release preview conversion means (linear/window/auto): **1296×964 Mono8: 2.583 / 5.147 / 6.367 ms**; **1280×1024 RGB: 7.909 / 23.318 / 25.249 ms**; **1296×964 Mono16, 13 significant high bits: 3.233 / 14.622 / 14.051 ms**. Reading and QWidget composition are excluded. Buffered 128-repeat SER writes measured **3,641.71 / 4,283.34 / 3,863.77 MB/s**, respectively, including Qt flush but excluding display previews. These generated TEST clips exercise roughly 160/503/320 MB and are not sustained physical-disk guarantees. The measured headroom on the available 22.5/39.3 MB/s inputs does not justify increasing the conservative writer share. Full measurements are in [docs/feature-5-measurements.json](docs/feature-5-measurements.json).

Acceptance **1, 3, 4, and 5 pass**. Item **2 is partial**: controls, native-bit handling, and constructed faint-detail visibility pass; a real earthshine recording remains unidentified in the survey. Auto-stretch of the sole Mono16 archive file shows noise, not usable lunar terrain. This shares Feature-9's field-data gap. No dependencies or license terms changed. The original proposal is preserved above; the feature has not moved into implemented history while that field check remains open.

### Optional live histogram — 15 November 2026

**View → Histogram** opens a dock below the preview. Its fixed bin array counts samples from the latest GUI frame up to five times per second when the dock is visible. The horizontal axis uses the declared native ADU range or the observed range with **Fit range** enabled. The vertical scale supports linear or logarithmic counts. The dock remembers visibility and the two scale options.

Counts are calculated before display conversion and use the declared sample alignment. A single distribution combines Bayer sample codes or the stored RGB channels. The readout includes the exact observed range and the fractions at zero and at the maximum code. The maximum code follows the stream's declared layout and can be 65535 for a camera that preserves full RAW16 words. Tests cover high/low alignment, Bayer and RGB, endpoint counts, range fitting, and the optional dock in Preview. A bench check used the histogram during RAW8 and RAW16 Preview and Monitoring on the ASI120MM Mini. [Preview and Monitoring](docs/preview-monitoring.md)

### Out of this step

- Hardware camera SDK integration (Feature-10).
- Advanced automated transit / silhouette tracking detectors (optical flow / Kalman filtering).
- Lunar limb sub-pixel registration.

### Order

After Feature-9 and before Feature-10. If the recordings used for Feature-9 are 16-bit and too dark to place a blip on with the fixed shift, the stretch moves ahead of Feature-9; nothing in it depends on the trigger.

---

## Feature-10 — First light: camera adapters (ZWO, then QHY) and manual save

> **15 November 2026 — ZWO, QHY and manual capture software are integrated.** The Mini has passed RAW8/RAW16, readback, evidence, injection, induced-loss and two 30-minute soak checks. QHY loads the user-installed runtime. QHY hardware validation and lunar observing acceptance are pending.

This is the step where SeleneWatch watches the real Moon. By now the path has run on recordings, the trigger has been measured on them, clips are SER, and the preview can show the earthshine. What is missing is the camera.

The observer has a ZWO camera and a QHY camera, and both SDKs are installed on the development machine (paths in [local environment.txt](local%20environment.txt)). ZWO comes first. QHY follows in the same feature, because the second adapter is what shows which code the two really share.

### The two cameras

| | ZWO ASI120MM | QHY ALccd5L-II colour (a QHY5L-II-C) |
| --- | --- | --- |
| Sensor | mono | colour, Bayer mosaic |
| Frame | 1280×960, 3.75 µm pixels | 1280×960, 3.75 µm pixels |
| Sample depth | 8 bits, or 12 bits in a 16-bit word | 8 bits, or 12 bits in a 16-bit word |
| GPS | none | none |
| SDK | ASICamera2 | qhyccd; its header lists the QHY5L-II-C |

The two have the same frame size and pixel size, and between them they cover the two cases that matter: mono and colour. That makes them a good pair to build the adapters on.

At this frame size the numbers are small. A frame is 1.2 MB at 8 bits and 2.5 MB at 16. At 30 fps that is 37 or 74 MB/s, and five seconds of history is 184 or 369 MB. The larger rows of the scaling table in the project description are for other observers' cameras. The frame rates these two actually reach over USB, at 8 and at 16 bits, are measured at first light and written down here.

### What an adapter is

A camera adapter is a frame source (Feature-6). It opens one camera, applies the settings, and on its worker thread fetches each frame straight into a writable frame from its `FramePool`. Both SDKs fill a buffer the caller supplies, so a frame is not copied on the way in.

An adapter reports, in `StreamInfo` and on each frame:

- camera model and serial number, and the SDK version read at run time;
- frame size, storage format, significant bits, where those bits sit in the word, and the Bayer pattern if the sensor is colour;
- exposure, gain, offset, binning, and ROI as the camera reports them back, not as they were requested;
- what its frame time means: the host's arrival time, or a GPS-stamped exposure where the camera has one;
- whether its frame index is a hardware counter, and how dropped frames are known.

An adapter that cannot answer one of these says "unknown". It does not guess (Evidence rules 2 and 4).

Nothing outside an adapter may include a vendor header or depend on a vendor's behaviour.

### Build

- Each adapter has its own CMake switch and SDK path: `SELENEWATCH_ZWO_SDK_DIR` and `SELENEWATCH_QHY_SDK_DIR`. With a path set, the adapter is built and the SDK's DLLs are copied next to the executable after the build. With none set, the application builds and runs on synthetic and recorded sources, as it does today.
- The SDKs stay outside the repository.
- Each SDK's license and redistribution terms are recorded in `THIRD_PARTY_LICENSES.md` before its adapter is merged.

### ZWO ASI (ASICamera2 SDK)

Read from the SDK installed on the development machine:

- **License.** The SDK folder carries a permissive license text (MIT wording, copyright ZWO 2015). Using and redistributing the DLL with its notice is allowed. It is recorded when the adapter is merged.
- **Capture.** Video mode: `ASIStartVideoCapture`, then `ASIGetVideoData` with a timeout, into the pool frame.
- **Formats.** The adapter offers `RAW8` and `RAW16` only. `RGB24` and `Y8` are processed inside the SDK and are not what the sensor delivered. A colour camera's raw output is stored as the Bayer mosaic with its pattern.
- **12-bit alignment.** The project description notes that ZWO's `RAW16` puts 12-bit data in the top bits. The adapter does not rely on that note. A test on the camera (the lowest bits of a real frame are zero or they are not) confirms it, and the frame records what was found.
- **Frame index and drops.** The SDK has no per-frame counter. It reports a running count of dropped frames (`ASIGetDroppedFrames`). The adapter reads it after each frame and advances the frame index by the frames delivered plus the frames dropped since the last one, so a drop shows as a gap in the index (Evidence rule 4). `StreamInfo` says the index is derived, not a hardware counter.
- **Frame time.** The host's clock when `ASIGetVideoData` returns. That is after the exposure, the readout, and the USB transfer, and `StreamInfo` says so.
- **GPS is left for later.** Some ZWO models have a GPS module and deliver a stamped exposure time through `ASIGetVideoDataGPS`. The observer's camera is not expected to have one, and a GPS path cannot be tested without such a camera. Nothing blocks it: `StreamInfo` already says what the frame time means, and an adapter that has a GPS time would report it there. It is added when a GPS camera is in hand.
- **Controls.** Exposure, gain, offset, binning, ROI, bit depth, high-speed mode, and USB bandwidth. Changing format, binning, or ROI stops and restarts the stream, because the frame layout changes.
- The SDK's guide-port calls (`ASIPulseGuideOn`) are not used. Guiding goes through the mount (Feature-8).

### QHYCCD (qhyccd SDK)

Read from the SDK installed on the development machine (version 25.06.16):

- **License.** The SDK folder carries no license text. Its use and redistribution terms must be found and recorded before the adapter is merged. Until then the adapter is built on the development machine only and nothing of the SDK is distributed.
- **Capture.** Live mode: `SetQHYCCDStreamMode`, `BeginQHYCCDLive`, then `GetQHYCCDLiveFrame`, which is polled and reports width, height, bits, and channels with each frame.
- **Formats.** 8 and 16 bits through `SetQHYCCDBitsMode`. Where the significant bits sit is checked on the camera in the same way as for ZWO.
- **Frame index, drops, and time.** To be established from the SDK and the camera before the adapter is written: whether live mode exposes a frame counter, and what the host can know about dropped frames. Where the answer is "the SDK does not say", `StreamInfo` says unknown. The SDK has GPS support (`CAM_GPS`) and precise exposure timing (`GetQHYCCDPreciseExposureInfo`) for the models that have them; like the ZWO GPS path, that is left for later.
- The SDK brings its own runtime DLLs (USB and threading libraries). They are copied with it and listed in `THIRD_PARTY_LICENSES.md`.

**Shared code.** When the QHY adapter is written, what it repeats from the ZWO adapter (the fetch loop with its timeout and stop check, the pool, the error report, the restart on a layout change) moves into one helper. It is not designed ahead of the second adapter.

### Mono and colour

Both kinds of camera are supported, and the difference is kept to three places.

- **Stored.** A colour camera's raw output is the Bayer mosaic, stored as it arrives in Mono8 or Mono16 with its pattern recorded (Evidence rule 2). The SDKs' processed colour formats are not used.
- **Shown.** The preview debayers for the screen only (Feature-5).
- **Analysed.** The trigger compares each pixel with its own history, so it runs on the mosaic unchanged (Feature-9). The feature lock bins a mosaic 2×2 before matching, so the colour pattern does not enter the match (Feature-8).

Everything else, including the ring, the clips, and the writer, does not know whether the sensor is mono or colour.

### Camera controls in the window

The source selector lists the cameras the built adapters find. The controls an adapter reports are shown with their ranges. Values are read back from the camera after each change, and the values in force are what `StreamInfo` records. A camera that is unplugged ends the source thread with a reported error, as any source failure does.

### Dropped frames and overload, shown

The status bar shows the dropped-frame count for the session beside the frame rate. Gaps inside a clip are written into its metadata (Evidence rule 4). If the frame path cannot keep pace with the camera, the SDK drops frames and the count rises; that is the overload signal the project description asks for, and it is visible, not silent.

### Manual save

**Save now** keeps the current ring and a short interval after it, as a clip marked manual, with a line in `candidates.txt`. It costs almost nothing since clips are lists of shared frames, and it matters from the first night: it is how the observer keeps something the detector did not fire on.

### Done when

1. With no SDK path set, the application builds, runs, and passes its tests.
2. With the ZWO SDK path set and the camera connected, the camera appears as a source, streams at 8 and at 16 bits, and its frames reach the same ring, detector, and writer as a recording's.
3. The same with the QHY camera.
4. For each camera: where the significant bits sit has been checked on the hardware, and frames carry the result.
5. For each camera: a clip saved from the live stream opens in SER Player, PIPP, or Siril with the right size, depth, frame count, and times, and `event.txt` holds the camera, its settings, the SDK version, and what the frame time means.
6. Loading the machine until frames are dropped raises the dropped count on screen, and a clip saved then records the gaps.
7. Unplugging the camera while streaming is reported, and the application stays usable.
8. **Save now** writes a clip marked manual and a line in `candidates.txt`.
9. A first session on the Moon of at least an hour with each camera: the frame rate held, the frames dropped, the detector's cost per frame, the candidates logged, and the write rate are written down here.

### Manual capture implementation review — 14 November 2026

Save now is available for both existing source types. A source-thread request marks the latest acquired frame, retains the current time/byte-bounded history, and collects the configured quiet tail. Requests join an open clip without interrupting analysis, and a manual tail can continue across its cap. The record distinguishes host request UTC, original applied source UTC/index, and continuation frames. Manual marks do not invent threshold measurements or increment the automatic-candidate count; repeated requests remain separate marks. Native SER output, the shared allocation ceiling, writer refusal, partial endings, and pending/outcome logging use the existing path.

Tests cover configured history/tail duration, overlapping automatic/manual capture, continuation, pending requests at Stop/EOF/failure/loop boundaries, explicit rejection when no frame exists, writer refusal logging, and exact Mono8/low-high 12-bit Bayer/RGB pixel and timestamp round-trips. An actual offscreen window plays a constructed native recording and saves it manually with the original file unchanged. Acceptance item **8 is implemented and checked** for synthetic/recorded sources. Items involving camera adapters, hardware formats, dropped frames, SDK terms, and hour-long first-light sessions remain open. Details are in [docs/reference-and-manual-validation.md](docs/reference-and-manual-validation.md). No camera SDK/dependency was added; this feature remains an open proposal.

### Out of this step

- Guiding through a camera's ST-4 port.
- GPS-stamped frame times, until a camera with GPS is in hand.
- Cooling control, filter wheels, and long single exposures.
- Cameras other than these two. Each later family is its own adapter, added when an observer has the hardware.
- Continuous recording and the full time model (the session record stage).

### Order

After Feature-5, so the preview can show what the camera is pointed at. ZWO first, then QHY, then the shared helper. Manual save can be built at any point after Feature-7.

### ZWO implementation and hardware review — 14 November 2026

The ZWO portion is implemented behind optional `SELENEWATCH_ZWO_SDK_DIR`; without it, synthetic and recorded sources still build and run without the ASI DLL. The only vendor-header consumer is `cameras/zwo_sdk.cpp`. `ZwoSource` owns the camera and worker, fills shared-budget pool buffers directly, and uses the existing Session/detector/ring/writer. A narrow API boundary is actually used by deterministic camera tests; no shared QHY helper is introduced ahead of the second adapter. The installed SDK reports 1.41.0.0. Its MIT-worded ZWO 2015 notice and exact DLL are copied by CMake, and its source/version/obligations are recorded in `THIRD_PARTY_LICENSES.md`.

The connected device identifies as **ZWO ASI120MM Mini**, USB2, 1280×960 mono, advertised 12-bit ADC, bins 1/2. RAW8 and RAW16 work through the real Session. The Camera tab discovers devices explicitly, shows supported ranges and reads back exposure, gain, offset, raw format, bin, ROI and bandwidth. Unsupported high-speed mode is absent. Every applied change restarts acquisition and finishes the old clip, keeping settings snapshots separate; invalid geometry does not stop a healthy stream. SDK dark subtraction and flip are disabled. Raw colour binning beyond 1 remains refused pending camera-specific Bayer verification.

**Packing decision, revising the proposal's fixed-depth expectation:** bin-1 RAW16 samples had zero low four bits (`word OR 0x3ff0`), but the bin-2 sequence used lower bits (`0x7ffc`, including bits 2 and 3). Sampled zeros do not establish a padding contract. Frames therefore retain and interpret all 16 delivered word bits, low aligned, and record the advertised ADC depth separately as 12. No native bits are removed or rescaled. New SER files declare the interpretation in the standard Observer text (`SeleneWatch;layout=16L`, with the existing TEST prefix where applicable), so live and replay detector units agree. Malformed/contradictory declarations are refused; foreign/legacy inference remains available. Existing clips are unchanged.

The source records host UTC immediately after the SDK read, an accompanying steady time, and cumulative SDK losses. Its index is delivery ordinal plus observed losses, explicitly derived with approximate gap placement because the SDK buffers frames. A failed/reset drop counter preserves the just-received frame with unknown telemetry, reports failure and closes. Stop polls at 100 ms; no-frame failure allows twice the actual exposure plus five seconds. Physical USB removal is not claimed as tested.

One-minute, full-frame Release runs at 1000 µs, gain 0, offset 9 and bandwidth 80 delivered **1662 RAW8 frames at 28.020 arrival fps** and **830 RAW16 frames at 14.001 fps**, with zero SDK drops and zero image/analysis memory refusals. Charged peaks were 647.6/630.4 MB; these are payload values, not process RSS. A 640×480 bin-2 RAW16 run at gain 20/offset 12 delivered 41 frames at 14.005 fps. A one-frame Gaussian test (FWHM 4, peak 8192 delivered-word units, K=6) produced a new candidate matched to an actually brightened sample and saved marked test data. This does not establish observing sensitivity.

A manual QA-only delayed consumer induced **343 real SDK drops** in 15 seconds; its TEST clips recorded frame gaps. The window test separately verifies the on-screen drop count and saved gap metadata. Up to 20 sampled preview-frame SHA256 hashes per hardware run matched saved native pixels. Siril 1.4.4 independently read every frame of new 31-frame RAW8, 30-frame RAW16 and 43-frame overload TEST files; independent checks matched sizes, storage depths, frame counts, exact UTC trailer ticks and sidecars. Overlapping clips intentionally repeat frames; write counters measure buffered output rather than durable disk speed.

All **nine CTest targets pass in Debug and Release, with and without the SDK**, without touching physical hardware. Cases cover cleanup, readback clamps, native buffers, geometry/Bayer constraints, timeout/stop, simulated disconnect, loss/reset/telemetry failure, immutable settings across window restarts, native manual evidence and declared-layout replay decisions. Offscreen window rendering is inspected; no manual desktop interaction is claimed. Full results and limits are in [docs/zwo-validation.md](docs/zwo-validation.md) and [docs/zwo-measurements.json](docs/zwo-measurements.json).

Acceptance items **1 and the ZWO portion of 2/5/8** are checked. Item 4 is addressed by preserving all delivered bits and explicitly reporting unverified ADC packing. Item 6 has real SDK induced-loss/gap evidence plus a deterministic window-count check; a loaded native desktop session is still a field check. **QHY, physical unplug, raw-colour hardware, manual real-camera desktop use and the hour-long Moon sessions remain open**, as do Feature-9/Feature-5 observing calibration. Feature-10 remains here; no historical record is moved or condensed.

### Unattended soak implementation and review — 14 November 2026

The manual camera probe now has a soak mode with periodic manual clips and flushed progress records. It uses the normal Session/detector/ring/writer and keeps automatic capture active. It records acquisition, SDK losses/unknown state, charged payload, sampled Windows working set/private memory, writer backlog/bytes, candidates and output size. Once-per-second output/free-space checks end acquisition visibly; pending and available partial evidence finishes, so output can exceed the stop threshold by those final writes. No clip is deleted to enforce a quota. A `stop.request` file ends an unattended run gracefully. The actual command is exercised with the deterministic SDK boundary, including native RAW8/RAW16 files, sampled byte checks, periodic gaps, output limits, disconnect partial endings, invalid configuration, existing-output protection and requested stops. All **10 CTest targets pass in Debug/Release with and without the ASI SDK**.

Two 30-minute runs used full-frame 1280×960, bin 1, exposure 1000 µs, gain 0, offset 9, bandwidth 80, and the normal 5/3/10-second capture windows/2 GiB allocation ceiling. RAW8 acquired **50,417 frames** and RAW16 **25,192**, with zero SDK-reported drops and zero memory refusals. After warm-up their progress-derived rates were **28.020/14.000 fps**. All **210 native byte samples per run** matched saved pixels; 3253/1633 frames were kept in periodic clips, with successful evidence outcomes and a drained writer. Charged peaks were 292.5/296.2 MB. Ten-second process-memory snapshots can miss brief collection/write peaks and do not establish leak-free behavior; post-warm-up private usage changed by 4.5/5.6 MB. Builds/tests ran concurrently, and neither illumination nor temperature was controlled. No candidates fired; this is not a lunar sensitivity or false-positive result.

Siril independently read the first and last clips from each run, with exact size/depth/count/tick checks and unchanged SER hashes. Initial capture controls and RAW8/full-frame/bin-1 settings were read back and restored before the camera was closed. Full measurements, sampling limits and reproduction are in [docs/zwo-soak-validation.md](docs/zwo-soak-validation.md), [docs/zwo-soak-measurements.json](docs/zwo-soak-measurements.json) and [docs/camera-soak.md](docs/camera-soak.md). **Physical unplug, desktop/hardware QHY qualification and the one-hour lunar sessions remain open**; these bench runs do not close acceptance item 9. The complete proposal and previous reviews remain intact.

### QHY development draft review — 14 November 2026

The original BSD-2-Clause QHY wrapper/source/controls are prepared on `feature-10-qhy-development` in an isolated worktree. The selected Windows SDK is 25.6.16.15, confirmed by header/file/runtime version, with all 26 used exports present. No SDK-wide Windows redistribution grant was found in the installed package or inspected vendor documentation/download pages; the compatibility header's copyright/license reference does not establish DLL rights. SDK terms and exact redistribution/notice scope remain unverified and are recorded in `THIRD_PARTY_LICENSES.md`. The draft stays unmerged, off by default, loads the installed DLL in place when explicitly configured, and copies/bundles no vendor files. No vendor contact, driver installation or firmware operation was performed.

`QhySource` uses the existing Session/detector/ring/writer and a narrow API boundary implemented by the SDK and deterministic TEST device. SDK-reported buffer length is allocated from the shared pool; excess capacity remains charged, while only checked native image bytes are published. Unexpected width/height/depth/channel counts fail visibly and finish available prior evidence. RAW8/RAW16, finite/ranged exposure/gain/offset/USB-traffic/transfer-speed controls and ROI are supported as reported/read back. SDK debayer is disabled, as are reported row-noise/automatic-exposure controls. RAW16 retains all word bits with ADC depth separate. The opaque SDK device ID is separate from an unknown serial. Transfer-speed units are separate from ZWO bandwidth/high-speed controls.

The draft intentionally limits binning to 1 because no independent bin getter was established and colour/bin behavior needs hardware qualification. It labels bin 1 as commanded and checks delivered geometry, rather than presenting a request as an independent readback. ROI origin adjusts the SDK-reported Bayer phase. Host arrival UTC/steady time are recorded; exposure timing is unknown. A hardware exposure counter has not been decoded, so indices are delivery ordinals and dropped frames remain unknown in metadata, the window and diagnostics. Arrival timing does not invent loss counts or prove adjacency for two-frame confirmation.

The SDK live read has no timeout argument in the installed API. Returned generic error/not-ready states are retried under a twice-exposure-plus-five-second deadline; this cannot bound a call that blocks inside the driver. Actual Stop/removal/reconnect behavior remains a required hardware check. A shared production worker abstraction was not introduced around these different error/telemetry contracts. Concrete shared code now consists of native-unit settings editing, Bayer phase, and the ASI/QHY diagnostic loop. Source selection uses kind data rather than assuming SDK entry positions.

All **11 CTest targets pass in six configurations** (Debug/Release without SDKs, QHY alone, both SDKs), using fake APIs without opening hardware. New cases cover cleanup, buffer tails/capacity accounting, exact native bytes and Bayer origin, clamped fractional controls, not-ready/disconnect/start/layout failures, manual native evidence and unknown loss/serial semantics. An actual offscreen window changes RAW8 to RAW16/ROI/gain, checks separate evidence settings and partial endings, and shows unknown losses. The shared soak runner checks QHY Bayer clips and sampled bytes too. Native-unit editing preserves values offset from a fractional step grid. The installed SDK's version-only command and DLL export/import policy are checked independently of cameras.

The observer-assisted checklist, SDK review, commands and limitations are in [docs/qhy-development.md](docs/qhy-development.md), with [matrix results](docs/qhy-development-measurements.json). **QHY hardware, colour orientation/packing, getters, Stop/unplug/reconnect, sustained throughput and lunar acceptance remain unverified**, alongside SDK redistribution terms. This is software preparation, not completion of Feature-10 or a hardware-qualified release. The full proposal and historical reviews remain preserved.

---

### Preview and Monitoring implementation review — 14 November 2026

The observer-approved workflow separates tuning from evidence capture. **Start preview** runs acquisition, bounded history and the same detector with capture OFF; it creates neither automatic/manual clips nor candidate records. **Start monitoring** enables automatic event clips and Save now. **Stop monitoring** finishes the current available clip with `monitoring stopped`, returns to Preview and keeps acquisition running. Previously queued immutable jobs finish writing independently. **Stop stream** ends acquisition and closes the camera. A prominent banner and explicit buttons expose the current state and pending transitions. Preview diagnostic decisions are labelled **Would trigger**, rather than appearing as observing candidates. Evidence rule 5 is explicitly scoped to Monitoring; analysis remains active in both modes.

In Preview, every supported writable control currently exposed by the ZWO adapter—exposure, gain, offset, USB bandwidth and high-speed mode—can be adjusted live. A coalesced request reaches the camera worker between SDK reads; actual controls/layout are read back and copied safely to the UI. Requested values remain separate from actual quantised values. Unsupported controls are absent, flip stays disabled for raw orientation, and format/bin/ROI changes explicitly restart Preview after preflight validation. Monitoring fixes acquisition settings; display stretch and detector thresholds remain editable and each trigger retains its effective thresholds.

Camera and Detection docks are visible together. Measurements include peak sigma versus K and percentage of threshold, patch size versus bounds, compactness versus its limit, native noise, persistence, active patches, maps and recent history. Live camera edits invalidate history/reference and show estimated **Settling** before threshold feedback resumes; charged arrays are reused during repeated reference initialisation. SDK buffer age and per-frame settings association remain unknown. Consequently camera Monitoring starts through a fresh open/readback with empty history/reference; no buffered tuning frame is labelled with the new fixed evidence snapshot. The short transition interruption and initially short history are deliberate. Synthetic/recorded streams retain valid same-configuration history/reference and reconfirm patches on arming, including an already-active Preview patch and two-frame persistence.

`Session` owns mode state and processes changes on the source thread before frames or at camera wait checkpoints. Stop monitoring therefore also works during a long exposure without a hardware restart. A live readback outside Preview fails before changed frames can reach evidence, and a camera change racing a pending arm request cannot enable capture without a reopen. Preview creates no candidate journal; entry to Monitoring queues the existing session-start record. Existing QA commands explicitly start in Monitoring, preserving their unattended capture behavior.

Tests cover Preview without output, Would-trigger feedback through a real offscreen window, native live controls/readback clamps on the acquisition worker without restart, invalid geometry, history and persistence boundaries, disarm-frame exclusion, a pending-arm race, long-exposure mode changes, fixed evidence metadata, partial clips and simulated disconnects. The connected Mini additionally passed automated real-SDK RAW8/RAW16 window checks of live exposure/gain/offset/bandwidth changes, fresh Monitoring starts, manual native clips, continuing acquisition after disarming and balanced connection cleanup. High-speed mode is tested with the SDK boundary because this Mini does not expose it. Detailed build/hardware results and the limits of settling estimates are in [Preview and Monitoring](docs/preview-monitoring.md).

This refines Feature-10 without closing QHY, physical unplug, manual desktop use, raw-colour hardware or lunar observing acceptance. No dependencies or licence terms change. The complete original proposal and all previous reviews remain above; `implemented features.md` is unchanged.

### QHY runtime discovery and Preview/Monitoring integration — 15 November 2026

The QHY adapter moved to `main` with the user's installed QHY All-In-One driver and DLL as its runtime. `SELENEWATCH_QHY_SDK_DIR` supplies headers for the optional build. Automatic lookup checks the application directory, the vendor's Windows installation directory and the Desktop SDK folder. The camera panel saves an explicit DLL path and reports an unusable path until the observer changes it or restores automatic discovery. The panel and inspection probe report the loaded version and path. The build uses dynamic lookup and includes no QHY runtime, import library, drivers or firmware. Any future bundling requires verifying redistribution terms.

QHY Preview supports reported exposure, gain, offset, USB traffic and transfer speed. The source worker reads back each change and notifies the Session, which resets the detector reference and history. Changes to and from exposures of at least one second restart SDK live video on the same open connection. Layout changes reopen Preview. Monitoring starts with a fresh camera open and an empty reference and history. The Session handles Stop monitoring at the next delivered frame or when the SDK returns without a frame. Bin 1, unknown loss counts and host arrival timestamps remain the QHY evidence contract.

All 11 CTest suites passed in each of eight build configurations. The real QHY runtime probe verified discovery, override and missing or incompatible DLL diagnostics with an empty device list. A combined Release run on the ASI120MM Mini matched all 10 saved native RAW8/RAW16 frames and verified restoration of the original settings. The physical QHY camera, unplug and reconnect, native Bayer validation and lunar sessions remain open. [Runtime details and qualification checklist](docs/qhy-development.md)

---

## Feature-8 — Feature lock and mount guiding

A long session only works if the Moon stays in the field. A mount's lunar rate follows the Moon's mean motion in right ascension. It does not follow the motion in declination, which reaches about a quarter of an arcsecond per second, or half the Moon's diameter in an hour. Polar misalignment, flexure, and refraction add their own drift. Without correction the watched region walks out of the frame, and the observer has to keep nudging the mount.

This feature lets the observer lock on a lunar feature, shows how far it has drifted, and, when asked, corrects the mount with small guide pulses.

Mount control was out of scope in Feature-4 and in the README. This feature reverses that on purpose. The reason is that field drift decides how long an unattended session can run, and the measurement needed to correct it (the image shift) is the same one registration needs later.

### What shapes the design

1. **A correction is a disturbance to the detector.** It moves the image under a held reference, and edges light up in the difference. So corrections are few, slow, and recorded. Guiding keeps the feature inside a tolerance box; it does not chase the seeing.
2. **A mount command is a physical action.** SeleneWatch sends guide pulses and nothing else. A pulse ends by itself, so a crash or a lost connection cannot leave the mount moving. Slew, sync, park, tracking on or off, axis rates, and rate offsets are never sent.
3. **A wrong measurement must not move the telescope.** No command is sent on a weak match, a lost lock, or a mount state the guider does not expect. When in doubt the guider stops and says why.
4. **The measurement is useful without a mount.** The lock and the drift readout stand alone. They tell the observer when to nudge by hand, and they put the field offset on record.
5. **It is testable without hardware.** The loop is closed in tests with a drifting synthetic scene and a simulated mount.

### Part A — lock and drift readout (no mount)

**Lock.** The observer drags a rectangle on the preview, or clicks to get a default box. `FeatureLock` keeps that region as a template, with the position it was taken from. A large region is binned by a whole factor so the template is at most 96 pixels on its longer side. A Bayer mosaic is always binned by an even factor, at least 2×2, so the colour pattern does not enter the match. The preview needs a mapping from widget to frame coordinates; the blip of Feature-6 adds it.

**The region must be lockable.** A flat or saturated region has nothing to match. A straight piece of limb or terminator fixes the position across the edge and not along it. At lock time the gradients of the template are checked in both directions. A region with too little contrast, or with structure in only one direction, is refused with that reason.

**Measurement.** A few times a second the guide thread takes the latest published frame and finds the template in a search window around the last found position. The match is a zero-mean normalised cross-correlation in plain loops, which does not depend on gain, exposure, or bit depth. A parabola through the peak and its neighbours gives the sub-pixel position. A measurement is valid when the peak score is above `kMinLockScore` (starting value 0.6) and the peak is not on the edge of the window.

**Smoothing.** Seeing moves a single frame by an arcsecond or two at random. The drift estimate is the median of the valid measurements of the last few seconds. Decisions use the estimate, never one measurement.

**Lock lost.** When valid measurements stop for longer than `kLockLostSeconds` (cloud, a bumped mount, the feature out of the window), the state is "Lock lost". The search widens for a while. If the feature is found again the lock resumes. Otherwise it stays lost until the observer locks again.

**Shown.** A box on the preview at the lock position and a second one at the found position. The status line shows the offset in pixels, the score, and the state. Once a calibration exists the offset is also shown in arcseconds.

**Recorded.** `events/guiding.txt` gets one line per second while a lock is held: UTC time, frame index, offset, score, state. The writer thread appends it; the guide thread never touches the disk.

### Part B — the guiding loop, on a simulated mount

**One narrow mount type.** It connects and disconnects, reports a state (connected, tracking, slewing, parked, side of pier, whether it can pulse-guide), sends one pulse (north, south, east, or west, for a number of milliseconds), and says whether a pulse is still running. It has no other motion. Two implementations exist from the start, the simulated mount and the ASCOM adapter of Part C, so the interface is justified.

**Simulated pointing.** `SimulatedPointing` is a small object holding where the telescope points relative to the Moon. A drift rate moves it. Pulses from `SimulatedMount` move it back at a guide rate, with an optional declination backlash. The synthetic source paints the disk at that offset, turned by a camera angle. The synthetic scene gains a few fixed crater-like features, because its present texture repeats every 78 pixels and a repeating pattern cannot test a lock.

**Calibration.** Guiding needs to know which way the image moves for each pulse direction, and how far per millisecond. That is measured, not entered. With the lock held and guiding off, the guider pulses west until the feature has moved a set distance, returns east, then does the same north and south, taking up the declination backlash first. The result is two image vectors, one per mount axis. Calibration fails, with the reason, if the feature does not move, if the two vectors are far from perpendicular, or if the lock is lost on the way. The result covers camera angle, mirror flips, plate scale, and guide rate in one measurement. It is kept for the run and is tied to the side of pier it was made on.

**Control.** The rule is a tolerance box with a pause.

- While the estimate is inside `kGuideTolerancePx` on both axes, nothing is sent.
- When it leaves the box, one pulse per axis is sent, sized from the calibration to remove `kGuideGain` of the error (starting value 0.7) and never longer than `kMaxPulseMs`. One axis moves at a time.
- After a pulse the guider waits for the pulse to end, for a settle time, and for a fresh estimate, before it decides again.
- Declination is not reversed for a small overshoot. The Moon's declination drift keeps one sign for days, and each reversal costs the backlash.

The decision is a plain function of the estimate, the mount state, and the time. It has no thread and no clock of its own, so a test can step it.

**Speed of a correction.** A pulse moves the image at the mount's guide rate. At half sidereal rate and 2 arcseconds per pixel that is about 0.13 pixels per frame at 30 fps, which is less than the seeing. The calibration measures this number, and the window shows it. If it is more than `kMaxGuideSpeedPxPerFrame`, guiding refuses to start and asks for a lower guide rate on the mount.

**Nothing in guiding assumes a telescope or a camera.** SeleneWatch is meant for many observers and many combinations, so the guider is given no focal length and no pixel size. Everything it needs it measures in the calibration, and its limits are checked against those measurements. The example below shows why the limits exist; it is a worked case, not a target configuration.

**A worked example at long focal length.** With 3.75 µm pixels on a 2350 mm telescope the scale is 0.33 arcseconds per pixel and the field is 7.0 by 5.3 arcminutes. That is a high-resolution setup, dominated by the seeing. Three things follow.

- The declination drift alone, up to 0.27 arcseconds per second, is 0.8 pixels per second. A feature in the middle of the frame reaches the edge in about ten minutes. Without guiding, a session at this focal length is short.
- At half sidereal rate a pulse moves the image 23 pixels per second, 0.76 pixels per frame at 30 fps. That is too fast for the detector. The mount's guide rate has to be set near a tenth of sidereal, which gives 0.15 pixels per frame. The check above is what catches this.
- A tolerance of a few pixels would be about one arcsecond, smaller than the seeing, and the guider would chase noise. At this scale the box is tens of pixels. This is why the tolerance is better stated in arcseconds or as a fraction of the field (see the open decisions).

A shorter telescope scales all three numbers down in proportion to its focal length.

### Part C — the ASCOM adapter

ASCOM is the driver layer almost every mount on Windows provides, so it is the first real mount. The adapter talks to the installed driver through late-bound COM (`IDispatch`). The driver is found at run time by its ProgID, chosen with ASCOM's own chooser dialog. No ASCOM code is copied into the repository or linked at build time.

- The adapter reads `Connected`, `CanPulseGuide`, `Tracking`, `Slewing`, `AtPark`, `SideOfPier`, `AlignmentMode`, the guide rates, and `IsPulseGuiding`. It writes `Connected` and calls `PulseGuide`. It uses nothing else.
- Some drivers block in `PulseGuide` until the pulse ends; others return at once. Both work, because all mount calls are on the guide thread and the guider waits for `IsPulseGuiding` to clear.
- The driver object is created and used on the guide thread only (COM apartment rules).
- It is built behind its own CMake switch, like a camera adapter. Without the ASCOM Platform installed the app still builds, and the mount list shows the simulated mount only.
- The ASCOM Platform's license and redistribution position is checked and recorded in `THIRD_PARTY_LICENSES.md` before the adapter is merged.
- **Known risk:** SeleneWatch is a 64-bit program, and a 32-bit in-process driver cannot be loaded into it. Connecting to one fails with a clear message. Drivers that run as their own process are not affected. If this turns out to be common, the second adapter is ASCOM Alpaca (HTTP), which reaches any driver through ASCOM Remote.
- **On the development machine the risk does not apply.** It has ASCOM Platform 7.1, and every telescope driver installed on it (EQMOD, GS Sky, Device Hub, and the simulators) is registered as its own process. The OmniSim telescope simulator is installed and is the first target for this part; the observer's mount driver follows.

### Evidence

Guiding changes no pixel, so Evidence rules 1 to 8 stand. It adds rule 9: **mount corrections are recorded.**

- Every pulse is a line in `events/guiding.txt`: UTC time, direction, duration, the estimate that caused it, and the mount state. Calibration pulses are logged the same way and marked as calibration.
- A clip's metadata records the guiding state at the trigger (off, locked, guiding, lock lost), the field offset nearest the trigger, and every pulse that falls inside the clip's span.
- A candidate whose trigger falls inside a pulse or its settle time is marked "during mount correction" in `candidates.txt` and in the clip. It is still a candidate and is still saved (Evidence rule 5). The mark tells the reviewer that the image was moving.
- The writer does the matching. It already receives clips, candidate lines, and guide lines in order, so it compares their times when it writes. `EventCapture` and the detector know nothing about the mount.

### Threads and ownership

| Thread | Owns and runs | Must never |
| --- | --- | --- |
| Guide thread | The template, the measurements, the estimate, the guiding decision, the calibration, and the mount connection | Sit on the frame path; it takes the latest frame and skips the rest |

- The source thread offers the latest `FramePtr` through a one-slot mailbox, exactly as it does for the preview. This is why the feature needs the shared frames of Feature-7.
- A driver call that hangs stalls the guide thread only. Acquisition, detection, and the window continue. The window shows "Mount not responding" when a call exceeds a time limit.
- The window sends requests to the guide thread (lock, unlock, calibrate, guide on or off, connect) and receives status. It holds no guiding state of its own.

### Safety rules

1. Guiding is off at every start. The observer turns it on, and only with a valid lock, a connected mount, and a calibration.
2. Only guide pulses are sent, each no longer than `kMaxPulseMs`.
3. No pulse is sent unless the mount reports that it is connected, tracking, not slewing, and not parked.
4. No pulse is sent on an invalid estimate or in "Lock lost".
5. A change of side of pier ends guiding and discards the calibration.
6. If `kMaxFruitlessCorrections` corrections in a row do not reduce the error, guiding stops with "Corrections have no effect". This catches a wrong calibration, a mount that ignores pulses, and a feature that is not what was locked.
7. The total pulse time per minute is capped. Reaching the cap stops guiding.
8. Stopping the stream, a source failure, or closing the window ends guiding. Because only pulses are sent, nothing has to be undone on the mount.
9. An alt-azimuth mount (`AlignmentMode`) is refused in this step; see the open decisions.

A stop by rules 5 to 7 is not resumed automatically. The observer looks, and turns guiding on again.

### Done when

1. A test locks on a feature of the synthetic scene and moves the scene by a known sub-pixel amount: the measured offset agrees within a tenth of a pixel. The same test passes on a Mono16 scene and on one with half the brightness.
2. A test offers a flat region and a straight edge: both are refused at lock time, each with its reason.
3. A test blanks the scene for longer than `kLockLostSeconds`: the state is "Lock lost" and the guiding decision sends nothing. Restoring the scene restores the lock.
4. A test runs calibration against the simulated mount with a camera angle of 30° and a mirror flip: the two vectors match the simulation. With a mount that ignores pulses, calibration fails with its reason.
5. A test runs the stepped loop with a constant drift for a simulated hour: the feature stays inside the tolerance box, the pulse count is close to what the drift and the box predict, and declination is never reversed.
6. Tests for the safety rules: slewing, parked, not tracking, a pier flip, a mount that moves the wrong way, and the per-minute cap each stop or withhold pulses as written above.
7. A test fires a flash during a pulse: the candidate is saved and marked "during mount correction", and the clip lists the pulse.
8. By hand, with the synthetic source and the simulated mount: lock, calibrate, turn guiding on, and watch the drift readout stay in the box while `guiding.txt` fills.
9. By hand, with the ASCOM telescope simulator: connect through the chooser, calibrate, and guide. Then the same on a real mount under the Moon, with the readout and the pulse log checked against what the mount did.

### Out of this step

- Slewing to the Moon, goto, plate solving, or any pointing model.
- Rate offsets or axis-rate commands. They would remove a steady drift without pulses, but they persist after a crash; see the open decisions.
- Guiding through a camera's ST-4 port. It gives no mount state, so safety rule 3 cannot be checked.
- Alt-azimuth mounts and field rotation.
- Using the measured shift to align the detector's reference. That is registration (Stage 12). The shift is recorded so that it can be added later.
- Saving a calibration between runs. It waits for the settings page.

### Open decisions

- **Template refresh.** Shadows near the terminator change over an hour, and the score will fall. The proposed rule is to replace the template with the current patch at the matched position when the score has stayed low but valid for some minutes, and to log each refresh, since each one can shift the lock point by a fraction of a pixel. It is added only if a real session shows the score decaying.
- **Rate offsets.** If pulses turn out to disturb the detector on real data, a mount that accepts tracking-rate offsets could be steered without pulses. That needs a watchdog on the mount side or an explicit decision to accept a rate that outlives a crash.
- **Alt-azimuth mounts.** The image axes turn against the mount axes through the night, so one calibration does not last. Either recalibrate on a timer or compute the rotation. Decided when someone needs it.
- **Tolerance in arcseconds.** The starting values are in pixels. The observer's setup already shows that pixels are the wrong unit: the same box is one arcsecond on the C9.25 and several on a short telescope. The calibration gives pixels per millisecond of pulse, not arcseconds per pixel, unless the mount reports its guide rate. The choice is between arcseconds (from the reported guide rate, or a focal length the observer enters) and a fraction of the field (which needs neither). Decided in Part B.

### Order

Part A needs the shared frames of Feature-7 and real recordings to try a lock on (Feature-6), and can start once those exist. It reuses the preview coordinate mapping that the blip of Feature-6 adds. Part B follows and needs no hardware. Part C needs the ASCOM Platform and is only meaningful with a camera (Feature-10). Parts A and B can run alongside Feature-9 and Feature-5.

### Part A implementation — 15 November 2026

Part A is built. Parts B and C are not started. No mount code exists yet.

**Where it lives.** `guiding/` has one job, the lock, and depends only on frames: `FeatureLock` (template, lockability check, search), `LockTracker` (the steppable state of one lock: last found position, the median estimate, the lost-lock rule), and `Guider` (the guide thread, its newest-frame mailbox, requests, and the guiding log lines). The `Session` owns the `Guider`, beside the writer it feeds. It offers each published frame after detection and does nothing more when no lock is held or requested. The window has a **Guiding** dock (View → Guiding), lock and found boxes on the preview, and a `Lock:` readout in the status bar. It holds no lock state.

**Choosing a region.** A click already selects a pixel for the test flash and the detection probe, so it stays that way. A **drag** of more than 5 screen pixels selects a region and locks on it at once. **Lock at selection** locks a 64 × 64 box centred on the selected pixel; this is the plan's "click for a default box". A lock taken while the window shows a slightly older preview frame uses the newest frame; the feature moves far less than a pixel in that time.

**Lockability, as built.** All are measured on the binned template and reported in the panel and in `guide_lock`.
- *Saturation:* refused when more than half the region's pixels are at the stream's highest code.
- *Contrast:* the template's standard deviation divided by the pixel noise. The noise comes from a 3 × 3 operator that is a second difference across times a second difference down. Smooth shading and ramps give it almost nothing and white noise gives six times its standard deviation; the median of its magnitude is used. A first try with neighbouring-pixel differences counted crater detail as noise and refused good regions. The threshold is 3: with noise in template and frame, a perfect match then scores about 1 − 1/c² = 0.89, safely above `kMinLockScore`; a flat region measures about 1.
- *Structure:* the eigenvalues of the summed gradient outer products, less the share the noise alone contributes. Refused when the weaker is below 0.1 of the stronger. Upright and tilted straight edges are refused with that reason.
- The region is clipped to the frame and must be at least 8 × 8 binned pixels on each side, so a long thin strip is refused.

**Searching, as built.** A Bayer mosaic is binned by an even factor of at least 2. The search base on a mosaic is kept at an even offset, so each block holds the same arrangement of colour sites as the template's blocks. Each search window's mean and variance come from integral images; the cross term is a plain loop over the template.

**Starting values.** Measuring interval 0.2 s of *source* time, so a recording played fast measures as it did at the telescope, and the CPU limit only skips more frames. Estimate window 3 s, at least 3 valid matches. Track radius 24 px; after `kLockLostSeconds` = 2 s without a valid match the state becomes *lock lost, searching* with a 64 px radius around the last found position. A single match while searching could be a look-alike, so the lock resumes only when two consecutive matches agree within 3 px. After `kSearchSeconds` = 60 s of searching the state becomes *lock lost* and nothing more is measured until the observer locks again.

**Across runs.** The lock survives Stop and Start, including the camera reopen when Monitoring starts. A restarted stream, a loop boundary, or a clock that goes back starts a fresh estimate and fresh timers. A frame of another size, format or colour pattern releases the lock with that reason.

**Recorded.** `guiding.txt` sits beside `candidates.txt`. Like the candidate record, it is written only while Monitoring; Preview writes nothing. Lines carry the frame's UTC time: `guide_lock` (region, binning, contrast, structure, saturated share), `guide_refused`, `guide_state` (each change, with the reason), `guide_offset` (one per second of source time: state, region and binning, so the line stands alone when the lock was taken in Preview; latest dx, dy, score, valid; the estimate or `estimate=none`; samples) and `guide_unlock`. The writer appends them in order with its other work. A batch that cannot be written is counted in `WriterUsage::guidingWriteFailures` and shown in the Guiding dock. It is not reported as a broken candidate record.

**Tests.** `SeleneWatchGuidingTests` (CTest `guiding`) uses a scene of 360 fixed pseudo-random crater-like blobs with noise. It does not use the synthetic source's texture, which repeats every 78 px.

| Done-when item | Result |
| --- | --- |
| 1. Sub-pixel shift, Mono8 / 12-bit Mono16 / half brightness | worst error 0.020 / 0.017 / 0.041 px over five shifts up to 7.3 px (limit 0.1) |
| also: 2 × 2 binned region; RGGB mosaic | 0.015 px; worst 0.053 px |
| 2. Flat region; upright and tilted straight edge | refused for contrast; refused as one direction |
| 3. Blank longer than `kLockLostSeconds`, then restored | Searching, no estimate; restored after two agreeing matches 36 px away; *lock lost* after 60 s of searching |

The tests also cover the median rejecting one outlier, old matches leaving the window, a match on the window edge being invalid, restart and layout change, and the guider writing nothing in Preview and once per second in Monitoring. A window test drags a lock on the synthetic source, unlocks it, then locks again through the selected pixel; Preview leaves the output folder empty.

**Real recordings (manual probe).** `SeleneWatchProbe lock INPUT OUTPUT.csv [X Y W H]` locks on the first frame and follows the feature at the guider's rate, reading only the frames it measures. Without a region, it picks the most lockable 64 × 64 box of a grid. Debug build:

| Recording | Frames, span | Searches valid | Score min / median | Drift (linear fit) | Scatter about the fit | Search time |
| --- | --- | --- | --- | --- | --- | --- |
| 2015.12.19 C9.25, `Moon_180548.avi`, 8-bit | 500, 27.6 s | 124 of 124 | 0.975 / 0.988 | +1.10, −0.82 px/s | 1.24, 1.49 px RMS | 24 ms |
| 2015.12.21 8" Newtonian, `Moon_165104.ser`, 8-bit | 714, 38.7 s | 177 of 177 | 0.986 / 0.991 | −1.29, −0.44 px/s | 1.89, 2.44 px RMS | 24 ms |

Findings for Part B:
- The field drifted 1.4 px/s on both nights. At 0.33″/px that is about 0.45″/s, more than the Moon's declination drift alone. A feature in mid-frame would leave a 1296 px frame in about 8 minutes. This confirms the reason for guiding.
- Single-frame seeing scatter is 1.2–2.4 px RMS. A tolerance box has to be several times this.
- The 3 s median lags a steady drift by about half its window, here about 2 px. The control rule should allow for that lag, or estimate the rate, rather than shorten the window.
- The widened 64 px search costs about seven times a tracking search. In a Debug build that is roughly 0.2 s per search, which is acceptable on the guide thread.

Still open for Part A: the earthshine side (no recording yet), a terminator region over an hour (template refresh, see the open decisions), and a check by hand at the telescope with the ZWO camera.

### Part B implementation — 15 November 2026

**Where it lives.** `guiding/mount.h` is the narrow mount type: connect, disconnect, state, and one guide pulse. `SimulatedPointing` and `SimulatedMount` are the simulation. `Calibrator` and `GuideController` are plain state machines, stepped with a `GuideObservation` (time, lock, estimate, mount state, frame interval). Each returns a `GuideDecision`: a pulse to send, and whether to restart the estimate. Neither has a thread or a clock. `Guider` (the guide thread) owns the mount, the calibration, and both machines; it reads the mount, steps the active machine, sends its pulse, and records it. The window's Guiding dock has Mount and Guiding sections and holds no guiding state.

**The simulation.** The offset is kept in arcseconds along RA and Dec. Defaults are 0.10″/s RA and 0.25″/s Dec drift, a guide rate of 3.75″/s (a quarter of sidereal), 1″ per pixel, and a 30° camera angle. Declination backlash and a mirror flip are optional; so is a mount that ignores pulses, as a test fault. Connecting the simulated mount activates the pointing and disconnecting freezes it, so the synthetic Moon moves only while that mount is in use. The synthetic scene gained 14 fixed crater-like features (dark floor, bright rim) in a ring between 0.25 and 0.85 of the radius, clear of the centre and of the fixed flash. The disk, its texture, the craters, and the flash are painted at the pointing's offset; the pacing marker stays fixed in the frame.

**Calibration, as built.**
- Settle 1 s, then a fresh estimate of at least 8 matches.
- West in 500 ms steps until the feature has moved 25 px (at most 30 s), then a fresh estimate. East for the same pulse time, then another fresh estimate.
- The two legs share the field's own drift, so their sum gives the drift and their difference the west vector, with the drift removed.
- The backlash is taken up by pulsing north until the feature moves 4 px beyond the drift (at most 10 s). Then a north leg is measured the same way, less the drift, and the mount returns south for the north and backlash time together.
- Fails, with the reason, when a leg moves the feature less than 8 px, when the two axes are more than 20° from perpendicular, when the lock is lost, when the pier side changes, when the mount may not be pulsed, or when a pulse overruns by 10 s.
- The result records both vectors, the drift, the axis angle, the backlash time, the pier side, and arcseconds per pixel when the mount reports its declination guide rate.

**Control, as built.**
- The estimate is split along the two calibrated mount axes. The tolerance box applies to each *mount* axis, not to the image's x and y, since corrections are made per mount axis.
- When an axis component passes the tolerance, a pulse removes 70 % of it. Pulses are 20–1500 ms; RA goes first and declination after it ends.
- After the pulses, the guider waits for each to end (time and `IsPulseGuiding`), settles 1 s, restarts the estimate, and waits for a fresh one.
- A declination pulse opposite to the last one is withheld unless the error exceeds twice the tolerance.
- A correction is fruitless when the fresh error is not smaller than before; three in a row stop guiding.
- Pulse time in any minute is capped at 20 s.
- A pulse is refused when it would move the image more than 0.25 px per frame, measured from the calibration and the frame interval.

**Open decision closed: the tolerance unit.** The observer sets the tolerance as a share of the frame's shorter side (default 3 %, so 14 px on the 480-line synthetic frame and 29 px on the 960-line cameras). The dock shows it in pixels, and in arcseconds when the calibration has an image scale. This needs neither a focal length nor a guide rate. The finding from the recordings (1.2–2.4 px of seeing scatter at 0.33″/px) puts the default well above the scatter.

**Evidence, as built.** Every pulse goes to the writer as a `GuidePulseRecord` (start, duration, settle, direction, calibration or guiding). Its `guide_pulse` line in `guiding.txt` is written only while Monitoring and gives the estimate, the last match, the RA and Dec error, and the mount state. The once-per-second `guide_offset` line also carries a `GuideStateRecord`. The writer keeps 15 minutes of both and does the matching itself, as the plan says:
- a candidate whose trigger falls inside a pulse or its settle time gets `during_mount_correction=yes` in `candidates.txt` and on its `event.txt` trigger line;
- `event.txt` gains `guiding_at_first_trigger` (the nearest state record within 2 s, or "no lock record") and `mount_pulses_in_clip` with one `mount_pulse` line per pulse overlapping the clip.

`EventCapture` and the detector know nothing about the mount. Calibration and guiding starts and stops, mount connects and disconnects, and failures are also logged.

**Ownership and threads.** The mount is created by a factory on the guide thread, used only there, and released there when the guider ends. While a mount is connected, the guide thread wakes at least every 100 ms to follow pulses. It reads the mount's state every step while calibrating or guiding, and once a second otherwise. A mount call running for more than 5 s shows "Mount not responding"; the window also polls once a second, because a hung call publishes nothing. `Session::start` and `Session::stop` end calibration and guiding (safety rules 1 and 8). The lock and the calibration survive a restart; a new lock or an unlock ends guiding. Disconnecting discards the calibration.

**Tests (`guiding`, 28 cases, about 4 s).**

| Done-when item | Result |
| --- | --- |
| 4. Calibration, 30° camera angle; mirrored with 20″ backlash | west −3.236, −1.903 px/s against −3.248, −1.875 simulated; mirrored 3.250, 1.822 against 3.248, 1.875; axes within 5° of 90°; 1.0″/px recovered; backlash 6.5 s taken up (20″ at 3.75″/s = 5.3 s) |
| 4. A mount that ignores pulses | fails: "does not respond to pulses" |
| 5. One hour of constant drift, 10 px tolerance, 0.5 px seeing noise | worst 10.4 px RA, 10.9 px Dec; 222 pulses against 180 predicted; no declination reversal |
| 6. Slewing, parked, not tracking, pier flip (calibration discarded), cannot pulse-guide, alt-azimuth | each stops with its reason and sends nothing |
| 6. A mount that moves the wrong way; the per-minute cap | "corrections have no effect" after three; "cap" |
| 6. Lost lock or invalid estimate | no pulse |
| 7. A flash during a pulse | saved, marked `during_mount_correction=yes`; its clip lists the pulse and the guiding state; a later clip lists none |

Also: the simulation's pulses and backlash; a guide rate too fast for the frame rate is refused. The window test connects the simulated mount, starts a calibration, disconnects, and finds the output folder empty in Preview.

**Through the real threads (done-when 8, by tool).** `SeleneWatchProbe guide-sim OUTPUT 180` runs a Monitoring session on the synthetic source with the simulated mount and the full frame path. It locks on the most lockable 64 × 64 box, calibrates, and guides for 180 s (Debug build):
- Calibration took 46.5 s. It measured west −3.247, −1.874 px/s against −3.248, −1.875 simulated, and north 1.872, −3.257 against 1.875, −3.248; axes 89.9° apart, 0.998″/px, 0.126 px per frame.
- The lock held in all 178 one-second samples.
- From where guiding started, the field stayed within 9.8 px in RA and 2.9 px in Dec (tolerance 14.4 px). Unguided, it would have drifted 18 and 45 px.
- Each correction was one 1500 ms north pulse about every 22 s. The pulse cap limited each correction to 5.6 px, so the field was held at the edge of the box rather than brought back to centre, as a tolerance box should.
- `guiding.txt` recorded every calibration and guiding pulse.

**Finding: field motion triggers the detector on the synthetic scene.** In that run the detector fired from 2.7 s after the simulated mount connected, before any pulse, and kept firing. It produced 26 candidates in chained clips, 9 of them correctly marked `during_mount_correction=yes`. The cause is the scene: the new craters have sharp rims on a noise-free render, so the per-pixel noise there is about 1.5 ADU, and 0.27 px/s of drift is many sigma against a 2 s reference. Real frames carry seeing motion and sensor noise in the noise model, but whether steady drift and guide pulses disturb the detector on real data is exactly design point 1 of this feature. It has to be checked on the first guided lunar session, before trusting unattended candidates during guiding. If they do disturb it, the open decision on rate offsets, or registration (Stage 12), is the remedy.

### Part C implementation — 15 November 2026

`AscomMount` (`guiding/ascom_mount.cpp`) uses late-bound COM by ProgID. It reads and writes only the members listed in Part C; DISPIDs are looked up once per member. The driver is created on the guide thread, in the multithreaded apartment. Errors carry the driver's own exception text. A driver that cannot be created gets the 32-bit in-process explanation. Optional members (`SideOfPier`, `AlignmentMode`, guide rates) stay unknown when a driver does not implement them. **Choose…** opens `ASCOM.Utilities.Chooser` on the GUI thread, and the chosen ProgID is remembered. The CMake option `SELENEWATCH_WITH_ASCOM` is on by default for Windows; it needs no SDK and links only `ole32` and `oleaut32`. Without the Platform, the dock offers the simulated mount only. License position: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) (Platform CC BY-SA 3.0; nothing distributed or linked).

Checked by hand with `SeleneWatchProbe ascom ASCOM.OmniSim.Telescope OUTPUT.txt`, from a 64-bit worker thread:
- Driver created in 335 ms; the first full state read took 34 ms.
- OmniSim reports German equatorial, pier east, `CanPulseGuide`, guide rates of 0.004178°/s (1× sidereal), and tracking off.
- `PulseGuide` returned within 12 ms each time (an asynchronous driver), and `IsPulseGuiding` cleared about 390 ms after each 300 ms pulse in all four directions.
- Safety rule 3 correctly reported "not tracking", so the guider would have refused to pulse.

Still open for Part C, by hand: connecting through the chooser in the window; EQMOD with the observer's mount; and calibrate and guide under the Moon. There, check the readout and `guiding.txt` against what the mount did. With OmniSim and a camera the image does not respond to the mount, so calibration correctly fails with "does not respond".

### Guide corrections and the detector on recordings — 16 November 2026

Part B's finding asked whether field motion and guide pulses disturb the detector on real data. Before the first guided session, `SeleneWatchProbe guide-pulses` measured it on five archive recordings: mare, terminator, crater rim, the largest RGB file, and a 104 s mare-and-limb file. It moves a crop of every frame the way corrections move the image, in whole pixels, every 6 s, at 0.05 to 1 px per frame for 0.3 or 1.5 s, and compares the candidates with an unmoved control in the same windows, at K 6, 12, 18 and 24. Method, tables and limits: [docs/guide-pulse-detector.md](docs/guide-pulse-detector.md); every count: [docs/guide-pulse-measurements.json](docs/guide-pulse-measurements.json).

- **At K 12 to 24, corrections at the allowed speeds add almost nothing.** In 648 corrections, no trial had more than one candidate above its control, each extra one fell inside a window the writer marks `during_mount_correction=yes`, and no trial or control had a candidate after a window. At K 18 and 24 nothing fired after start-up, even at four times the speed limit.
- **At K 6, every recording already fires unmoved** (35 to 435 candidates after start-up). Corrections add more, almost all inside the marked windows: 114 against 27 on the mare at the strongest allowed correction. After the windows, trials stay within the control's own range, so the 1 s settle time is long enough here.
- **The 0.25 px per frame limit stays.** At 1 px per frame, the mare and the crater rim fire at K 12.
- **The patch size and shape rules carry part of the protection.** On the RGB file the peak rose to 37.8 σ during corrections (control 13.3 σ) without a candidate.
- **The synthetic finding does not carry over.** The recordings' controls, with their own drift and seeing, are quiet at K 18 and 24. The synthetic scene's sharp, noise-free rims make it far more sensitive to motion than a real frame.

So for guiding, marking is enough on this evidence, and neither rate offsets nor registration is needed. Sunlit terrain, whole-pixel steps and a simulated cadence are its limits. The first guided session under the Moon, on earthshine, is still the check of Done-when item 9.

---

## Feature-11 — Sky watch

*Proposed on 16 November 2026, from a discussion with the observer.*

The same camera, ring, detector and clips can watch the open sky: a camera with a wide lens, left outside for a night, saving anything that happens. Fireballs and meteors, aircraft, satellites and lightning are what it is for.

The lunar design treats most of these as false positives (PROJECT_DESCRIPTION.md, Section 3), and its settings reject them on purpose. A meteor crossing the frame at an angle is too spread out for the box test. A bright one is larger than the maximum patch. A frame where much of the image brightens at once is never split into patches. Watching the sky also brings problems the Moon does not have: stars drift past a fixed camera, the ground has lights and trees, and an unattended night can fill a disk.

So SeleneWatch gets a second mode. **Moon** is the program as it is today. **Sky** changes what watching the sky needs and nothing else.

### What shapes the design

1. **Moon mode does not change.** Its settings, its detector and its clips stay exactly as they are, so lunar results and the measurements of Feature-9 still hold. Each Sky behaviour below is off in Moon mode.
2. **One pipeline.** Sky mode uses the same sources, ring, detector, writer and evidence rules. It adds settings and a few behaviours; it is not a second program.
3. **The mode is on record.** Every clip and every candidate line says which mode recorded it, with the Sky settings in force.
4. **A night outside runs unattended.** Nobody watches the window, so the program must not fill the disk or record one star all night.

### Part A — the mode and its settings

- A **Target** choice, Moon or Sky, on the Detection panel. It changes only while the stream is stopped, because both the detection and the capture settings change with it.
- Each mode keeps its own detection and capture settings. Switching never changes the other mode's values. The settings saved today become Moon's, so nothing changes for an observer who never picks Sky.
- The Guiding tab is off in Sky mode.
- `event.txt` and `candidates.txt` record `mode=moon` or `mode=sky`.

Starting values for Sky, to be checked on the first nights:

| Setting | Moon (today) | Sky |
| --- | --- | --- |
| Sensitivity K | 6 | 6 |
| Maximum patch | 400 px | 3000 px |
| Maximum box / patch | 4× | 30× |
| Allow a one-frame trigger | on | on |
| Pre-event history | 5 s | 3 s |
| Quiet tail | 3 s | 5 s |
| Maximum after trigger | 10 s | 20 s |

- **Box / patch 30×.** A 45° streak 30 px long and 3 px wide is about 90 pixels in a box of about 24 × 24, near 6×. At the same angle the ratio grows with the streak's length, so 30× passes a streak of about 180 px.
- **Quiet tail 5 s.** A fireball's flare and repeated lightning strokes stay in one clip.
- **Maximum after trigger 20 s.** At full frame (1280 × 960, 8 bits, 30 fps) a clip grows by about 37 MB a second. Its history and 20 s fit the clip's share of the default 2 GiB RAM ceiling. A longer event continues in a linked clip.

### Part B — a trigger for large brightenings

Lightning and a fireball's flare light a large part of the frame at once. Today such a frame cannot fire. When more pixels pass K than four times the maximum patch (and at least 4096), the frame is not split into patches. A smaller bright area is still larger than the maximum patch. On the Moon this is right, because seeing, haze and cloud do the same.

In Sky mode a frame also fires when the pixels at or above K are at least `kBrighteningShare` of the unmasked frame (starting value 2%). This is a new trigger kind, `brightening`, recorded with the share, the median rise in ADU, and the median rise in units of the noise. One frame is enough, because a lightning stroke can last a single frame.

- While the share stays above the limit, the frame counts as activity, so the quiet tail runs from the last bright frame. Repeated strokes inside the quiet tail stay in one clip.
- The background does not learn a brightening frame, so the flash does not raise the noise estimate for the frames after it. A brightening that lasts longer than the release limit of Part C is learned like a patch.
- Slow changes such as twilight, moonrise and thin cloud are learned by the background as they happen and do not reach K.
- A change larger than the maximum patch but smaller than the share fires nothing. That is where moving cloud usually falls. The first nights set both numbers.
- Masked pixels (Part D) are not counted.

### Part C — release slow drifters

A patch that is firing is not learned into the background. That is what keeps a long event on the Moon from fading into it. On a fixed camera the stars move. A bright star or planet can fire on its leading edge and then keep firing as it drifts, and the clips would continue until it leaves the frame.

In Sky mode, a patch that has kept firing for longer than `kSkyReleaseSeconds` (starting value 20 s) is no longer protected, and its pixels are learned with the normal time constant. An aircraft or a satellite crosses each pixel faster than the background learns, so it keeps firing after the release. A star drifts slowly enough to fade into the background, and its clip ends one quiet tail later. The clip records the release and its time.

Part C is needed only for a camera that does not track. On a tracking mount the stars stay still (see the open decisions).

### Part D — a detection mask

The ground, trees, buildings, street lights and passing cars do not belong to the sky. The observer drags rectangles on the preview to exclude them, and the masked area is shaded on the preview.

- Masked pixels are not tested and do not count towards a patch or a brightening.
- They are still recorded in the SER file unchanged (Evidence rule 1).
- The mask belongs to the Sky settings, and each clip lists its rectangles.
- Rectangles come first. A polygon follows only if a horizon needs it.

### Part E — a night unattended

- **Disk reserve.** Before a clip is written, the writer checks the free space on the output drive. Below `kDiskReserveBytes` (starting value 10 GiB, editable) the clip is not written. Its candidates are still logged, with `not_saved=disk reserve` (Evidence rule 5). The window says that saving has stopped and how much space is left. Today the program has no such check: a full disk shows only as failed writes, and a full system drive harms the rest of the computer.
- **Linked clips without repeated history.** Today a clip that continues a long event starts with the frames then in the ring, so it repeats up to a whole pre-event history from the clip before. An aircraft in view for two minutes makes several linked clips, and the repeats add up. In Sky mode a continuing clip starts at the frame after the last frame of the clip before, and its metadata names that clip.
- **The whole path in one picture.** The previews written today show one moment: the baseline, the peak frame, and its difference. A meteor in the peak frame is a short segment of its path. Feature-12's overview draws everything that changed during the clip on one frame, so a meteor, an aircraft or a satellite shows as its whole path. Sky mode adds nothing of its own here; it relies on that overview.

### Evidence

Sky mode changes no pixel and writes evidence the same way, so the evidence rules stand. Each clip records the mode, the mask, the release limit, the brightening share, and any release or brightening (rule 8). A clip refused for the disk reserve still has its candidates logged (rule 5).

### Done when

1. A test switches modes: each keeps its own settings, Moon's are the ones saved before this feature, and clips and candidate lines name the mode.
2. A test moves a 3-pixel-wide streak at 45° across a synthetic sky at 20 px per frame. Sky with its starting values records it as one clip from entry to exit. Moon with its starting values does not fire; that is the documented difference, not a fault.
3. A test brightens 40% of the frame for one frame, three times, 0.3 s apart. Sky records one clip with three brightening triggers, and the noise estimate after the last stroke is within 10% of the one before the first. Moon records nothing. A brightness ramp over ten minutes fires nothing in either mode.
4. A test drifts a bright point at 0.2 px per second, as a fixed camera sees a star. Once it fires, it is released after the limit, and its clip ends one quiet tail later. A point moving 5 px per frame keeps firing past the limit until it leaves the frame.
5. A test flashes inside and outside a masked rectangle. Only the outside flash fires, and the SER frames hold the masked pixels unchanged.
6. A test sets the free space below the reserve, through a seam in place of the drive. The clip is not written, its candidates are logged with `not_saved=disk reserve`, and the window says that saving stopped.
7. A test keeps an event firing for 50 s with a 20 s maximum. The linked clips' frame indices are contiguous and do not overlap, and each names the clip before.
8. A test moves a point across a synthetic sky in Sky mode: the clip's overview (Feature-12) shows a line along its whole path, with the trigger marked.
9. By hand, a night outside with the ASI120MM Mini and a wide lens: the clips by kind, the disk used per hour, whether a star or planet stayed firing, and whether the starting values hold.

### Out of this step

- Starting and stopping at dusk and dawn.
- Changing exposure and gain as the sky brightens and darkens.
- Sorting clips by kind (meteor, aircraft, satellite, lightning) from speed, duration and blinking.
- Satellite predictions, astrometric calibration of the lens, meteor trajectories, and triangulation with other stations. The UTC frame times already recorded are what those need.
- Polygon masks.

### Open decisions

- **Fixed camera or on the mount.** On the tracking mount the stars stay still and Part C can wait. A fixed camera needs it from the start.
- **Camera and lens.** The starting values in Part A assume the ASI120MM Mini with a wide lens. Another lens changes how far a meteor moves per frame and how fast the stars drift.
- **Disk reserve in Moon mode.** Proposed: yes. It changes nothing until the disk is nearly full.
- **Repeated history in Moon mode.** Moon's linked clips repeat the history on purpose, so each clip stands on its own. Sky stops this. Whether Moon follows is the observer's choice.
- **Release in Moon mode.** Not proposed. The camera follows the Moon, and a lunar event does not last 20 s.

### Order

Part A comes first, and the others build on it. Part E's disk reserve has no dependency and can be built before the rest, for both modes. Parts B, C and D are independent of each other. Feature-12 is built before this feature, so the first sky clips already have overviews.

---

## Feature-12 — An overview of every clip

*Proposed on 16 November 2026, from a discussion with the observer.*

A night can leave dozens of clips, and each has to be judged by a person: a flash on the earthshine, a plane, a hot pixel, a guide pulse. The observer needs to see at a glance what triggered, where, how it changed, and when. The clip's previews do not show that today.

- `01_baseline.png` is the last frame before the onset. `02_event_peak.png` is the frame with the strongest patch, with no mark on it.
- `03_diff_annotated.png` is that frame's difference from the background, with a red box one pixel wide around the strongest patch and its sigma in the corner. A flash patch can be 3 × 3 pixels in a 1280 × 960 frame. The box disappears in a thumbnail and is hard to find at full size.
- `01` and `02` are converted linearly over the whole sample range. On a 12- or 16-bit frame the earthshine is almost black, and a faint flash hardly shows.
- Only the strongest patch is marked. Other triggers in the same clip are not.
- No image carries the time, the frame number, or the clip's flags (during a mount correction, dropped frames, test signal).
- Each image is one moment. A flash appearing and fading, or the path of a moving object, cannot be seen.
- A dark silhouette, such as a plane, the ISS or a bird in front of the sunlit Moon, does not show in a difference that keeps only rises.

### What shapes the design

1. **One image per clip, `00_overview.png`, readable as a thumbnail and at full size.** The existing three previews stay as they are.
2. **The same layout for every clip.** The contents adapt to what happened: a flash, a moving object, a silhouette, or a manual save in which nothing is found.
3. **It is made from the clip's own frames.** A manual save, a clip that continues a long event, and the history before a trigger are covered the same way. The overview does not depend on what the detector held in memory.
4. **It is a display image, not evidence** (Evidence rule 3). The writer makes it while the clip's frames are still in memory, with its own conversion beside the writer code, and charges its buffers to the memory budget.

### The picture

```
┌───────────────────────────────────────────────────────────────┐
│  Main view: the peak frame, stretched so the earthshine       │
│  shows. Rises orange, falls blue, over the image. Each        │
│  trigger in a thick box with ticks pointing at it, numbered.  │
├──────────┬──────────┬──────────┬──────────┬──────────┬────────┤
│ before   │  −0.03 s │   0.00 s │  +0.07 s │  after   │ change │
│ (crop)   │  (crop)  │  (peak)  │  (crop)  │  (crop)  │ (peak) │
├──────────┴──────────┴──────────┴──────────┴──────────┴────────┤
│ 2026-11-16 21:43:07.512 UTC · frame 18 342 · automatic · Moon │
│ 9.8 σ · 14 px · 3 frames, 0.10 s · during mount correction    │
└───────────────────────────────────────────────────────────────┘
```

**Activity.** What changed is measured from the clip's frames with the detector's own per-pixel model: a running mean and variance with the detector's time constant, and its noise floor. The first second of the clip only learns. After that, a pixel is active on a frame when it rises or falls by at least K times its noise, with K taken from the clip's first trigger (the detector's current K for a manual save). For each frame the overview keeps how many pixels rose and fell, where they are, and how far they spread. For each pixel it keeps the largest rise and the largest fall over the clip. A slow drift of the image, unguided, is learned by the running mean as it is by the detector, so it does not paint every crater edge.

**Main view.** The peak frame is the frame with the most activity, or the detector's peak frame when there is one. It is stretched with the display's automatic stretch, so the earthshine and the sky show, and scaled to at most 1280 pixels wide.

- Pixels that rose by at least K are drawn in orange, and pixels that fell by at least K in blue. The stronger the change, the more opaque the colour, so the frame shows through. A flash is an orange dot, a meteor an orange line, a plane against the Moon a blue band with orange dots for its lights, the ISS a thin blue line.
- Each trigger is marked after scaling, so the marks keep their size: a box at least 24 pixels across with a margin around the patch, four ticks pointing at it from outside, and its number when there are several.
- In Sky mode the masked area (Feature-11) is hatched.

**Film strip.** Up to six cells, each a crop enlarged without smoothing, so single pixels stay visible.

- The cells are the frame before the activity starts, up to three frames spread over the activity (always including the peak), and the frame after it ends. The last cell is the change at the peak, rises orange and falls blue.
- Each crop is centred where the activity is on that frame, so the strip follows a moving object. A crop covers the activity's spread with a margin, and is at least 32 pixels across. For a flash every crop is in the same place, and the strip shows it appear and fade.
- Each cell is labelled with its time from the first trigger and its frame number.

**Caption.**

- The UTC time and frame number of the first trigger, the trigger kind (automatic, manual, a continuation), the mode, and the number of triggers.
- The peak in sigma, the patch size in pixels, and how many frames the activity lasted and for how long.
- For activity that moved more than a few pixels: its direction and speed in pixels per second, and in arcseconds per second when the image scale is known.
- Flags: during mount correction, dropped frames inside the clip, test signal, a clip limited by RAM.
- When no activity is found, for example in a manual save, the caption says so, and the main view is the save's frame with no marks.

### Also for recordings and old clips

`SeleneWatchProbe overview <SER file> <PNG file>` makes the same picture for any SER file, without triggers. The tests use it, and the observer can use it on archive recordings and on clips written before this feature. It writes only where it is told, never into a clip folder (Evidence rule 7).

### Evidence

The overview changes no pixel and no evidence file. It is listed in the clip's metadata like the other previews, with the frame it shows as its main view and the K it used. Unlike the other three previews, whose failure fails the clip today, an overview that cannot be made (for example when the memory budget refuses its buffers) leaves a complete clip, with the reason in its metadata. A review aid must not cost the evidence.

### Done when

1. A test fires a one-frame 3 × 3 flash on a dim Mono16 scene. The overview exists, its mark is at least 24 pixels across at full size and still visible when the image is scaled to 320 pixels wide, the strip shows the flash appear and fade in the same place, and the caption gives the UTC time, frame number and sigma.
2. A test moves a bright point across the frame. The main view shows an orange line along its whole path, the strip's crops follow it, and the caption's speed is within 10% of the true speed.
3. A test moves a dark disk across a bright scene and saves it with Save now. The main view shows a blue path, and the strip shows the disk.
4. A test saves a clip with Save now while nothing happens. The overview says that no activity was found.
5. A test drifts the scene at 1 pixel per second with no event. The main view marks almost nothing: fewer than 0.1% of the pixels.
6. A test with two separate triggers in one clip marks and numbers both.
7. A test runs the writer under a tight memory budget. The overview's buffers are charged to it, and a refusal leaves a complete clip with the failure noted.
8. The probe command makes an overview of an archive recording.
9. Measured: the time the overview adds to writing a 10 s full-frame clip, in Release, recorded here.
10. By hand: the overviews of real lunar clips, reviewed by the observer.

### Out of this step

- A browser for the clips in the window (Stage 12).
- An index of a night's overviews in one page or contact sheet.
- Animated previews.

### Order

This feature comes before Feature-11 and Feature-13, so the clips they produce are reviewed with it from the start.

### Implementation — 16 November 2026

Built. Done-when items 1 to 9 pass; item 10, the observer's review of real lunar clips, is open.

**Where it lives.** `recording/clip_overview` measures the activity and draws the picture; it knows nothing of clips or triggers. `EventWriter::writeOverview` turns a clip into an `OverviewRequest` (the first trigger's K and model, its onset, the detector's peak frame, the trigger boxes, and the caption lines) and saves `00_overview.png` after `event.ser` is closed and before the frames are released. `event.txt` gets an `overview:` line: the main frame, K, the active frames and the changed pixels, or why it was not written. `SeleneWatchProbe overview` (`tools/overview_probe`) makes the picture of any recording. No dependency was added.

**Changes from the proposal, and why.**

- **Learning.** The overview learns for the first second of the clip, or until the first trigger's onset if that comes sooner. A clip whose trigger falls in its first second would otherwise show nothing.
- **A run stays in one place.** The first picture of an archive recording (the mare file, K 6) chained the seeing on 29 frames across 3.6 s into one run and reported a motion of 76 px/s. A frame now joins a run only when its activity is within 48 pixels, or four times its spread, of the run's nearest frame; activity elsewhere in between is passed over. A motion is reported only for a run of three frames or more, because two frames can be seeing on neighbouring edges (the RGB file showed this). The run on the mare is now one frame.
- **The change cell** shows the clip's change around the peak, not the peak frame's change alone. For a flash they are the same; for a moving object it shows the path through the crop.
- **No arcseconds per second.** The writer does not know the image scale. It stays open until the scale is known to the writer (the guiding calibration, or an entered focal length).
- **Scale.** A frame narrower than 640 pixels is enlarged by a whole factor; one wider than 1280 is reduced, and each picture pixel then takes the strongest change in the block it covers, so a one-pixel flash survives.
- **Colour.** The main view is debayered as the display does. The strip averages a Bayer mosaic over its 2 × 2 cells, so the filter pattern does not show when enlarged. The activity is measured on the mosaic, as the detector measures it.

**Tests.** A new suite, `overview` (8 tests): a one-frame 3 × 3 flash on a dim Mono16 scene, marked and visible in a 320-pixel thumbnail, with the strip before, at and after it in one place (item 1); a 3 × 3 point at 90 px/s, its path coloured end to end and its speed within 10% (item 2); a dark disk on a bright scene without a trigger, reported as a fall at 120 px/s (item 3); a Save now with nothing in it (item 4); a textured scene drifting 1 px/s, under 0.1% of pixels marked (item 5); two triggers, both marked (item 6); the writer saving the overview beside the evidence; and a budget that refuses the overview's buffers, leaving a complete clip that says `overview: not written: RAM limit` (item 7). Two older writer tests now count four PNG files in a clip folder instead of three. All 13 suites pass in Debug and Release.

**Measured** (Release, Ryzen 9 9955HX, one writer thread; item 9):

| Recording | Frames | Overview |
| --- | --- | --- |
| `2015.10.26 Moon/raw/Moon_200059.avi`, 1296 × 964 Mono8 | 150 / 300 / 450 | 238 / 465 / 636 ms |
| `2012.11.25 moon 25.11.2012/raw/alccd5_00023.AVI`, 1280 × 1024 RGB888 | 300 | 574 to 657 ms |

About 1.4 ms a frame at 1.25 megapixels, so a full-frame clip of 5 s history and 10 s after the trigger at 30 fps adds about 0.6 s to its write. Item 8: the probe made the pictures above, and `SeleneWatchProbe capture` on the crater-rim recording (`2015.12.21 Moon 8 Newt`, K 6) wrote three clips, each with its overview. Their 76, 29 and 32 numbered marks, scattered along the crater rims, show at a glance that those clips are seeing.

---

## Feature-13 — A trigger for darkening

*Proposed on 16 November 2026, from a discussion with the observer.*

The detector counts a pixel only when it gets brighter. That is right for an impact flash. But a plane, the ISS, a satellite or a bird in front of the sunlit Moon is a dark silhouette, and the pixels it covers get darker. Today such a transit is never a candidate. Only its bright parts can fire: a plane's lights against the earthshine, the sunlit ISS beside the Moon, or by chance the shimmer from a plane's exhaust along a crater edge. Transits are among the events the project means to record (PROJECT_DESCRIPTION.md, Section 3), and the observer can catch an unexpected one today only with Save now.

### What shapes the design

1. **The same test with the sign reversed.** A pixel counts as dark when it falls by at least K times its own noise, measured by the same per-pixel model. Dark pixels form their own patches. A rising patch and a falling patch are never merged, even when they touch, because a moving object makes both and each is reported for what it is.
2. **Silhouettes are larger than flashes.** At the observer's 0.33″ per pixel, the ISS at 400 km is about 170 pixels across. A plane 40 km away is several hundred pixels across, and one overhead is larger than the 7′ field. A bird near the telescope is far out of focus: a large, faint, soft shadow. Dark patches have their own maximum size and box ratio.
3. **They cross fast.** The ISS crosses a 7′ field in about a tenth of a second, three or four frames at 30 fps. A plane takes from a few frames to about a second. The persistence rule stays as it is: one frame, or two consecutive frames.
4. **It is measured on recordings before it changes Moon mode.** Seeing darkens a crater edge as often as it brightens it, so the dark side may fire as often as the bright side does on sunlit terrain. Darkening is off by default in both modes until the measurement in Part C decides.

### Part A — dark patches

- A checkbox, **Also trigger on darkening**, in the Detection panel, saved per mode (Feature-11).
- Falling pixels at or above K form 8-connected patches, tested against **Minimum patch** (shared with the bright side), **Maximum dark patch** (starting value 20 000 px) and **Maximum dark box / patch** (starting value 10×).
- The persistence rule, the one-frame option, and the following of a moving patch from frame to frame are those of bright patches.
- A firing dark patch is protected from learning like a bright one, so a silhouette that stops in the field is not learned as background at once. Feature-11's release limit applies to it in Sky mode.
- The search for dark patches has its own bound, like the bright one: four times the maximum dark patch, and at least 4096 pixels.
- The trigger is recorded as `darkening`, with the size, the box, the deepest fall in sigma and in ADU, and the position of the deepest pixel.
- The Detection panel's readouts show the dark side beside the bright side: the deepest fall in sigma, and the size of the largest dark patch.

### Part B — large darkenings

A silhouette larger than the maximum dark patch, such as a plane overhead or a bird filling the field, darkens a large share of the frame. Feature-11 Part B adds a trigger for a large share that rises; this part adds the same for a share that falls, `kDarkeningShare` of the unmasked frame. It is off by default in both modes, because a cloud edge crossing the field does the same, and the first measurements decide whether it is worth having.

### Part C — measurement on recordings

The probe that measured guide corrections (`docs/guide-pulse-detector.md`) runs the same five archive recordings with darkening on, at K 6, 12, 18 and 24.

- The count of dark candidates against bright candidates, per recording and per K, after start-up.
- A synthetic silhouette injected into the same recordings: a dark ellipse at the size of the ISS and of a distant plane, crossing in 3 and in 15 frames, at several depths. The share of crossings that fire, per K.
- The result is written in `docs/darkening-trigger.md`, with every count in a JSON file beside it, and summarised here. It decides whether darkening is on by default in Moon mode, and the starting values of the dark limits.

### Evidence

Darkening changes no pixel. A dark trigger is a candidate like any other: it is logged in `candidates.txt` with its kind, saved as a clip, and described in `event.txt` with the dark limits in force (rules 5 and 8).

### Done when

1. A test darkens a 5 × 5 patch for two frames on a bright synthetic scene. With darkening on, it fires as `darkening`; with it off, nothing fires. The existing flash tests pass unchanged.
2. A test moves a dark disk 40 pixels across a bright scene at 30 pixels per frame. One clip, with its triggers recorded as `darkening`.
3. A test puts a rise and a fall side by side on one frame. Two patches, one of each kind, never merged.
4. A test darkens 30% of the frame for three frames with the large-darkening trigger on: one trigger. A darkening over 30 s fires nothing.
5. A test fills the frame with dark pixels beyond the search bound. The search stops at the bound and says so, as the bright search does.
6. The overview (Feature-12) of a dark trigger shows the path in blue.
7. Part C is measured and recorded, and the defaults are set from it.
8. By hand: a real transit, a plane or a predicted ISS pass, recorded and reviewed.

### Out of this step

- Telling a plane, a satellite, a bird and a cloud apart.
- Predicting ISS and satellite transits, and recording around a predicted time. That is continuous recording (PROJECT_DESCRIPTION.md, Section 7).

### Order

After Feature-12, so dark triggers are reviewed with the overview. Part A and its tests first, then Part C on recordings, then the defaults. Part B shares its code with Feature-11 Part B and is built after it.
