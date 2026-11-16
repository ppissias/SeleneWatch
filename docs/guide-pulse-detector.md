# Guide corrections and the detector

16 November 2026. Feature-8, design point 1: guiding moves the image, and the detector compares each frame with a reference that learns over 2 s. Part B found that on the synthetic scene, field motion alone made the detector fire. This note measures what guide corrections do to the detector on recorded lunar frames, before the first guided session.

## Method

`SeleneWatchProbe guide-pulses` (`tools/pulse_probe.cpp`) plays a recording once. It runs the detector as the application does, on a crop of every frame 32 px in from each edge: `FlashDetector` with the application's default settings except K, each frame analysed before the reference learns from it. The control crop stays put. Each trial moves its crop the way a guide correction moves the image: from 6 s on, every 6 s, a pulse moves it at a fixed speed for a fixed time, along +x, +y, −x and −y in turn.

- **Speeds:** 0.05, 0.10 and 0.25 px per frame. The guiding code refuses a guide rate that moves the image faster than 0.25 px per frame. The trials at 0.5 and 1.0 show what that limit prevents.
- **Pulse lengths:** 0.3 s, and 1.5 s, the longest pulse the controller sends. One pulse every 6 s is about the controller's shortest cycle: the pulse, 1 s of settling, and a fresh 3 s estimate.
- **Whole pixels.** Offsets are rounded to whole pixels, so no pixel is resampled and the noise is the recording's own. A smooth glide becomes one-pixel steps, and a 0.3 s pulse at 0.05 px per frame moves nothing.
- **Windows.** A window is a pulse and the 1 s settle time after it. The writer marks a candidate there `during_mount_correction=yes`. Candidates are counted in the windows, between windows, and before the first pulse, for each trial and for the control in the same windows.
- The recording's own drift and seeing are in every trial and in the control alike.
- **K:** 6, the default, and 12, 18 and 24. Feature-9 found 18, 24, 12 and 24 quiet on the first four recordings.

| Label | Recording in `D:/space/02_Planetary` | Frames, span | Format and frame times |
| --- | --- | --- | --- |
| Mare | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_180548.avi` | 500, 27.5 s | 1296×964 Mono8; 18.1 fps from FireCapture's log |
| Terminator | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_181337.avi` | 500, 27.2 s | 1296×964 Mono8; 18.3 fps from the log |
| Crater rim | `2015.12.21 Moon 8 Newt/raw/videos and stacked/Moon_165506.ser` | 500, 27.2 s | 1296×964 Mono8; SER frame times |
| Largest RGB | `2012.11.25 moon 25.11.2012/raw/alccd5_00023.AVI` | 500, 49.9 s | 1280×1024 RGB888; 10 fps from the header |
| Long | `2015.10.26 Moon/raw/Moon_200059.avi` | 1883, 103.6 s | 1296×964 Mono8; 18.2 fps from the log |

The archive was only read. Each crop is 64 px smaller than the frame in both directions.

## Results

The strongest correction the controller allows is 0.25 px per frame for 1.5 s, which moves the image 7 px at 18 fps and 4 px at 10 fps. Four times the limit, 1 px per frame for 1.5 s, moves it 27 or 28 px, and 15 px at 10 fps. Each cell gives the trial's candidates, then the unmoved control's in the same windows in brackets. The control's candidates after start-up are those from 6 s on.

