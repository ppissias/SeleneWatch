# SeleneWatch features

Open proposals, ideas not yet proposed, and what remains to check in the field. What is already in the program, with its full design and review history, is in [implemented features.md](implemented%20features.md). When moving completed features to `implemented features.md`, append them in full detail without removing or condensing specifications, review notes, or design decisions.

On 21 November 2026 every feature whose software was complete moved there: Features 5, 8, 9, 10, 12, 13, 14, 16, 17, 18, 22 to 26 and 28 to 31, and Feature-32 once it was built the same day. Their remaining checks at the telescope or on hardware are listed under [Field validation](#field-validation) at the end of this file.

## Contents

| Feature | Focus | Status |
| --- | --- | --- |
| [Feature-27 — Detector tuning, capture status and nightly review](#feature-27--detector-tuning-capture-status-and-nightly-review) | Explain detector feedback, show what is being saved, review a night's coverage and candidates, and validate tuning on earthshine | Item 3 (nightly review) implemented; items 1 and 2 proposed |
| [Feature-34 — Auto-tune sets the camera itself](#feature-34--auto-tune-sets-the-camera-itself) | Step 1 of auto-tune changes exposure and gain live, measuring after each change, until the brightness is in the aim; then the camera's best gain from its measured noise | Stage 1 (the live loop) implemented; stage 2 (photon transfer) proposed |
| [Ideas not yet proposed](#ideas-not-yet-proposed) | Work set aside in earlier features, waiting for data, hardware or a decision | Not proposed |
| [Field validation](#field-validation) | Checks of implemented features that need the sky, a mount or a camera | Open |

## Feature-27 — Detector tuning, capture status and nightly review

*Proposed on 21 November 2026 following a review of the observing workflow.*

The existing observing modes fit three different jobs. Monitoring preserves unexpected events and their lead-up, Record covers the full interval of a predicted event, and Time-lapse stacks selected frames to show gradual change. Display stretch lets the observer examine faint earthshine while the SER retains the original samples. Keep those choices. This proposal addresses what the observer needs to know while tuning, during capture and the following morning.

### 1. Explain what the sensitivity trace measures

The quick start advises raising K above the ten-second peak trace. The slider help says a mark beyond K would trigger. A single noisy pixel can produce that mark. The detector also tests connected patch size and shape and, when selected, confirmation on a second frame. Following the current advice can cost sensitivity even when the detector was already rejecting the peaks. In the terminator replay, K=6 recovered four of five marked flashes. K=24 produced a quiet control run and recovered one of five. These are results for that particular recording. [Quick start](README.md) · [Slider help](detection_panel.cpp) · [Replay measurements](docs/reference-and-manual-validation.md)

Update the trace label, help and observing instructions to explain the peak in pixel-noise units. Direct observers to **Would trigger** for the detector's actual decision and to a marked test flash to check recovery at a chosen position, amplitude and duration. This wording correction can ship independently of Feature-26.

### 2. Say what is being saved

An observer can press **Record** in Preview and see “capture OFF” above a running recording readout. Name the two activities explicitly. For example, while a SER is being written in Preview, show **Automatic event clips off · Continuous recording on**. In Monitoring, show whether automatic clips are armed and whether a continuous recording is also running. Keep recording failures visible. [Mode banner](mainwindow.cpp) · [Observing modes](MANUAL.md)

### 3. Make the first event-browser view a night in chronological order

After a patrol, the observer already has an overview, a short animation and raw frames for each saved clip. The harder question is “When was I watching, and which candidates deserve another look?” Show Monitoring intervals and known interruptions alongside the candidates. For example, a sky-follower exposure change briefly restarts Monitoring. A quiet interval, a known interruption and a candidate whose clip could not be saved should each be identifiable. Where older logs establish only the start of an interval, show its end as unknown. [Sky patrol and saved outputs](MANUAL.md)

Let the observer step through candidates using the existing overviews and animations, open the original clip, and mark a candidate for follow-up. Store that decision beside the evidence. Start with the candidate log and clip metadata. Record any missing Monitoring boundaries needed to establish coverage for future nights.

**Implemented for nightly review.** Each intentional Moon or Sky monitoring run owns its logs, triggered clips, continuous recordings and offline HTML report. The finished report is offered in the main window and can be reopened from the Session menu. Coverage, clip previews, suggested-kind filters, trigger outcomes and follow-up marks support morning review. The other Feature-27 proposals and lunar field calibration remain open. [User manual](MANUAL.md)

### Field validation and acceptance

- In Preview, use an isolated above-K pixel that fails the configured minimum patch size. The trace shows the peak and the detector reports no candidate. The quick start and in-app help explain both readings consistently.
- Exercise Preview and Monitoring with continuous recording both on and off. In each combination the banner correctly names both activities, including after recording stops or fails.
- Review a watch with zero candidates and a watch containing a known interruption, a saved clip and an unsaved candidate. The observer can account for each outcome, open a saved preview and find their follow-up mark after reopening the night.
- Make a representative earthshine recording the next lunar validation milestone. Replay the same span with candidate settings under consideration for Feature-26. Count candidates and clips requiring review on the original data. In separate marked trials, report flashes recovered out of those injected, with their positions, amplitudes and durations. Use both measurements when choosing an observing profile. [Open lunar calibration work](docs/feature-9-validation.md)

---

## Feature-34 — Auto-tune sets the camera itself

*Proposed and agreed on 22 November 2026, from a discussion with the observer.*

**What the observer saw.** Step 1 of auto-tune (Feature-26) moved in small steps. Each **Measure camera** → **Apply** → check round changed the gain by at most a quarter of the camera's range (usually one to three twentieths), and the exposure by at most ×4 before the black level was known. The observer then had to press **Apply** again. Earthshine ten times too dark took three or four rounds of clicks. The observer asked for auto-tune to change exposure and gain live, taking as long as it needs, to find the settings for each case.

**Why the steps were small.** Gain units differ between cameras (tenths of a decibel on ZWO, arbitrary steps on others), and Feature-26 suggested one bounded step at a time because every suggestion waited for the observer. Once the observer has asked auto-tune to set the camera, nothing is gained by stopping between rounds, and each measurement shows how this camera answers.

### Stage 1 — the live loop

1. **One explicit start.** **Tune camera** in step 1 replaces **Measure camera**. It runs only in Preview with a camera, never in Monitoring or during a recording, as the camera settings themselves.
2. **Measure, change, settle, measure again.** Each round measures the area's brightness for two seconds, as before. It then sets the next exposure or gain live through the camera settings editor, waits for the camera to report the values back and for Preview to settle, and measures again. It goes on until the brightness is in the aim, or the camera cannot get closer, or 12 rounds have passed (the brightness may then be changing: clouds or twilight).
3. **One control at a time, in each workflow's order** (as Feature-26):
   - **Moon, flashes:** an exposure outside 10–40 ms is first brought into it; then gain; the exposure moves within 10–40 ms only when the gain is at its limit.
   - **Moon, transits, and the sky:** a darker scene lengthens the exposure first (up to 10 ms for transits, the longest exposure for meteors for the sky), then raises gain; a brighter scene lowers gain first, then shortens the exposure.
4. **Steps from the measurements, not fixed caps.**
   - **Exposure** moves on a logarithmic scale. The first move is in proportion to the brightness. With a black level that move falls short and never overshoots, so the next round corrects it.
   - **The black level** *(added while building)*: once two measurements at the same gain and different exposures differ by more than quantisation, they give the camera's black level, and every later move, exposure or gain, aims at the signal above it. Without it, a gain move after a dark sky's exposure moves overshot to the end of the range and came back.
   - **No answer** *(added while building)*: when the brightness did not change after a move (the signal still hidden under the black level), the next move goes twice as far.
   - **Gain:** the first move assumes one stop for every tenth of the camera's gain range. After that, each move uses the measured response of the last two measurements (a secant step on the logarithm of the brightness).
   - **Never past what was measured:** the measurements on each side of the aim bound the next move. A move that would leave those bounds halves the interval instead, so the loop cannot swing back and forth.
   - **Saturation** hides the true level. A saturated measurement only bounds the move. The next move is a quarter of the exposure, or a quarter of the gain range down.
   - It stops when the next setting would equal the current one, for example when the aim lies between two gain units.
5. **Shown as it goes.** The panel lists each round: the measured level, and the control moved, from and to. The preview shows each change live.
6. **Safe as before.**
   - The camera's own range and each workflow's limits are never exceeded.
   - Changing a camera control by hand, the target, the mask or the measured area stops the loop, and so does **Cancel**.
   - **Restore previous settings** sets the exposure and gain that were in use before the loop began.
   - Every change the loop makes is logged in `candidates.txt` as an `autotune_camera` line with its round, the measured level and the values set. The camera's readback is logged as an `autotune_readback` line, as before.
7. **What stays from Feature-26.** At the end the panel shows the verdict and warnings, as before. The Offset (pixels at 0) and, for the sky, the exposure follower's limits remain rows the observer ticks and applies; applying an Offset runs the loop again, since it shifts the black level.

### Stage 2 — the camera's best gain from its measured noise (proposed)

"In the aim" is a stand-in. What decides the faintest flash that can be caught is the threshold, K × the noise, counted in photons rather than ADU. While the loop steps through gains, it could measure, per gain, the variance of each pixel over time against its mean (a photon-transfer measurement). This gives the camera's conversion factor (ADU per electron) and read noise at each gain, from the camera itself. The loop would then choose, within the aim, the gain whose threshold in photons is lowest. It would be a measured optimum for the observer's camera and sky on the night, not a rule of thumb. It needs stage 1 first, and the per-pixel temporal variance that the detector already keeps.

### Stage 1 status & verification

Built on 22 November 2026.

- **Core:** `nextCameraStep` in `auto_tune.cpp` is a plain function over the loop's measurements, like the rest of auto-tune. `suggestCamera` still gives the final verdict, warnings and the Offset and follower rows; the loop removes its exposure and gain rows.
- **Window:** `mainwindow_autotune.cpp` runs the rounds: measure, `nextCameraStep`, set through the camera settings editor, wait for the readback, measure again. **Measure camera** and the check after Apply are replaced by **Tune camera**; the panel lists the rounds and has **Restore previous settings**.
- **Rounds on model cameras** (counting the final measurement in the aim): dark earthshine 4, bright earthshine 3, an exposure outside 10–40 ms 3, a saturated Moon for transits 5, the night sky 1, the dusk sky 3; gain curves from 5 to 60 units per stop on ranges of 100 and 600, 2 to 5. The fake camera of the window test, a dark sky under an 8-bit black level, takes 6: three exposure moves to 250 ms, then two gain moves.
- **Tests:** `auto_tune` (`theLoopReachesEachAimInAFewRounds`, `theLoopLearnsAnyGainCurve`, `saturationOnlyBoundsTheLoopsMove`, `theLoopStopsWhenTheAimIsOutOfReach`); `ui` (`autoTuneTunesTheCameraLiveThenWatchesK` with a fake camera whose frames follow exposure and gain, including Restore; `autoTuneOutlinesTheEarthshineBox` ending out of reach).
- **Not run:** the real-window test `tests\gui_e2e_test.ps1` (its element name was updated).
- **Pending:** a night with a real camera on the earthshine and on the sky.

### Done when (stage 1)

- A model camera with a black level and an unknown gain curve reaches the aim in each workflow in a few rounds, from far too dark, from far too bright and from saturation, and never beyond the camera's or the workflow's limits.
- The loop stops with the reason when the aim is out of reach, and when the aim lies between two settings.
- A window test with a fake camera whose brightness follows exposure and gain: one press of **Tune camera** brings it into the aim through live changes, logs each round, and **Restore previous settings** sets the earlier values back.
- MANUAL, the camera settings guide and this record describe the loop.

---

## Ideas not yet proposed

Work that earlier features set aside. None is designed yet; each would become a numbered feature when the observer asks for it or the data shows the need.

| Idea | Raised in | Waiting for |
| --- | --- | --- |
| Measure the flash width from the blur of the Moon's limb, instead of the typical seeing | Feature-30 | — |
| Watch the Moon's flashes and shadows at the same time | Feature-31 | The observer's decision ("not yet") |
| Start a session by itself: at dusk, or when the Moon rises above an altitude | Feature-32 (**Watch from dusk to dawn** removed) | The observer's interest; it would extend **Ends** |
| Register frames against field shift before detection | Feature-9, Feature-5 | Longer unclipped earthshine and bright-limb comparisons ([registration experiment](docs/registration-experiment.md)) |
| Guiding: refresh the lock template over a session; alt-azimuth mounts | Feature-8 (open decisions) | A real session whose lock score decays; an observer with an alt-azimuth mount |
| Binning above 1 for colour cameras (ZWO, Player One) and for QHY | Feature-10, Feature-16 | Hardware checks of the Bayer cell and the binned data |
| A GPS time reference | Feature-10, Feature-14 | A camera or receiver with GPS |
| One machine-readable clip record beside `event.txt` | [Project description](PROJECT_DESCRIPTION.md), open decisions | A decision on the format |
| Predicted ISS and satellite transits, and a transit tracker | Feature-13, Feature-5 | A real transit recorded first (see below) |
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
| Moon, sunlit | Feature-14 | A predicted ISS transit recorded with a schedule. |
| Night sky | Feature-26 | Auto-tune in full darkness: the follower's limits and gain ceiling. |
| Overnight | Feature-18 | A Moon and a planet time-lapse at the telescope. |
