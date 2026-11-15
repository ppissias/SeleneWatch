# SeleneWatch User Manual

This manual is for observers who use SeleneWatch at the telescope or with their own recordings. It describes the application as it is now. SeleneWatch is still evolving, so names, defaults and layouts may change; this manual changes with them. It has no screenshots yet for the same reason.

For building SeleneWatch from source, see [Getting started](README.md#getting-started-development) in the README. For the design and the evidence rules, see [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md).

> **Candidates, not discoveries.** SeleneWatch flags *candidate* events. A candidate is not a confirmed lunar impact. Confirming a flash normally needs an independent observation from another telescope or site. SeleneWatch keeps each candidate's frames, timing and settings so that comparison is possible.

## Contents

1. [Before you start](#1-before-you-start)
2. [The window](#2-the-window)
3. [Choosing a source](#3-choosing-a-source)
4. [Preview and Monitoring](#4-preview-and-monitoring)
5. [Camera settings](#5-camera-settings)
6. [Display, levels and histogram](#6-display-levels-and-histogram)
7. [Detection](#7-detection)
8. [Testing sensitivity with a test flash](#8-testing-sensitivity-with-a-test-flash)
9. [History, clip length and memory](#9-history-clip-length-and-memory)
10. [What SeleneWatch saves](#10-what-selenewatch-saves)
11. [Feature lock and guiding](#11-feature-lock-and-guiding)
12. [An observing session, step by step](#12-an-observing-session-step-by-step)
13. [Troubleshooting](#13-troubleshooting)
14. [What has been tested](#14-what-has-been-tested)

---

## 1. Before you start

**Computer.** Windows, 64-bit. SeleneWatch is a native Qt application. It never waits on the disk, the display or the mount, but a fast disk and enough RAM help it keep long histories.

**Cameras.** Install the camera maker's Windows driver before you connect the camera.

| Camera family | What to install | Status |
| --- | --- | --- |
| ZWO ASI | ZWO's camera driver. The SDK library ships next to SeleneWatch. | Tested on an ASI120MM Mini on the bench |
| QHYCCD | QHY's Windows *All-In-One* driver and SDK package. SeleneWatch finds the installed SDK library when you select the camera. | Built; hardware test pending |

**Mount (optional).** To let SeleneWatch keep the Moon in the field, install the [ASCOM Platform](https://ascom-standards.org/) and your mount's ASCOM driver, for example EQMOD. SeleneWatch only sends small guide pulses; it never slews or parks the mount.

**Where files go.** Clips and logs are written to a folder called `events` next to `SeleneWatch.exe`. SeleneWatch never writes into the folders it reads recordings from.

**Settings are remembered.** Detection, display, capture, histogram and guiding choices are saved when you stop a stream or close the window, and restored the next time you start SeleneWatch.

---

## 2. The window

From top to bottom:

- **Top row:** the **Source** selector and **Open…** (for recordings), then **Start preview**, **Start monitoring** / **Stop monitoring**, **Stop stream**, **Save now**, and **Fixed flash** (synthetic source only). Then the **Target FPS**, playback **Speed**, and **Loop first … s** controls.
- **Banner:** a coloured line that always says whether capture is **OFF** (Preview) or **ON** (Monitoring).
- **Test flash:** controls for injecting a known, marked test signal (see [section 8](#8-testing-sensitivity-with-a-test-flash)).
- **Preview:** the live or recorded image, fitted to the window. Click to select a pixel; drag to lock on a feature.
- **Docks:** panels you can show, hide, move or float from the **View** menu.
  - **Detection**, **Display and capture**, and **Guiding** share the right-hand side as tabs.
  - **Camera** (ZWO) and **QHY camera** appear on the left when that camera is the source.
  - **Histogram** opens below the preview.
- **Status bar** (bottom), from left to right:
  - the latest message;
  - **Status**;
  - **FPS**, and **Frames** with the camera's reported dropped frames;
  - **Candidates**;
  - **History** and **RAM** use;
  - the frame size and format;
  - **Lock**: the drift readout, and whether guiding is active.

---

## 3. Choosing a source

Choose the source while the stream is stopped.

### Synthetic

A painted test Moon with craters, a small moving pacing marker, and sensor-like noise in the background. It needs no hardware and is the quickest way to learn the controls. **Target FPS** sets its frame rate. **Fixed flash** paints a bright spot for a few frames, as a quick check that capture works. Under **Display and capture → Synthetic pixels** you can make it produce 12-bit, 16-bit or Bayer colour frames, to try the display controls.

### Recording file

Choose **Recording file**, press **Open…**, and pick a file.

- **SER** files (8-bit and 16-bit mono, as written by FireCapture and others) are read with their own frame times when present.
- **AVI** files are read when they are uncompressed:
  - 8-bit grey and Y800 frames;
  - 24-bit and 32-bit colour frames, kept as colour;
  - YUY2, which is reduced to its brightness channel and marked as converted.

  Compressed AVI files are refused with a message.
- A **FireCapture log** (`.txt`) next to the file supplies the UTC start time, exposure and gain when it names that file. Otherwise frames get nominal times from the start and the **Target FPS**.
- Frames that the capture program recorded as dropped stay as gaps in the frame numbers.

**Speed:** **Real time** follows the recording's own frame times. **Fast** runs the detector through the file as quickly as the computer allows, which is useful for tuning. You can change the speed while the file plays. The history and clip windows are measured in the recording's own time, so they do not change with speed.

**Loop first … s** plays the first seconds of the file again and again, which is useful for tuning the detector on one stretch of sky. Each lap restarts detection, and frame numbers and times keep counting up.

When a recording ends, the stream stops by itself and any clip still being collected is saved.

### ZWO ASI camera / QHY camera

These appear in the Source list when SeleneWatch was built with that camera family.

- **ZWO:** select **ZWO ASI camera**; the **Camera** dock opens on the left. Press **Refresh cameras** if your camera is not listed, then choose it.
- **QHY:** select **QHY camera**; the **QHY camera** dock opens. If the SDK library is installed somewhere unusual, **Choose QHY DLL…** points SeleneWatch at it, and **Use automatic discovery** returns to the default search.

See [section 5](#5-camera-settings) for the camera controls.

---

## 4. Preview and Monitoring

SeleneWatch has two running states. In both, every frame goes through the same detector.

| State | What happens | Camera controls | What is written |
| --- | --- | --- | --- |
| **Preview** | Frames are shown, analysed and kept in the history | Live, where the camera allows | Nothing |
| **Monitoring** | Same analysis; candidates are captured as clips | Fixed | Clips, the candidate log, the guiding log |

**Start preview** is the normal first step: focus, frame the Moon, set exposure and gain, and watch the detector's readout. The banner says **capture OFF**. The Detection panel shows **Would trigger — capture OFF** when the detector would have fired.

**Start monitoring** turns capture on. With a camera, SeleneWatch closes and reopens the camera with the chosen settings and reads them back. This is so every saved frame carries verified settings. You will see a short gap and the frame counter restart. The detector then learns the sky afresh.

**Stop monitoring** saves the clip being collected, if there is one, and returns to Preview without stopping the camera.

**Stop stream** ends acquisition, saves any open clip, and closes the camera.

**Save now** (Monitoring only) keeps the current history and the following few seconds as a *manual* clip, even if the detector did not fire. Use it when you see something by eye.

After a live camera change in Preview, the panel shows **Settling** for a short time while the detector relearns the background.

---

## 5. Camera settings

The Camera dock lists the controls the camera itself reports. For each, it shows what you asked for and what the camera actually set; cameras often round values.

- **In Preview**, exposure (in ms), gain, offset, USB bandwidth and, where available, high-speed mode apply **live** shortly after you change them.
- **Raw format** (RAW8 or RAW16), **Binning**, and the **ROI** (region of interest: width, height and origin) need the camera to stop. Use **Apply format / ROI and restart Preview**.
- **In Monitoring**, camera settings are locked. Stop monitoring to change them. Display and detection settings stay adjustable.
- **RAW16** keeps every bit the camera delivers. The sensor's ADC depth (for example 12 bits) is recorded separately in the clip metadata.
- **Dropped frames** reported by the camera's SDK appear in the status bar and are recorded. Their position is approximate, because the SDK buffers frames.
- **Frame times** are the computer's UTC clock when each frame arrived. These cameras have no GPS timing, so keep the computer's clock synchronised for the session.

QHY cameras run at bin 1 in this version, and QHY's SDK does not report dropped frames, so they are shown as unknown.

---

## 6. Display, levels and histogram

Everything in this section changes **only what you see**. The saved pixels and the detector always use the original samples.

The **Display and capture** tab offers three display modes:

- **Linear / high bits:** the full sample range shown as it is.
- **Black / white levels:** set the black and white points in native sample units (ADU), with the number fields or the sliders. Useful for the faint earthshine side.
- **Auto-stretch:** levels chosen from the frame.

**Invert brightness** and **Show Bayer colour** (for colour sensors) are further display options. Level controls work while the stream is stopped too.

**View → Histogram** shows the distribution of the original samples, before any stretch or colour conversion, in native ADU. It reports the observed range and the share of samples at zero and at the highest possible value, which shows clipping at a glance. **Fit range** zooms to the values present. **Log counts** makes sparse values visible.

---

## 7. Detection

### How it works, briefly

For every pixel, SeleneWatch learns the usual brightness and how much that pixel normally varies (its noise), from the last few seconds. A pixel stands out when the new frame is brighter than usual by more than **K** times its own noise. A candidate needs a compact group of such neighbouring pixels (a *patch*), not a single hot pixel. Bright moving edges and big changes are rejected by the size and shape limits. While a patch is active, the background keeps learning everywhere else, and new separate patches are still caught.

### Controls (View → Detection)

| Control | Meaning | Starting value |
| --- | --- | --- |
| **Sensitivity K** | How many noise units above normal a pixel must be | 6 sigma |
| **Minimum patch** / **Maximum patch** | Allowed patch size in pixels | 8 / 400 px |
| **Maximum box / patch** | How spread out a patch may be: its bounding box area divided by its pixel count | 4.0 × |
| **Allow a one-frame trigger** | Fire on a single frame; when off, two consecutive frames must pass | On |
| **Background and noise → Reference time** | How quickly the learned background follows changes | 2.0 s |
| **Background and noise → Minimum noise** | The smallest noise assumed for any pixel, in ADU | 0.5 ADU |
| **Preview overlay** | **Sigma map** (pixels at or above K in red) or **Noise map** (local noise in ADU), on screen only | None |

Changes apply from the next frame. Every saved candidate records the settings it was found with.

### Reading the panel

- **Peak sigma** is shown against K, with a percentage and a recent high (amber marks). The sigma history covers the last ten seconds.
- **Patch** shows the size and shape of the strongest patch against the limits, and names the check it failed when it is rejected.
- **Would trigger — capture OFF** in Preview, or **Candidate — capture ON** in Monitoring, shows the detector firing.
- Clicking a pixel in the preview makes the panel and the test-flash readout report the noise at that pixel.

The starting values are not a calibrated recommendation. Tune them on your own recordings (next section).

---

## 8. Testing sensitivity with a test flash

A test flash adds a small, known Gaussian spot to real frames, so you can see what the detector would catch.

1. Start Preview, or play a recording.
2. Click the preview where the flash should appear. The earthshine side is the interesting case.
3. Under **Test flash**, set:
   - **FWHM:** the spot's width in frame pixels;
   - **Peak:** the brightness added, in native ADU (12-bit units for 12-bit data);
   - **Duration:** the number of frames.
4. Press **Inject blip**.

The readout gives the requested peak in noise units at that pixel. In Preview the Detection panel shows whether it **would trigger**. In Monitoring a triggered test flash is saved like any candidate, and **every injected frame is marked as test data** in the clip and the log.

A good routine: play a recording with no injection to see its normal noise, then inject flashes of decreasing peak until they are no longer caught. Saturation and bright moving edges near the limb limit what can be recovered there.

---

## 9. History, clip length and memory

Set these under **Display and capture → Capture settings** before pressing Start:

| Setting | Meaning | Default |
| --- | --- | --- |
| **Pre-event history** | Seconds kept *before* a trigger | 5 s |
| **Quiet tail** | Seconds kept after activity stops | 3 s |
| **Maximum after trigger** | Longest a single clip continues after its trigger; longer activity continues in a new, linked clip | 10 s |
| **RAM ceiling** | Memory for frames, analysis and clips waiting to be written | 2048 MiB |

The status bar shows how much history is actually held and how much RAM is in use. At high frame rates or large frames, the RAM ceiling may hold less than the requested history; the clip metadata then says why collection ended. If the writer falls behind and its share is full, a clip that does not fit is not saved, but its candidate is still logged as *not saved*.

---

## 10. What SeleneWatch saves

Everything goes under the `events` folder next to `SeleneWatch.exe`, and only while **Monitoring**.

### One folder per clip

Each clip folder is named by its UTC trigger time, for example `20261115-143156-930`.

| File | Contents |
| --- | --- |
| `event.ser` | The frames, lossless and exactly as delivered, at their native bit depth, with per-frame UTC times |
| `event.txt` | Everything needed to interpret the clip, listed below |
| `01_baseline.png`, `02_event_peak.png`, `03_diff_annotated.png` | Quick-look previews for review. Display images only, not evidence. |

`event.txt` contains:
- the camera and its settings, and the stream description;
- each trigger, with its detector settings;
- test-signal marks;
- the guiding state and any mount pulses during the clip;
- the capture limits and why collection ended;
- each frame's number and time.

A clip is complete when `event.txt` ends with `complete: yes`. Clips are never rewritten after they are finished.

### `candidates.txt`

The session's running record. It gets:
- a `session_start` line each time Monitoring starts, naming the source and camera settings;
- a `candidate_pending` line *before* a clip is written;
- an outcome line afterwards: `clip=<folder>` or `not_saved=<reason>`.

Manual saves are marked `kind=manual`, test signals `contains_test_signal=yes`, and triggers during a mount correction `during_mount_correction=yes`.

### `guiding.txt`

The lock and the mount's record, written while Monitoring:
- the lock and its quality;
- one offset line per second;
- every guide pulse, with the estimate that caused it and the mount's state;
- calibration results;
- guiding starts and stops, with reasons.

### If SeleneWatch crashes

Finished clips and log lines already written are safe. A clip still being collected, or still waiting in memory for the writer, can be lost. Its `candidate_pending` line remains, so you know it existed.

---

## 11. Feature lock and guiding

Open **View → Guiding**. The dock has three parts: **Feature lock**, **Mount**, and **Guiding**.

### Locking on a feature

The lock measures how far the image drifts. It works on its own, without a mount.

- **Drag a rectangle** on the preview around a crater or other detail with structure in more than one direction. A plain click still selects a pixel.
- Or click a pixel and press **Lock at selection** for a 64 × 64 pixel box around it.

A region is refused, with the reason, if it is:
- **mostly saturated**;
- **too flat** (too little detail against the noise);
- **detailed in one direction only**, such as a straight piece of limb or terminator, which fixes the position across it but not along it.

Once locked:
- the **dashed box** marks where the feature was locked;
- the **solid box** marks where it is now: green while the lock holds, amber when the last search failed.

**Drift** is the median of the last few seconds of measurements, in frame pixels. Positive is right and down. It is also shown in arcseconds once a calibration knows the image scale.

If the feature is not found for 2 s (cloud, a bump), the state becomes **lock lost, searching**, and SeleneWatch looks in a wider area. It resumes when it finds the feature twice in the same place. After 60 s it stops searching; lock again.

The lock survives Stop and Start of the stream. A change of frame size or format releases it. **Unlock** releases it by hand.

### Connecting a mount

Under **Mount**, choose **ASCOM telescope** and:

1. Press **Choose…**. ASCOM's own chooser opens. Select your mount's driver, for example EQMOD, and close the chooser.
2. Press **Connect**. The dock shows the mount's state: tracking, slewing, parked, pier side, and guide rates.
3. Make sure **tracking is on** in the mount's driver. SeleneWatch never turns tracking on or off.

Choose **Simulated mount** to try everything with the **Synthetic** source. The simulated mount drifts the painted Moon slowly and responds to guide pulses.

Drivers that run as their own program (EQMOD, GS Sky, Device Hub, the ASCOM simulators) work. A 32-bit driver that loads into the program cannot be used by 64-bit SeleneWatch; connecting to one fails with that explanation.

### Calibrating

With a lock held and the mount connected and tracking, press **Calibrate**. SeleneWatch:
1. pulses **west**, then **east** back;
2. takes up the declination **backlash**;
3. pulses **north**, then **south** back.

It measures how the image moves each time, so it needs no focal length, pixel size, or camera angle. This takes about a minute, and the dock shows each step. The result shows each axis's speed and direction on the image, the angle between the axes (near 90°), the field's own drift, and how far a pulse moves the image per frame.

A calibration holds for the **side of the pier** it was made on. After a meridian flip, calibrate again; SeleneWatch discards the old one itself.

### Guiding

Set the **Tolerance**: how far the feature may wander before SeleneWatch corrects. It is a percentage of the frame's shorter side, default 3 %, and the dock shows it in pixels (and arcseconds when known). Make it several times larger than the seeing's jitter. Then press **Start guiding**.

- While the drift stays inside the tolerance on both mount axes, nothing is sent.
- When an axis leaves it, one short guide pulse removes 70 % of that error. SeleneWatch then waits for the image to settle and for a fresh measurement before deciding again.
- Declination is not reversed for a small overshoot, which avoids fighting backlash.

**What SeleneWatch will and will not do with the mount:**
- It sends **guide pulses only**: never a slew, sync, park, tracking change, or rate change. A pulse ends by itself, so a crash cannot leave the mount moving.
- Guiding is **off whenever a stream starts** and ends when the stream stops. Press **Start guiding** again.
- Guiding **stops and says why** if:
  - the mount is not tracking, is slewing or parked, cannot pulse-guide, or is alt-azimuth;
  - the mount changes pier side;
  - the lock is lost;
  - three corrections in a row fail to reduce the error;
  - pulses would add up to more than 20 s in a minute.

  It does not restart by itself.
- If the mount's guide rate moves the image more than 0.25 pixels per frame, guiding is refused, because the detector would see the motion. Lower the guide rate in the mount's driver and calibrate again.

**Evidence.** Every pulse is logged in `guiding.txt` while Monitoring. A candidate whose trigger falls during a pulse, or the settle time after it, is marked `during_mount_correction=yes`, and its `event.txt` lists the pulses inside the clip. The candidate is still saved; the mark tells the reviewer the image was moving.

> **Watch this on your first guided session.** Steady field drift and guide pulses move bright edges, which the detector may notice. On the synthetic test scene, drift alone caused triggers. Check how often candidates appear while guiding on real data before trusting an unattended session.

---

## 12. An observing session, step by step

1. Connect the camera and, if guiding, the mount. Start SeleneWatch.
2. Choose the camera under **Source** and press **Start preview**.
3. Focus and frame the Moon. Set exposure and gain in the Camera dock. Use **Display and capture** levels and the **Histogram** to check that the bright limb is not badly clipped and that the earthshine side is visible.
4. Watch the **Detection** panel for a minute. If **Would trigger** appears often with nothing there, raise **K** a little or check the size limits. A test flash on the earthshine side shows what is still caught.
5. If guiding:
   1. Lock on a crater near the area you watch.
   2. Connect the mount and check that it is tracking.
   3. **Calibrate**, then **Start guiding**.
6. Press **Start monitoring**. With a camera, the stream briefly restarts, and you must start guiding again.
7. Leave it running. Check the status bar now and then: frames, dropped frames, candidates, RAM, and the lock readout.
8. **Stop monitoring**, then **Stop stream**, at the end.
9. Review the clip folders and `candidates.txt`. Compare candidates with other observers' records by UTC time.

---

## 13. Troubleshooting

| Problem | What to try |
| --- | --- |
| The camera does not appear under **Source** | That camera family was not built into this copy of SeleneWatch. See the README's build section. |
| The camera is not in the device list | Check the cable and the vendor driver, then press **Refresh cameras**. For QHY, check the All-In-One installation or use **Choose QHY DLL…**. |
| **Start monitoring** is greyed out | The source has ended or failed, or a mode change is still in progress. |
| A recording is refused | Compressed AVI files are not supported. Convert to SER or uncompressed AVI. |
| "RAM ceiling …" messages | Lower the history or the frame size, or raise **RAM ceiling** before starting. |
| Too many candidates | Raise **K**, lower **Maximum patch**, or turn off one-frame triggers. Use the sigma map to see what fires. |
| Lock refused: "one direction only" | Choose a region with craters or detail in two directions, not a straight edge. |
| Calibration fails: "does not respond" | The mount is not moving the image. Check that the right driver is connected, that the mount accepts guide pulses, and that the guide rate is not tiny. With a simulator and a real camera this is expected. |
| Calibration fails: axes "not near 90" | The lock may have slipped or the field moved unevenly; calibrate again. |
| Guiding refused: "pixels per frame" | The guide rate is too fast for this image scale. Lower it in the mount's driver and calibrate again. |
| Guiding stopped: "corrections have no effect" | The calibration no longer matches the mount, or the lock is on the wrong feature. Check, lock, and calibrate again. |
| "Mount not responding" | A driver call has not returned. Acquisition and detection continue. Check the mount's connection and its driver window. |
| `guiding.txt could not be written` | The disk is full or the `events` folder is not writable. Guiding continues, but its record is incomplete. |

---

## 14. What has been tested

| Area | State |
| --- | --- |
| Synthetic source, recordings, detector, clip writing | Automated tests; checks on the author's SER and AVI archive |
| ZWO ASI120MM Mini | Bench tests in RAW8 and RAW16, including two 30-minute unattended runs |
| QHY cameras | Software complete; hardware test pending |
| Feature lock | Automated tests; followed features through two archive lunar recordings |
| Calibration and guiding | Automated tests and a full simulated run; ASCOM checked against the ASCOM telescope simulator |
| A real mount under the Moon | **Pending** |
| Detector settings for real observing | Partial; tuning on real earthshine data is pending |

Details are in [features.md](features.md), [implemented features.md](implemented%20features.md), and the validation notes in [docs](docs/).
