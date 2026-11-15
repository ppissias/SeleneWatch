# Feature-10 — Preview and Monitoring

Implemented 14 November 2026. The observer tunes a stream before enabling event capture. The names are **Preview** and **Monitoring**; “Watching” is not a separate state.

| State | Acquisition and analysis | Camera controls | Evidence output |
| --- | --- | --- | --- |
| Stopped | Off | Configure / read back | Existing queued jobs can finish |
| Preview | Active; same detector as Monitoring | Every supported writable live control; layout changes restart Preview | No new automatic/manual clips or candidate records |
| Monitoring | Active on every delivered frame | Fixed acquisition settings; display and detection controls remain editable | Automatic event clips and explicit Save now |

**Start preview** is the default. **Start monitoring** enables capture. **Stop monitoring** hands off the current available clip with `collection_end: monitoring stopped`, returns to Preview, and leaves acquisition running. The disarm frame is outside the old clip. Existing writer jobs retain their immutable pixels and metadata and drain in the background. **Stop stream** ends acquisition and closes a camera. The mode change stays pending until the worker reaches a frame or the SDK returns without one.

Save now is disabled in Preview intentionally: its capture-OFF banner promises no new recordings. Test injection works in either running state. Preview injections show diagnostic decisions; Monitoring injections use the existing per-frame TEST markings and evidence path. Previously written files are never changed.

## Live tuning and feedback

The ZWO panel queues exposure, gain, offset, USB bandwidth and high-speed edits where the camera reports them writable. QHY queues reported exposure, gain, offset, USB traffic and transfer speed. The panels coalesce edits for 180 ms and submit typed values after the edit is committed. The worker applies controls between SDK reads, reads the actual controls and layout back, and publishes copied snapshots. The panels show requested values alongside actual readbacks when the camera quantises them. ZWO flip stays disabled to preserve raw orientation. QHY changes to or from exposures of at least one second restart the SDK's live capture on the same connection. [QHY integration and validation](qhy-development.md)

Raw format, binning and ROI require **Apply format / ROI and restart Preview** because the SDK requires capture to stop. Invalid geometry is rejected before stopping a healthy Preview. These changes and all camera controls are disabled during Monitoring. Stopping Monitoring enables tuning again without disconnecting. Detection settings and display stretch remain adjustable during Monitoring; every trigger retains the detection settings it actually used.

The Camera dock and Detection dock are visible together. Live readouts show:

- peak sigma, K and percentage of the threshold, with the recent maximum;
- selected patch pixels versus the size limits and bounding-box area per patch pixel versus the adjustable compactness limit;
- noise floor in native ADU, peak position, and one-frame/two-frame persistence status;
- the existing 10-second sigma history, sigma/noise maps, active patches and reference-learning diagnostics;
- **Would trigger — capture OFF** in Preview, or **Candidate — capture ON** in Monitoring.

The panel names the rejection that applies to the displayed patch. Below K, the peak readout remains available and the patch measurements appear once pixels reach K. A dense frame with too many threshold pixels is reported as a whole frame. **Background and noise** holds the reference learning time and the minimum noise bound. Settings are copied to each trigger and to the clip's measured peak.

Open **View → Histogram** to check the original sample distribution in Preview or Monitoring. It refreshes from the GUI's latest frame up to five times per second while visible. The end-code percentages use the stream's declared sample range. **Fit range** adjusts the ADU axis and **Log counts** changes the vertical count scale.

After a live camera edit, the history and model are invalidated. Preview shows **Settling** instead of meaningful threshold proximity for an estimate of twice the longer old/new exposure plus one second. Every received frame still enters the analysis path; during this interval the reference is reinitialised using reused, charged arrays, so a delayed brightness step cannot latch the old reference. The normal model then learns again. The SDK does not stamp individual frames with their settings, so this estimate is not proof of a frame's configuration. A long old exposure is also allowed to complete before the no-frame watchdog uses the shorter new exposure.

## Capture boundaries and ownership

