# Feature-9 reference/rearming and Feature-10 manual capture — 14 November 2026

The approved implementation batch is complete: background learning no longer stops because a clip is open, separate compact patches have independent persistence, and **Save now** uses the existing history and evidence writer. Registration remains a manual experiment. Feature-9's observing calibration, Feature-5's real earthshine display check, and Feature-10's camera work remain open.

## Reproduced faults and the reference decision

Three new regressions failed on the previous implementation: a second spatially separate flash could not trigger while a stronger patch stayed active; two unrelated patches in successive frames could incorrectly confirm each other in two-frame mode; and a clip's quiet tail prevented learning noise in an unrelated quiet region. All three pass with the revised policy.

The earlier Feature-9 proposal deliberately froze the whole mean/variance while a passing frame or clip was active. The registration experiment exposed how that can hold a cold or stale model and conceal later events behind one global firing state. This implementation revises that policy explicitly:

- Every frame is measured before its reference changes. The first frame initializes the model; there is no additional warm-up interval that disables detection.
- Each compact passing patch protects its bounding box plus one pixel. Mean and variance elsewhere update at the existing cumulative startup / two-second exponential rate, including while a clip is collected or written. Pending two-frame patches are protected too.
- Adjacent or overlapping boxes inherit persistence. Merges/splits inherit an already firing state instead of making duplicate events. Spatially separate patches can start new candidates while another remains active. A source-index gap breaks pending two-frame confirmation.
- Every new passing patch run is retained, including multiple candidates in one frame. The displayed summary selects the strongest new candidate, then the strongest already firing/passing patch. A stronger old patch cannot hide a separate new candidate from the log. The clip's peak snapshot independently retains the strongest passing patch, so highlighting a weaker new candidate does not lose that stronger peak.
- Analysis and reference update are explicit phases. `EventCapture` copies a peak's actual pre-update difference between them. Native published frames remain immutable. The charged one-byte-per-pixel protection mask shares the existing image/analysis budget.

The detection panel shows active patch count, longest active duration, and protected pixel count. Trigger metadata includes those diagnostics and the model's learning-frame count. That count advances only when background pixels can learn; it does not claim every protected pixel has learned. The held region is reported separately. No extra tuning controls or dependency were introduced.

This is spatial persistence, not object classification or a complete transient detector. Connected/overlapping activity can still conceal a new rise inside the same patch; a merge cannot distinguish independent events that become connected. Dense threshold changes still encounter the existing bounded sparse-search policy. Long or drifting activity can remain active locally. These limits must be judged with injected recovery and passing occupancy, not candidate count alone.

## Constructed and window verification

The eight CTest targets pass in Debug and Release. Existing native-depth, noisy-half, overlays, evidence, duration, continuation, memory-pressure, source-boundary, and window tests remain in place. New detector cases cover the three reproduced faults, every simultaneous candidate, dropped-frame confirmation, merge/split persistence, and the pre-update snapshot contract.

A 359.9-second constructed run keeps one stronger 3×3 patch active and introduces 98 separate one-frame flashes beside it, including at the first analysed opportunity. All 98 flashes and the initial persistent patch produce the expected 99 candidates, with no duplicate candidate for the persistent patch. Collection continues through more than 300 capped clips. At most 50 of 3072 pixels are protected; unrelated background keeps learning. This is an explicitly constructed TEST sequence, not a six-minute observing-rate measurement.

Manual tests cover history and source-time tails, merging manual and automatic requests without stopping detection, preserving a manual tail across an existing clip's duration cap, and pending requests at Stop, EOF, source failure, and loop boundaries. A real controlled worker makes those boundaries deterministic. An accepted request with no acquired frames reports failure explicitly rather than silently disappearing.

Writer tests compare every saved native byte and timestamp for Mono8, low/high-aligned 12-bit Bayer words, and RGB. A writer refusal still records manual pending/outcome lines. The actual-window test opens a constructed high-aligned 12-bit SER, clicks Save now, waits for a complete manual clip, stops, verifies every saved native frame, and checks the original input bytes remain unchanged. Its inspected screenshot is `out/qa-manual/window.png`. Window tests use Qt offscreen rendering; no manual desktop interaction is claimed.

## Complete-recording checks

The manual `rearm` probe streams each of the four selected archive recordings once. Each contains 500 frames. For each sigma setting it runs a fresh control and a fresh injected trial on the same original frames. Injections last one frame, FWHM five pixels, at positions 1, 90, 180, 300, and 450. Coordinates change between trials in time, as specified in the versioned JSON; they are raw-frame coordinates and do not follow registration. Every candidate is checked, and recovery requires a **new** candidate whose peak sample was actually brightened. Counts are never subtracted to infer recovery.

| Scene | Source-time span | Requested peak | K=6 control: candidates / passing frames / longest firing run | K=6 matched injections | Higher K / matched injections |
| --- | --- | --- | --- | --- | --- |
| Mare | 27.722 s | 64 ADU | 127 / 392 / 385 | 4/5 | 18 / 4/5 |
| Terminator | 27.722 s | 128 ADU | 333 / 498 / 498 | 4/5 | 24 / 1/5 |
| Crater rim | 27.224 s | 192 ADU | 137 / 171 / 70 | 5/5 | 12 / 4/5 |
| Largest RGB | 49.900 s | 128 ADU | 249 / 341 / 306 | 5/5 | 24 / 0/5 |

All higher-K controls have zero candidates/passing frames over these short sequences. Both trial types update background on all 499 frames after initialization. At K=6, maximum protected areas in the controls are 3095, 6387, 1982, and 4590 pixels respectively: at least 99.48% of each field remains available for background updates. Several matched injections occur with another firing run already active; this directly checks the original rearming problem.

