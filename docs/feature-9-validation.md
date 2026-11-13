# Feature-9 validation — 14 November 2026

The detector, controls, overlays, persistence, and evidence metadata are implemented. Acceptance item 7 remains open: no suitable earthshine sequence was identified in the archive survey, and the available bright-limb test saturates before an unclipped sensitivity comparison is possible. Feature-9 therefore remains in `features.md`. These measurements describe short recording prefixes, not a calibrated observing prescription or a verified impact rate.

## What was built and revised

The source thread owns a float running mean and variance per pixel. Mono16 uses every significant bit in native ADU, including high-aligned samples; RGB uses the channel mean and Bayer stays raw. The noise floor is `max(0.5 ADU, 1.4826 × MAD)` on at most 4096 regularly sampled residuals, after removing their median. Quiet frames learn cumulatively during startup, then exponentially with a two-second time constant in source time. The first frame initializes the reference and cannot trigger. Passing frames and open clips freeze mean and variance; the robust floor still updates.

Defaults are K=6, 8–400 pixels, bounding-box area at most four times the count, and one-frame triggering enabled. Threshold comparison is inclusive. A run fires once and rearms after a rejected frame; disabling the one-frame path requires two consecutive passing frames at the same settings. A live settings edit clears a pending confirmation and does not duplicate an already-triggered run.

Two changes deliberately revise the proposal:

- Decisions and reference updates use two passes. The whole-frame patch decision must be known before any reference sample changes; this avoids a rollback buffer.
- A sparse 8-connected patch search replaces the single global bounding box. A regression showed that one unrelated moving/hot pixel could otherwise veto a compact flash. Plain C++ binary searches over the threshold positions need no new dependency. Work is capped at `max(4096, 4 × maxPixels)` positions, or the image size if smaller; denser changes are rejected before component work. The strongest passing patch is reported; when none passes, the strongest rejected patch is reported. Very dense interference can still mask a flash. Only one selected patch/run is reported per frame.

The synthetic pacing marker now has radius one (five pixels), below the default minimum; the fixed flash retains radius eight. The detector has no special knowledge of the synthetic disk.

The Detection dock has logarithmic sliders with exact spin boxes, current/recent markers, ten seconds of source-time peak history, and sigma/noise overlays. Settings are copied before each frame and saved at Stop/close. A frame and its immutable diagnostics/map travel together through the preview mailbox, normally at most 20 Hz, also on a trigger. Every source frame is analysed. Maps are made from the exact pre-update values used for that frame. Hiding the dock disables map requests and leaves detection running. A selected test-flash centre shows the requested amplitude divided by its measured local noise.

Each trigger retains its settings, local-normalized peak, noise floor, patch size, and coordinates. The strongest passing frame retains its actual difference from the held reference. `03_diff_annotated.png` scales the positive difference for display and marks the patch; `event.ser` receives unchanged native bytes. The peak frame and its own settings are identified separately from the first trigger. An injection appearing after the first SER frame still sets the source-wide test marker. Refused writer jobs release their difference buffers as well as their frames.

The software revision header refreshes on every build, not only at CMake configuration. An isolated temporary Git repository check verifies clean and dirty revisions, a new commit without reconfiguration, and no header rewrite for an unchanged revision. Incremental builds therefore cannot silently retain the earlier clean revision after source edits.

## Automated and window checks

All five CTest targets pass in Debug and Release: core, preview, SER, detection, and UI. The final pre-commit runs took 22.38 s and 13.20 s respectively. Checks include:

- Exact-threshold and just-below-threshold patches, isolated hot pixels, broad/scattered changes, and a flash alongside an unrelated brighter pixel.
- Quiet/noisy halves: equal ADU additions behave differently; equal local-noise additions are recovered in both. A three-ADU signal in 12-bit data fires even though it is below the preview's eight-bit resolution.
- Two-frame persistence, held references, next-frame settings, original trigger/peak settings after later edits, continued detection during collection/writing, stop/EOF, and clips flushed at loop boundaries.
- Overlay toggles leave decisions and bytes unchanged. The mailbox retains a matching frame/measurement pair.
- The actual `MainWindow`, rendered with Qt's offscreen platform: sliders, both overlays, selected-centre noise readout, a one-frame injection with the dock hidden, resize, stop, destruction, and settings restored in a second window. Unmodified frames read back from its SER equal the original synthetic painter byte for byte. This also caught and fixed a dock visibility callback during base-window destruction.

