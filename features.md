# SeleneWatch features

Open proposals, ideas not yet proposed, and what remains to check in the field. What is already in the program, with its full design and review history, is in [implemented features.md](implemented%20features.md). When moving completed features to `implemented features.md`, append them in full detail without removing or condensing specifications, review notes, or design decisions.

On 21 November 2026 every feature whose software was complete moved there: Features 5, 8, 9, 10, 12, 13, 14, 16, 17, 18, 22 to 26 and 28 to 31. Their remaining checks at the telescope or on hardware are listed under [Field validation](#field-validation) at the end of this file.

## Contents

| Feature | Focus | Status |
| --- | --- | --- |
| [Feature-27 — Detector tuning, capture status and nightly review](#feature-27--detector-tuning-capture-status-and-nightly-review) | Explain detector feedback, show what is being saved, review a night's coverage and candidates, and validate tuning on earthshine | Item 3 (nightly review) implemented; items 1 and 2 proposed |
| [Feature-32 — The end of a session](#feature-32--the-end-of-a-session) | One **Ends** setting for Monitoring and Time-lapse (local time, duration, dawn, Moon below an altitude); keep or stop the mount's tracking at every end; lost guiding ends the session | Proposed; the observer's decisions taken |
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

## Feature-32 — The end of a session

*Proposed on 21 November 2026, from a discussion with the observer.*

The observer often leaves the telescope unattended. A session must then end by itself, at a time the observer chose, and leave the mount in a known state. Today:
- **The sky** ends at dawn through **Watch from dusk to dawn**, which also starts it.
- **A time-lapse** has its own stop condition: until Stop, a fixed duration, a UTC end time, or a number of stacks. The end time is in UTC only.
- **The Moon** has no end: Monitoring runs until the observer stops it or something fails.
- **After any end** the mount keeps tracking. On a German equatorial mount, tracking for hours unattended can drive the telescope into the pier.
- **Lost guiding** does not end anything. When the lock finds no match for 2 s, it searches wider for 60 s and then gives up until the observer locks again. Guiding sends nothing meanwhile, and a time-lapse only shows a warning. A Moon or planet session then goes on while the target drifts out of the field.

### What shapes the design

1. **One way to end, for every mode.** Monitoring (Moon or sky) and Time-lapse share one **Ends** setting, with the same choices and the same wording. Times are shown in local time with UTC beside them.
2. **An end is complete.** Whatever ends the session, the evidence is finished and the mount is left as the observer chose.
3. **The mount: keep or stop tracking, nothing else.** SeleneWatch does not park, slew or unpark. The observer chooses, before the night, whether the mount keeps tracking or stops tracking at the end. The choice cannot be forgotten: it has no default, it is always shown, and keeping tracking carries a warning.
4. **Guiding that is lost ends the session.** An observer who turned guiding on wants a guided session. Without guiding the target drifts away and nobody is there to lock again.
5. **Every end says why.** The reason and what was done to the mount are logged and shown in the nightly report.

### The Ends setting

A row in the stream controls above the preview, beside **Start monitoring**, applies to Monitoring and to a time-lapse. It shows a summary and **Change…**. For example: *Ends 04:55 local (03:55 UTC): the Moon below 10° · then the mount stops tracking.*

**Change…** offers these conditions. Any number can be ticked; the session ends at the first one met. With none ticked, it runs until the observer stops it.
- **At a local time.**
- **After a duration**, from the start.
- **At dawn**: when the Sun rises above the **Sun below** limit. This needs the site's position.
- **When the Moon sinks below** an altitude (10° by default), for the Moon and a Moon time-lapse. This needs the site's position. The Moon's altitude comes from a low-precision lunar position with its parallax (accurate to a few tenths of a degree), in the same file as the Sun's (`sky_schedule`), with no new library. This is the natural end for an evening crescent: earthshine is observed in twilight and ends at moonset, not at dawn.

What changes in the existing controls:
- **The sky:** **Watch from dusk to dawn** becomes **Start at dusk**. Its stop at dawn becomes the **At dawn** condition, ticked for anyone who had the schedule on, and it can be combined with an earlier local time. The rule that the schedule stops only what it started stays.
- **Time-lapse:** **Fixed duration** and **UTC end time** move to **Ends**, the end time becoming local. **Number of stacks** stays in the time-lapse panel and is one more condition: whichever comes first.

### What an end does

**Which ends count:**
- an **Ends** condition being met;
- a failure: the camera lost, the source failing, the RAM ceiling;
- the disk reserve. *(New for Monitoring:* today a clip refused for the disk reserve is not saved, and Monitoring goes on saving nothing. It now ends.*)*
- lost guiding (below).

**What happens:**
- **Monitoring:** the open clip and any continuous recording are finished, the nightly report is written, guiding is turned off and the camera is closed.
- **A time-lapse:** the current period is finished and the video is built from the completed stacks, as at an early stop today.
- **Then the mount** is kept tracking or stopped, as chosen.
- **The record:** a `session_end reason="…" mount="…"` line in `candidates.txt` (or `timelapse.txt`). The nightly report shows the end and its reason. The window says what ended, when and why.

**The observer's own Stop leaves the mount as it is.** The observer is there, and may only be changing a setting in Preview.

### The mount at the end

**The choice.**
- **Keep tracking** or **Stop tracking**, shown on the **Ends** row whenever a mount is connected.
- It has no default. The first start of Monitoring or a time-lapse with a mount connected asks for it; it is then remembered, and **Change…** changes it.
- **Keep tracking** shows a warning on the row: *The mount keeps tracking after the session ends. On a German equatorial mount it can reach the pier: set the meridian limits in the mount's driver.*

**How it is done.**
- **Stop tracking** sets ASCOM `Tracking` to false, then reads `Tracking` back.
- A mount that does not let its tracking be set (`CanSetTracking` false) has the choice disabled, with the reason.
- If the mount is disconnected or does not answer, the failure is logged and the window says the mount may still be tracking.

**The mount interface gains one command, `stopTracking`.** It stops motion, so the rule that nothing SeleneWatch sends can leave the mount moving still holds. The [project description](PROJECT_DESCRIPTION.md)'s mount section and `guiding/mount.h` say so. `SimulatedMount` implements it for the tests.

**The sky needs no mount.** It is watched with a fixed camera and lens, so the mount choice is not shown in Sky mode, and the mount stays Moon-only as today.

### Lost guiding ends the session

While guiding is on, for Monitoring and for a time-lapse, the session ends:
- **At once** when guiding stops for a safety reason:
  - the mount stopped tracking, is slewing or is parked;
  - the side of pier changed;
  - corrections have no effect;
  - the pulse cap was reached;
  - a pulse does not end;
  - the mount does not answer.
- **After the grace period** when the lock is lost and not found again. The lock's wide search, 60 s today, lasts the grace period instead.
  - **The grace period** is set in the Guiding tab: 2 minutes by default, from 30 s to 10 minutes. Passing clouds often last longer than 30 s. With the mount tracking, the Moon drifts only slowly meanwhile, and the search covers 64 px.
  - **When the lock is found again** within the grace period, guiding goes on as now and nothing ends.

**Only guiding counts.** A lock without guiding, used only for the drift readout, ends nothing.

**The Guiding tab says it** beside **Start guiding**: *While guiding, a lost lock (after the grace period) or a guiding stop ends the session.* The same text is the control's help.

### Done when

1. **Core tests:**
   - the end decision is a plain function: the first condition met among local time, duration, dawn and the Moon's altitude, with none ticked meaning no end;
   - the Moon's altitude agrees with published positions within 0.3°.
2. **A scheduled end, with the simulated mount:** Monitoring ends, its clip and recording are finished, the stream is closed, tracking stops and the `session_end` line names the reason and the mount.
3. **Lost guiding, with the simulated mount:**
   - a mount that stops tracking ends the session at once;
   - a lock lost for longer than the grace period ends it;
   - a lock found again within the grace period ends nothing.
4. **The disk reserve:** a clip refused for the disk reserve ends Monitoring.
5. **Time-lapse:** an end at a local time builds the video; a lost lock while guiding ends the time-lapse.
6. **The mount choice:**
   - the first start with a mount connected asks for it;
   - **Keep tracking** shows the warning;
   - a mount that cannot set its tracking has the choice disabled;
   - the observer's own Stop leaves the mount tracking.
7. **The sky:** no mount choice is shown, and the existing dusk and dawn tests pass with **Start at dusk** and **At dawn**.
8. **Window:** every new control has help (Feature-24) and fits the small screen (Feature-23).
9. **Documentation:** the MANUAL and README describe **Ends**, the mount choice and the grace period.
10. **By hand:** EQMOD stops tracking at a scheduled end and after guiding is lost.

### Out of this step

- Parking, slewing or unparking the mount (the observer's decision).
- Ending a session because tracking stopped while not guiding.
- Starting a Moon session when the Moon rises above an altitude: a natural follow-up, cheap once the Moon's altitude exists.
- A mount for the sky.

---

## Ideas not yet proposed

Work that earlier features set aside. None is designed yet; each would become a numbered feature when the observer asks for it or the data shows the need.

| Idea | Raised in | Waiting for |
| --- | --- | --- |
| Measure the flash width from the blur of the Moon's limb, instead of the typical seeing | Feature-30 | — |
| Watch the Moon's flashes and shadows at the same time | Feature-31 | The observer's decision ("not yet") |
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
