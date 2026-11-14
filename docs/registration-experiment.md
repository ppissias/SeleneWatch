# Feature-9 follow-up: whole-frame registration — 14 November 2026

The manual integer-translation experiment is complete. **It does not justify enabling registration in the application.** At K=6, fewer candidates often mean one nearly continuous firing run, rather than a quieter detector. At the previously quiet comparison settings, alignment does not improve the tested mare or terminator recovery bracket. The crater-rim and RGB recoveries require saturated centres. These are short, single-position experiments, not observing calibration or a measured false-impact rate.

The prototype is linked only into `SeleneWatchProbe` and its registration tests. The application still analyses original native frames. No archive file, evidence file, license, or dependency changed. Feature-9's earthshine/bright-limb calibration and Feature-5's real-earthshine display check remain open.

## Controlled comparison

Four recordings are read once into a cache capped at 512 MiB of source pixels. The three Mono8 prefixes contain 240 frames each; the largest RGB prefix stops at 136 because of that cap. Each trial restarts `EventCapture` and the detector. Detection uses minimum 8 pixels, maximum 400, one-frame triggering, no overlay, and the current Feature-5 capture defaults: five seconds of history, three seconds of quiet tail, ten seconds maximum after a trigger. The manual experiment is not a Session memory-pressure or writer-throughput test.

The fixed template is the first original frame. An 8×8 box-average normalized cross-correlation finds a coarse shift, then a native-sample search sampled every eight pixels refines it to an integer pixel. Search is ±48 pixels. Correlation below 0.7, a distant coarse peak gap at most 0.00001, or insufficient texture rejects the estimate; rejection falls back to zero shift and is recorded. The controls are:

- **Full:** original field, no translation, no injection; this diagnoses effects of cropping.
- **Cropped:** a fixed 64-pixel border removed, no translation.
- **Aligned:** the same output size, translated by copying native pixel words from within that border. No interpolation, rescaling, or exposed-edge filling is used.

The last two use the same reference coordinate system and output dimensions. Their source pixels differ as the Moon moves; that is the intended translation. For each sigma value, both run a no-injection control and peak additions of 8, 16, 32, 64, 128, and 192 native ADU. All injections last one frame with FWHM five pixels. Shifts are estimated **before injection**, on the original recordings. The injected centre in the raw frame is the chosen first-frame coordinate plus that frame's measured shift, identical for both modes. This keeps the comparison on the same estimated lunar target and avoids an injection steering registration. It does not measure how a future online estimator responds to a flash.

Every candidate position, passing-frame count, firing-frame count, and longest consecutive firing run is reported. A matched recovery requires a new trigger during injection whose selected peak pixel was actually brightened. A candidate elsewhere or a pre-existing firing run does not count. Only the selected strongest patch is exposed by this detector, so a different patch can hide a local response. Actual centre addition and prior-run state accompany the requested amplitude.

Inputs below are relative to the read-only `D:\space\02_Planetary` archive. Coordinates refer to the first frame, not necessarily the later raw-frame coordinate used in the older Feature-9 calibration.

| Scene | Input | Native layout | Frames / source-time span | Reference centre / injection position | Quiet comparison K |
| --- | --- | --- | --- | --- | --- |
| Mare | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_180548.avi` | 1296×964 Mono8 | 240 / 13.278 s | (800,600) / 180 | 18 |
| Terminator | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_181337.avi` | 1296×964 Mono8 | 240 / 13.278 s | (700,700) / 180 | 24 |
| Crater rim | `2015.12.21 Moon 8 Newt/raw/videos and stacked/Moon_165506.ser` | 1296×964 Mono8 | 240 / 13.042 s | (310,440) / 180 | 12 |
| Largest RGB | `2012.11.25 moon 25.11.2012/raw/alccd5_00023.AVI` | 1280×1024 RGB888 | 136 / 13.500 s | (640,400) / 80 | 24 |

SER times are the file's UTC trailer. The two FireCapture AVI times come from the log start plus nominal frame rate. RGB AVI has an unknown start counted from zero at its nominal rate. These spans are last timestamp minus first timestamp, not wall time. This run uses current source-time clip holds and a shifted injection target; its candidate counts and recovery brackets must not replace the historical Feature-9 measurements.

## Translation and background activity

All 856 estimates pass the prototype's acceptance checks; none reaches the search boundary. This is internal acceptance, **not ground-truth shift accuracy on the archive**. Controlled known-shift tests provide that check for constructed fields.

| Scene | dx range | dy range | Minimum correlation | Mean / 95th percentile shift cost |
| --- | --- | --- | --- | --- |
| Mare | 0…15 px | −15…1 px | 0.983736 | 5.431 / 5.487 ms |
| Terminator | 0…18 px | −5…3 px | 0.985811 | 5.346 / 5.371 ms |
| Crater rim | −16…0 px | −14…2 px | 0.985725 | 5.345 / 5.376 ms |
| Largest RGB | 0…36 px | −18…21 px | 0.984301 | 5.976 / 6.286 ms |

At K=6 the unmodified-recording controls give:

| Scene / mode | Candidates | Candidates/min over prefix | Passing frames / total | Longest firing run |
| --- | --- | --- | --- | --- |
| Mare, full | 9 | 40.67 | 106/240 | 75 frames |
| Mare, cropped | 8 | 36.15 | 137/240 | 90 frames |
| Mare, aligned | 1 | 4.52 | 238/240 | 238 frames |
| Terminator, full | 13 | 58.74 | 100/240 | 64 frames |
| Terminator, cropped | 22 | 99.41 | 103/240 | 53 frames |
| Terminator, aligned | 11 | 49.71 | 226/240 | 143 frames |
| Crater rim, full | 11 | 50.61 | 145/240 | 66 frames |
| Crater rim, cropped | 8 | 36.80 | 150/240 | 120 frames |
| Crater rim, aligned | 1 | 4.60 | 239/240 | 239 frames |
| Largest RGB, full | 2 | 8.89 | 132/136 | 107 frames |
| Largest RGB, cropped | 1 | 4.44 | 135/136 | 135 frames |
| Largest RGB, aligned | 1 | 4.44 | 135/136 | 135 frames |