The UI test uses its own settings namespace and temporary event directory. Rendered windows and manual logs are under ignored `out/qa-feature9/`; native Windows input automation was unavailable, so this is Qt window testing rather than a manual mouse-driven desktop check.

## Recording selection and method

The archive `D:/space/02_Planetary` was opened read-only. A first-frame survey of 241 paths selected by lunar names opened 240; the compressed CRAM file was refused. Contact sheets showed bright terrain, limbs, terminators, and clipped dark sky. None was identified as a usable earthshine exposure. The sole Mono16 archive file, `2013.06.25 ISS/raw/Moon_202643.ser`, shows a noisy field rather than suitable lunar terrain. It validates format handling, not earthshine sensitivity.

The measured files, relative to that archive, are:

| Label | Recording | Prefix / nominal duration | Format |
| --- | --- | --- | --- |
| Mare | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_180548.avi` | frames 0–239 / 13.278 s | 1296×964 Mono8, 18 fps |
| Terminator and bright limb | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_181337.avi` | frames 0–239 / 13.278 s | 1296×964 Mono8, 18 fps |
| Crater rim | `2015.12.21 Moon 8 Newt/raw/videos and stacked/Moon_165506.ser` | frames 0–239 / 13.042 s | 1296×964 Mono8, trailer times |
| Largest RGB | `2012.11.25 moon 25.11.2012/raw/alccd5_00023.AVI` | frames 0–135 / 13.5 s | 1280×1024 RGB888, 10 fps |

AVI intervals are nominal, not measured exposure times. The older RGB AVI has no known absolute start. Each case begins with a fresh model and uses the same `EventCapture` as the app, including held references and clip tails. A read-only cache capped at 512 MiB excludes USB reading from detector timings; it limits RGB to 136 frames. These are deterministic recovery trials at one position/time per region, not recovery probabilities. Counts include startup. “Background” means a candidate outside an injected signal; no ground-truth classification of natural events is claimed.

## Candidate rates and sensitivity

With minimum 8, maximum 400, and one-frame triggering on:

| Recording | Background candidates at K=6 | Candidates/minute over prefix | A tested K with zero candidates |
| --- | --- | --- | --- |
| Mare | 8 | 36.15 | 18 |
| Terminator / bright limb | 10 | 45.19 | 24 |
| Crater rim | 21 | 96.61 | 12 |
| Largest RGB | 2 | 8.89 | 24 |

Zero over roughly thirteen seconds does not establish a small long-session false-alarm rate. Raising K is also not monotonic here: it changes which components pass and when the reference freezes. For example, the terminator prefix gives 1 candidate at K=12, 40 at K=18, and zero at K=24. Recalibrate each setting from a fresh run rather than assuming a higher K can only remove candidates.

Gaussian injections use FWHM=5 pixels, one and three frames, at source position 180 (RGB: 80). The amplitude grid is K, 1.25K, 1.5K, 2K, 3K, and 4K times the centre's noise on the preceding frame. Recovery requires a trigger during injection whose selected peak pixel was actually brightened; a candidate elsewhere does not count. All tested no-injection prefixes at the listed K are quiet.

| Region / centre (x,y) | K | Centre noise before injection (ADU) | Largest tested miss → smallest tested recovery, requested peak | Actual centre addition at recovery |
| --- | --- | --- | --- | --- |
| Mare (800,600) | 18 | 1.600 | 22.5σ / 36.01 ADU → 27σ / 43.21 ADU | 43 ADU, unclipped |
| Terminator (700,700) | 24 | 5.033 | 30σ / 150.98 ADU → 36σ / 181.18 ADU | 181 ADU, unclipped |
| Crater rim (310,440) | 12 | 8.773 | 15σ / 131.59 ADU → 18σ / 157.91 ADU | 158 ADU, centre unclipped |
| Bright limb (900,200) | 24 | 8.800 | 48σ / 422.38 ADU → 72σ / 633.57 ADU | 122 ADU, saturated |
| Largest RGB (640,400) | 24 | 4.889 | 48σ / 234.68 ADU → 72σ / 352.02 ADU | 132 ADU, saturated |