These results do **not** establish reliable universal sensitivity or a quiet K=6. The first injected opportunity is missed at K=6 in the mare and terminator, while the higher K recovers it; changed threshold connectivity means raising K is not a monotonic recovery rule. Later higher-K misses coincide with position-dependent learned noise. Rim centre additions range from 146 to 192 ADU, with some saturated; RGB additions are only 87–120 ADU because of saturation. The RGB injected run remains active for 499 frames, versus 306 consecutive frames in the control, illustrating that an early injected event can change later model/persistence history. Recovery is therefore a measured outcome for these sequences, not a probability or proof of no missed flash. Suitable unclipped earthshine and bright-terrain data are still needed.

Native SER trailer times are used for the rim. FireCapture AVI uses its logged start plus nominal frame rate; RGB AVI uses nominal rate from an unknown start. The archive was opened read-only, and each file's size/modification time was checked unchanged before/after the run. These checks are not a whole-file hash comparison.

## Registration comparison after the correction

The earlier 120 registration cases were repeated into a **new** output directory with identical prefix lengths, coordinates, shifts, amplitude grid, and 64-pixel crop. The original reports remain intact. At K=6 the aligned controls change as follows:

| Scene | Previous passing frames | Revised passing frames | Revised longest firing run |
| --- | --- | --- | --- |
| Mare | 238/240 | 15/240 | 11 frames |
| Terminator | 226/240 | 27/240 | 14 frames |
| Crater rim | 239/240 | 15/240 | 13 frames |
| Largest RGB | 135/136 | 135/136 | 135 frames |

The mare/terminator/rim result now measures fewer passing frames, rather than simply fewer global run starts. The RGB result still fails that comparison. Revised candidate totals count **all new patch runs**, including several in one frame; previous totals counted one global run per frame path, so their totals are not interchangeable. At the quiet comparison settings, the tested cropped/aligned recovery brackets remain as before: mare 32→64 ADU, terminator 64→128 ADU, rim aligned only at 192 ADU (actual 146, saturated), RGB aligned only at 128 ADU (actual 113, saturated). This is further evidence for continued experiments, not acceptance for production registration.

## Cost, reproduction, and remaining gates

Release MSVC / Qt 6.12.0 on AMD Ryzen 9 9955HX, with no concurrent builds/tests. The four full-stream trials are interleaved per source frame. Detector times include measurement, patch tracking, mask, and reference update; they exclude reading, injection/copy, clip snapshot work, GUI, and disk output.

| K=6 unmodified control | Mean / 95th percentile detector time |
| --- | --- |
| Mare Mono8 | 4.790 / 6.536 ms |
| Terminator Mono8 | 5.243 / 8.930 ms |
| Rim Mono8 | 4.887 / 8.777 ms |
| Largest RGB | 6.415 / 8.374 ms |

These are below the selected recordings' nominal 55.6/100 ms periods on this machine; they are not an end-to-end camera or sustained-disk guarantee. The protection mask adds one charged byte per pixel. Patch metadata is bounded by the existing sparse search and is additional container metadata under the documented payload-budget scope.

`SeleneWatchProbe rearm INPUT OUTPUT.json CONFIG.json` streams the configured prefix and writes the JSON plus `OUTPUT.json.csv`, with no event/evidence output. For example:

```json
{"frameLimit":500,"sigmaLevels":[6,18],"injections":[{"atFrame":1,"x":800,"y":600,"amplitude":64,"fwhm":5},{"atFrame":180,"x":800,"y":600,"amplitude":64,"fwhm":5}]}
```

Use disposable workspace output paths. Output aliases/hardlinks to the source are refused. `register` retains its common-crop comparison role and now identifies the revised detector policy. The versioned [reference-rearming-measurements.json](reference-rearming-measurements.json) contains every full-stream trial/pulse and all 120 repeated registration cases; complete CSVs, configs, images, and logs are under ignored `out/qa-rearming/`.

Keep registration experimental, retain the partial field-acceptance status, and collect longer unclipped comparisons at multiple positions before choosing observing thresholds. No claim about one K serving earthshine and the bright limb has been made. Camera adapters, new automatic threshold rules, and mount guiding are outside this batch.

## Manual capture behaviour

Save now works with any existing frame source. The GUI sends one pending request; the source applies it after the next analysed frame, or against the last retained frame when Stop/EOF/failure/loop boundary arrives first. If no frame exists, the observer is told. The record distinguishes host UTC when the button was requested from the source time/index at which it was applied, which is essential during replay.

If idle, collection starts from the current ring. If already collecting, the manual request joins the open clip and extends its quiet tail. Automatic analysis/candidates continue. A manual tail that crosses an existing clip's duration/byte cap continues in an overlapping clip; a continuation retains the original request identity and does not restart its tail indefinitely. Normal source endings and memory/writer limits are still explicit. Repeated manual requests are separate marks, not fake automatic detections.

`event.txt` and `candidates.txt` identify `kind=manual`, request host time, applied source frame/time, and detector settings at that moment. They do not invent measured threshold peaks for a manual request. The preview role is labelled when no automatic passing peak exists. Refused clips retain manual pending/outcome records through the same writer journal. As already documented for automatic candidates, an open clip or job waiting in RAM is not crash-durable until the writer consumes it; this batch does not add continuous recording or an immediate durable journal.

Feature-10 acceptance item 8 is implemented and checked on synthetic/recorded sources. Camera-specific items, first-light measurements, and SDK licensing checks stay in the open Feature-10 proposal. No dependency or license changed, and no implemented-history section was moved or condensed.
