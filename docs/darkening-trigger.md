# The darkening trigger on recordings

16 November 2026. Feature-13, Part C. The detector counts a pixel that falls by K times its noise only with darkening on. Before darkening is offered for observing, this note measures two things on the same archive recordings as [guide-pulse-detector.md](guide-pulse-detector.md). First, how often the dark side fires on an unchanged lunar field. Second, how often it catches a silhouette crossing that field.

## Method

`SeleneWatchProbe darkening` (`tools/darkening_probe.cpp`) plays a recording once and runs `FlashDetector` as the application does. Every setting is the default except K, with darkening on and no large darkening. Each frame is analysed before the reference learns from it. One detector per K watches the unchanged recording (the control). The other detectors each watch the recording with one kind of silhouette crossing it.

- **The silhouette.** An ellipse inside which every channel's light is scaled by 1 − depth. There are two sizes: 60 × 24 px (a satellite), and 160 × 60 px (the ISS near the zenith at 0.33″/px, or a distant plane). Neither is a model of a real outline.
- **Depths:** 0.3, 0.6 and 0.9. A plane or the ISS against the sunlit Moon is nearly black, so 0.9 is the realistic case. The shallower ones stand for a thin, out-of-focus or partly transparent shadow.
- **Speeds.** A crossing takes 3 frames, as the ISS takes over a 7′ field, or 15 frames, as a plane takes. The ellipse crosses the whole frame horizontally, along the middle, upper-third and lower-third rows in turn, alternating in direction. Each frame shows it at the middle of that frame's stretch of the path, so even a three-frame crossing shows some of it on every frame.
- **Schedule.** From 6 s, when the reference has learned for three of its time constants, one crossing every 6 s.
- **Counting.** A crossing fires when a dark candidate falls in its window: its frames and the frame after. The control is counted after a start-up of 3 s.
- **K:** 6, the default, and 12, 18 and 24.