One- and three-frame trials had the same tested recovery bracket at each position. Three-frame additions vary with the underlying pixels; the table gives the one-frame result. “Centre unclipped” does not assert every neighbouring sample is unclipped. The three unsaturated-centre results are brackets, not exact faint limits. The bright-limb and RGB requested values above 255 ADU broaden saturated wings and are **not** physical peak measurements or meaningful unclipped faint limits. A peak above K can still fail because fewer than eight neighbouring pixels pass. The measured normalized peak also differs from requested amplitude/noise because the current frame differs from the held mean and the floor updates each frame.

A disk-writing control on the mare prefix at K=18 saved no event folders without injection. Adding the one-frame 43.21-ADU blip saved exactly one complete folder: 149 preceding frames, the trigger frame, and 30 following frames. Its sidecar identifies 11 threshold pixels, a 27.1115σ peak, all settings, and exactly one injected frame. Installed Siril 1.4.4 independently opened the 224,883,538-byte, 180-frame SER as 1296×964 Mono8, read its test marker, reported 18 fps and ordered timestamps, and read UTC start 2015-12-19T17:05:50.055222. The trigger is frame 180 at 17:05:58.333 UTC (nominal AVI/log timing).

The annotated unmodified-recording differences show widespread crater rims and the limb, consistent with image motion/seeing against the held reference. This is an inference from the image structure, not a measured separation of global drift and local seeing. A whole-frame registration experiment should move ahead of camera deployment; these data do not justify a region settings UI. Earthshine versus bright-limb agreement in local-noise units remains unanswered. Obtain suitable unclipped earthshine/bright-limb data and repeat the trials before claiming that one K serves both.

## Processing cost

Release MSVC build, Qt 6.12.0, AMD Ryzen 9 9955HX, no concurrent builds/tests. K=24, quiet prefixes, maps requested **every frame** for the two overlay rows. Times cover detection including requested map construction; GUI colouring, reading, and disk output are excluded. Collection adds less than 0.01 ms/frame in these quiet cases. This is not an end-to-end camera guarantee.

| Recording | Map | Mean ms/frame | 95th percentile | Maximum |
| --- | --- | --- | --- | --- |
| 1296×964 Mono8 | None | 3.577 | 3.666 | 3.880 |
| 1296×964 Mono8 | Sigma | 4.230 | 4.313 | 4.421 |
| 1296×964 Mono8 | Noise | 4.276 | 4.563 | 8.931 |
| 1280×1024 RGB888 | None | 5.050 | 5.130 | 6.891 |
| 1280×1024 RGB888 | Sigma | 5.666 | 5.816 | 7.747 |
| 1280×1024 RGB888 | Noise | 5.635 | 5.742 | 5.998 |

The largest archive frame is the RGB case. Both formats stay below their nominal 100 ms / 55.6 ms frame periods on this machine, so no separate detection thread is needed from these measurements. The mean/variance occupy eight bytes per pixel (about 10 MiB here); an enabled full map or retained peak difference adds four bytes per pixel. Frame storage, sparse patch work, and queued clips add to that. The existing “MB live” readout counts frame-pool bytes only; Feature-5 owns the full RAM budget.

## Reproduction

Build `SeleneWatchProbe` in Release with the configured Qt runtime on PATH. It is a manual tool, excluded from CTest; always choose workspace output paths. For a single trial:

```powershell
out/build/release/SeleneWatchProbe.exe measure INPUT.avi out/trial.csv out/trial.json
```

Example config:

```json
{"frameLimit":240,"sigma":18,"minPixels":8,"maxPixels":400,"oneFrame":true,"x":800,"y":600,"fwhm":5,"atFrame":180,"amplitudeSigma":27,"duration":1,"saveEvents":true}
```

Omit both `amplitude` and `amplitudeSigma` for the control. `measure-batch INPUT OUTPUT CONFIG.json` takes `{"sourceConfig":{...},"cases":[{"name":"quiet"},{"name":"blip","amplitudeSigma":27}]}`; it reads once and restarts the capture for each case. `survey LIST.txt OUTPUT` produces a manifest and first-frame thumbnails. A trial writes per-frame CSV and a JSON report. The recorded measurement subset is in [feature-9-calibration.json](feature-9-calibration.json); full local configs, CSVs, SERs, images, and Siril log are in `out/qa-feature9/`. No new runtime dependency or license change was introduced.