Starting Monitoring with a camera stops/closes Preview, reopens it with the chosen settings, reads them back, starts fresh acquisition and clears history/reference. No buffered tuning frame is assigned the new fixed evidence snapshot. The history starts short and builds normally; the first frame initialises the new reference. There is a brief acquisition interruption at this transition, visible in the frame/readout reset. This deliberately favours accurate evidence metadata over uninterrupted tuning-to-capture history.

Synthetic and recorded sources can retain their valid history and learned reference while switching modes. Arming clears patch persistence so an ongoing Preview patch is confirmed again and can be captured. A two-frame trigger needs two passing frames after arming; confirmation never combines a Preview frame with a Monitoring frame. Capture uses the same noise/size/compactness algorithm in both modes.

`Session` owns the mode and applies requests on the source thread. `EventCapture` gates clip creation and notifications while retaining the detector/ring path. Live camera requests, actual-profile snapshots and `StreamInfo` snapshots use mutex-protected copies. Preview creates no candidate journal; entering Monitoring queues the existing `session_start` record. A live settings notification outside Preview ends acquisition before any frame with changed settings can be recorded under the old metadata. Arming is rejected after a tuning notification until the source is reopened, including when the notification races with a pending arm request.

The manual recording and camera QA commands explicitly start in Monitoring, preserving unattended capture behavior.

## Validation

The hardware-independent tests cover Preview's lack of files/records, live diagnostic Would-trigger feedback, fresh persistence at arming, retained same-configuration history, exclusion of the disarm frame, history invalidation on camera changes, a pending-arm race, disarming during camera waits, native control readback clamps and worker serialization, rejection of live layout/orientation changes, fixed Monitoring metadata, simulated disconnect, partial evidence, and the existing manual camera probe. Offscreen window screenshots are inspected with Camera and Detection visible together.

The initial 14 November ZWO implementation passed all 10 CTest suites in Debug and Release, both with and without the ASI SDK. The offscreen UI checks cover mode-label changes when no new exposure arrives. The later QHY results are recorded alongside its hardware checklist. [QHY validation](qhy-development.md)

The connected **ASI120MM Mini**, SDK **1.41.0.0**, passed a separate automated offscreen `MainWindow` check using the real SDK:

| Check | RAW8 | RAW16 |
| --- | --- | --- |
| Full-frame layout | 1280×960, bin 1 | 1280×960, bin 1 |
| Live exposure / gain / offset / bandwidth | 2000 µs / 2 / 10 / 60 | 3000 µs / 4 / 11 / 50 |
| Preview frames received before Monitoring | 38 | 23 |
| Live edits without stop/start/reopen | Passed | Passed |
| Settling shown, then threshold feedback resumed | Passed | Passed |
| Preview candidate record absent | Passed | Passed |
| Monitoring used a fresh camera open | Passed | Passed |
| Stop monitoring kept the stream running | Passed | Passed |
| All saved native frame hashes matched | 7 / 7 | 3 / 3 |

Each mode wrote one manual clip, ended by stopping Monitoring, with matching exposure/gain/offset/bandwidth metadata. A forwarding SDK wrapper hashed each received native buffer before Session publication and compared every saved frame with the digest from that acquisition run/index. Independent header/length checks verified full frame size, native depth, frame count and complete timestamp trailers. This check used SeleneWatch's reader; it does not claim a new Siril/other-reader check. An earlier short smoke run also matched 7 RAW8 and 3 RAW16 frames, but did not wait for the complete visible settling cycle, so the table uses the subsequent verified run.

There were four video starts/four stops and seven opens/seven closes, including inspection and restoration. Before the final close, actual readbacks matched RAW8, 1280×960, bin 1, origin (0,0), exposure 1000 µs, gain 0, offset 9, bandwidth 40 and flip 0. No video was started for restoration. A later SDK initialization may select its own defaults; this is a before-close readback check. The camera is idle. The Mini does not expose high-speed control, which is covered by the deterministic SDK-boundary test instead.

The normalized [measurement record](preview-monitoring-measurements.json) omits the physical serial. Raw clips, hashes, harness and screenshots remain under ignored `out/qa-preview/`. Illumination was uncontrolled, builds/tests ran concurrently, and the clips are short workflow checks. These results do not establish throughput, lunar sensitivity, physical-unplug behavior, QHY qualification or a manual desktop observing session.