Here firing and passing counts are equal because the one-frame path is enabled. Cropping alone changes the detector substantially, so a full-field versus aligned count would be a confounded comparison. More importantly, the aligned mare and rim fire from near startup through the end. The reference is held while a frame passes or a clip is open. Candidate counts alone therefore hide the detector's loss of opportunities to start a new run. This observation identifies an investigation into startup, reference holding, and rearming; it does not establish their exact causal contribution relative to residual seeing or integer-alignment error.

At each scene's higher comparison K, all three no-injection controls have **zero candidates and zero passing/firing frames**. Roughly thirteen seconds of quiet does not establish an acceptable long-session rate. No background candidate has been classified as a real or false astronomical event.

The labelled first-frame, unaligned, and aligned terminator PNGs were inspected. They show a plausible global displacement and substantial remaining changes in sharpness/terrain. This is qualitative context, not a quantitative separation of global drift from local seeing. The PNGs are derived TEST previews with labels painted into the display image; they are never evidence.

## Injected recovery and cost

At the quiet comparison settings, the one-frame grid gives:

| Scene / K | Cropped: largest tested miss → first recovery | Aligned: largest tested miss → first recovery | Actual centre addition at aligned recovery |
| --- | --- | --- | --- |
| Mare / 18 | 32 → 64 ADU | 32 → 64 ADU | 64 ADU, unclipped |
| Terminator / 24 | 64 → 128 ADU | 64 → 128 ADU | 128 ADU, unclipped |
| Crater rim / 12 | No matched trigger through 192 ADU | 128 → 192 ADU | 146 ADU, saturated |
| Largest RGB / 24 | No matched trigger through 192 ADU | 64 → 128 ADU | 113 ADU, saturated |

All listed recoveries start with no prior firing run. These are brackets on a sparse grid at one position and time, not recovery probabilities or universal faint limits. Saturation changes the injected shape and its wings; the rim/RGB results do not establish an unclipped sensitivity gain. At K=6 only the aligned terminator yields matched new triggers, at 128 and 192 ADU; the other cases' persistent background activity makes that threshold unsuitable for a sensitivity claim from this test.

Release MSVC, Qt 6.12.0, AMD Ryzen 9 9955HX; no builds or tests ran concurrently with measurement. Shift timing includes conversion, binning, and search, but excludes initial template construction and file reading. Trial pipeline timing includes injection if present, crop allocation/copy, and `EventCapture`/detector work. No maps, evidence writing, or GUI composition are included.

| Scene / quiet K | Cropped pipeline mean | Aligned pipeline mean | Shift + aligned means |
| --- | --- | --- | --- |
| Mare / 18 | 2.974 ms | 2.996 ms | 8.427 ms |
| Terminator / 24 | 3.079 ms | 3.045 ms | 8.391 ms |
| Crater rim / 12 | 2.992 ms | 3.028 ms | 8.373 ms |
| Largest RGB / 24 | 4.665 ms | 4.645 ms | 10.621 ms |

These rows use no-injection controls. Adding means describes the measured component cost; shifts were precomputed, so it is not an end-to-end acquisition benchmark. The costs fit the selected recordings' nominal periods on this machine. That establishes feasibility for a further experiment, not camera throughput, long-run stability, or scientific acceptance.

## Decision, checks, and reproduction

Keep registration outside production. The next detector investigation should measure startup and rearming under sustained edge/seeing activity, track passing occupancy and independent injections throughout long runs, and preserve one-frame recovery while addressing reference holding. Repeat the common-crop control at multiple positions/times on unclipped recordings, including earthshine. Subpixel translation, local deformation, template renewal, and raw Bayer phase handling remain untested. Raw Bayer is explicitly refused by this prototype rather than shifting its mosaic into a different colour phase.

The registration suite checks known integer shifts including the search boundary under gain/offset changes; rejects featureless/ambiguous fields, Bayer, invalid search sizes, and invalid crop bounds; and verifies Mono8, low/high-aligned 12-bit words, and RGB native-copy alignment with unchanged original bytes/timestamps/layout. The seven CTest targets pass in Debug and Release, including the existing evidence, display/memory, and actual-window suites. No GUI integration was added for registration.

Build `SeleneWatchProbe` in Release and set the configured Qt runtime on PATH. Save this mare config as `out/registration-mare.json`:

```json
{"frameLimit":240,"maxShift":48,"sigmaLevels":[6,18],"amplitudes":[8,16,32,64,128,192],"minPixels":8,"maxPixels":400,"x":800,"y":600,"atFrame":180,"duration":1,"fwhm":5}
```

```powershell
out/build/release/SeleneWatchProbe.exe register "D:/space/02_Planetary/2015.12.19 Moon C9.25/raw/videos and stacked/Moon_180548.avi" out/registration/mare out/registration-mare.json
```

Use the other inputs, coordinates, injection position, and comparison K from the table for the remaining scenes. Outputs are `registration.json`, `translations.csv`, and three labelled PNGs; choose a disposable workspace directory. Aliases/hardlinks from those output names to the original input are refused. A disposable SER hardlinked to `reference.png` was refused before any trace was written, with its SHA-256 unchanged. No events or SERs are written. The versioned [registration-measurements.json](registration-measurements.json) preserves all 120 trial summaries, configs, shift ranges, and timing context. Full local traces, images, configs, and logs are under ignored `out/qa-registration/`.
