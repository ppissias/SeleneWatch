# SeleneWatch features

Open proposals, ideas not yet proposed, and what remains to check in the field. What is already in the program, with its full design and review history, is in [implemented features.md](implemented%20features.md). When moving completed features to `implemented features.md`, append them in full detail without removing or condensing specifications, review notes, or design decisions.

On 21 November 2026 every feature whose software was complete moved there: Features 5, 8, 9, 10, 12, 13, 14, 16, 17, 18, 22 to 26 and 28 to 31, and Feature-32 once it was built the same day. Feature-33 to Feature-44 followed on 22 November 2026, and Feature-45 to Feature-49 on 23 November 2026. Their remaining checks at the telescope or on hardware are listed under [Field validation](#field-validation) at the end of this file.

## Contents

| Feature | Focus | Status |
| --- | --- | --- |
| [Feature-27 — Detector tuning, capture status and nightly review](#feature-27--detector-tuning-capture-status-and-nightly-review) | Explain detector feedback, show what is being saved, review a night's coverage and candidates, and validate tuning on earthshine | Item 3 (nightly review) implemented; items 1 and 2 proposed |
| [Feature-50 — Optional runtime sub-pixel registration for stabilized detection](#feature-50--optional-runtime-sub-pixel-registration-for-stabilized-detection) | Opt-in real-time shift stabilization before detection for Moon/Sun flashes, shadows, and planetary targets, benchmarked first for benefit and CPU cost | Stage 1 benchmark complete ([report](docs/stabilized-detection-benchmark.md)); Stage 2 awaiting review |
| [Ideas not yet proposed](#ideas-not-yet-proposed) | Work set aside in earlier features, waiting for data, hardware or a decision | Not proposed |
| [Field validation](#field-validation) | Checks of implemented features that need the sky, a mount or a camera | Open |

## Feature-27 — Detector tuning, capture status and nightly review

*Proposed on 21 November 2026 following a review of the observing workflow.*

The existing observing modes fit three different jobs. Monitoring preserves unexpected events and their lead-up, Record covers the full interval of a predicted event, and Time-lapse stacks selected frames to show gradual change. Display stretch lets the observer examine faint earthshine while the SER retains the original samples. Keep those choices. This proposal addresses what the observer needs to know while tuning, during capture and the following morning.

### 1. Explain what the sensitivity trace measures

The quick start advises raising K above the ten-second peak trace. The slider help says a mark beyond K would trigger. A single noisy pixel can produce that mark. The detector also tests connected patch size and shape and, when selected, confirmation on a second frame. Following the current advice can cost sensitivity even when the detector was already rejecting the peaks. In the terminator replay, K=6 recovered four of five marked flashes. K=24 produced a quiet control run and recovered one of five. These are results for that particular recording. [Quick start](README.md) · [Slider help](src/ui/detection_panel.cpp) · [Replay measurements](docs/reference-and-manual-validation.md)

Update the trace label, help and observing instructions to explain the peak in pixel-noise units. Direct observers to **Would trigger** for the detector's actual decision and to a marked test flash to check recovery at a chosen position, amplitude and duration. This wording correction can ship independently of Feature-26.

### 2. Say what is being saved

An observer can press **Record** in Preview and see “capture OFF” above a running recording readout. Name the two activities explicitly. For example, while a SER is being written in Preview, show **Automatic event clips off · Continuous recording on**. In Monitoring, show whether automatic clips are armed and whether a continuous recording is also running. Keep recording failures visible. [Mode banner](src/ui/mainwindow.cpp) · [Observing modes](MANUAL.md)

### 3. Make the first event-browser view a night in chronological order

After a patrol, the observer already has an overview, a short animation and raw frames for each saved clip. The harder question is “When was I watching, and which candidates deserve another look?” Show Monitoring intervals and known interruptions alongside the candidates. For example, a sky-follower exposure change briefly restarts Monitoring. A quiet interval, a known interruption and a candidate whose clip could not be saved should each be identifiable. Where older logs establish only the start of an interval, show its end as unknown. [Sky patrol and saved outputs](MANUAL.md)

Let the observer step through candidates using the existing overviews and animations, open the original clip, and mark a candidate for follow-up. Store that decision beside the evidence. Start with the candidate log and clip metadata. Record any missing Monitoring boundaries needed to establish coverage for future nights.

**Implemented for nightly review.** Each intentional Moon or Sky monitoring run owns its logs, triggered clips, continuous recordings and offline HTML report. The finished report is offered in the main window and can be reopened from the Session menu. Coverage, events grouped by what was measured (Feature-46), clip previews, trigger outcomes and follow-up marks support morning review. The other Feature-27 proposals and lunar field calibration remain open. [User manual](MANUAL.md)

### Field validation and acceptance

- In Preview, use an isolated above-K pixel that fails the configured minimum patch size. The trace shows the peak and the detector reports no candidate. The quick start and in-app help explain both readings consistently.
- Exercise Preview and Monitoring with continuous recording both on and off. In each combination the banner correctly names both activities, including after recording stops or fails.
- Review a watch with zero candidates and a watch containing a known interruption, a saved clip and an unsaved candidate. The observer can account for each outcome, open a saved preview and find their follow-up mark after reopening the night.
- Make a representative earthshine recording the next lunar validation milestone. Replay the same span with candidate settings under consideration for Feature-26. Count candidates and clips requiring review on the original data. In separate marked trials, report flashes recovered out of those injected, with their positions, amplitudes and durations. Use both measurements when choosing an observing profile. [Open lunar calibration work](docs/feature-9-validation.md)

---

## Feature-50 — Optional runtime sub-pixel registration for stabilized detection

*Proposed on 24 November 2026.*

### Goal

Add an **opt-in checkbox** (off by default) that stabilizes frames against frame-to-frame translation `(dx, dy)` inside the detector's working comparison before evaluating `FlashDetector` — working identically for **both flashes (rises) and shadows (falls)** on the **Moon, Sun, and planetary targets (e.g., Jupiter/Saturn)**.

Unlike the historical 14 November 2026 integer-only prototype ([registration experiment](docs/registration-experiment.md)), which ran on the older whole-frame-hold detector and left ±0.5 px edge residuals, this feature targets the current per-pixel hold-out, single-direction detector (Features 9, 11, 13, 45), supports **sub-pixel** alignment, reuses existing `FeatureLock` measurements where available, and **benchmarks both detection benefit and CPU cost first** before shipping in the UI.

### Scope and design

1. **Opt-in user control (`DetectionSettings::stabilizeDetection`):**
   - A checkbox (**Stabilize detection against drift/shake**) in the **Detection** panel, saved per watch in `watch_settings` (`false` by default).
   - Available on Moon/Sun watches (flashes and shadows) and future planetary watches; hidden and forced off on fixed-camera Sky watches (where horizon masks are fixed and stars drift intentionally).

2. **Two shift estimators (reusing existing code, one implementation per behaviour):**
   - **When Feature Lock is active (Moon / Sun surface):** Reuse the live sub-pixel `(dx, dy)` already computed by `FeatureLock` on every frame (~0 ms extra registration cost).
   - **When no Feature Lock is set (or for isolated planetary / full-disk targets):** A lightweight automatic estimator:
     - *Isolated bright disk (planets, full Moon, full Sun):* thresholded intensity-weighted **Center of Gravity (CoG)** centroid (< 0.3 ms, ~0.05–0.1 px precision).
     - *Partial lunar surface without a lock box:* fast downsampled **2D phase correlation** (or auto-picked high-contrast anchor patch via `FeatureLock`) on an interior ROI.

3. **Zero-copy shifted sampling inside `FlashDetector` (evidence rules preserved):**
   - Raw `Frame` objects in `FrameRingBuffer` and saved `event.ser` clips remain **100% untouched** (native bytes, timestamps, and layout unchanged).
   - `FlashDetector::analyze(frame, display, shift)` reads shifted coordinates `(x + dx, y + dy)` on the fly without allocating a warped full-frame copy:
     - **Mono8 / Mono16 / RGB:** bilinear sub-pixel interpolation (benchmarking integer vs bilinear sub-pixel directly in Stage 1).
     - **Raw Bayer mosaics:** never interpolate across colour channels; either shift each of the 4 Bayer sub-planes (`R`, `G1`, `G2`, `B`) independently or compare on a 2×2-cell luma grid while keeping raw evidence untouched.
   - Any detected trigger's `(peakX, peakY)` and bounding `box` are mapped back to **native raw frame coordinates** `(x + round(dx), y + round(dy))`.
   - Border pixels outside the valid shifted overlap are treated as masked for that frame so exposed edges never false-trigger.
   - `event.txt` records whether stabilization was active, its state, and the measured `(dx, dy)` on each trigger.

4. **Failure handling — what happens when registration cannot lock or loses lock:**
   Registration failure is expected in real observing (clouds, slewing, a large shadow covering the lock region, low contrast, or target drifting out of frame). Every failure mode is handled explicitly and without false-trigger storms:
   - **Transient loss (short dip ≤ grace window, e.g., up to 0.5 s / ~15 frames — such as a bird/aircraft crossing the lock region, a sudden seeing blur, or an implausible single-frame jump `> maxStepPx`):**
     - Reject the bad measurement and **hold the last valid `(dx, dy)`** during the grace window so detection stays active through the exact moment a large shadow passes over the lock area.
   - **Sustained loss (grace window expires — clouds, slew, target left frame, or no lockable texture):**
     - **Never** compare an unshifted frame against a shifted `mean_` (which would fire across every edge in the frame).
     - Transition explicitly to **`Registration lost (unstabilized fallback)`**: cleanly reset the detector's reference at `(0, 0)`, emit one `registration_lost reason=...` entry in `session.txt` / `candidates.txt` (no log spam), show clear status in the Detection panel (`Stabilization: lost — running unstabilized`), and continue watching with standard unregistered detection so the observer never goes blind.
     - When registration produces consecutive valid matches again (e.g., 3 frames), cleanly re-anchor the reference and log `registration_locked`.
   - **Large accumulated drift (unguided mount drifting past margin, e.g., `|dx|` or `|dy| >= 32 px`):**
     - **In-place integer recenter** of `mean_` and `variance_` by `(stepX, stepY)` (initializing only the newly exposed border strip from the current frame and subtracting `(stepX, stepY)` from the accumulated offset), keeping the learned background intact across the interior with zero blind gap and zero false triggers.

### Stage 1 — Benchmark plan (before UI integration)

Add a reproducible CLI benchmark subcommand to `SeleneWatchProbe` (`stabilize-bench`) comparing **unstabilized** vs **integer-shifted** vs **sub-pixel bilinear** detection on the same recordings and detector code:

1. **Detection benefit (quietness & sensitivity):**
   - *Moon / Sun shadows:* `nasa_iss_lunar_2019_leadin.avi`, `nasa_iss_solar_2020_leadin.avi`, `iss_lunar_transit_1080p.ser`, plus real archive sunlit lunar recordings (`test_data/Moon_*.avi`, `test_data/Moon_*.ser`) — measure false-trigger count vs `K` (`6, 8, 12, 18`), limb/edge noise reduction, and ISS/injected-shadow recovery.
   - *Moon impact flashes:* `nasa_msfc_lunar_impact_20130317.ser`, `FDS_moon_2.avi`, and injected test flashes on drifting lunar recordings — measure false triggers vs `K` and faintest recovered flash amplitude.
   - *Planetary:* real archive Mars/Jupiter recordings (`test_data/Mars_*.avi`, etc.) with injected synthetic bolide flashes and dark transits — measure disk-centroid stability, limb false triggers, and recovery.
   - *Failure injection:* simulate synthetic clouds, sudden 20 px wind jump, and a dark disk crossing the lock region to verify grace-hold and clean fallback with zero false-trigger storms.

2. **Computational cost (Release x64 build, mean & p95 ms/frame):**
   - Measure shift estimation time (`FeatureLock` NCC, disk CoG, phase correlation) separately from shifted `FlashDetector::analyze` + `updateReference` time.
   - Measure across Mono8, Mono16, and Bayer/RGB at representative sizes: planetary ROI (`480×480`, `640×480`), mid lunar (`1280×960`), and large lunar (`2128×2128`), verifying headroom against real camera frame intervals (30–150 fps).
   - Record all benchmark results in [docs/stabilized-detection-benchmark.md](docs/stabilized-detection-benchmark.md) and review with the maintainer before starting Stage 2 (UI checkbox + Session integration).

**Stage 1 outcome (24 November 2026):** Measured in Release x64 ([benchmark report](docs/stabilized-detection-benchmark.md)). Sub-pixel bilinear stabilization improved shadow recovery on drifting sunlit lunar footage by +60% at `K = 12` (8/11 vs. 5/11; 11/11 at `K = 8`, 0 false) and eliminated false triggers on real drifting Mars footage at `K = 8` (0 vs. 9 false, `DiskCoG` 0.09 ms/frame, 100% lock), at a total cost of ~1.3 ms/frame at `480×480`, ~8.0 ms/frame at `1280×960`, and ~28 ms/frame at `2128×2128`. Integer-only shifting frequently performed worse than unstabilized, and 2D phase correlation performed significantly worse than both `DiskCoG` (planets) and `FeatureLock` (partial lunar views). Stage 2 should therefore ship **sub-pixel bilinear** with **`FeatureLock` + `DiskCoG` only** (zero OpenCV dependency in `src/detection/`), plus periodic anchor-preserving template refresh to handle multi-hour slow field rotation and lunar lighting changes cleanly.

### Stage 2 — Implementation & acceptance criteria (after benchmark review)

- `DetectionSettings::stabilizeDetection` (default `false`) wired through `watch_settings`, Detection panel checkbox + live status readout, `Session`, `FlashDetector`, and `event.txt`.
- Automated unit and integration tests covering:
  - Known fractional `(dx, dy)` shifts on bright flashes and dark shadows recover true native coordinates and suppress limb false triggers.
  - Raw Bayer mosaic phase is preserved (no cross-channel interpolation artifacts) and raw `event.ser` frames remain byte-for-byte untouched.
  - Transient lock loss holds `(dx, dy)` through a shadow crossing the lock box; sustained lock loss resets cleanly to unstabilized fallback with zero false-trigger burst; large drift recenters in-place without losing the learned interior model.
- `MANUAL.md` updated with the checkbox, when to use it, its CPU note, and what happens when lock is lost.

---

## Ideas not yet proposed

Work that earlier features set aside. None is designed yet; each would become a numbered feature when the observer asks for it or the data shows the need.

| Idea | Raised in | Waiting for |
| --- | --- | --- |
| Measure the flash width from the blur of the Moon's limb, instead of the typical seeing | Feature-30 | — |
| Watch the Moon's flashes and shadows at the same time | Feature-31 | The observer's decision ("not yet") |
| Start a session by itself: at dusk, or when the Moon rises above an altitude | Feature-32 (**Watch from dusk to dawn** removed) | The observer's interest; it would extend **Ends** |
| Register frames against field shift before detection | Feature-9, Feature-5 | Promoted to [Feature-50](#feature-50--optional-runtime-sub-pixel-registration-for-stabilized-detection) (building on [registration experiment](docs/registration-experiment.md)) |
| Guiding: refresh the lock template over a session; alt-azimuth mounts | Feature-8 (open decisions) | A real session whose lock score decays; an observer with an alt-azimuth mount |
| Binning above 1 for colour cameras (ZWO, Player One) and for QHY | Feature-10, Feature-16 | Hardware checks of the Bayer cell and the binned data |
| A GPS time reference | Feature-10, Feature-14 | A camera or receiver with GPS |
| One machine-readable clip record beside `event.txt` | [Project description](PROJECT_DESCRIPTION.md), open decisions | A decision on the format |
| Predicted ISS and satellite transits, and a transit tracker | Feature-13, Feature-5 | A real transit recorded first (see below) |
| Leave out a fresh detector's first seconds when Monitoring starts without a Preview before it (on a recording, Feature-48's runs saved a clip in the first second) | Feature-48 | Whether it happens at the telescope, where Monitoring follows Preview |
| Time-lapse: multi-point lunar alignment, wavelet sharpening, PNG copies of the stacks | Feature-18 | — |
| More camera families | Project description, stage 12 | An observer with the hardware |

---

## Field validation

What each implemented feature still needs outside the tests. Grouped by the kind of session, so that one night can cover several rows. Details are in each feature's own section in [implemented features.md](implemented%20features.md).

| Session | Feature | What to check |
| --- | --- | --- |
| Camera indoors | Feature-10 | QHY camera: native layout and bits, colour orientation, getters, Stop, unplug and reconnect, sustained frame rate. ZWO: a physical unplug. |
| Camera indoors | Feature-16 | A Player One and a ToupTek camera: RAW8 and RAW16 stream, bit placement, a clip opens in Siril with the right size, depth and times, drops are reported. Needs someone who owns one. |
| Camera indoors | Feature-25 | A slider drag in Preview reaches the camera once per pause. |
| Camera indoors | Feature-26 | Auto-tune's gain step and the readback check after **Apply**. |
| Camera indoors | Feature-30 | The ZWO and QHY report their pixel size, so the flash width needs only the focal length. |
| Mount indoors | Feature-8 | Connect through the ASCOM chooser in the window; EQMOD with the observer's mount. |
| Mount indoors | Feature-32 | EQMOD stops tracking at a scheduled end and after guiding is lost; **Lock lost after** feels right for passing clouds. |
| Laptop | Feature-23, Feature-28 | The main window and the auto-tune window at 125 % scaling: nothing cut off, box labels readable. |
| Moon, earthshine | Feature-27 | A representative earthshine recording: the next lunar validation milestone, which the rows below share. |
| Moon, earthshine | Feature-9 | Faint limit and false-candidate rate on earthshine, the sunlit limb and the terminator; whether one K serves both halves of the Moon. |
| Moon, earthshine | Feature-5 | Auto-stretch shows earthshine detail on a 16-bit recording. |
| Moon, earthshine | Feature-26 | On a replay: the suggested K stays quiet over a known span, and marked test flashes are recovered (Done-when 3). |
| Moon, earthshine | Feature-30 | The suggested minimum patch and the footprint on real earthshine. |
| Moon, earthshine | Feature-31 | The flashes watch with **Ignore brighter than** leaves out the sunlit side and keeps the earthshine. |
| Moon, earthshine | Feature-8 | Guiding under the Moon: the drift readout and `guiding.txt` agree with what the mount did; whether the lock score decays. |
| Moon, any phase | Feature-10 | An hour on the Moon with each camera: frame rate, drops, detector cost, candidates and write rate. |
| Moon, any phase | Feature-12 | The observer reviews the overviews of real lunar clips. |
| Moon, sunlit | Feature-13, Feature-31 | A real transit (plane or ISS) caught by the shadows watch with its defaults (K 12, 20 px). |
| Moon, sunlit | Feature-45 | The shadows watch on a live sunlit stream: no candidate from a brightening, the readouts, trace and sigma map follow the deepest fall, and Maximum patch / Maximum box / patch change the shadow limits. |
| Moon, sunlit | Feature-14 | A predicted ISS transit recorded with a schedule. |
| Night sky | Feature-26 | Auto-tune in full darkness: the follower's limits and gain ceiling. |
| Moon, earthshine | Feature-34 | **Tune camera** on a real camera: the rounds it takes, the dark rounds, and the gain chosen by noise; ideally with a camera that has a high-conversion-gain mode. |
| Night sky | Feature-34 | **Tune camera** in full darkness: exposure to the watch's longest, then gain, then the comparison by noise. |
| Night sky | Feature-35 | A night with each sky watch: the fast one at 40 ms on meteors, the slow one's detection defaults (background over 10 s, release after 60 s) on real frames. |
| Night sky | Feature-36 | The slow sky's time-lapse from dusk with the follower on: the frames, the video's stretch through the night, and the disk use. |
| Night sky | Feature-46 | **Open SER** and **Show in folder** from a real report in Edge or Chrome after a Release start: the browser's prompt, the SER player opening the clip, Explorer selecting it. |
| Night sky | Feature-46 | The groups over more nights: brief events kept apart from aircraft crossings, a long crossing shown as one event, a meteor on three frames or more (moving), and the Moon's watches. |
| Moon, sunlit | Feature-49 | A night on the sunlit Moon with the shadows watch's one-frame default: how often seeing, a flicker or a bird makes a one-frame dark candidate, and a real ISS or satellite crossing caught. |
| Any session | Feature-47 | **Status on your phone** at the telescope: the Windows firewall's question, the QR code read by the phone's camera, the page over Wi-Fi through a night (lock lost, clips, the session's end), `<name>.local` on the observer's phones, and Tailscale away from home. |
| Overnight | Feature-18 | A Moon and a planet time-lapse at the telescope. |
| Moon, sunlit | Feature-37 | A Moon time-lapse with **Also watch for transits** on a real sunlit lunar stream: gap coverage, throughput, burst grace, and saved clips. |
| Moon, any phase | Feature-40 | Guided lunar run with rough polar alignment and real Dec backlash: verify drift compensation, backlash take-up, and no false fruitless stops. |