| Label | Recording in `D:/space/02_Planetary` | Frames, span | Format |
| --- | --- | --- | --- |
| Mare | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_180548.avi` | 500, 27.5 s | 1296×964 Mono8 |
| Terminator | `2015.12.19 Moon C9.25/raw/videos and stacked/Moon_181337.avi` | 500, 27.2 s | 1296×964 Mono8 |
| Crater rim | `2015.12.21 Moon 8 Newt/raw/videos and stacked/Moon_165506.ser` | 500, 27.2 s | 1296×964 Mono8 |
| Largest RGB | `2012.11.25 moon 25.11.2012/raw/alccd5_00023.AVI` | 500, 49.9 s | 1280×1024 RGB888 |
| Long | `2015.10.26 Moon/raw/Moon_200059.avi` | 1883, 103.6 s | 1296×964 Mono8 |

The archive was only read.

## Results

The control columns give the unchanged recording's candidates after start-up, rising and falling. The depth columns give the crossings that fired, for both sizes and both speeds together. The last column is the largest number of dark candidates outside the crossing windows in any injected run, to compare with the control's falls.

| Recording | K | Control: rising | Control: falling | Depth 0.9 | Depth 0.6 | Depth 0.3 | Falls outside windows |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Mare | 6 | 89 | 1 | 16 / 16 | 16 / 16 | 16 / 16 | 1 |
| Mare | 12 | 0 | 0 | 16 / 16 | 15 / 16 | 0 / 16 | 0 |
| Mare | 18 | 0 | 0 | 15 / 16 | 0 / 16 | 0 / 16 | 0 |
| Mare | 24 | 0 | 0 | 0 / 16 | 0 / 16 | 0 / 16 | 0 |
| Terminator | 6 | 175 | 77 | 16 / 16 | 16 / 16 | 15 / 16 | 74 |
| Terminator | 12 | 0 | 0 | 16 / 16 | 13 / 16 | 0 / 16 | 0 |
| Terminator | 18 | 0 | 0 | 13 / 16 | 4 / 16 | 0 / 16 | 0 |
| Terminator | 24 | 0 | 0 | 8 / 16 | 0 / 16 | 0 / 16 | 0 |
| Crater rim | 6 | 77 | 6 | 16 / 16 | 16 / 16 | 16 / 16 | 6 |
| Crater rim | 12 | 0 | 0 | 16 / 16 | 16 / 16 | 0 / 16 | 0 |
| Crater rim | 18 | 0 | 0 | 16 / 16 | 8 / 16 | 0 / 16 | 0 |
| Crater rim | 24 | 0 | 0 | 15 / 16 | 0 / 16 | 0 / 16 | 0 |
| Largest RGB | 6 | 224 | 13 | 31 / 32 | 32 / 32 | 32 / 32 | 13 |
| Largest RGB | 12 | 0 | 0 | 32 / 32 | 32 / 32 | 13 / 32 | 0 |
| Largest RGB | 18 | 0 | 0 | 32 / 32 | 28 / 32 | 0 / 32 | 0 |
| Largest RGB | 24 | 0 | 0 | 32 / 32 | 10 / 32 | 0 / 32 | 0 |
| Long | 6 | 514 | 170 | 64 / 68 | 60 / 68 | 57 / 68 | 146 |
| Long | 12 | 15 | 0 | 52 / 68 | 46 / 68 | 0 / 68 | 0 |
| Long | 18 | 0 | 0 | 46 / 68 | 1 / 68 | 0 / 68 | 0 |
| Long | 24 | 0 | 0 | 15 / 68 | 0 / 68 | 0 / 68 | 0 |

**At K 12 and above, darkening adds no candidate to an unchanged field.** None of the five controls had a dark candidate after start-up at K 12, 18 or 24. Nor did any injected run outside its crossing windows.

**At K 6, the dark side fires less than the bright side, but not never.** The counts were 1, 77, 6, 13 and 170 dark candidates, against 89, 175, 77, 224 and 514 rising ones. On the terminator and the long recording, darkening adds a third to a half again to an already busy field. Why falls are rarer than rises here was not established. K 6 is already too low for these sunlit fields, as [guide-pulse-detector.md](guide-pulse-detector.md) found.

**A near-black silhouette is caught at K 12.** At depth 0.9, every crossing fired on four of the five recordings, at both sizes and both speeds, including the three-frame crossings. Depth 0.6 is caught almost as often at K 12, and much less at K 18. Depth 0.3 is lost at K 12 everywhere except the RGB recording, where about four in ten fired. A thin or out-of-focus shadow, such as a bird near the telescope, needs a low K, and at a low K the sunlit field fires on its own.

**The long recording missed more than the others:** 52 of 68 crossings at depth 0.9 and K 12, and 15 of 68 at K 24. Part of its field is sky beyond the limb, where a silhouette has nothing to darken. That does not by itself explain whole crossings missed, and the probe does not record where or when each crossing ran, so the cause is open.

**The dark limits held.** Both sizes, about 1100 and 7500 pixels, passed the 20 000-pixel maximum and the 10× box ratio. At the three-frame speed each frame's silhouette is a new patch, hundreds of pixels from the one before, so a fast crossing gives a dark candidate on each frame. That is many triggers in one clip, not many clips.

## What it decides

- **Darkening stays off by default, in both modes.** At the default K of 6 it would add up to half again as many candidates on sunlit fields. Moon mode is meant to behave as it did before Feature-13. An observer who wants transits turns darkening on.
- **With darkening on, use K 12 or more on sunlit fields.** There it adds nothing to the control, and a near-black crossing is caught on four of the five fields.
- **The dark limits keep their starting values**, 20 000 px and 10×.
- **Large darkening stays off.** No recording here has a cloud or a plane filling the field to measure it on.

## What it does not show

- **No real transit.** The silhouettes are ellipses with hard edges, added to the frames. A real plane has a sharp but irregular outline, exhaust shimmer and contrails; a bird near the telescope is far out of focus.
- **Sunlit terrain only.** The archive holds no earthshine recording. Against the earthshine a silhouette takes away far less light, and its depth in sigma is smaller.
- **Short recordings.** 4 to 17 crossings each.
- **A fresh detector.** Each detector starts at the first frame; start-up candidates are left out.

## Reproduction

Release build; 52 detectors run on each frame, about 2 to 7 minutes per recording.

```
out/build/release/SeleneWatchProbe.exe darkening INPUT OUTPUT.json 6,12,18,24 [FRAME_LIMIT]
```

The probe writes a new JSON report only and prints one line per K and silhouette. The five reports of this note are combined in [darkening-measurements.json](darkening-measurements.json).