| Recording | K | Control's candidates after start-up | Strongest allowed: in windows | Strongest allowed: after windows | Four times the limit: in windows |
| --- | --- | --- | --- | --- | --- |
| Mare | 6 | 35 | 114 (27) | 11 (8) | 525 (27) |
| Mare | 12 | 0 | 0 (0) | 0 (0) | 8 (0) |
| Mare | 18, 24 | 0 | 0 (0) | 0 (0) | 0 (0) |
| Terminator | 6 | 67 | 69 (30) | 9 (37) | 251 (30) |
| Terminator | 12, 18, 24 | 0 | 0 (0) | 0 (0) | 0 (0) |
| Crater rim | 6 | 54 | 35 (17) | 23 (37) | 360 (17) |
| Crater rim | 12 | 0 | 0 (0) | 0 (0) | 3 (0) |
| Crater rim | 18, 24 | 0 | 0 (0) | 0 (0) | 0 (0) |
| Largest RGB | 6 | 231 | 89 (119) | 118 (112) | 88 (119) |
| Largest RGB | 12, 18, 24 | 0 | 0 (0) | 0 (0) | 0 (0) |
| Long | 6 | 435 | 231 (153) | 275 (282) | 572 (153) |
| Long | 12 | 12 | 13 (12) | 0 (0) | 14 (12) |
| Long | 18, 24 | 0 | 0 (0) | 0 (0) | 0 (0) |

**At K 12, 18 and 24, corrections at the allowed speeds add almost nothing.** There were 648 corrections across the five recordings, three speeds, two pulse lengths and three K values; the shortest, slowest ones moved the image less than half a pixel and so not at all. At K 18 and 24, nothing fired after start-up, even at four times the limit. At K 12, no trial had more than one candidate above its control, and every extra candidate fell inside a marked window. No trial and no control had a candidate after a window. In the long recording the unmoved control itself produced 12 candidates at K 12, in bursts that fell inside two windows; the moved trials produced 11 to 14 there.

**At K 6, every recording already fires without any motion**, 35 to 435 candidates after start-up, as Feature-9 found. Corrections add more, and almost all of the extra fall inside the marked windows: 114 against 27 on the mare and 231 against 153 on the long recording, at the strongest allowed correction. After the windows, the trials stay within the control's own range, from fewer (9 against 37 on the terminator) to slightly more (11 against 8 on the mare). The 1 s settle time is long enough on these fields.

**The speed limit matters.** At four times the limit, the mare and the crater rim fire at K 12 (8 and 3 candidates against none), and at K 6 the extra candidates in windows multiply: 525 against 27 on the mare.

**The patch size and shape rules carry part of the protection.** On the RGB recording at K 12, the peak during corrections rose to 37.8 σ against 13.3 σ in the control, and nothing triggered: the moving edges made patches outside the size and shape limits (8 to 400 pixels, box ratio 4). Changing those limits changes this result.

**The synthetic finding of Part B does not carry over.** The recordings' own drift and seeing are in every control, and the controls are quiet at K 18 and 24, and at K 12 on four of the five. The synthetic scene has sharp rims on a noise-free render, so its noise model is far tighter than a real frame's.

**For the first guided session:** at a K where the unguided field is quiet, guiding at the allowed speed is not expected to add candidates, and those it adds fall inside the windows the writer marks. On this evidence neither registration nor rate offsets is needed for guiding; the marking is enough. K 6 is too low for these sunlit fields with or without guiding.

## What it does not show

- **Sunlit terrain only.** The archive holds no earthshine recording. Moving edges are what the detector sees: the limb, the terminator, crater rims. An earthshine field is fainter, but the sunlit crescent's limb may be in it.
- **Steps, not a glide.** A real correction moves the image smoothly, and the image also smears within an exposure.
- **Short recordings.** 4 to 16 corrections each, at the controller's fastest cadence.
- **A fresh detector.** Each trial's detector starts at the first frame. Its start-up candidates fall before the first pulse and are the same in the control.
- **No mount.** Backlash, a pulse that starts slowly, periodic error and wind are not modelled. The first guided session is still the test.

## Reproduction

Release build, about 4 ms per detector per frame; 44 detectors run on each frame.

```
out/build/release/SeleneWatchProbe.exe guide-pulses INPUT OUTPUT.json 6,12,18,24 [FRAME_LIMIT]
```

The probe writes a new JSON report only and prints one line per trial. The report holds every count for every trial and the control in the same windows. The five reports of this note are combined in [guide-pulse-measurements.json](guide-pulse-measurements.json).
