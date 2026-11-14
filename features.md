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

### Out of this step

- Hardware camera SDK integration (Feature-10).
- Advanced automated transit / silhouette tracking detectors (optical flow / Kalman filtering).
- Lunar limb sub-pixel registration.

### Order

After Feature-9 and before Feature-10. If the recordings used for Feature-9 are 16-bit and too dark to place a blip on with the fixed shift, the stretch moves ahead of Feature-9; nothing in it depends on the trigger.

---

## Feature-10 — First light: camera adapters (ZWO, then QHY) and manual save

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

### Out of this step

- Guiding through a camera's ST-4 port.
- GPS-stamped frame times, until a camera with GPS is in hand.
- Cooling control, filter wheels, and long single exposures.
- Cameras other than these two. Each later family is its own adapter, added when an observer has the hardware.
- Continuous recording and the full time model (the session record stage).

### Order

After Feature-5, so the preview can show what the camera is pointed at. ZWO first, then QHY, then the shared helper. Manual save can be built at any point after Feature-7.

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
