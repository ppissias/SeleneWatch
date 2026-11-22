# Implemented features

Historical record of completed features in the program. Open proposals stay in [features.md](features.md). When moving completed features from `features.md`, append them here in full detail without removing or condensing specifications, review notes, or design decisions. This file is an append-only history.

Features 5, 8 to 10, 12 to 14, 16 to 18, 22 to 26 and 28 to 32 were moved here on 21 November 2026, when their software was complete. Some of their texts say they stayed in `features.md` until a field check was done; those checks are now listed under [Field validation](features.md#field-validation) in `features.md`.

## Contents

- [Feature-2 — Preview and synthetic source (Stage 2)](#feature-2--preview-and-synthetic-source-stage-2)
- [Feature-3 — RAM ring buffer (Stage 3)](#feature-3--ram-ring-buffer-stage-3)
- [Feature-4 — First detector and event capture (Stage 4)](#feature-4--first-detector-and-event-capture-stage-4)
- [Code review — November 2026](#code-review--november-2026)
- [Feature-7 — Evidence safety: shared frames, no blind detector, and a lossless file path](#feature-7--evidence-safety-shared-frames-no-blind-detector-and-a-lossless-file-path)
- [Feature-6 — Recordings in, SER out](#feature-6--recordings-in-ser-out)
- [Feature-11 — Sky watch](#feature-11--sky-watch)
- [Feature-15 — Sky extras](#feature-15--sky-extras)
- [Feature-19 — Event clip animated GIF preview](#feature-19--event-clip-animated-gif-preview)
- [Feature-20 — GitHub release notification](#feature-20--github-release-notification)
- [Feature-21 — Lunar and planetary time-lapse wording](#feature-21--lunar-and-planetary-time-lapse-wording)
- [Feature-9 — A measured trigger](#feature-9--a-measured-trigger)
- [Feature-5 — Display stretch and a ring sized in seconds](#feature-5--display-stretch-and-a-ring-sized-in-seconds)
- [Feature-10 — First light: camera adapters (ZWO, then QHY) and manual save](#feature-10--first-light-camera-adapters-zwo-then-qhy-and-manual-save)
- [Feature-8 — Feature lock and mount guiding](#feature-8--feature-lock-and-mount-guiding)
- [Feature-12 — An overview of every clip](#feature-12--an-overview-of-every-clip)
- [Feature-13 — A trigger for darkening](#feature-13--a-trigger-for-darkening)
- [Feature-14 — The full session record](#feature-14--the-full-session-record)
- [Feature-16 — A third camera family](#feature-16--a-third-camera-family)
- [Feature-17 — One remembered output folder and Preview manual recording](#feature-17--one-remembered-output-folder-and-preview-manual-recording)
- [Feature-18 — Time-lapse mode](#feature-18--time-lapse-mode)
- [Feature-22 — Separate Moon and Sky controls](#feature-22--separate-moon-and-sky-controls)
- [Feature-23 — A window that fits a small laptop screen](#feature-23--a-window-that-fits-a-small-laptop-screen)
- [Feature-24 — Help on every control](#feature-24--help-on-every-control)
- [Feature-25 — Sliders for camera settings](#feature-25--sliders-for-camera-settings)
- [Feature-26 — Auto-tune and suggested camera settings](#feature-26--auto-tune-and-suggested-camera-settings)
- [Feature-28 — Auto-tune's own window and explicit preview drawing](#feature-28--auto-tunes-own-window-and-explicit-preview-drawing)
- [Feature-29 — Site and instrument presets](#feature-29--site-and-instrument-presets)
- [Feature-30 — Minimum patch from auto-tune](#feature-30--minimum-patch-from-auto-tune)
- [Feature-31 — The Moon's flashes and shadows watched apart](#feature-31--the-moons-flashes-and-shadows-watched-apart)
- [Feature-32 — The end of a session](#feature-32--the-end-of-a-session)
- [Feature-33 — A Record button that becomes Stop](#feature-33--a-record-button-that-becomes-stop)
- [Feature-34 — Auto-tune sets the camera itself](#feature-34--auto-tune-sets-the-camera-itself)
- [Feature-35 — Two ways to watch the sky](#feature-35--two-ways-to-watch-the-sky)
- [Feature-36 — A time-lapse beside the slow sky watch](#feature-36--a-time-lapse-beside-the-slow-sky-watch)
- [Feature-37 — Transit watch between lunar time-lapse bursts](#feature-37--transit-watch-between-lunar-time-lapse-bursts)
- [Feature-38 — Switch observing watches during Preview](#feature-38--switch-observing-watches-during-preview)

## Feature-2 — Preview and synthetic source (Stage 2)

A `Frame` and a threaded `SyntheticFrameSource` draw a lunar disk with a slow moving marker. The window starts and stops the stream, shows the latest frame, and can drop preview frames when it falls behind.

The status line shows the stream state, the acquisition frame rate, the acquired frame count, and the resolution. The rate counts frames the source produces, including frames the preview drops. A one-second timer publishes it.

While the source is running it asks Windows for a 1 ms timer, so `sleep_until` can keep 80 and 120 fps. The request is released when the source stops.

## Feature-3 — RAM ring buffer (Stage 3)

The source thread pushes every acquired frame into a `FrameRingBuffer` of 150 frames. The buffer is independent of the window. When it is full, the oldest frame is discarded. `push` swaps pixel buffers with the caller, so a warm ring does not allocate a new buffer per frame.

The window reads the stored count and the memory use once a second. The text is `Buffer: 150/150 frames (46.1 MB)` at 640×480 Mono8. The same 150 frames are about 311 MB at 1920×1080 Mono8.

There is no frame-source interface and no `snapshot()`. One source does not justify the interface. A full copy of the stored frames waits for a trigger, which is the open work in [features.md](features.md).

### Decisions kept from the review

- `FrameRingBuffer` exposes `push`, `clear`, `size`, `capacity`, and `memoryUsageBytes`.
- A `std::mutex` guards the slots. A lock-free queue was not needed.
- The readout is not updated from the preview path. A timer samples it even when preview frames are dropped.

## Feature-4 — First detector and event capture (Stage 4)

Stage 3 is in place: the synthetic source fills a 150-frame ring, and the window previews the latest frame. Stage 4 of the roadmap in [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) is a simple flash detector and a saved copy of the frames before and after it fires.

The proof is one button. While the stream is running, **Inject flash** paints a short bright spot on the lunar disk. The detector notices that spot, and the program writes the surrounding frames to a folder. No camera, no video file, and no frame-source interface. One source still does not justify that interface.

### Synthetic flash

`SyntheticFrameSource` gains a `requestFlash()` method. It asks the worker to add a bright disk, for a fixed few frames, at a fixed place on the lunar disk. The spot is larger and much brighter than the frame-to-frame change of the slow moving marker, so the two are easy to tell apart. The button is enabled only while streaming.

The moving marker stays as it is. It is the thing the detector must not save.

### Detector

One small type, not a detector interface. It keeps the previous frame's pixels in a reused buffer and compares them with the new frame before that frame is pushed.

A pixel counts only when its gray value rises by more than a fixed threshold. A frame counts only when enough of those pixels lie on the lunar disk. A candidate starts only after that happens on two consecutive frames. The threshold, the pixel count, and the two-frame rule are named constants. A thin edge from the moving marker stays under the count. The injected spot does not.

The source thread runs this check. It does not allocate a new pixel buffer per frame.

### Saving the event

On the frame that starts a candidate, after that frame has been pushed:

- Copy the frames currently stored in the ring into a clip. That copy is the pre-event history plus the triggering frame. It is the one moment a full copy is justified. The window never calls it, and nothing calls it on a timer.
- Remember a fixed number of frames still to come. As each of those arrives, append a copy of that one frame. Do not copy the whole ring again.
- Ignore new candidates until this clip is finished, so one flash does not write a stack of folders.
- Hand the finished clip to another thread for writing. The source thread must not wait on disk.

`FrameRingBuffer` grows two copy methods for this, and nothing else. One copies every stored frame. One copies only the newest stored frame. Both may hold the mutex while they copy. That stall is acceptable once per event, not once per frame.

Write the clip as a directory of PNG files plus one text file, under `events/<timestamp>/` in the current working directory. The text file records the trigger frame index, the trigger time, the threshold, the bright-pixel count, and how many frames were kept before and after the trigger. PNG is enough to open the flash and confirm it. SER, FITS, and a video encoder wait.

The status bar says when a candidate starts and shows the folder path when the write finishes.

### Done when

Start the stream, let the buffer fill, and press **Inject flash**. A folder appears. The images show the disk before the spot, the spot itself, and the disk after it. A run with no injected flash does not create a folder.

### Out of this step

- A camera SDK, and a frame-source interface.
- Video or image-sequence playback.
- A second detector, registration, or a false-positive catalogue.
- An event browser. The folder is the review tool for now.
- Telescope guiding or mount control.

---

### Review notes

1. **Circular buffer unrolling (`copyAll`):**
   In a ring buffer, stored slots wrap around `head_`. The method that copies all stored frames must unroll them into strict chronological order (oldest first, newest last) rather than a raw array copy, so the resulting PNG sequence is chronologically ordered.

2. **Post-trigger frame collection:**
   When a post-trigger frame arrives on the acquisition thread, `frame` is already available in local memory right before `ringBuffer_->push(frame)` swaps it. Copying that single frame directly in the pipeline avoids acquiring the ring buffer mutex a second time for a `copyNewest()` call.

3. **Two-frame detection rule & timing:**
   With an injected flash duration of 3 to 4 frames:
   - Frame 1 of flash: bright-pixel count exceeds threshold. Detector records 1 consecutive detection frame. Frame 1 is pushed to the ring buffer.
   - Frame 2 of flash: bright-pixel count exceeds threshold again. Detector reaches 2 consecutive frames and fires the trigger.
   - Pre-event copy: copying the ring buffer at the moment of trigger naturally includes undisturbed baseline frames plus the initial onset frame (Frame 1).

4. **Lockout / debounce:**
   While the coordinator is collecting post-trigger frames and writing them to disk, the detector must be locked out so the ongoing flash does not trigger overlapping clips.

5. **Asynchronous disk writing:**
   Encoding 150–200 frames to PNG via Qt's `QImage::save(..., "PNG")` takes ~200–400 ms. Doing this on a dedicated background thread ensures the live acquisition thread is not blocked and dropped-frame counts remain zero during disk I/O.

6. **Candidate parameters:**
   - Pre-event frames: All frames currently in the ring (up to 150 frames).
   - Post-event frames: 30 frames (~1 second at 30 fps).
   - Flash duration: 3 frames.
   - Flash radius: 8 pixels (area ~200 pixels, vs moving marker radius 4, area ~50 pixels).
   - Flash intensity: 255.
   - Delta threshold: +60 gray levels.
   - Minimum qualifying pixel count: 120 pixels.

### Remarks on the review

Most of the review should stand. One detection step is wrong as written, and one performance claim is stronger than the copy allows.

**Accepted**

- `copyAll` must walk the ring from oldest to newest. A raw copy of the slot array is wrapped and, before the buffer is full, includes empty slots. The PNG sequence has to be chronological.
- Do not add `copyNewest`. The post-event frame is still in hand before `push` swaps its buffer away. Copy that one frame there. The ring buffer gains only the full chronological copy, used once when a candidate fires.
- One flash produces one clip. Ignore further candidates while post-event frames are being collected and while that clip is still being written.
- Encode and write the PNGs on another thread. The source thread must not call `QImage::save`.
- The starting numbers fit the current synthetic image. The disk is about 145–185 gray. A spot set to 255 clears a +60 rise. The moving marker is about 50 pixels and shifts by 3 pixels a frame, so the new pixels it exposes stay well under 120. A radius-8 spot is about 200 pixels and clears 120. Keep these as named constants.

**Changes**

- Frame 2 of the flash must not be compared with frame 1. Both are already bright, so their difference is about zero and the two-frame rule never fires. Hold the pre-flash image as the reference. Frame 1 and frame 2 are both compared with that quiet image. If frame 2 also passes, push it and then copy the ring: the copy contains the baseline, the onset frame, and the triggering frame. If frame 2 fails, drop the count and let the reference advance.
- A background write does not keep dropped frames at zero. Copying the ring still holds the mutex for one pass over the stored pixels, and that can stall acquisition for that pass. That one stall is accepted. The background thread only removes the disk time, which is the long part.

---

## Code review — November 2026

A review of the whole tree as of the uncommitted Stage 4 work (detector, event clip, writer). The code builds cleanly with MSVC at `/W4 /permissive-`; the only warnings come from Qt headers. The Stage 4 flow works as specified. The findings below are ordered by weight. Each item says what is wrong and what fixes it. None of them asks for a new interface or framework.

### A. Correctness and evidence integrity

1. **Writer shutdown can hang on exit.** [`EventWriter::shutdown`](event_writer.cpp#L33-L43) calls `request_stop()` and `notify_all()` without holding `mutex_`. The worker can test the wait predicate (no stop, queue empty), lose the CPU, and only then block. A notify sent in that gap is lost, and `join()` waits forever. The window closes, the process does not. Fix: take the lock around `request_stop()`, or use `std::condition_variable_any` with the `stop_token` overload of `wait`.

2. **Per-frame timing is not saved.** [`event.txt`](event_writer.cpp#L108-L114) records only the trigger time. Each frame carries `frameIndex` and a timestamp, but the PNG files are named `0000.png`, `0001.png`… and lose both. For a flash lasting one to three frames, the time of each frame *is* the evidence. Write one line per frame (file name, frame index, UTC timestamp) in the metadata file.

3. **Times are local, not UTC.** The folder name and `trigger_time` come from `QDateTime::fromMSecsSinceEpoch(...)`, which is local time. Observations are compared in UTC. Write `trigger_time` in UTC with an explicit `Z`, and name the folder from UTC as well.

4. **The onset frame is not identified.** The detector fires on the *second* bright frame. `trigger_frame_index` is that second frame, and `frames_before` counts the onset frame as "before". Someone reading the metadata would place the flash one frame late. Record the first rising frame index next to the trigger index. The detector already knows it: it is the frame that set `consecutive_` to 1.

5. **The two-frame rule will miss most real impact flashes.** Published lunar impact flashes at 25–30 fps are often visible in a single frame. The rule is right for the synthetic stage, where it rejects the marker edge, but it should not be carried to real data as is. Before Stage 5, decide whether a single frame above a higher threshold also counts as a candidate. A missed flash is not recoverable. An extra candidate costs a review. This is a design decision for the next stage, not a bug today.

6. **The detection threshold is written from the detector, not from the clip.** [`event_writer.cpp`](event_writer.cpp#L2) includes `flash_detector.h` to print `FlashDetector::kDeltaThreshold`. The writer then reports what the constant is *now*, not what the detector used. It also makes the recording side depend on the detection side. Put the threshold (and the minimum pixel count) in `EventClip` when the candidate starts, and drop the include.

7. **A partial write leaves no metadata.** `writeClip` writes all PNGs first and `event.txt` last. If PNG number 40 fails, the folder holds 40 images and no note to say what they are. Write `event.txt` first, and append a final line (or a second small file) when all frames are on disk.

8. **The status can show "Candidate" after Stop.** [`beginCapture`](mainwindow.cpp#L260-L263) queues a lambda that sets "Status: Candidate". If Stop is pressed while that lambda is still queued, it runs after `onStopClicked` has set "Stopped". When the partial clip is written, [`onEventWritten`](mainwindow.cpp#L186-L189) only resets the label if the source is running, so "Candidate" stays. Set the label in `onEventWritten` from the current state in every branch.

9. **Events are saved relative to the working directory.** `QDir::current()` depends on how the program was launched (Visual Studio, the shortcut, Explorer). Clips can end up in an unexpected folder or fail in a read-only one. Until there is a setting, resolve the folder once at start (for example next to the executable or under the user's Pictures/Documents) and show the full path in the status bar, which already happens on success.

10. **An exception on the source thread ends the process.** `copyStored()` allocates a full copy of the ring on the source thread. At 1920×1080 and a larger ring that is hundreds of MB. A `std::bad_alloc` there escapes the `jthread` and calls `std::terminate`. The same holds for the writer thread. Catch at the top of both thread functions, stop cleanly, and report the error to the window. Do not try to recover inside the hot loop.

### B. Design and ownership

11. **The event coordinator lives inside `MainWindow`.** The detector, the clip under construction, `collecting_`, `postRemaining_`, the lockout, and the ring buffer pushes are all `MainWindow` members, and [`onFrameArrived`](mainwindow.cpp#L207-L232) runs them on the source thread. This is the "Event coordinator" box of the architecture, inside the UI class. It breaks two stated rules: the processing core should not depend on Qt UI classes, and each piece of mutable state should have one clear owner. Those fields are touched by the source thread while running and by the GUI thread after `stop()` joins. That is correct today, but only because of the join, and nothing in the code says so.

    Move this into one plain class (for example `EventCapture`) that owns the ring buffer, the detector, and the clip in progress, and exposes `onFrame(Frame&)` plus a "clip finished" callback. `MainWindow` keeps the preview, the counters, and the buttons. The new class can be tested without a window (see item 18). This is a layer with its own responsibility, not an abstraction for its own sake.

12. **The lunar disk geometry is written twice.** Both [`SyntheticFrameSource::generateFrame`](synthetic_frame_source.cpp#L184-L187) and [`FlashDetector::countRisingPixels`](flash_detector.cpp#L56-L59) compute "centre of frame, radius 0.38 × short side". The detector only works because it knows how the synthetic image is drawn. A real Moon is not centred and changes size. For now, give the disk region to the detector from one place (a small struct or a precomputed mask), so the coupling is visible and there is one number to change. Precomputing the mask also removes the per-pixel distance test from the hot loop.

13. **`Frame` depends on QtGui.** [`Frame::toQImage`](frame.h#L27-L45) puts `QImage` in the core frame type. Every core file that includes `frame.h` pulls in QtGui. Move the conversion to a free function used by the preview and the writer. `Frame` stays plain data.

14. **Frame fields are copied by hand in two places.** [`FrameRingBuffer::push`](frame_ring_buffer.cpp#L16-L26) and [`copyStored`](frame_ring_buffer.cpp#L50-L58) list every metadata field. When `Frame` gains a field (exposure, gain, sensor temperature), both must change, and missing one fails silently. In `copyStored`, `Frame copy = slot;` does the same work. In `push`, swap the buffers first and then assign the metadata in one statement, or keep a short helper next to `Frame`.

15. **The trigger frame is passed after its pixels are gone.** In the trigger path, `ringBuffer_->push(frame)` swaps `frame.data` away, and then `beginCapture(frame)` receives it. It only reads `frameIndex` and `timestamp`, so it works, but the next person who reads pixels there will get the previous slot's image. Pass the index and time explicitly, or call `beginCapture` with what it needs before the push.

### C. Unused or misleading code (principle 3)

16. **Unused API in `SyntheticFrameSource`.** `setResolution`, `width()`, `height()` and `targetFps()` have no caller. `setResolution` also writes `width_`/`height_` without synchronisation against the worker thread. `start()` always returns `true` and the result is ignored. Either remove them, or use `width()`/`height()` for the resolution label, which is currently the hard-coded text `"640 x 480 (Mono8)"`.

17. **The Qt 5 path has no caller.** [`CMakeLists.txt`](CMakeLists.txt#L11) searches for Qt 6 *or* Qt 5, and [`qt.cmake`](qt.cmake) defines Qt 5 fallbacks. The code needs Qt 6 (the `QSpinBox::valueChanged` connect does not compile against Qt 5), and the Qt 5 macro has a bug (`add_library(name ...)` uses the literal word `name`). Require Qt 6 directly, mark the components `REQUIRED`, and delete `qt.cmake`. Link Qt `PRIVATE`, not `PUBLIC`; nothing links against the executable. Smaller items: the FPS clamp in `setTargetFps` allows 240 while the spin box stops at 120, and the `context_ == nullptr || !finished_` check in the writer guards a case the only caller never creates.

### D. Tests and tooling

18. **There are no automated tests.** The project description asks for them, and the current pieces are easy to test without a window or a camera:
    - `FrameRingBuffer`: oldest-first order after wrap, before the first wrap, after `clear()`.
    - `FlashDetector`: the marker alone never fires; a 3-frame spot fires on frame 2; a 1-frame spot does not; the reference holds during a candidate.
    - The event capture class from item 11: one clip per flash, the right number of post frames, a partial clip on stop.

    Qt Test is already available under the same LGPL terms as the rest of Qt and needs no new dependency. Add a `tests/` target with CTest. The Stage 4 "Done when" check is then automatic instead of manual.

19. **Compiler settings.** Set `/W4 /permissive- /utf-8` in `CMakeLists.txt` (MSVC only). The tree is already clean at `/W4`, so this costs nothing and keeps it clean. `/utf-8` matters because comments and the window title contain non-ASCII characters (`–`, `—`).

### E. Documentation drift

20. Stage 4 is implemented but still described above as open, and [implemented features.md](implemented%20features.md) stops at Stage 3. It also still says `FrameRingBuffer` has no copy method; it now has `copyStored`. The review notes above call the method `copyAll`. [README.md](README.md) says only the Qt scaffold exists. Move Stage 4 to *implemented features.md* when it is committed, keep the review decisions, and update the README status line.

21. The rules file is referred to as `AGENTS.md`, but the file in the tree is `Agents.md`. Windows does not care; GitHub links and other tools do. Pick one name.

### Not raised

These were checked and are fine as they are: the pixel-buffer swap in `push` and its reuse in `generateFrame`; the preview back-pressure flag; the 1 ms timer request and its release; the oldest-first walk in `copyStored`; the lockout covering post-event collection and the write; the queued delivery of writer results to the window, including on shutdown, where Qt discards events for a destroyed receiver.

### Suggested order

Items 1–4 and 6–8 are small and touch evidence directly; do them first. Item 11, with tests from item 18, is the one structural change, and it is easier to make before a real camera arrives than after. Item 5 should be decided before Stage 5. The rest can follow.

### Remarks on the code review

The review is right about the evidence and about one real hang. The structural move is justified later, together with tests. A few items should stay smaller than the review suggests.

**Fix now**

- Item 1 is a real lost wakeup. `shutdown` sets the stop flag and notifies without holding `mutex_`, so the worker can pass the predicate, miss the notify, and block in `wait` forever. Take the lock around `request_stop`, or wait with the `stop_token` overload.
- Items 2, 3, 4, 6, and 7 are the clip's scientific record. Save UTC times, one line per frame (file name, frame index, timestamp), and the index of the first rising frame next to the trigger index. The detector does not store that index today; remember it when `consecutive_` becomes 1. Store the threshold and the pixel count that were used, on the clip. Write `event.txt` before the PNGs, and mark it complete after the last file.
- Item 8 is a real status bug. The queued "Candidate" update can run after Stop has set "Stopped", and a successful save does not correct the label when the stream has already stopped.
- Item 15 is a footgun. `beginCapture` is called after `push` has swapped the pixel buffer away. It only reads the index and the time today. Pass those two values in before the swap.

**Do with the next structural pass**

- Item 11 belongs with item 18, not before the evidence fixes. One plain `EventCapture` type that owns the ring, the detector, and the clip is a real boundary, and it is the seam the tests need. It should not be an interface.
- Item 12: keep a single disk description so the generator and the detector cannot drift. A precomputed mask can wait.
- Item 13: move `toQImage` off `Frame` when the tests arrive, so the core frame type does not pull in Qt Gui.
- Item 19 is cheap and worth setting. `/W4` is already clean. `/utf-8` matches the non-ASCII text already in the sources.

**Decisions, not defects in this stage**

- Item 5 is right, and it is not a bug in the synthetic stage. A real impact flash is often one frame. Decide that rule before a camera is attached. An extra saved candidate is cheaper than a missed flash.
- Item 9: pick one stable folder, next to the executable, and keep showing the full path. Do not add a settings window for it.
- Item 10 can throw at a much larger frame, and an exception on the `jthread` would end the process. Catch it at the thread boundary when `EventCapture` exists. Do not put recovery in the per-frame loop. At 640×480 this is not the failure we are hitting.
- Item 16: remove `setResolution`, `width`, `height`, and `targetFps`. Nothing calls them, and `setResolution` writes sizes the worker also reads. Do not keep them just to fill the resolution label. `start()` returning `true` with no failure path can stay until there is a failure to report.
- Item 17: require Qt 6 and delete the Qt 5 fallback. The `add_library(name ...)` line is a real bug, and it is also dead. The spin box stops at 120 fps while `setTargetFps` allows 240. Make the clamp match the spin box.
- Items 20 and 21 are documentation. Stage 4 is in the program and `implemented features.md` does not say so yet. The rules file was created as `Agents.md`. Tools that look up `AGENTS.md` will miss it on a case-sensitive checkout, so the canonical name should be `AGENTS.md`.

**One smaller edit**

- Item 14: `copyStored` can push a copy of the slot instead of listing every field. `push` must keep the explicit metadata write and the buffer swap. Assigning the whole `Frame` there would copy the pixels and throw away the recycle. A metadata helper is optional.

### Close-out

The behaviour above is in the program, including the corrections under "Remarks on the review" and the items marked "Fix now" in the remarks on the code review.

The structural pass those remarks scheduled with the tests is also in:

- `EventCapture` owns the ring, the detector, and the clip in progress. The source thread calls `onFrame`. The window calls `reset`, `finishPartial`, and `onWriteFinished` only while that thread is joined or has not started. The window keeps the preview, the counters, the buttons, and the status text.
- `LunarDisk::centered` is the disk used by the generator and the detector. The marker and flash constants sit beside it in `synthetic_scene.h`. A precomputed mask is still later work.
- `copyStored` copies each slot as a `Frame`. `frameToQImage` is a free function, and `Frame` does not include Qt Gui.
- The source thread and the writer thread catch exceptions at the top of their thread functions and report them to the window. An exception ends that thread.
- MSVC builds use `/W4 /permissive- /utf-8`.
- `setResolution`, `width`, `height`, and `targetFps` are gone. The build requires Qt 6 and links it privately. The frame-rate clamp stops at 120, matching the spin box.
- Qt Test covers ring order, the detector rules, one clip per flash, the write lockout, a partial clip on stop, and the `event.txt` record.
- Event folders are written next to the executable. The rules file is `AGENTS.md`.

The single-frame rule for real impact flashes remains a decision for the camera stage.

---

## Feature-7 — Evidence safety: shared frames, no blind detector, and a lossless file path

Two faults in the Feature-4 code break evidence rules 3 and 5 of [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md#4-evidence-rules). Neither shows on the synthetic source today. Both would lose or damage real data. This step fixes them before Feature-6 brings recordings and 16-bit frames, and Feature-10 a camera.

It also changes who owns a frame, and it does that first. Fixing the blind detector means clips that overlap and clips that wait for the writer. If each of them copies the ring, the fix is paid for in RAM and in a stall at every trigger. With shared frames it costs nothing, and the rules for the writer become simpler.

### Step 1 — published frames are shared, not copied

Today [`FrameRingBuffer::copyStored`](frame_ring_buffer.cpp#L35-L52) duplicates every stored pixel when a candidate fires and holds the ring lock while it does. The preview makes its own copy on the source thread in [`MainWindow::publishPreview`](mainwindow.cpp#L230-L242). At 640×480 both are cheap. At 1920×1080 Mono16 a 150-frame ring is about 620 MB, and one trigger doubles it and stalls acquisition for the length of the copy.

- **A frame has one writer, then it is published.** The source takes a writable frame from a `FramePool`, fills it, and hands it on as the sole owner. Publication turns it into a `FramePtr` (`std::shared_ptr<const Frame>`). After publication nobody writes it. Feature-9 places the injected test signal just before publication; nothing else changes pixels (Evidence rules 1 and 6).
- **The ring, a clip, and the preview hold pointers.** `FrameRingBuffer` stores `FramePtr`. A push drops the oldest pointer. `copyStored` becomes `stored()`: it copies the pointers, oldest first, under the lock, and copies no pixel. `EventClip::frames` is a list of `FramePtr`, so two clips that overlap share their frames.
- **The pool recycles pixel buffers.** When the last pointer to a frame is dropped, on whichever thread, its buffer goes back to the pool. A warm stream still allocates no pixel buffer per frame. The pool keeps only a few spare buffers; when a written clip lets go of hundreds of frames at once, the rest are freed. The small reference-count block that `std::shared_ptr` allocates per frame is accepted. This replaces the `std::swap` recycling in `push`. The pool belongs to the source. A frame that outlives its pool is simply freed.
- **The preview converts on the GUI thread.** The source thread puts the latest `FramePtr` in a one-slot mailbox; a newer frame replaces an unread one. The GUI thread takes it and converts it. This is what the threads table in the project description already says, and it keeps a later display stretch off the source thread.
- **Memory is bounded by what holds frames.** Frames alive = the ring, plus the open clip, plus the clips waiting for the writer. The ring is fixed. The open clip is bounded by the clip length cap (Step 2). The waiting clips are bounded by the writer's queue limit (Step 3). The buffer readout shows the bytes of all live frames, not only the ring.

**Why this and not protected ring slots.** The project description left the choice open, to be decided by measurement together with the SER writer. Shared frames are chosen now because Step 2 needs overlapping clips, and three readers (ring, clips, preview) already want the same frame; Feature-8 adds a fourth. The cost is a pool and a reference count in place of a swap. What stays open for measurement is the writer's speed at camera sizes, which decides how many frames wait in RAM (Feature-5).

### Problem 1 — the detector is blind while a clip is busy

[`EventCapture::onFrame`](event_capture.cpp#L21-L47) calls `observe` only when no clip is being collected and no write is in flight. Otherwise it calls `advanceReference`, which moves the reference and never tests the frame. The blind period is the post-event frames (30) plus the time the writer needs. At 640×480 that is about a second plus a few hundred milliseconds of PNG encoding. With a camera and a ring of several seconds it is several seconds per candidate. A second flash in that period is neither saved nor logged, and nothing records that the detector was not looking.

The lockout also runs through the window. `writesInFlight_` is raised by the source thread and lowered by [`MainWindow::onEventWritten`](mainwindow.cpp#L184-L213) after a queued call, so the detector's state depends on the GUI event loop.

### Step 2 — detect on every frame

- The detector runs on every frame. The busy path no longer calls `advanceReference` in place of `observe`.
- **A new candidate during collection joins the open clip.** A new candidate is a new onset after at least one quiet frame; frames that keep firing in the same run are the same candidate. The clip records each trigger (onset index, trigger index, time, measured values). The post-event count runs from the last frame on which the detector fired, not from the trigger frame: a clip ends after `kPostEventFrames` quiet frames in a row, so a long event is kept whole and the tail follows its end.
- **A clip has a length cap.** After its first trigger a clip collects at most `kMaxPostEventFrames` frames (starting value 300, about 10 s at 30 fps). At the cap the clip is handed off. If the detector is still firing, a new clip starts at once from the ring. Its history overlaps the previous clip; the two share those frames.
- **A candidate during a write starts a new clip.** The write lockout goes, and `writesInFlight_` and `onWriteFinished` go with it. `EventCapture` no longer knows how busy the writer is. It hands every finished clip to the writer, and the writer decides (Step 3).
- **Every candidate is logged.** `events/candidates.txt` gets one line per trigger, saved or not: UTC time, onset and trigger index, measured values, and either the clip folder or the reason it was not saved. The writer thread appends the lines after it has written or refused the clip, so the folder or the reason is known. The source thread still never touches the disk.
- `event.txt` lists every trigger in its clip.
- **Inject flash** stays enabled while the stream runs, so two quick flashes can be tested by hand.

### Step 3 — the writer owns its backlog and survives a bad clip

Today one exception anywhere in [`EventWriter::run`](event_writer.cpp#L65-L110) ends the writer thread. The next clip is queued for a thread that no longer exists, and with the Feature-4 lockout the detector then stays blind for the rest of the session behind a single "Save failed" message.

- **A bounded queue, counted in frames.** `submit` never blocks. A clip is accepted when the frames already waiting plus its own stay within `kMaxQueuedFrames` (starting value 900: two clips of the maximum length, 150 frames of history plus 300 after the trigger). A clip that does not fit is dropped at once, which releases its frames, and its triggers are logged as "not saved: writer busy". The limit becomes a number of bytes when Feature-5 adds the RAM limit.
- **The window is told, not asked.** The writer reports each result to the window for the status bar. Nothing in the frame path waits for that report or depends on it.
- **A failed clip is one failed clip.** An error or an exception while a clip is being written fails that clip. Its triggers are logged as "not saved" with the reason, its folder is left without the completion marker, and the writer goes on to the next clip.
- **A failed record is a session state.** If `candidates.txt` itself cannot be written, or the writer thread ends for any other reason, the window shows "Recording failed" and keeps showing it until the stream is restarted. Detection continues and the candidate count on screen still rises (Evidence rule 5). A clip submitted to a writer that has ended is refused and counted, never queued.
- **The output folder is given to the writer.** `EventWriter` takes the output root in its constructor. The window passes `events/` next to the executable, as decided in Feature-4. The tests pass a temporary folder and stop writing into the build tree.
- **A folder is never reused.** If `events/<UTC stamp>/` already exists, the writer adds `-2`, `-3`, and so on (Evidence rule 7). Today this cannot happen. With Feature-6 it happens whenever the same recording is played twice, because a recording's frame times come from the file.

### Problem 2 — files are written through the preview conversion

`EventWriter` saves each frame with `frameToQImage`, the same function the window uses for the preview. For Mono8 and RGB888 the two needs happen to agree. Feature-6 adds Mono16 and says its preview is "a fixed shift so the image is visible". If that shift goes into the shared function, every saved clip silently becomes 8-bit.

### Step 4 — two conversions, two contracts

- `frameToQImage` is renamed `frameToPreviewImage` and belongs to the window. It may shift, stretch, or invert, and it returns an 8-bit image.
- The writer gets its own conversion, next to the writer code, which returns the stored samples unchanged: Mono8 as `Format_Grayscale8`, RGB888 as `Format_RGB888`, and Mono16 as `Format_Grayscale16` once Feature-6 adds it. A format it cannot write losslessly is an error reported like any other write failure, never a fallback to 8 bits.
- The two functions are nearly identical today. The duplication is intended: they have different contracts, and they diverge with Feature-6 (preview shift, SER) and Feature-5 (stretch).
- When clips move to SER (Feature-6), the SER frame write takes over the writer's conversion. The few preview PNGs in the event folder may then use the preview conversion, because they are not evidence.

### Done when

1. A test fills the ring past its capacity and takes `stored()`: the result points at the same frame objects the ring holds, and the number of pixel buffers the pool has allocated stops growing once the ring is warm.
2. A test keeps a clip alive while the ring wraps twice: the clip's pixels are unchanged, and its buffers return to the pool only when the clip is dropped.
3. A test feeds two flashes ten frames apart: one clip with two triggers, and the quiet tail counted from the end of the second flash.
4. A test feeds a flash while the previous clip is still waiting for the writer: a second clip is produced, and the frames the two clips have in common are the same objects.
5. A test submits a clip larger than the writer's queue limit: a "not saved: writer busy" log line and no folder.
6. A test with a trigger that never stops: the clip is handed off at the cap and a new clip starts.
7. A test submits a clip the file conversion cannot write, then a good one: the first is logged as not saved with its reason, the second is written.
8. A test writes two clips with the same trigger time: two folders.
9. A Mono8 clip written to PNG and read back has the same pixel values, bit for bit. When Feature-6 adds Mono16, the same test runs on a Mono16 frame with values above 255 and with the low bits set.
10. By hand: two quick presses of **Inject flash** while streaming are both saved, and `candidates.txt` lists both.

### Out of this step

- The writer's speed at camera sizes, and a queue limit in bytes. PNG encoding of large 16-bit frames is slow; that is settled with the SER writer (Feature-6) and measured in Feature-5.
- Merging rules tuned for long transits.
- An event browser.

### Order

Before Feature-6. Step 1 comes first inside this feature, because Steps 2 and 3 are written against shared frames. The conversion split must exist before the first Mono16 frame is written. The detection change must exist before a recording or a camera is monitored.
### Close-out

Feature-7 is in the program as specified above. Steps 1 to 4 are all built, and the ten "Done when" items pass.

**Decisions made while building it**

- **The quiet tail runs from the last firing frame.** The first draft counted post-event frames from the trigger. A long event would then have been cut off 30 frames after its trigger while it was still going on. The text above was corrected before this move.
- **The detector fires once per run.** `FlashDetector::observe` is true on the one frame where a run becomes a candidate. `firing()` says whether the latest frame still belongs to that run. `advanceReference` had no caller left and was removed.
- **A clip that carries on after the cap is not a new candidate.** Its trigger is marked `continues_previous_clip` in `event.txt` and in `candidates.txt`, and the window's candidate count does not rise for it.
- **The pool keeps at most four spare frames** (`FramePool::kMaxSpareFrames`). The by-hand run showed why: after a 187-frame clip was written the readout stayed at 81 MB, because every returned buffer was kept for reuse. With the limit it returns to 47 MB.
- **`FramePool::acquire` takes the frame's size in bytes.** The pool can then report the bytes it holds exactly. `SyntheticFrameSource` owns the pool and reports `liveFrameBytes()` for the buffer readout.
- **The preview mailbox is a small class, `LatestFrameSlot`.** Feature-8 will use a second one for the guider.
- **The writer reports a `ClipWriteResult`:** the folder, or the reason the clip was not saved, and whether the session's record is broken. The window shows `Candidates: N (M not saved)` and a `Recording failed` status that stays until the stream is restarted. The status label is worked out from the current state each time, so a late notification cannot leave a stale text.
- **File formats.** `event.txt` has one `trigger:` line per candidate. `candidates.txt` has one line per candidate: the UTC time, the frame indices, the measured values, the settings, and last either `clip=<folder>` or `not_saved=<reason>`.
- **Removed, because nothing calls them any more:** `EventCapture::writesInFlight`, `onWriteFinished`, and `memoryUsageBytes`; `FrameRingBuffer::memoryUsageBytes` and `copyStored`.
- **Renamed:** `frame_image` to `preview_image`, and `tests/stage4_tests.cpp` to `tests/core_tests.cpp`. The CTest name is now `core`.

**Tests**

`SeleneWatchTests` has 27 tests in six classes: the pool, the ring, the mailbox, the detector, the capture, and the writer. Beyond the "Done when" list they cover a frame that outlives its pool, a clip submitted after the writer has stopped, an output folder that cannot be created (a record failure), and a run from the capture through the writer to the files on disk.

**By hand (item 10), 13 November 2026**

The application was started and its buttons pressed through Windows UI Automation, on the development machine.

- Two presses of **Inject flash** 150 ms apart: both detected. They fell six frames apart, so they joined one clip with two triggers: 149 frames before, 37 after, 187 PNG files, the completion marker, and two lines in `candidates.txt`. **Inject flash** stayed enabled between the presses.
- Two presses 1.5 s apart: two clips. The second started 46 frames after the first trigger, while the first clip was with the writer. Both are complete, and `candidates.txt` has both.
- The stream held 30 fps throughout. The readout was 46.4 MB before the flashes and 47.3 MB after the clips were written.

**Carried forward**

- The Mono16 half of item 9 belongs to Feature-6, where clips become SER. Its round-trip test is item 4 of that feature.
- Qt Test's text output does not reach the console or CTest's log on this machine; only the pass or fail result does. Running `SeleneWatchTests -o results.txt,txt` writes it to a file. Worth fixing when the tests are next touched.


---

## Feature-6 — Recordings in, SER out

SeleneWatch has only ever seen its own painted Moon. The observer has years of real lunar recordings, as SER and as AVI. This step lets those recordings run through the same path as the synthetic source, and it replaces the PNG clip with a SER clip. After it, every later step, starting with the trigger of Feature-9, is developed and tested on real data.

The SER reader and the SER writer are built together. They are one piece of knowledge about one format, and each is the other's test.

### Selectable source

Several sources now do the same job, so one narrow source type is justified. It starts, stops, and delivers frames on a worker thread, with the same error report the synthetic source already has. Each delivered frame is writable and has one owner, as set out in Feature-7. There is no plugin registry.

- **Synthetic.** The source that exists. Its fixed flash button remains.
- **Video file.** The user picks a `.ser` or `.avi` recording. Both are read by SeleneWatch's own readers. A file that cannot be read is reported on the status bar with the reason, and the stream does not start.
- **Cameras** are adapters of the same source type and arrive in Feature-10. Nothing in this step may assume that a source is a file.

The capture path does not care which source produced the frame. The ring, the detector, and the writer stay where they are. The Feature-4 trigger still runs in this step; on real recordings it will fire wrongly or not at all, and that is expected until Feature-9.

**The frame path moves out of the window.** Today `MainWindow::onFrameArrived` is the frame path: it counts the frame, offers it to the preview, and calls the capture. With a selectable source, the blip, and trigger settings, that path would grow inside the UI class. A plain `Session` class takes it over. It owns the selected source, `EventCapture`, and the writer. On the source thread it publishes the frame, runs the capture, and offers the frame to the preview mailbox. Part C adds the blip there, just before publication. The window owns the `Session`, sends it settings, and reads its counters. The need is real: a recording is this project's calibration tool, and a `Session` without a window lets a test, and later a command-line scan, run a recording through the whole path.

**The source describes its stream.** When it starts, a source reports a `StreamInfo` that holds for the run: what the source is (camera model and serial, or file path), exposure, gain, offset, binning, ROI, and frame-rate setting where it knows them, what its frame time means (Section 5 of the project description), and whether its frame index is a hardware counter. A value the source cannot give is marked unknown, not guessed. The writer copies `StreamInfo` into each clip (Evidence rule 8).

`Frame` gains `PixelFormat::Mono16`, stored as little-endian `uint16_t` in the existing byte buffer. There is no separate 12-bit format. A frame also records its number of significant bits, whether they sit at the top or the bottom of the word, and its Bayer pattern if it is a raw colour mosaic. A 12-bit frame is stored exactly as its source delivers it, as Mono16 with 12 significant bits and its alignment recorded (Evidence rule 2). The preview reads those fields to pick a fixed shift so the image is visible. A Bayer mosaic is previewed as the grey mosaic it is; debayering for display comes with the stretch in Feature-5.

### Playback

A recording is played in one of two ways, chosen before Start.

- **The whole file, once.** At the end the source reports that it has ended, the way it already reports a failure. On the source thread the `Session` hands off any clip still being collected, so the last candidate is kept. The window then stops the stream and shows "Playback finished". The window still touches the capture only while the source thread is joined.
- **A loop over the first seconds of the file.** The observer sets a length in seconds. The source plays the frames in that span and starts again. A loop gives a steady stream while trigger settings are changed or a blip is placed by hand, which is how the trigger is tuned (Feature-9).

**The jump at the end of a loop is not a change in the sky.** Over a few seconds the Moon drifts and the seeing changes, so the jump from the last frame back to the first looks to the detector like a large sudden change, and it would fire on every lap. The source therefore says that the next frame does not follow the previous one, and the `Session` restarts the capture there as if Start had been pressed: the open clip is handed off, and the ring and the detector start empty. A clip never spans the jump.

**Frame index and time keep increasing across laps.** Lap n adds n times the loop's frame count to the index and n times the loop's length to the time. Every clip and every line of `candidates.txt` then stays distinct, and no folder name repeats. `StreamInfo` says that the playback is a loop and over which frames, so the position in the file can be worked out from the index. A loop is a test mode. Its times after the first lap are not the file's times, and the clip says so.

**Speed.** Real time, or as fast as the detector can run. The fast speed is how a recording is searched. The preview may drop frames, as it already does. The detector may not. Real time follows the intervals between the file's own frame times, so it replays the original jitter and gaps. No single interval is waited for longer than one second, so a long gap in a file does not stall playback. The speed can be changed while the stream runs.

### SER reader

- Mono and raw Bayer, 8 and 16 bits per sample, and 8-bit RGB and BGR. A file of another kind is reported as unsupported and does not start.
- The frame time is the time in the file's trailer, not the time of playback. A file without a trailer gets times counted from the header's start time and the playback rate, and `StreamInfo` says the times are nominal. SER has no frame-rate field, so that rate is the one set in the window's frame-rate box, and `StreamInfo` records it. When the header's `DateTime_UTC` is zero, the start time is unknown and the times count from zero.
- The frame index is the position in the file.
- **Significant bits.** For 16-bit data the reader combines (bitwise OR) the samples of the first frames. Bits at the bottom of the word that are zero in every sample are not significant: with `PixelDepthPerPlane` 16 and four such bits, the frames record 12 significant bits at the top of the word. With `PixelDepthPerPlane` between 9 and 15 and no sample wider than that, the bits sit at the bottom.
- A file shorter than its header says plays the whole frames it holds, without a trailer. `StreamInfo` says how many of the header's frames are there.
- 16-bit data in a file whose `LittleEndian` flag is 1 is read as big-endian, as SER Player, PIPP, and Siril read it, and is turned into little-endian samples.
- The header fields that affect correctness are listed in the [project description](PROJECT_DESCRIPTION.md#ser-details-that-affect-correctness): the 178-byte header, the trailer epoch, and the `LittleEndian` flag that is used inverted in practice.
- **What the observer's SER files show** (27 files, all written by FireCapture 2.x, which signs itself `LUCAM-RECORDER`, from a mono Point Grey Chameleon):
  - `LittleEndian` is 0 in every file, and the 16-bit samples are little-endian. This confirms the inverted use of the flag on real files.
  - The 16-bit file holds 12-bit data in the top bits: the low four bits are zero in every frame checked, and `PixelDepthPerPlane` is 16. So the reader cannot learn the significant bits from the header alone. It reads `PixelDepthPerPlane`, and when that is 16 it looks at the low bits of the first frames and records what it finds.
  - *Corrected when Part A was built (13 November 2026).* Reading every sample of 21 frames spread through the file shows that the low four bits are not zero everywhere: 11 samples out of 26 million are a half-step (low bits `1000`), one in each of 11 of those frames. That is most likely the camera's defect-pixel correction averaging two neighbours. The reader records 13 significant bits for this file, because the data uses 13. The samples are stored unchanged either way.
  - The same file (`2013.06.25 ISS\raw\Moon_202643.ser`) holds no lunar image. Its samples have a median of 1 and a maximum of 65 in 12-bit units, and a stretched frame is noise. It was probably a transit attempt on dark sky. It tests the 16-bit path, but it is no 16-bit lunar test case. The archive has none; one is needed before Feature-5 measures the stretch on the earthshine.
  - Every file has a full trailer, and the trailer times are UT. The header's "local" `DateTime` also holds UT in these files, so the reader takes its start time from `DateTime_UTC` and never from the local field.
  - The trailer intervals are not regular. One 18 fps file has intervals from 10 to 112 ms. These are the host's arrival times, with its jitter, and `StreamInfo` says so. Another file has intervals up to 653 ms, which is what dropped frames look like in a file with no counter.
- **Still to check.** The archive has no file from SharpCap and none from the ZWO or QHY cameras. How those store 12-bit data is checked on the first such file, before the writer's convention is fixed, and the finding goes into the project description.

### SER clip writer

Instead of writing hundreds of separate PNG files—which costs CPU time for DEFLATE compression, scatters one event over many files, and carries no standard astronomical metadata—candidate clips will be saved in the astronomical standard **SER v3 format** (`.ser`). PNG itself is lossless and supports 16-bit grayscale, so it remains acceptable for preview images.

#### Advantages:
- **Zero compression overhead:** Streams sequential raw pixel blocks directly to disk. Writing hundreds of frames takes fractions of a second on NVMe/SSD storage without stalling acquisition.
- **Native 8, 12, and 16-bit support:** Standardized by `PixelDepth` (8, 12, or 16) and `ColorID` in the SER header.
- **Per-frame 64-bit UTC timestamps:** SER v3 appends a trailer with one 64-bit UTC timestamp per frame, in 100 ns ticks since 0001-01-01 (the .NET epoch, not Windows `FILETIME`, which counts from 1601). See also the SER notes in [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) on the inverted `LittleEndian` flag and 12-bit alignment.
- **Ecosystem compatibility:** Natively supported by SER Player, PIPP, AutoStakkert!, Siril, RegiStax, and Python astropy pipelines.

#### Event folder and preview images

To allow immediate visual inspection of detections without opening an external SER player, each event directory will contain:

```text
events/
└── 20261111-170522-123Z/
    ├── event.ser               # Lossless raw SER v3 clip with trailer timestamps
    ├── event_summary.json      # Structured metadata (trigger time, onset frame, SNR, ROI, parameters)
    ├── 01_baseline.png         # Reference frame immediately prior to event onset
    ├── 02_event_peak.png       # Peak detection frame (maximum flash intensity or transit silhouette)
    └── 03_diff_annotated.png   # Differential image with ROI bounding box / trajectory annotation
```

`event.ser` is the evidence. Its frames go through the writer's own lossless conversion from Feature-7, never through the preview conversion. `event.txt` stays the metadata file in this step; moving to `event_summary.json` is the open decision on the clip metadata format in the project description, and it is taken once, for all clips. The baseline and peak images are previews, written through the preview conversion, and are not evidence. The annotated difference image needs the patch the new trigger measures, so it arrives with Feature-9.

The completion marker of Feature-7 is written after `event.ser` is closed. A SER file whose header frame count does not match its size is also recognisably partial.

### AVI reader

The observer has many lunar recordings as AVI, so AVI is a real input and not a speculative one. The headers of all 352 AVI files in the archive were read in November 2026. They decide what the reader must handle:

| Kind | Files | Becomes |
| --- | --- | --- |
| 8-bit DIB with an identity grey palette (FireCapture, mono camera) | 101, most of the lunar work since 2015 | Mono8 |
| `Y800`, 8-bit grey | 17 | Mono8 |
| 24-bit DIB (FlyCapture, FireCapture, K3CCD, the ALccd5 capture program, VirtualDub exports) | 208 | RGB888 |
| 32-bit DIB | 4 | RGB888, the fourth byte dropped |
| `YUY2`, 16 bits per pixel (webcams, 2010–2012) | 20 | Mono8 from the luma samples, marked as derived |
| Microsoft Video 1 (`CRAM`), compressed | 1 | refused |
| Not a RIFF file | 1 | refused |

- **One small RIFF reader covers all but two files.** No decoder library is needed. Windows Media Foundation is not used.
- **OpenDML.** Larger recordings continue in further `RIFF AVIX` chunks; 22 files in the archive do, with up to three chunks. The reader follows them.
- **Frame chunks.** Uncompressed frames appear under both `00db` and `00dc` (45 files use the second). The reader accepts both and checks the chunk size against the frame layout.
- **Row order.** Every DIB in the archive is bottom-up. The reader turns the rows the right way up and swaps the stored BGR order to RGB. Neither loses anything, and `StreamInfo` says both were done. For `Y800` and `YUY2` the row order is confirmed by eye on the first file of each kind.
- **An 8-bit palette that is not an identity grey ramp** is refused, because the stored bytes would not be the pixel values.
- **Grey stored as colour.** Many 24-bit files come from mono cameras and have equal red, green, and blue in every pixel. They are still stored as RGB888, as delivered. The trigger's mean of the three channels is then the grey value. Reading them as Mono8 is an optimisation, added only if playback speed needs it.
- **`YUY2` is not kept as delivered.** Only its luma is read, so a clip cut from such a file says that its pixels are derived. These are old webcam recordings. They are useful for exercising the path, not as evidence.
- **AVI has no frame times.** The frame time is counted from a start time and the frame rate, and `StreamInfo` says the times are nominal.
- **FireCapture's log, when it is there.** FireCapture writes a text file beside each recording (`Moon_180548.txt` beside `Moon_180548.avi`) with `key=value` lines. When the file exists, the reader takes the start time from `Date` and `Start(UT)`, and the camera, shutter, and gain for `StreamInfo`. Without it, the start time is unknown and the times count from zero.

### Injected blip

*Moved here from Feature-9 on 13 November 2026, with its text unchanged except where noted below. The observer wants to test with it as soon as recordings play, and Feature-9 then finds its calibration tool ready.*

A blip is a small spot added to the live frames for a chosen number of frames. It is how a recording becomes a measurement. "Inject flash" on the synthetic source is the same idea with fixed numbers. The blip works on every source.

The blip is a Gaussian added to the pixel values and clipped at the format's maximum. A real flash is a point of light spread by the optics and the seeing, and it adds to the surface under it. A hard-edged disk painted at one gray level has neither property, and a faint limit measured with it would be too optimistic.

The user sets the centre by clicking the preview, and sets the width (FWHM in pixels), the peak amplitude in gray levels of the frame's own format, and the duration in frames. The status line shows that amplitude again as a multiple of the noise at the centre, so a value of 200 ADU on the bright limb and on the earthshine is not mistaken for the same event. *(The noise at the centre is measured by the trigger of Feature-9. Until then the status line shows the amplitude in gray levels only.)*

The `Session` adds the blip before the frame is published, so the frames with the blip are what the detector sees and what the clip stores. Each such frame is marked (Evidence rule 6). `event.txt` records that the blip was injected and records its centre, width, amplitude, and duration. A clip from a blip is a test. A clip with no blip line is a candidate from the data.

Added when the blip moved:

- **The blip respects the frame's bit layout.** On a Mono16 frame whose significant bits sit at the top of the word, the amplitude is in units of those bits and is shifted into place. The unused low bits stay zero, and the frame is still a valid 12-bit frame.
- **Clicking the preview needs a mapping from widget to frame coordinates.** It is added here, and the feature lock of Feature-8 reuses it.
- **The Feature-4 detector will mostly ignore a realistic blip.** It needs about 120 pixels each rising by more than 60 gray levels, on two frames in a row. A Gaussian reaches that only at roughly 9 px FWHM at full brightness. That is the detector's limit, not the blip's, and Feature-9 removes it.

### Done when

1. The window can run the synthetic source or a chosen recording through the same capture path. A file that cannot be read is reported and does not start.
2. A test runs a SER file through a `Session` with no window and finds the expected clip folder.
3. The fast playback speed runs the detector over the file without waiting for real time.
4. A SER file written by SeleneWatch's writer is read back by its reader with the same pixels, frame count, bit depth, and timestamps, for Mono8 and for Mono16 with values above 255 and the low bits set. The header bytes are checked against the documented layout.
5. A 12-bit frame delivered in the top bits and one delivered in the low bits are both stored unchanged, each with its significant bits and alignment, and both give a visible preview.
6. A clip is written as `event.ser`, `event.txt`, and the two preview images. Playing the same recording twice gives two folders.
7. An uncompressed AVI written by the test, split into two RIFF chunks the way OpenDML does, is read back with the same pixels, for 8-bit grey and for 24-bit. A compressed AVI is refused with its FourCC. A FireCapture log written by the test supplies the start time.
8. By hand, from the observer's archive (the path is in [local environment.txt](local%20environment.txt)): one file of each kind in the table above opens and plays the right way up, among them an 8-bit FireCapture lunar AVI with its log, a multi-chunk AVI above 2 GB, a 24-bit FlyCapture AVI, the 16-bit SER, and an 8-bit SER. For each, the size, bit depth, frame count, and times agree with the capture program's log or with SER Player.
9. By hand: an `event.ser` written from one of those recordings opens in SER Player, PIPP, or Siril with the right frame count, bit depth, and times.
10. Played whole, a recording ends with "Playback finished", and a clip that was open at the end is saved.
11. A test plays a loop for several laps: the capture restarts at each lap, the frame index and time keep increasing, and the jump back to the first frame fires no candidate.
12. A Mono16 clip written to PNG and read back has the same values, above 255 and with the low bits set. This is the half of Feature-7's item 9 that waited for Mono16.
13. While a recording plays, clicking the preview sets the blip centre. Width, amplitude, and duration are editable. Injecting the blip adds it for that many frames, those frames are marked, and `event.txt` records the blip. *(Item 1 of Feature-9, moved with the blip.)*
14. A test adds a blip to a 12-bit frame stored in the top bits: the low bits stay zero, the values clip at the format's maximum, and the frames before and after the blip are unchanged.

**Which part delivers which item.** Part A: 1 and 8 for SER files, 2, 3, 5, 10, 11, and 12. Part B: 1 and 8 for AVI files, and 7. Part C: 13 and 14. Part D: 4, 6, and 9.

### Out of this step

- The new trigger. That is Feature-9.
- Display stretch, debayering for display, and a duration-based ring. Those are Feature-5.
- Cameras. Those are Feature-10.
- Continuous recording to SER. The writer built here is the one it will use, but the recording mode, its controls, and its disk-rate display come with the session record stage.
- Decoding compressed video. One file in the archive needs it.
- An event browser.

### Order

The feature is built in four parts. Each one ends with the tests passing and the app usable.

- **Part A — playback.** The source type, `Session`, `StreamInfo`, the Mono16 frame with its significant bits, the SER reader, and playback: the whole file or a loop, at real time or fast. Clips are still PNG.
- **Part B — AVI.** The AVI reader: 8-bit grey first, since that is most of the lunar archive, then 24- and 32-bit, then `Y800` and `YUY2`, and the FireCapture log.
- **Part C — the blip.** The injected blip, its controls, and the preview coordinate mapping.
- **Part D — SER clips.** The SER writer, the round-trip tests, and the new clip folder.

The synthetic source stays, and its fixed flash remains the check that the whole path still works.

### Decisions taken before building (13 November 2026)

- **The order changed.** The first plan had sources, then SER clips, then AVI, with the blip in Feature-9. The observer's best test recordings may be AVI, and the blip makes a played recording a test bench, so both come first. SER clips have to exist only before a camera is used (Feature-10), so they move to the end of this feature.
- **The playback is written once.** Whole file or loop, the speed, and the end of the file sit in one recording source, with the file reader beneath it. In Part A the source uses the SER reader directly. The reader becomes an interface in Part B, when the AVI reader is its second implementation.
- **The SER reader is tested against a file the test builds byte by byte** from the documented layout. That file does not come from SeleneWatch's writer, so a mistake made the same way in the reader and the writer (the inverted `LittleEndian` flag, the .NET epoch) is still caught. Part D adds the writer, its header-byte check, and the round trip. The observer's own FireCapture files remain the authority, checked by hand.
- **The Feature-4 detector reads one value per pixel.** Mono8 as stored, Mono16 as its top 8 significant bits, so its 60-gray-level threshold keeps its meaning, and RGB888 as the mean of the three channels. It looks at the whole frame. The centred disk it used was a property of the painted image, not of a recording, and it made detection depend on the synthetic source. Its thresholds stay as they are until Feature-9.
- **Clips name their source from Part A.** `StreamInfo` goes into `event.txt`. `candidates.txt` gets a `session_start` line at each Start that names the source and the playback, so a candidate that was not saved still says which recording it came from (Evidence rule 5).
- **The writer's file conversion gains Mono16** as a 16-bit greyscale PNG while clips are PNG. Part D replaces it with the SER frame write.
- **The directory split waits for Part D.** The project description expects `sources/` and `recording/` with this feature. Where the SER format knowledge sits is easier to decide when both the reader and the writer exist.
- **Qt Test output** that does not reach the console (carried forward from Feature-7) is fixed in Part A, since Part A touches the tests.

### Part A — built (13 November 2026)

Part A is in the program. Done-when items 2, 3, 5, 10, 11, and 12 pass as tests. Items 1 and 8 pass for SER files by hand. The observer compared the orientation in SeleneWatch with another player on 13 November 2026 and confirmed it.

**What it added**

- `FrameSource`, the one source type, with `FrameSourceListener`. `SyntheticFrameSource` and `RecordingSource` implement it.
- `SerReader`, which reads only. `RecordingSource` plays a file through it: whole or looped, at real time or fast.
- `StreamInfo`, the source's description of its stream, written at the top of `event.txt`.
- `Session`, which holds the frame path that used to be `MainWindow::onFrameArrived`. It owns the source, the capture, the writer, and the preview slot.
- `Frame` gains `Mono16`, `significantBits`, `alignment`, and `bayer`. The preview, the Feature-4 detector, and the writer's PNG conversion read them.
- The window has a source selector (synthetic or recording file), **Open…**, a speed box, and **Loop first … s**. **Inject flash** works on the synthetic source only, until the blip (Part C).

**Decisions made while building it**

- **A source is opened, then started.** `open` reads the file and works out the `StreamInfo` on the window thread, before any worker exists. `start` runs the worker. The `Session` copies the `StreamInfo` before the first frame, so the source thread only ever reads it.
- **A source reports to a listener.** `FrameSourceListener` has four calls: a frame, a discontinuity, the end, and a failure. The `Session` implements it, and the tests have a second, collecting listener. It replaces the synthetic source's frame and error callbacks.
- **The window makes a new source at each Start**, from the controls. The file and the loop are read then. The speed and the synthetic frame rate can change while it runs.
- **A late notification is ignored.** The window acts on "ended" or "failed" only when `Session::sourceFinished()` says the current run's source has finished, so a notification from an earlier run cannot stop a new one.
- **A recording's monotonic time is when the frame was played.** Its UTC time is the file's.
- **The 1 ms timer resolution** is a small class, `FineSleepResolution`, used by both sources.
- **The `session_start` line** has quoted fields, `camera`, `playback`, and `source`, with the path last. Its time is the host's clock at Start; the candidate lines after it carry the frames' own times. A `ClipWriteResult` with neither a folder nor a reason reports a `session_start` line that could not be written.
- **The Feature-4 detector throws** for a frame whose data does not match its size and format. That is a bug in a source, and the exception ends the source thread with a report.
- **CTest runs the tests with `-o -,tap`.** On Windows, Qt Test's plain text goes to the debugger when the process has no console of its own, which is how CTest may start it. TAP always reaches stdout. Run by hand from a console, the tests print plain text. This was the cause of the Feature-7 note.

**Tests**

`SeleneWatchTests` has 51 tests in ten classes. New in Part A: the SER reader (10 tests, against files built byte by byte from the documented layout), the recording source (4), the session (4), the detector on Mono16 and RGB frames (2), the preview of 12-bit data (1), and the writer (3: the Mono16 PNG round trip, the stream lines in `event.txt`, and the `session_start` line). The suite passed three runs in a row.

**By hand, 13 November 2026**

The application was driven through Windows UI Automation on the development machine.

- `2015.12.21 Moon 8 Newt\...\Moon_165104.ser`, 8-bit, FireCapture 2.4: 1296×964 Mono8, 714 frames, with a trailer. The first frame is at 15:51:04.505 UT and the file spans 38.899 s; the log says `Start(UT)=155104.516` and 38.945 s. The preview shows the Clavius region. Real time played the file in 39.3 to 39.9 s, fast in 4.1 s (about 175 fps with the detector, in a Debug build), and both ended with "Playback finished". The Feature-4 detector fired at frames 316 and 664 on every run. Both clips were written with the file, the camera, and the playback in `event.txt`. Played three times, the clips went to `…-746`, `…-746-2`, and `…-746-3`.
- The same file looped over its first 5 s at fast speed for 8 s: 92 frames per lap, about 17 laps, 1558 frames, no candidate.
- `2013.06.25 ISS\raw\Moon_202643.ser`, 16-bit: opens as 1296×964 Mono16 with 13 significant bits at the top, 1286 frames over 213.0 s, as its log says (1286 frames, 213 s). The preview is black because the file is (see the SER reader notes above).

**Seen while testing, for later**

- **Closing the window waits for the writer.** With two 180-frame PNG clips at 1296×964 queued, the Debug build took 55 s to close, and the window looked frozen. That is the Feature-7 rule: shutdown writes what is queued. SER clips (Part D) remove most of the cost. If it still matters then, the window can say what it is waiting for.
- **A killed process loses the log lines of the clip it was writing.** The clip is left partial, without its completion marker, so Evidence rule 7 holds. But its candidates are not in `candidates.txt`, because the writer logs a candidate after its clip is written or refused (Feature-7). Rule 5 asks that every candidate be on record. Writing each candidate's line when it is queued, and the clip's outcome after, would close this. That is a change to the log format, to decide with the observer.
- The status bar's message area is squeezed by the permanent labels, so a message like "Playback finished" is cut off at the left. Cosmetic.

### Part B — built (13 November 2026)

Part B is in the program. Done-when item 7 passes as tests. Items 1 and 8 pass for AVI files by hand. The observer checked the AVI files' orientation on 13 November 2026 and confirmed it.

**What it added**

- `RecordingReader`, the interface beneath the playback, now that it has two implementations: `SerReader` and `AviReader`. `RecordingSource` picks one by the file's extension and plays either the same way.
- `AviReader`, for every kind in the table above. It reads only.
- `FireCaptureLog`, the reader of FireCapture's text file beside a recording. Both readers use it.
- `StreamInfo` gains `exposure`, `gain`, `frameRate`, and `conversion`, and `event.txt` writes them.

**Decisions made while building it**

- **Frames are found through the file's index.** The archive is on a USB hard disk. Walking an AVI chunk by chunk costs one seek per frame, so opening a 2 GB file would take tens of seconds. The reader uses the OpenDML index of a file in several RIFF parts, else the AVI 1.0 `idx1`, and walks the `movi` lists only when neither is usable. An `idx1` covers only the first RIFF part, so a file of several parts without an OpenDML index is walked.
- **`idx1` offsets** count from the `movi` type field in most files and from the start of the file in some. The reader tries both against the first frame's chunk header.
- **An empty frame chunk is a frame dropped during capture.** The plan did not foresee these. The first survey with the new reader found them in 20 files, all webcam and K3CCD captures from 2010 and 2011, five of them lunar. AVI timing is by position, so skipping such a chunk would shift every later frame's time. Instead the empty chunk keeps its number: the frames after it keep their numbers and times, and the gap shows in the frame index, which is how Evidence rule 4 records a dropped frame. `RecordingReader::frameNumber` carries this; for SER it is the position. A loop counts the gaps too.
- **The UTC start from FireCapture's log.** `Date` and `Start` are local and `Start(UT)` is UT, so near midnight the local date is not the UT date. The reader takes the zone offset from the difference between `Start` and `Start(UT)` and subtracts it from the local date and time. A log without `Start(UT)` (FireCapture 2.2) gives no start, rather than a guessed zone. A log whose `Filename` names another file is ignored. Every log in the archive has `Date_format=ddMMyy`, and only that format is read.
- **FireCapture's log is read beside SER files too.** It supplies the exposure and the gain. A SER file's times still come from its trailer.
- **A frame's AVI time is its number at the header's rate**, from the log's start time or from zero.

**Tests**

`SeleneWatchTests` has 59 tests in eleven classes. New in Part B: the AVI reader (7 tests) and two recording-source tests. All of them use AVI files built byte by byte from the documented layout: one or two RIFF parts with an OpenDML index, `idx1` with relative or absolute offsets, or no index; padded and bottom-up DIB rows; BGR and BGRA; Y800 and YUY2; a colour palette and a compressed stream, both refused; FireCapture logs across midnight, without `Start(UT)`, and naming another file; and an empty chunk, which leaves gaps through all three ways of finding frames, whole and looped.

**By hand, 13 November 2026**

- **The whole archive.** The reader opened 350 of the 352 AVI files and read their first and last frames. The two it refuses are the ones the table above expects: the `CRAM` file and the file that is not RIFF. 142 files were found through their OpenDML index and 185 through `idx1`; opening took a median of 23 to 29 ms from the USB disk. Three files have no usable index. Two of them, `2011.01.16 moon\raw\dark_00017.AVI` and `dark_00024.AVI`, are dark frames whose capture stopped before writing an index, and opening them took 47 s and 12 s. Nothing is done about that now. 99 files took their start time from FireCapture's log.
- `2015.12.19 Moon C9.25\...\Moon_180548.avi`, 8-bit FireCapture with its log: 500 frames, 1296×964 Mono8, first frame at 17:05:48.333 UT. The log says 500 frames and `Start(UT)=170548.333`. Camera, shutter, and gain match the log. The preview shows Archimedes, Autolycus, and Aristillus. The app played it whole at fast speed in 5.1 s.
- `2015.10.26 Moon\raw\Moon_200059.avi`, above 2 GB in two RIFF parts: 1883 frames, as the log says. The last frame, from the second part, shows the limb cleanly.
- `2019.09.15 moon\raw\moon_2019-09-15-011146-0000.avi`, 24-bit FlyCapture: 500 frames, 1280×960, 15 fps, no log, so the times count from zero.
- `2012.05.02 moon\raw\ps3eye 5_2_2012 11_08_44 PM.avi`, 32-bit: 466 frames, colour kept.
- `2011.01.16 moon\raw\K3CCD_0005.avi`, YUY2: 754 frames of Copernicus and its rays, no striping from the colour samples, marked derived.
- `2010.08.18 jupiter\raw\video0001 10-08-18 00-22-35.avi`, Y800: 2679 frames numbered 0 to 2836, with the 158 dropped frames as gaps.
- **Still for the observer:** compare orientation once with another player (SER Player does not read AVI; VirtualDub or VLC will). The DIB rule (bottom-up unless the height is negative) is certain. The Y800 and YUY2 files follow Microsoft's rule that YUV frames are stored top-down. Their images are coherent, but a mirrored crater still looks like a crater.

**Seen while testing, for later**

- **The header rate is not the real rate.** FireCapture writes 18.000 fps into the header, but its logs record the real duration: `Moon_180548` holds 500 frames in 27.602 s, and `Moon_200059` holds 1883 frames in 103.672 s. Timed at the header rate, the last frame of the second file is 0.9 s late. The times are marked nominal, so nothing is claimed wrongly. But the log's `Start(UT)` and `End(UT)` would give the mean rate, and with it nominal times within a frame or two everywhere. That is a small change, to decide with the observer: it needs only to be settled what FireCapture's `End` marks.
- **Opening runs on the window thread.** For the two unindexed dark-frame files, the window waits 12 to 47 s at Start. Moving the open onto the worker thread, or showing that it is reading, would fix it if such files turn out to matter.

### Part C — built (13 November 2026)

Part C is in the program. Done-when items 13 and 14 are covered by automated checks. Feature-6 remains open until Part D supplies SER clips and the independent-player check.

**What it added**

- `BlipInjector`, owned by `Session`. It adds a Gaussian to the writable source frame immediately before publication. Preview, detection, ring, and writer then share that same marked, read-only frame.
- A **Test flash** group with width (FWHM in original frame pixels), peak amplitude (native ADU), duration (delivered frames), and **Inject blip**. Clicking the image selects the centre. The readout repeats the amplitude in ADU; the noise-unit readout still waits for Feature-9.
- `PreviewWidget`, which fits the image and maps clicks through the same rectangle it paints. It ignores margins, keeps the selected frame pixel through a resize, and clears the selection for a new run or image size. Its screen-only marker leaves the centre clear so it does not hide a small blip. Feature-8 can reuse the coordinate mapping.
- `BlipSettings` and `BlipMark`: every affected frame records the settings, the first injected frame's index, and its ordinal within the injection. `event.txt` says `contains_test_signal: yes`, lists each injection's full settings, and marks the individual frame lines. The settings are still available when the start of an injection is outside the clip.
- The original synthetic flash remains as **Fixed flash**. Its frames now carry a separate `synthetic_flash` mark. SER and AVI readers and the synthetic painter clear stale test marks when reusing a frame.
- `candidates.txt` adds `contains_test_signal=yes` for candidates whose clip contains injected frames or the fixed synthetic flash. A refused clip keeps that flag after its pixels have been released.

**Decisions made while building it**

- **One injection at a time.** The request slot and the busy flag are protected by a short lock. The source thread owns the active injection and does the pixel work outside the lock. A second request is refused until the current one ends. Editing controls changes the next request, not the settings already in flight.
- **The Gaussian is additive and rounded to native ADU.** For a high-aligned 12-bit frame, the rounded addition is shifted into the high bits, and saturation is at the aligned 12-bit maximum. Previously zero low bits stay zero. Low-aligned data stays low-aligned. RGB receives the same addition in each channel, with independent saturation; Bayer samples are changed in place without debayering or changing the pattern. No source file is modified.
- **There is no arbitrary spatial cutoff.** Only pixels for which the Gaussian can round to at least half an ADU are visited. A requested frame is marked even if saturation or rounding leaves its samples unchanged, so it cannot be mistaken for untreated data.
- **Duration counts delivered frames.** Index gaps do not use up frames of the injection. An injection may continue over a playback-loop boundary; the capture still resets there and no clip spans the jump. Stop cancels pending and unfinished injections, and a new run starts without one.
- **The existing detector stays unchanged.** A small realistic blip can be correctly injected without firing the Feature-4 trigger. The button's tooltip explains this. Broad, bright multi-frame blips exercise today's complete capture path; faint and single-frame sensitivity is Feature-9's work.
- **Clip logging order stays unchanged.** The known process-kill gap documented under Part A still belongs with the writer/logging follow-up. The added test flag does not change when a candidate line is written.

**Tests and verification**

- Debug and Release configure, build, and CTest runs passed: 75 test cases across the core and preview executables, excluding Qt Test's lifecycle entries. No dependency was added.
- Six injector tests cover the additive peak and half-maximum width, unchanged timing and indices, high- and low-aligned 12- and 16-bit samples, saturation, preserved Bayer pattern, RGB channel saturation and an image-edge centre, exact duration across index gaps, settings snapshots, stale marks, rejected requests, and cancellation.
- The end-to-end recording test has Mono8, high-aligned 12-bit Bayer, and RGB cases. A quiet SER recording plays through a real `Session`; a requested three-frame blip fires the existing detector. The saved PNG pixels, per-frame marks, full settings, and candidate-log test flag are checked, and the input file remains byte-for-byte unchanged. A separate stop/restart test verifies that an unfinished injection cannot leak into the next run.
- A writer test checks two injections in one clip, an injection whose first frame is outside the clip, the fixed synthetic mark, and retention of the test flag when the writer refuses a clip.
- Four widget tests cover edge pixels, margins, left/right clicks, resizing, selection lifetime, and a marker that changes neither the supplied image nor its centre pixel.
- The actual `MainWindow` was also exercised and rendered in a temporary offscreen Qt harness: Start, choose a pixel, inject one frame, resize, and Stop. The layout was inspected at 900×679 and 1280×900. Native desktop automation could not connect to its helper, so no live desktop smoke check is claimed.

### Part D — built (13 November 2026)

Feature-6 is complete. Done-when items 4 and 6 pass automated checks, and item 9 passed with the installed Siril 1.4.4 command-line reader. Full verification and measured times are in [docs/feature-6-validation.md](docs/feature-6-validation.md).

**What it added and decisions made**

- `SerWriter` streams unchanged pixels and UTC trailer times to a seekable output. It validates every frame's layout, leaves the count zero until finalization, and reports short writes and finalization failures. The `QIODevice` boundary is exercised by the full-disk tests.
- `event.ser` replaces per-frame PNG evidence. `event.txt` stays the sidecar; JSON migration is still a separate open decision. The folder also contains `01_baseline.png` and `02_event_peak.png`, both display previews. An unavailable pre-onset baseline is labelled explicitly. The current peak preview uses the trigger with the highest bright-pixel count; Feature-9 supplies a measured peak over the candidate.
- Mono16 sample words are never shifted. Low-aligned 9–15-bit data names its significant depth; high-aligned data names depth 16. Both use two-byte samples. Exact significance and alignment remain in the sidecar. Constructed examples of both 12-bit alignments were independently accepted by Siril. The absent SharpCap/ZWO/QHY samples remain a camera-adapter validation task, not a reason to guess their layouts.
- Metadata records the software revision, exact SER UTC ticks, source indices and gaps, and test marks. A test clip also carries `SeleneWatch TEST DATA` in the SER Observer field; replay conservatively marks that entire source as a test even if its sidecar is missing.
- The writer flushes a `candidate_pending` line before starting a clip write, then appends the outcome with the same UUID. Killed writes therefore retain their pending candidate. Discovery still waits for collection and queued work; RAM-only work can be lost in a process crash. This does not promise power-loss durability.
- Format readers/writers are in `recording/`, and acquisition/playback in `sources/`. A single `selenewatch_core` static library is shared by app, tests, and manual probe and has no Qt Widgets dependency. The remaining small core files stay at the root.
- `SeleneWatchProbe` provides explicit-path, manually invoked archive checks. It is excluded from CTest. Its `crash` mode is only for a dedicated disposable writer process.

**Verification**

- Debug and Release build/CTest passed: 87 cases excluding Qt lifecycle entries. Nine SER layout combinations, independent header-byte and epoch constants, exact pixel/time round trips, native low bits, all four Bayer patterns, failed writes, changed layouts, and replayed test marks are covered. Existing overlap, queue, stop/EOF, and failed-clip recovery checks remain.
- Siril opened five constructed layout clips and a six-frame sample of the observer's 16-bit SER, agreeing on size, type, depth, frame counts, UTC start, and ordered timestamps. All validation output was written outside the archive.
- A dedicated writer was killed with 67,109,042 bytes in its partial SER. Its pending record survived; the header count was zero and no completion marker existed.
- The 714-frame lunar SER produced the same candidates at frames 316 and 664. Release source processing/collection took 15,922 ms in this run, total including writer drain 15,931 ms. The residual writer wait was 9 ms. A six-frame 1296×964 Mono16 sample including previews took 78 ms to write and shut down. USB/cache effects limit comparison of total playback times.

### Follow-up — 16 November 2026: the AVI frame rate, and files without an index

Both items under Part B's "Seen while testing, for later" are settled.

**The frame rate from FireCapture's log.** The question left open was what FireCapture's `End` marks. FireCapture's SER files carry their own frame times and have the same log beside them, so they answer it. On the 24 lunar SER files with a log in `2015.12.21 Moon 8 Newt`, the last frame's own time is 38 to 57 ms before `End(UT)`, about one frame interval (54.6 ms), and the first is within 47 ms of `Start(UT)`. Spacing the frames evenly from `Start` at `Duration / Frames captured` put every frame within 80 ms of its recorded time; the mean difference per file was −20 to +16 ms. On two ISS captures whose rate varied, the same rule was off by up to 1.2 s and 5.8 s, so the times stay "nominal, not measured".

The headers are worse. FireCapture writes the rate at the start of a capture into the header, rounded to a whole number: 18 fps where the logs show 18.0 to 18.3, and also 18 fps for `2018.08.21 moon mars saturn\moon\raw\Moon_220119.avi`, whose log shows 175 frames in 47.625 s (`FPS (avg.)=3`). Timed at the header's rate, that file's last frame came 38 s early. `Moon_200059.avi`'s came 0.9 s late.

- `AviReader` uses `Frames captured / Duration` when the log gives `Duration` to the millisecond (FireCapture 2.4 and 2.5) and `Frames captured` equals the file's frame chunks, empty ones included. Otherwise it uses the header's rate, and says why when the log counts other frames. FireCapture 2.2 writes whole seconds, too coarse to time frames by.
- `frame_rate` names the source, for example `18.163 fps, the mean in FireCapture's log Moon_200059.txt (1883 frames captured in its Duration of 103.672 s); the AVI header states 18.000 fps`. `frame_time` adds that the frames are spread evenly over the log's Duration.
- An AVI frame time is rounded to the nearest clock tick instead of truncated, so frame 3 at 25 fps is at 120 ms, not one tick before.
- In the archive, 99 AVI files take their rate from the log. No log disagreed with its file's frame count.

**Files without an index.** `dark_00017.AVI` is 2.1 GB and holds 308 frames, 1.2 GB. The remaining 936 MB of its `movi` list are zeros: the capture program reserved the file and stopped early, without writing an index. The walk read every 8 bytes of zeros as an empty chunk, 117 million of them. That, not a seek per frame, was the 47 s.

- A chunk id is four printable ASCII characters. The walk stops at anything else, and `frame_index` records the bytes left unread.
- From the USB disk, `dark_00017.AVI` now opens in 2.2 s and `dark_00024.AVI` in 1.5 s, with the same 308 and 201 frames. Opening still runs on the window thread.
- All 352 archive AVI files were opened again with the changed reader: the same 350 open, each through the same index and with the same frame count as before, and every first and last frame reads. The median opening time is 26 ms.

**Tests.** Two reader tests are new: a walk that stops at reserved zero bytes and reports them, with an empty chunk before them; and a log that gives the mean rate, with the two cases where it is not used (another frame count, and whole seconds).

---

## Feature-11 — Sky watch

*Proposed on 16 November 2026, from a discussion with the observer.*

The same camera, ring, detector and clips can watch the open sky: a camera with a wide lens, left outside for a night, saving anything that happens. Fireballs and meteors, aircraft, satellites and lightning are what it is for.

The lunar design treats most of these as false positives (PROJECT_DESCRIPTION.md, Section 3), and its settings reject them on purpose. A meteor crossing the frame at an angle is too spread out for the box test. A bright one is larger than the maximum patch. A frame where much of the image brightens at once is never split into patches. Watching the sky also brings problems the Moon does not have: stars drift past a fixed camera, the ground has lights and trees, and an unattended night can fill a disk.

So SeleneWatch gets a second mode. **Moon** is the program as it is today. **Sky** changes what watching the sky needs and nothing else.

### What shapes the design

1. **Moon mode does not change.** Its settings, its detector and its clips stay exactly as they are, so lunar results and the measurements of Feature-9 still hold. Each Sky behaviour below is off in Moon mode.
2. **One pipeline.** Sky mode uses the same sources, ring, detector, writer and evidence rules. It adds settings and a few behaviours; it is not a second program.
3. **The mode is on record.** Every clip and every candidate line says which mode recorded it, with the Sky settings in force.
4. **A night outside runs unattended.** Nobody watches the window, so the program must not fill the disk or record one star all night.

### Part A — the mode and its settings

- A **Target** choice, Moon or Sky, on the Detection panel. It changes only while the stream is stopped, because both the detection and the capture settings change with it.
- Each mode keeps its own detection and capture settings. Switching never changes the other mode's values. The settings saved today become Moon's, so nothing changes for an observer who never picks Sky.
- The Guiding tab is off in Sky mode.
- `event.txt` and `candidates.txt` record `mode=moon` or `mode=sky`.

Starting values for Sky, to be checked on the first nights:

| Setting | Moon (today) | Sky |
| --- | --- | --- |
| Sensitivity K | 6 | 6 |
| Maximum patch | 400 px | 3000 px |
| Maximum box / patch | 4× | 30× |
| Allow a one-frame trigger | on | on |
| Pre-event history | 5 s | 3 s |
| Quiet tail | 3 s | 5 s |
| Maximum after trigger | 10 s | 20 s |

- **Box / patch 30×.** A 45° streak 30 px long and 3 px wide is about 90 pixels in a box of about 24 × 24, near 6×. At the same angle the ratio grows with the streak's length, so 30× passes a streak of about 180 px.
- **Quiet tail 5 s.** A fireball's flare and repeated lightning strokes stay in one clip.
- **Maximum after trigger 20 s.** At full frame (1280 × 960, 8 bits, 30 fps) a clip grows by about 37 MB a second. Its history and 20 s fit the clip's share of the default 2 GiB RAM ceiling. A longer event continues in a linked clip.

### Part B — a trigger for large brightenings

Lightning and a fireball's flare light a large part of the frame at once. Today such a frame cannot fire. When more pixels pass K than four times the maximum patch (and at least 4096), the frame is not split into patches. A smaller bright area is still larger than the maximum patch. On the Moon this is right, because seeing, haze and cloud do the same.

In Sky mode a frame also fires when the pixels at or above K are at least `kBrighteningShare` of the unmasked frame (starting value 2%). This is a new trigger kind, `brightening`, recorded with the share, the median rise in ADU, and the median rise in units of the noise. One frame is enough, because a lightning stroke can last a single frame.

- While the share stays above the limit, the frame counts as activity, so the quiet tail runs from the last bright frame. Repeated strokes inside the quiet tail stay in one clip.
- The background does not learn a brightening frame, so the flash does not raise the noise estimate for the frames after it. A brightening that lasts longer than the release limit of Part C is learned like a patch.
- Slow changes such as twilight, moonrise and thin cloud are learned by the background as they happen and do not reach K.
- A change larger than the maximum patch but smaller than the share fires nothing. That is where moving cloud usually falls. The first nights set both numbers.
- Masked pixels (Part D) are not counted.

### Part C — release slow drifters

A patch that is firing is not learned into the background. That is what keeps a long event on the Moon from fading into it. On a fixed camera the stars move. A bright star or planet can fire on its leading edge and then keep firing as it drifts, and the clips would continue until it leaves the frame.

In Sky mode, a patch that has kept firing for longer than `kSkyReleaseSeconds` (starting value 20 s) is no longer protected, and its pixels are learned with the normal time constant. An aircraft or a satellite crosses each pixel faster than the background learns, so it keeps firing after the release. A star drifts slowly enough to fade into the background, and its clip ends one quiet tail later. The clip records the release and its time.

Part C is needed only for a camera that does not track. On a tracking mount the stars stay still (see the open decisions).

### Part D — a detection mask

The ground, trees, buildings, street lights and passing cars do not belong to the sky. The observer drags rectangles on the preview to exclude them, and the masked area is shaded on the preview.

- Masked pixels are not tested and do not count towards a patch or a brightening.
- They are still recorded in the SER file unchanged (Evidence rule 1).
- The mask belongs to the Sky settings, and each clip lists its rectangles.
- Rectangles come first. A polygon follows only if a horizon needs it.

### Part E — a night unattended

- **Disk reserve.** Before a clip is written, the writer checks the free space on the output drive. Below `kDiskReserveBytes` (starting value 10 GiB, editable) the clip is not written. Its candidates are still logged, with `not_saved=disk reserve` (Evidence rule 5). The window says that saving has stopped and how much space is left. Today the program has no such check: a full disk shows only as failed writes, and a full system drive harms the rest of the computer.
- **Linked clips without repeated history.** Today a clip that continues a long event starts with the frames then in the ring, so it repeats up to a whole pre-event history from the clip before. An aircraft in view for two minutes makes several linked clips, and the repeats add up. In Sky mode a continuing clip starts at the frame after the last frame of the clip before, and its metadata names that clip.
- **The whole path in one picture.** The previews written today show one moment: the baseline, the peak frame, and its difference. A meteor in the peak frame is a short segment of its path. Feature-12's overview draws everything that changed during the clip on one frame, so a meteor, an aircraft or a satellite shows as its whole path. Sky mode adds nothing of its own here; it relies on that overview.

### Evidence

Sky mode changes no pixel and writes evidence the same way, so the evidence rules stand. Each clip records the mode, the mask, the release limit, the brightening share, and any release or brightening (rule 8). A clip refused for the disk reserve still has its candidates logged (rule 5).

### Done when

1. A test switches modes: each keeps its own settings, Moon's are the ones saved before this feature, and clips and candidate lines name the mode.
2. A test moves a 3-pixel-wide streak at 45° across a synthetic sky at 20 px per frame. Sky with its starting values records it as one clip from entry to exit. Moon with its starting values does not fire; that is the documented difference, not a fault.
3. A test brightens 40% of the frame for one frame, three times, 0.3 s apart. Sky records one clip with three brightening triggers, and the noise estimate after the last stroke is within 10% of the one before the first. Moon records nothing. A brightness ramp over ten minutes fires nothing in either mode.
4. A test drifts a bright point at 0.2 px per second, as a fixed camera sees a star. Once it fires, it is released after the limit, and its clip ends one quiet tail later. A point moving 5 px per frame keeps firing past the limit until it leaves the frame.
5. A test flashes inside and outside a masked rectangle. Only the outside flash fires, and the SER frames hold the masked pixels unchanged.
6. A test sets the free space below the reserve, through a seam in place of the drive. The clip is not written, its candidates are logged with `not_saved=disk reserve`, and the window says that saving stopped.
7. A test keeps an event firing for 50 s with a 20 s maximum. The linked clips' frame indices are contiguous and do not overlap, and each names the clip before.
8. A test moves a point across a synthetic sky in Sky mode: the clip's overview (Feature-12) shows a line along its whole path, with the trigger marked.
9. By hand, a night outside with the ASI120MM Mini and a wide lens: the clips by kind, the disk used per hour, whether a star or planet stayed firing, and whether the starting values hold.

### Out of this step

- Starting and stopping at dusk and dawn.
- Changing exposure and gain as the sky brightens and darkens.
- Sorting clips by kind (meteor, aircraft, satellite, lightning) from speed, duration and blinking.
- Satellite predictions, astrometric calibration of the lens, meteor trajectories, and triangulation with other stations. The UTC frame times already recorded are what those need.
- Polygon masks.

### Open decisions

- **Fixed camera or on the mount.** On the tracking mount the stars stay still and Part C can wait. A fixed camera needs it from the start.
- **Camera and lens.** The starting values in Part A assume the ASI120MM Mini with a wide lens. Another lens changes how far a meteor moves per frame and how fast the stars drift.
- **Disk reserve in Moon mode.** Proposed: yes. It changes nothing until the disk is nearly full.
- **Repeated history in Moon mode.** Moon's linked clips repeat the history on purpose, so each clip stands on its own. Sky stops this. Whether Moon follows is the observer's choice.
- **Release in Moon mode.** Not proposed. The camera follows the Moon, and a lunar event does not last 20 s.

### Order

Part A comes first, and the others build on it. Part E's disk reserve has no dependency and can be built before the rest, for both modes. Parts B, C and D are independent of each other. Feature-12 is built before this feature, so the first sky clips already have overviews.

### Implementation — 16 November 2026

Built. Done-when items 1 to 8 pass; item 9, a night outside, is open. The open decisions were taken as proposed: Part C is built (it costs nothing on a tracking mount), the disk reserve applies to both modes, the Moon keeps repeating the history in linked clips, and the release limit is off on the Moon.

**Where it lives.**

- `Target` (Moon, Sky) and the disk reserve are capture settings, since both change only at Start; `CaptureSettings{}` has no reserve, and the window's default is 10 GiB. The large-brightening share, the release limit and the mask are detection settings, so every trigger records the values in force. They are zero or empty on the Moon, which is what makes Moon mode the detector as it was. The mask is a fixed array of up to 16 rectangles, because every measurement carries a copy of its settings. `target_defaults.h` holds each target's starting values.
- `FlashDetector` builds the mask as pixels when it or the frame size changes. Masked pixels are left out of the threshold, the peak, the patches and the noise-floor grid. A large brightening is a new `ChangeKind`, `LargeRise`; while it lasts every pixel is held from learning. A patch, or a brightening, that reaches the release limit is no longer protected, and the frame reports it as newly released.
- `EventCapture` records each release in the clip. In Sky mode a clip that continues a long event starts with the next frame. In both modes it records the first trigger time of the clip before, which `event.txt` prints as `continues_clip:` with that clip's folder name.
- `EventWriter` checks the free space before each clip, through a replaceable query (`setFreeSpace`) that tests use. Below the reserve the result is `not_saved=disk reserve` with the free bytes, and the window says saving has stopped. `event.txt` gets `mode:`, `disk_reserve_bytes:`, the releases, and the Sky settings on each trigger line when they are on (`brightening_share=`, `release_seconds=`, `mask=`). A brightening is `kind=brightening` with its `share=` and `median_difference_adu=`. Every `candidates.txt` line gets `mode=`.
- The window keeps each target's settings in its own `QSettings` group. The Moon keeps the keys used before (`detection/…`, `capture/…`); the sky's are under `sky/`. The Detection panel's choice box only asks for another target: the window saves the old target's settings first, then loads the new ones. Otherwise a switch from Sky to Moon would have saved the Sky settings with their Sky parts already emptied. The Sky controls and the mask buttons show only in Sky mode; the preview hatches the mask, and a drag adds a rectangle while **Draw mask** is on. In Sky mode the guiding panel is disabled, its tab reads "Guiding (Moon only)", and a drag never requests a lock.
- The overview (Feature-12) leaves the mask out of its measurement and hatches it, does not mark a large brightening (it covers much of the frame), and names the mode in its caption.

**Changes from the proposal.**

- The **median rise** recorded with a brightening is the frame's median difference from the background, the same number the noise floor removes.
- The **track image** of Part E became Feature-12's overview, which shows the whole path in every mode.
- A linked clip names the clip before by that clip's first trigger time, which is its folder name unless the name was taken.

**Tests.** A new suite, `sky` (8 tests), and one window test:

- **Streak (items 2 and 8):** a 3-pixel-wide streak at 45°, 20 px a frame: one Sky clip from entry to exit, nothing on the Moon, and an overview whose path covers the whole diagonal.
- **Lightning (item 3):** three strokes over 40% of the frame, 0.3 s apart: one Sky clip with three `brightening` triggers, the struck area's noise unchanged afterwards, and the Sky settings and `mode=sky` in the records.
- **Twilight (item 3):** a ramp from 10 to 200 ADU over ten minutes fires nothing in either mode.
- **Drifting star (item 4):** released at the limit, and its clip ends one quiet tail later. A moving light keeps firing past the limit until it leaves the frame. Both use a 3 s limit to keep the test short.
- **Mask (item 5):** a flash on the masked ground does not fire, one in the sky does, and the SER frame holds the masked pixels byte for byte.
- **Disk reserve (item 6):** the clip is refused and logged with `not_saved=disk reserve`, and written when there is room.
- **Linked clips (item 7):** in Sky mode they are contiguous and do not overlap; on the Moon they repeat the history. Each names the clip before.
- **Window (item 1):** each target keeps its own settings and defaults, the Moon's under the old keys, the mask saved and restored, and guiding off in Sky mode.

All 14 suites pass in Debug and Release. By hand, the Release window in Sky mode showed the Sky controls, the hatched mask on the preview, the Sky capture defaults with the 10 GiB reserve, and the "Guiding (Moon only)" tab.

**A finding about the Moon, unchanged by this feature.** In the lightning test the Moon, which has no brightening trigger, recorded 18 candidates on the third stroke. Each stroke is learned on the Moon, so the struck area's noise estimate rises. By the third stroke the area is only just above K, and it breaks into small patches that pass the size test. Thin cloud clearing from the Moon could do the same. The Moon's behaviour was left as it was; whether it needs a rule is a question for the earthshine measurements of Feature-9.

### Field acceptance — 18 November 2026

Verified by the observer watching the sky overnight with a fixed **ZWO ASI120MM Mini** (`K 13.9`, `patch 20 to 30761 px`, rooftop polygon horizon mask), successfully capturing planes, meteors/single-frame streaks (`2026-11-18 01:02:56 UTC`), cosmic rays, and flashes (`2026-11-17 23:56:58 UTC`); moved from `features.md` to `implemented features.md`.

---

## Feature-15 — Sky extras

*Proposed on 16 November 2026, from a discussion with the observer. Out-of-step items of Feature-11.*

A sky camera is meant to run unattended for whole nights. Four things stand in the way: someone has to start it at dusk and stop it at dawn; the exposure set at dusk is wrong an hour later; a night leaves many clips to sort; and a horizon of trees and roofs is not a set of rectangles.

### Part A — from dusk to dawn

- In Sky mode, with a site set (Feature-14), **Watch from dusk to dawn** with a Sun altitude (starting value −10°). Every 30 s the window computes the Sun's altitude from the site and the UTC time, by the NOAA formulas (within a few hundredths of a degree). When it is below the limit it starts the stream if stopped and starts Monitoring; when it rises above, it stops Monitoring and the stream. It acts only on what it started itself, and the panel shows the next change.

### Part B — exposure that follows the sky

- In Sky mode with a camera, **Follow the sky's brightness** keeps the sky's median, outside the mask, between bounds (starting 8% to 30% of the full scale, aiming at 15%). It is measured every 5 s from the latest frame.
- After 60 s outside the bounds: Monitoring stops (the clip in hand is written), the exposure changes by the ratio needed (at most a factor of two) within the observer's minimum and maximum; when the exposure is at a limit, the gain moves by a set step instead. Monitoring then starts again, with the fresh boundary a camera needs. At most one change every 2 minutes.
- Each change is logged in `candidates.txt` as an `exposure_change` line with the old and new values and the measured level, and shown in the window.

### Part C — a suggested kind for each clip

- The overview names a kind from its measurements, with the reasons: **flash** (in place, at most 0.2 s), **meteor** (a straight, fast, short rise), **satellite** (steady, straight and slow, without blinking, for seconds), **aircraft** (moving with blinking or several lights), **dark transit** (a moving fall), **lightning** (a large brightening), **seeing** (many triggers scattered over the frame), or **unclear**. Speeds are in degrees per second when the image scale is known (Feature-14), otherwise in frame widths per second.
- It appears in the caption, in `event.txt` as `suggested_kind:` and on the `candidates.txt` outcome line. It is a suggestion for review, not a decision (Evidence rule 7).

### Part D — polygon masks

- A mask shape is a polygon. With **Draw mask** on, clicks on the preview place corners and a double-click closes the shape; a drag still makes a rectangle. Up to 16 shapes of up to 32 corners. Saved, recorded and hatched as before. The detector and the overview fill them as pixels.

### Done when

1. A test computes the Sun's altitude for known places and times: within 0.1° of the NOAA calculator. A test steps the schedule through a night: the stream and Monitoring start once below the limit and stop once above, and nothing happens when the schedule is off or no site is set.
2. A test of the exposure rule: a dark sky raises the exposure, then the gain at the maximum exposure; a bright one lowers them; nothing changes within the bounds, before 60 s, or within 2 minutes of a change. A window test with a constructed camera shows Monitoring stopping, the exposure applied, Monitoring restarting, and the `exposure_change` line.
3. Tests of the kinds on constructed clips: a one-frame flash, a fast streak, a steady slow point, a blinking slow point, a dark disk, a lightning stroke, scattered seeing.
4. A test draws a triangle mask: a flash inside it does not fire and one outside does; the mask is saved, recorded and restored.
5. By hand: a night outside, started and stopped by the schedule, with the exposure following the sky.

### Out of this step

- Sorting by kind beyond the suggestion: no clip is moved, hidden or deleted by its kind.
- Moonrise and moonset for the Moon target.

### Implementation — 17 November 2026

Built. Done-when items 1 to 4 pass; item 5, a night outside, is open.

**Where it lives.**

- **Part A.** `sky_schedule`: the Sun's geometric altitude by the NOAA formulas; the next crossing of a given altitude, found in five-minute steps and halved to the second; and the decision, start, stop or nothing, as a plain function. The Detection panel's Sky group has **Watch from dusk to dawn** and **Sun below**. The window checks every 30 s and on any change, using the site saved in **Site and instrument**. With no position it says so and does nothing. It shows the Sun's altitude and the next start or stop. It starts once a night, so an observer's **Stop** in the night holds until the next dusk. The same holds when a camera fails: restarting after a failure is left for a later step. The window keeps what the schedule started and still owns: the stream, or only Monitoring when the observer's stream was already running. The observer's **Stop**, or Monitoring turned off, ends that ownership. At dawn it ends only what it owns.
- **Part B.** `sky_exposure`: the sky's level (the median of a bounded grid of samples outside the mask, as a share of full scale) and the rule as a plain function with its memory. The window measures every 5 s from the latest frame while a camera runs. It changes the requested exposure and gain in the camera's settings form, which gained `controlValue`, `controlRange` and `setControlValue`. While Monitoring it then starts Monitoring again, which reopens the camera with the new settings and reads them back. In Preview it applies them live. Each change goes into `candidates.txt` as an `exposure_change` line, through the session to the clip writer.
- **Part C.** The overview computes the kind from the run of activity it already measures: its sign, whether it moved (three linked frames or more), its speed, its straightness (every centre within the larger of 3 px and 5% of the path from the line between the first and last), and blinking (at least two separate jumps to 2.5 times the run's median strength, over 1.5 s or more). A large brightening in the clip makes it lightning, and eight or more triggers spread over a quarter of the frame make it seeing. The writer records `suggested_kind:` in `event.txt` and `suggested_kind=` before the outcome on the clip's `candidates.txt` line (spaces become underscores there). The kind and the reason end the overview's caption.
- **Part D.** `detection_mask`: a mask of up to 16 polygons of up to 32 corners, shared rather than copied, since every measurement carries a copy of its settings. A pixel is masked when its centre is inside (even-odd rule), so a rectangle masks exactly its pixels as before. It is filled by the detector, the overview and the sky level alike. It is saved as corners `x,y` separated by spaces and shapes separated by `;`, and a rectangle saved as `x,y,w,h` before this feature still reads. In `event.txt` the spaces become underscores, since a trigger line separates its fields by spaces. On the preview, clicks with **Draw mask** on place corners, joined by a dashed line, and a double-click closes the shape; a drag still makes a rectangle.

**Part C on an archive recording.** `2018.07.27-28 moon eclipse iss/iss/raw/Moon_223619.avi` (1296 × 964, about 18 fps, filed with the observer's ISS recordings) was run through the probe in 300-frame windows at K 12. A bright blob about 100 px across appears in single frames, about six frames (0.33 s) apart, along a straight line, in every window up to frame 2100. Each appearance is a run of its own, so the suggestion is "flash", or "unclear" where two blobs touch. A light that shows in single frames along a line would need runs linked across gaps. That waits until the observer says what the recording shows.

**A change to the overview found by the tests.** The overview's background model learned a changed pixel like any other. A slow light, crossing at half a pixel a frame, raised its own noise within two frames, and only its leading edge stayed at K, so it showed as a one-frame flash. The overview now holds a changed pixel from learning, as the detector holds a firing patch. Every earlier overview test passes unchanged.

**Tests.** A new suite, `sky_extras` (6 tests):

- **The Sun (item 1):** NREL's Golden, Colorado example within 0.1°, the June solstice noon in Athens within 0.1°, a winter midnight far below, the next dusk at −10° found to the second, and no crossing in polar night.
- **The schedule's decision (item 1).**
- **The exposure rule (item 2):** nothing within the bounds, before 60 s outside, or within 2 minutes of a change; exposure first and then gain when dark, gain first and then exposure when bright; nothing at every limit.
- **The sky level** leaves the mask out.
- **Kinds (item 3):** constructed clips give flash, meteor, satellite, aircraft, dark transit, lightning and seeing.
- **The polygon mask (item 4):** filled exactly, saved and read back (old rectangles too), and a flash under a triangle does not fire while one above it does.

Window tests:
- **The schedule:** off does nothing; with no position the panel says why; local midnight starts the stream and Monitoring; the observer's Stop in the night holds; the next night starts again and local noon stops it; on a stream the observer started, the night adds Monitoring and the day ends only that. The site is placed where it is midnight or noon now, so the test runs at any hour.
- **The exposure follower with the simulated camera:** its sky at 42% of full scale halves the exposure after a minute outside, Monitoring starts again, and the change is logged.
- **A mask shape:** clicked corners closed by the double-click; two corners are refused; the shape is saved.

All 17 suites pass.

### Field acceptance — 18 November 2026

Verified by the observer watching the sky overnight with a fixed **ZWO ASI120MM Mini** (`K 13.9`, `patch 20 to 30761 px`, rooftop polygon horizon mask), successfully capturing planes, meteors/single-frame streaks (`2026-11-18 01:02:56 UTC`), cosmic rays, and flashes (`2026-11-17 23:56:58 UTC`); moved from `features.md` to `implemented features.md`.

---

## Feature-19 — Event clip animated GIF preview

*Proposed and implemented on 18 November 2026, from a discussion with the observer.*

Every event clip in both **The Moon** (`events/moon/<UTC>/`) and **The sky** (`events/sky/<UTC>/`) modes already writes lossless `event.ser` evidence plus static PNG previews (`00_overview.png`, `01_baseline.png`, `02_event_peak.png`, `03_diff_annotated.png`). A compact animated GIF beside them lets the observer inspect whether a candidate blinked, flashed on one frame, or moved across the field before opening the full SER file.

### Design and constraints

1. **Both lunar and sky event clips:** `EventWriter::writeClip` generates `04_event_preview.gif` automatically for every saved event clip across `Target::Moon` and `Target::Sky` (flashes, dark transits, meteors, lightning, aircraft, satellites, and manual `Save now`).
2. **Peak-centred frame selection capped at 10 frames:**
   - Uses `ClipOverview`'s active run positions (`firstActivePosition`, `mainPosition`, `lastActivePosition`)—or falls back to the detector peak / trigger frame when no active run was measured—to span `[firstActive - 2 .. lastActive + 2]` clamped to the clip bounds.
   - Events whose `[first - 2 .. last + 2]` window is at most 10 frames (such as a 1-frame impact flash: `peak - 2, peak - 1, peak, peak + 1, peak + 2` = 5 frames) include every consecutive frame verbatim.
   - Longer events (`> 10` frames) are uniformly sampled to strictly **10 frames maximum**, pinning the pre-event boundary, peak frame, and post-event boundary.
3. **Downscaling and file-size ceiling:**
   - Large frames are downscaled immediately to fit within `512 px` long edge (`even × even` dimensions), preserving small frames un-upscaled.
   - Frames are rendered with one locked `AutoStretchCurve` (`computeAutoStretchCurve` anchored on the quiet pre/post reference frame, `renderAutoStretchedFrame`) so faint earthshine or night sky detail stays visible without frame-to-frame auto-exposure pumping.
   - `encodeEventPreviewGif(...)` encodes at `4 fps` via LGPL FFmpeg (`palettegen=max_colors=...:stats_mode=full` + Bayer dithering) and automatically steps down resolution/palette tiers if needed so `04_event_preview.gif` stays at or below **512 KiB** (524,288 bytes).
4. **Evidence safety:**
   - Like `00_overview.png`, `04_event_preview.gif` is explicitly a display preview (`preview_gif: 04_event_preview.gif ...; display only, not evidence` in `event.txt`). `event.ser` holds the native unaltered evidence pixels, and any `MemoryLimitError` or FFmpeg failure leaves the clip valid with `preview_gif: not written: ...` and `complete: yes`.

### Implementation and tests

- **Files:** [preview_image.h](preview_image.h), [preview_image.cpp](preview_image.cpp), [recording/clip_overview.h](recording/clip_overview.h), [recording/clip_overview.cpp](recording/clip_overview.cpp), [timelapse/video_exporter.h](timelapse/video_exporter.h), [timelapse/video_exporter.cpp](timelapse/video_exporter.cpp), [recording/event_writer.h](recording/event_writer.h), [recording/event_writer.cpp](recording/event_writer.cpp).
- **Tests:** `overview` (`gifFrameSelectionRespectsPaddingAndCapsAtTen`, `theWriterSavesTheOverviewBesideTheEvidence`, `largeEventClipProducesDownscaledBoundedAnimatedGif`, `aRefusedOverviewLeavesACompleteClip`), `sky` (`lightningIsOneClipOfLargeBrighteningsOnlyInSkyMode`), and `darkening` (`theCrossingSavesAsDarkening`).

## Feature-20 — GitHub release notification

**Status:** Implemented and verified on 20 November 2026.

### Request and reference behavior

The maintainer requested the same release notification used by SpacePixels: when a new GitHub release is published, an older application shows a notice at the top right of its GUI. The SpacePixels implementation was read in `ApplicationWindow.java`: it checks the public latest-release API once at startup, compares the release tag with the running version, and displays a clickable notice opening GitHub. Its check runs in the background and errors remain quiet.

### Specification

- Check `https://api.github.com/repos/ppissias/SeleneWatch/releases/latest` once per normal application launch, starting one second after the window is shown.
- Make the request asynchronously. It must not block startup, the GUI event loop, acquisition, detection, recording, guiding, or time-lapse processing.
- Compare the published tag with the compiled `SELENEWATCH_RELEASE` from CMake. Accept `vYYYY.MM-NN`, `VYYYY.MM-NN`, and `YYYY.MM-NN`, comparing the numeric year, month, and monthly release number. Leading-zero spelling differences do not make a version newer.
- Ignore equal/older versions, invalid tags or local versions, suffixes such as beta/dirty, draft/prerelease responses, and missing/malformed metadata. Month must be 1–12 and the release number must be positive.
- Hide the notice until a newer stable release is found. Then display **New version <tag> available** in a clickable button at the right corner of the menu bar.
- Clicking opens the fixed `https://github.com/ppissias/SeleneWatch/releases` page in the user's browser. It does not install software or download an asset automatically.
- Stay silent on offline/DNS/TLS failures, HTTP failures including GitHub rate limits, and a repository without a published release. There are no modal dialogs or retries during that launch.
- Bound the entire request with a four-second deadline and the buffered response with a 1 MiB limit. Keep HTTPS certificate verification enabled and redirects restricted to the API origin.
- Use the public API without credentials or observing/session data. The request identifies the application and release in its User-Agent.
- Skip the startup request for isolated package/GUI launches using `SELENEWATCH_SETTINGS_NAME`.
- Preserve the BSD-2-Clause application license and document/deploy the Qt dependency required for HTTPS.

### Design and rationale

- `ReleaseChecker` owns the request behavior and version comparison. It belongs to the UI library and has no Session or frame-path dependency. The processing core does not acquire a Qt Network dependency.
- Qt Network supplies asynchronous HTTP and normal certificate handling. This avoids a custom HTTP stack or an application-owned worker thread with shutdown/timeout coordination.
- The checker creates its own QObject-owned network manager by default. Tests can supply a `QNetworkAccessManager` subclass with deterministic replies; this is an actively used test seam, not a new generic transport interface.
- `MainWindow` owns the checker and the existing menu bar owns the notice widget. The checker's newer-release signal changes only the notice. The button uses the current Qt highlight/text palette so it follows the system theme.
- The comparison follows SeleneWatch's established date-based release identity rather than lexicographic string order or arbitrary digit extraction. Unknown formats are ignored rather than guessed. The fixed releases-page destination avoids depending on an API-provided browser URL.
- The behavior intentionally matches SpacePixels' startup check. It does not poll throughout a long observing session or implement a self-updater.
- Release publishers must increment `SELENEWATCH_RELEASE`, tag the release commit with `vYYYY.MM-NN`, and publish a stable GitHub Release marked latest. Pushing a tag without publishing a release does not create an announcement.

### Packaging and licensing

- Qt 6.12.0 Network is dynamically linked under LGPLv3. The Windows package includes `Qt6Network.dll` and `tls/qschannelbackend.dll` alongside the existing Qt libraries.
- Windows Schannel provides TLS without adding OpenSSL binaries. Unused Qt Svg, OpenSSL/certificate-only TLS backends, and network-information plugins remain excluded.
- The package checker requires the new network DLL and Schannel plugin and rejects unused backends, checks imported DLLs, and launches the packaged program with only its own files and Windows on PATH.
- Qt Network's libpsl (BSD-3-Clause) and Public Suffix List (MPL-2.0) attributions/license texts are included by the extended Qt notice generator. THIRD_PARTY_LICENSES.md records the versions, sources, and obligations. The project LICENSE is unchanged.
- README.md, MANUAL.md, and DEVELOPER.md document the notice, release publishing rules, tests, and package changes.

### Acceptance and validation

- [x] Release build succeeds with the new UI-only dependency.
- [x] All 22 CTest suites pass, including `release_check`.
- [x] Deterministic release tests cover newer/equal/older releases, numeric ordering and year/month rollovers, optional v/V, padding equality, malformed tags/JSON/flags, drafts/prereleases, no-release/forbidden/rate-limit/server/DNS/TLS failures, a silent pending request, oversized responses, and one-request-only behavior. No test contacts GitHub.
- [x] The timeout test confirms the Qt event loop remains responsive while a request is pending, then the overall deadline aborts it without a notice.
- [x] The offscreen UI test verifies that the button starts hidden, occupies the top-right menu corner, displays the supplied tag, opens the correct releases page through an intercepted URL handler, and leaves a running synthetic Preview acquiring frames. The UI suite was rerun after adding the live-frame assertion and passed.
- [x] Notice screenshots are saved in `out/qa-update/new-release.png` and `new-release-narrow.png` for inspection. Resizing respects the application's existing minimum width; the notice stays within the menu bar.
- [x] `Release-packages` produces ZIP and MSI packages. The ZIP file/content/DLL-dependency/isolated-launch checks pass with Qt Network and Schannel included.
- [x] The existing real Windows GUI E2E passes all four workflows: synthetic Moon bright/dark clips, synthetic Sky meteor/lightning clips, time-lapse with Re-sharpen/video rebuild, and real lunar AVI capture/time-lapse. Screenshots are in `out/gui-e2e/`.

### Verification limits and review notes

The newer-release branch is exercised with controlled API replies and a supplied version signal, without creating or publishing a GitHub release. A future public release is not fabricated to test the notice. The production path uses the documented public GitHub latest-release endpoint and Qt's HTTPS implementation. Isolated package and GUI E2E runs intentionally skip the live startup request.

No acquisition, detector, evidence format, raw-pixel handling, or observing configuration is changed. Generated packages are local validation artifacts; source changes remain available for maintainer review and a later commit/release.

## Feature-21 — Lunar and planetary time-lapse wording

**Date:** 20 November 2026. **Status:** Implemented.

### Request

Use **lunar and planetary time-lapses** throughout the application and website, replacing the former processing jargon everywhere. Allow a more evocative description of time-lapses, centered on planetary motion, animations, and shadows moving across the lunar surface.

### Specification and decisions

- Keep **Time-lapse** as the short control name and **lunar and planetary time-lapses** as the full feature name.
- Use **Watch planets turn and lunar shadows unfold** in the introductory time-lapse copy, without claiming unique observing capabilities or changing the scientific scope.
- Update the application startup tip, README, manual, camera settings guide, Feature-18 specification, developer terminology guidance, and the time-lapse session's header comment.
- Describe frame selection and stacking directly in technical instructions. Preserve exposure guidance, frame limits, evidence rules, operating states, and validation details.
- Apply the corresponding terminology, animation description, and factual homepage introduction to the separate SeleneWatchWeb project (Feature-Web-2).
- Preserve existing completed feature sections without rewriting their history. No occurrence of the retired term was present in those sections.

### Validation

- Release build and all 22 CTest suites pass, including the existing UI and time-lapse suites.
- ZIP/MSI packages rebuilt with the revised application tip and packaged documentation; package file, DLL dependency, and isolated launch checks pass.
- Repository-wide searches find no retired terminology in maintained application sources, documentation, or feature specifications.
- Website TypeScript/production build and rendered-page, navigation/ARIA, asset integrity, and production HTTP checks pass.
- Browser review of the revised website wording was stopped because the computer-use tool could not identify Chrome's current URL confidently. No new visual verification is claimed for this copy revision.

This is a wording change. Capture, frame selection, stacking, scientific evidence, and saved observing results are unaffected.

---

## Feature-9 — A measured trigger

> **14 November 2026: implementation complete; acceptance item 7 partly open.** The controls, detector, overlays, evidence, and automated/window checks are built. Recording measurements below cover mare, terminator, bright limb, crater rim, and RGB terrain. Earthshine was not identified in the survey, and the bright-limb comparison is saturation-limited. The full feature remains here until that observing calibration is completed; its proposal and review history are preserved below.

The detector today fires when two frames rise by more than 60 gray levels across 120 pixels of the synthetic disk. Those numbers match the painted test image. They do not match a recording or a camera: the Moon is not centred, the noise depends on gain and sky, and a published lunar impact flash is often one frame. The trigger has to be set from the frames being watched, and the same trigger has to run on the synthetic source, on a recording, and on a camera.

This is the step where the design is most likely to be wrong, because it is the first one judged by real lunar data. It comes before any camera work for that reason. Feature-6 supplies the recordings.

### Injected blip

The blip moved to Feature-6, Part C, on 13 November 2026. Its full description is in [implemented features.md](implemented%20features.md). The observer can test with it as soon as recordings play, and this feature finds its calibration tool ready.

What this feature adds to it: the status line shows the blip's amplitude as a multiple of the noise at its centre, once the trigger below measures that noise.

### The trigger

The trigger is four numbers, applied to the difference against a held reference. They are editable while the stream runs, and the values that fired are copied onto the clip.

- **Reference.** A running mean per pixel over the recent quiet frames, with a time constant of about two seconds (a named constant). It stays frozen while a candidate is open, so a bright frame is not compared with the previous bright frame.
- **Noise.** A running variance per pixel, kept beside the mean and frozen with it. Noise on the Moon is not one number. Photon noise and seeing make the sunlit limb and every sharp edge far noisier than the earthshine, and the earthshine is where impact flashes are found. One scale for the whole frame would either fire on the bright part or be deaf on the dark part. The per-pixel value has a floor: a robust scale of the whole difference, estimated from a subsampled grid so the frame is not sorted. The floor keeps a saturated or clipped pixel, whose own variance is zero, from firing on nothing. The floor updates every frame and is the "noise" number shown live.
- **Sigma.** A pixel counts when its rise exceeds K times its own noise. K is the sensitivity. The starting value is 6.
- **Size.** The counting pixels must form a compact patch: the count lies between a minimum and a maximum, and their bounding box is not much larger than the count. The minimum rejects a hot pixel. The maximum and the box reject a cloud, a bump of the telescope, or scattered noise. Starting values: minimum 8 pixels, maximum 400, box area at most 4 times the count.
- **Persistence.** One frame fires when its peak is at least K times the noise and the patch passes the size test. Two consecutive frames fire at the same K. The one-frame path is on by default, because a missed one-frame flash is not recoverable. If a recording shows that one-frame candidates are mostly noise, the user turns that path off and only the two-frame rule remains.

Hot pixels need no list of their own at first. A steady hot pixel does not rise. One that flickers has a large variance of its own, and the minimum size rejects a single pixel in any case. A learned list (a position that exceeds K on more than a third of the frames in a few seconds is ignored afterwards) is added only if a recording shows hot pixels that still fire.

The mean and the variance are two float buffers the size of the frame, updated in the same pass that tests the pixels. The cost is measured on the largest frame in use before the camera stage; the overload policy in the project description applies if it does not keep pace.

Every frame the detection panel (below) shows the noise floor, the peak in units of its own noise, the patch size, and whether the frame passed. That is the sensitivity meter. There is no separate filter stack. *(The first draft put this meter in a status-bar line. The panel replaced it on 13 November 2026.)*

### The detection panel

*Added on 13 November 2026, from a discussion with the observer.* Setting a trigger from numbers in a status line is hard. The observer needs to see, while the image plays, how close the ordinary frames come to firing. The panel is where the trigger is set and watched.

```
Detection ───────────────────────────────────────────────────
 Sensitivity K  [·····│·······▲·····┃········]  6.0σ
                     now 3.1σ   ▲ highest in last 10 s: 4.7σ
 Min size       [··▲┃························]  8 px   now 3 px
 Max size       [·▲··························┃]  400 px
 One-frame trigger [x]
 Peak σ, last 10 s:  ▁▂▁▃▂▁▅▂▁▁▂▃▁▁▂ ┄┄┄ K
 Noise floor 2.3 ADU   peak at (812, 433)   frame: quiet
 Overlay:  ( ) none   (•) σ map   ( ) noise map
```

- **A slider per setting, with the live value on it.** The slider for K carries the frame's peak in units of its own noise; the size sliders carry the current patch size. A value read at 30 frames per second flickers too fast to judge, so each slider also holds the highest value of the last few seconds. The distance between that mark and the setting answers "how close am I to a detection".
- **A short history.** A strip of the peak per frame over the last seconds, with K drawn across it. The ordinary noise shows as low grass, the margin above it is visible, and a blip shows as a spike.
- **Overlays on the preview.** A σ map colours each pixel by its rise in units of its own noise and marks the pixels over K. A noise map shows each pixel's noise, which is the threshold in gray levels divided by K: high on the sunlit limb and at edges, low on the earthshine. A box marks the peak.
- **A panel, not a mode.** The same detector runs whether the panel is open or not, and what the panel shows is what the detector does. It can stay open during a real session, where it shows cloud or a falling transparency as rising noise.
- **The rules it keeps.** The detector runs on the source thread and owns its buffers. It sends a small per-frame summary, and the overlay at preview rate, through a one-slot mailbox like the preview's; the window never reads the detector's state. Settings travel the other way and take effect from the next frame. The overlays are drawn for the screen only and never touch a stored frame (Evidence rules 1 and 3). The values in force when a candidate fires are copied onto its clip.
- **Settings are remembered between runs.** The panel is the first settings page of the program, so the last trigger values are kept and restored at the next start.
- **Not for the Feature-4 detector.** Its two numbers (60 gray levels, 120 pixels) are replaced by this feature, so the panel is built with the new trigger and not before.

### One set of numbers for the whole disk

The observer asked whether the bright part of the Moon and the earthshine need settings of their own. A single threshold in gray levels cannot serve both: one value either fires on the sunlit limb or is deaf on the earthshine. This trigger is built around that problem. Each pixel learns its own noise, and K counts in units of that noise, so one K gives every pixel its own threshold in gray levels, high where the image is noisy and low where it is quiet. The noise map overlay shows this directly.

Settings per region are therefore not built now, for three reasons.

- **The calibration tests the question.** Steps 2 and 3 of "How the numbers are chosen" measure the faint limit on the earthshine and on the bright limb in units of noise. If the two agree, one K serves the whole disk. If they differ, that measurement is the case for regions, and it is recorded under item 7 of "Done when".
- **The Moon moves under a drawn region.** It drifts through the field over a session, and the terminator and the earthshine move with it. A region drawn on the image would need to follow the feature lock of Feature-8. The per-pixel noise follows the drift by itself, within its time constant of about two seconds.
- **The likely weak spot is the edges, not the halves.** At the limb and the terminator, seeing moves the edges between frames. That noise has heavier tails than the per-pixel variance assumes, so false candidates may gather along edges at a K that is quiet elsewhere. If a recording shows that, the remedy is a rule tied to strong gradients, or registration. A hand-drawn region is not.

One kind of region may still earn its place later: a mask that watches only the dark side. Flashes cannot be seen against the sunlit surface anyway, so the mask would cut false candidates and work. It would have to follow the Moon's drift and be recorded on every clip. It is added only if recordings show the need.

### OpenCV

The project description lists OpenCV as something to consider, not as a requirement. It is the usual library for the later alignment work: a whole-frame shift when the mount drifts, and optical flow or a Kalman filter when a transit has to be followed. Feature-5 already leaves both of those out.

The flash trigger does not use them. The difference, the noise scale, the threshold, and the compact-patch test are plain loops over the pixels. A blur, a morphology pass, or a background-subtractor model would hide the same four numbers behind another library, and a background model treats lunar drift as the event. Connected components are not worth the dependency either: the bounding box of the pixels that passed the threshold is the clump test, and it is one pass.

OpenCV comes back only if a real recording shows that the noise floor is mount drift, the limb moving between frames, rather than photon noise and hot pixels. The measurement is the meter on that recording. If the ordinary peak is a bright rim that slides a few pixels a frame, the fix is a shift of the whole frame before the difference, and that is a later feature. A small FFT is enough for that one shift. OpenCV is the library to take if that feature also needs optical flow and feature matching. It is Apache-2.0, which the project can link dynamically. It is not added on speculation.

### How the numbers are chosen

The recording is the calibration source.

1. Play a recording of the lunar conditions that matter, with the blip off. Read the meter, looped over a few seconds if that helps (Feature-6, Playback). K and the minimum size have to sit above the ordinary peaks. If the ordinary patch is already large, the noise estimate or the box test is wrong, and raising K will not fix it.
2. Place a small blip on the dark limb. Step the amplitude down until the trigger stops. That amplitude, in noise units, is the faint limit for those conditions.
3. Repeat on the bright limb. The same amplitude in gray levels is a smaller multiple of the noise there. The gray-level number will differ. The sigma number is the one that should agree.
4. Inject a blip of one frame at the amplitude from step 2. If it must be kept, leave the one-frame path on and raise K only until a run with no blip writes no folder.
5. Leave the minimum size above a hot pixel and below the blip. Leave the maximum size above the blip and below a frame that changed across the disk.

Those four numbers are the session's trigger. They are stored on every clip from that run. They are not compiled in, and they are not claimed to be universal.

### Colour frames

The trigger works on one value per pixel. For an RGB frame it reads the mean of the three channels, and for a raw Bayer mosaic it reads the mosaic as it is. Both are derived views; the stored frame is not touched (Evidence rule 1).

### Done when

1. While a recording plays, the detection panel shows the blip's amplitude as a multiple of the noise at its centre. *(This item first described the blip's controls. Those moved to Feature-6 with the blip, as its item 13.)*
2. With the blip off, a recording whose ordinary peaks sit under the trigger writes no folder. The same recording with a blip above the trigger writes one folder. `event.txt` names the sigma, the size limits, the measured peak, and the blip, and the folder holds the annotated difference image.
3. Changing K or the size limits affects the next frame without a restart.
4. Tests cover the sigma rule on a constructed noisy frame: a compact patch at the threshold fires, a patch just below it does not, a single hot pixel does not, and a change spread across the frame does not. On a frame with a quiet half and a noisy half, the same blip in gray levels fires on the quiet half only, and the same blip in units of noise fires on both.
5. The synthetic source's fixed flash still fires the new trigger, and its moving marker still does not.
6. The cost of the trigger per frame is measured on the largest recording in hand and written down here.
7. By hand, on the observer's own recordings of at least three conditions (earthshine, the sunlit limb, the terminator): the steps under "How the numbers are chosen" are carried out, and the faint limit and the false-candidate rate found for each are written down here. If the ordinary peaks on those recordings come from drift or seeing at edges and not from noise, that finding is recorded and decides whether registration moves forward. Whether the faint limit in units of noise agrees between the earthshine and the bright limb is written down too, and decides whether settings per region are needed.
8. The detection panel shows each setting as a slider carrying the live value and the highest value of the last few seconds, the peak history with K across it, and the σ map and noise map overlays. Moving a slider changes the next frame.
9. A test shows that opening the panel or switching an overlay changes no stored frame and no detection result.
10. The trigger values in force at Stop are restored at the next start, and a clip records the values that fired, not the values shown later.

### Implementation and validation review — 14 November 2026

The implementation is in `FlashDetector`, `DetectionPanel`, `Session`, `EventCapture`, and the recording writer. It uses native ADU for Mono8/Mono16, channel-mean RGB, and the unchanged Bayer mosaic. Threshold comparison is inclusive. The reference learns cumulatively at startup, then at the named two-second exponential rate; the first frame initializes it. The noise floor is at least 0.5 ADU and uses the median absolute deviation on a bounded 4096-point grid.

Two original implementation choices above were revised on purpose. Detection and quiet reference updates use two passes: the global decision must be known before changing any reference sample. A sparse 8-connected patch search replaces the single global bounding box, because a regression showed that one unrelated hot/moving pixel could veto a real compact flash. This uses plain C++, with no OpenCV dependency, and rejects dense changes before the bounded sparse work. The strongest passing patch is selected, or the strongest rejected patch when none passes. The synthetic pacing marker was reduced to five pixels, below the default minimum; the fixed flash keeps radius eight. Detection has no synthetic-disk mask or other source-specific rule.

The dock implements the sliders and exact controls, current and recent markers, ten seconds of source-time history, selected-centre noise units, and full-resolution sigma/noise maps. Settings reach the next frame and are restored at the next launch. Frame and immutable measurements travel together at preview rate (also on triggers); maps use the actual pre-update noise. Closing the dock leaves every-frame detection running. Tests compare bytes and decisions across overlay modes and exercise the actual window with the dock hidden during injection.

Each trigger records its original K, size limits, persistence mode, normalized peak, noise floor, and patch coordinates. The strongest passing frame keeps its actual difference from the held reference for `03_diff_annotated.png`. This derived preview is separate from the raw SER; its frame and settings are recorded explicitly. Test markers survive a standalone SER replay, including an injection occurring after its first frame. The writer releases the difference buffer when it refuses a clip. The software-version header now refreshes on every build, so source edits and commits without reconfiguration cannot retain an earlier clean version in the metadata; an isolated Git-repository check covers both and verifies that unchanged headers are not rewritten.

All five CTest targets pass in Debug and Release, including threshold/size/noisy-half checks, native low/high-aligned Mono16, the fixed synthetic flash and nonfiring marker, next-frame settings, persistence, EOF/stop and loop boundaries, original trigger settings after later changes, and actual-window overlay/injection/settings checks. Window testing also found and fixed a dock visibility callback during destruction. No new dependency or license change was introduced.

Measured on an AMD Ryzen 9 9955HX in Release, the largest archive frame (1280×1024 RGB) takes **5.050 ms mean / 5.130 ms 95th percentile** without a map; sigma/noise maps made every frame take **5.666 / 5.635 ms mean**. The 1296×964 Mono8 case takes **3.577 ms mean / 3.666 ms 95th percentile**, or **4.230 / 4.276 ms mean** with maps. These exclude reading, GUI colouring, and disk writing. Both are below the recordings' nominal 100 / 55.6 ms periods; a separate detection thread is not justified by these measurements. Full model/map/difference memory is additional to the frame-pool readout and remains part of Feature-5's RAM-budget work.

The real-recording controls expose a sensitivity cost. At K=6, the roughly thirteen-second prefixes produced 8 mare, 10 terminator/limb, 21 crater-rim, and 2 RGB background candidates (36.15, 45.19, 96.61, and 8.89 candidates/minute over those short prefixes). They are unclassified candidates, not a verified false-impact rate. Tested quiet K values were 18, 24, 12, and 24 respectively. Changing K can change reference freezing, so candidate counts are not necessarily monotonic in K.

With FWHM five, one- and three-frame blips had these tested miss-to-recovery brackets: mare **22.5σ → 27σ (36.01 → 43.21 ADU)** at K=18; terminator **30σ → 36σ (150.98 → 181.18 ADU)** at K=24; crater rim **15σ → 18σ (131.59 → 157.91 ADU)** at K=12. These are single-position/time trials, not recovery probabilities or universal faint limits. The bright limb and RGB recover only after saturation in the chosen trials, so their requested amplitudes do not establish an unclipped faint limit. A real mare control writes zero folders without injection and exactly one complete 180-frame SER with a one-frame blip; Siril 1.4.4 independently reads its format, timestamps, and test marker.

The archive first-frame survey opened 240 lunar-named recordings and did not identify a suitable earthshine exposure. Black sky or clipped shadows are not substitutes. Annotated ordinary-frame differences show crater rims and the limb, consistent with motion/seeing against the held reference. **A registration experiment should move ahead of camera deployment**; whether global drift or local seeing dominates still needs measurement. **No region settings UI is justified yet:** earthshine versus bright-limb agreement is unanswered, and the available sunlit result is saturation-limited.

Acceptance items **1–6 and 8–10 are implemented and checked**. Item **7 is partial**: real-terrain rates and recovery brackets are recorded, but suitable earthshine/unclipped comparison data and the final one-K conclusion are still needed. The detailed methods, per-condition coordinates, timing meanings, saturation limits, runtime table, and reproduction commands are in [docs/feature-9-validation.md](docs/feature-9-validation.md); the measured data subset is [docs/feature-9-calibration.json](docs/feature-9-calibration.json). Full local QA artifacts are under ignored `out/qa-feature9/`. This incomplete calibration is why Feature-9 has not been moved into the append-only implemented history.

### Registration follow-up review — 14 November 2026

The planned whole-frame experiment now exists in the manual probe, with a tested native-word integer translator and four recording comparisons. It is linked into the probe/tests only. Cropped and aligned trials use the same 64-pixel interior and the same injected lunar target; a full-field control identifies cropping effects. All 856 shifts pass the prototype's internal checks, with mean estimation costs of 5.35–5.98 ms. No archive/evidence pixels or dependencies changed.

Candidate counts alone were misleading. At K=6 the aligned mare falls from eight cropped candidates to one, but 238/240 frames pass in one long firing run; the rim similarly has 239/240 passing frames. RGB remains active for 135/136 frames in both cropped and aligned modes. Passing occupancy and longest-run metrics are therefore now recorded alongside counts. At the previously quiet K values, all controls are quiet; the mare and terminator have unchanged tested recovery brackets, while the rim/RGB aligned recoveries are saturation-limited. Short, single-position trials establish neither a long-session rate nor an unclipped sensitivity improvement.

**Registration stays experimental.** Investigate startup, held references, and rearming during sustained edge/seeing activity before production integration, then repeat longer controls and independent injections at several positions/times with suitable unclipped earthshine/bright-limb data. The evidence does not yet separate residual seeing from reference holding or integer registration error. Feature-9 acceptance item 7 remains partial. Methods, all comparison tables, limits, and reproduction are in [docs/registration-experiment.md](docs/registration-experiment.md), with [docs/registration-measurements.json](docs/registration-measurements.json) retaining the measured cases. Earlier proposal and validation text above remain historical records.

### Reference and rearming implementation review — 14 November 2026

The sustained-activity follow-up reproduces and fixes three faults: one global run could hide a separate new patch, unrelated successive patches could confirm each other in two-frame mode, and an open clip prevented quiet background learning. The original whole-frame freeze rule is revised deliberately: passing patch boxes plus one pixel are held, while background elsewhere keeps learning through collection. Separate patches have independent persistence; adjacent/overlapping patches inherit state across merges/splits, and source-index gaps break pending confirmation. All new candidate patches, including several in one frame, reach the writer's records. A measured difference is copied before the explicit reference-update phase. Raw published pixels remain unchanged.

The panel and trigger records now expose active patch count, longest active duration, protected pixels, and model learning frames. A constructed 359.9-second TEST run recovers all 98 independent one-frame flashes beside a continuously stronger patch, with the expected 99 total new candidates and no duplicate for that persistent patch. Regression tests also cover merge/split identity and pre-update snapshots. The shared budget now charges the one-byte-per-pixel protection mask.

Four complete 500-frame recordings were checked with five independent one-frame injections each, at K=6 and the earlier quiet comparison K. At K=6 the matched counts are 4/5 mare, 4/5 terminator, 5/5 rim, and 5/5 RGB; several recover while other activity is already firing. Background updates on all 499 post-initialization frames. This does not establish universal recovery or a quiet default: higher-K recovery is position-dependent, some first-opportunity flashes are missed, and rim/RGB examples include saturation. The revised common-crop experiment reduces aligned passing occupancy from 238/240 to 15/240 on the mare, 226/240 to 27/240 on the terminator, and 239/240 to 15/240 on the rim; RGB remains 135/136. New candidate totals count independent patch runs and cannot be compared directly with older global-run totals. Registration stays experimental; acceptance item 7 remains partial.

Methods, runtime, unchanged historical measurements, scientific limitations, and validation are in [docs/reference-and-manual-validation.md](docs/reference-and-manual-validation.md), with complete trial summaries in [docs/reference-rearming-measurements.json](docs/reference-rearming-measurements.json). A connected/overlapping transient inside an already active patch remains unresolved by spatial persistence; no blind warm-up interval or automatic threshold tuning was added.

### Live threshold controls and rejection feedback — 15 November 2026

The observer can adjust six numeric settings with sliders and exact entry, plus the existing one- or two-frame confirmation choice. The maximum bounding-box area per above-K pixel defaults to 4.0. The **Background and noise** section exposes the reference time constant (2.0 s) and the minimum noise floor (0.50 ADU). Their values apply at the next frame and are remembered with the other detector settings. The time constant controls future reference updates. Each trigger, candidate record and measured clip peak carries its own settings.

The live panel distinguishes a peak below K, a patch outside the pixel bounds, a box-to-patch ratio above its limit, and a frame that exceeds the bounded patch search. It also shows the pending 1 / 2 frames state when two-frame confirmation is selected. The measure and measure-batch probes accept `maxPatchBoxRatio`, `referenceTimeConstantSeconds` and `minNoiseAdu` for replay calibration. Constructed tests show the corresponding changes in patch acceptance, noise-normalised response and background learning. Field calibration remains open. [Preview and Monitoring](docs/preview-monitoring.md)

### Out of this step

- Limb fitting, registration, optical flow, and a separate transit detector. OpenCV is not a dependency of this feature.
- A control that picks K by itself.
- Settings per region of the disk, and a dark-side mask. Each waits for a recording that shows the need (see "One set of numbers for the whole disk").
- An event browser.

### Order

After Feature-6, which supplies the recordings, the `Session`, and the blip. Before Feature-10, so that the trigger has been through real data before a night at the telescope depends on it.

---

## Feature-5 — Display stretch and a ring sized in seconds

> **14 November 2026: implemented; real-earthshine display acceptance remains open.** Native-bit stretch, Bayer display, source-time capture windows, a shared image/analysis byte budget, writer admission/rate reporting, and window checks are built. The constructed faint-recording check passes; no suitable real earthshine sequence was identified. The proposal and implementation notes stay together here until that field check is available.

Storing a Mono16 frame and reading and writing SER are Feature-6. The trigger is Feature-9. This feature keeps the display-stretch controls and the duration-based ring under one RAM limit. Both are needed before a camera is used: without a stretch the earthshine side of a 16-bit image is black on the screen and cannot be pointed at or focused, and a ring counted in frames means a different length of history at every frame rate. Both can be built and tested on 16-bit recordings, with no hardware.

Astronomical cameras provide 8-bit, 12-bit, or 16-bit raw pixel streams. Faint lunar phenomena (such as impact flashes on the unilluminated limb or earthshine) occupy the lower bit ranges, while bright illuminated terrain occupies the high end. This feature introduces a non-destructive display stretch engine for live preview and a configurable time-based ring buffer.

### 1. Event duration research and buffer sizing

To determine the ideal ring buffer capacity and recording durations, events observable against the Moon are categorized by their physics and duration:

| Event Type | Typical Duration | Frame Count (30–60 fps) | Detection & Capture Strategy |
| :--- | :--- | :--- | :--- |
| **Lunar Impact Flash** | 20 ms – 150 ms (rarely up to 1–2 s) | 1 – 5 frames | Sudden point-source brightening on earthshine or night side. Fast onset; 1–2 frame trigger. |
| **Atmospheric Meteor Transit** | 0.1 s – 0.8 s | 3 – 45 frames | Fast luminous streak crossing the lunar disk. |
| **ISS / Tiangong Transit** | 0.4 s – 1.5 s (up to ~2.5 s at low elevations) | 15 – 150 frames | Rapid silhouette crossing. At 400 km orbital altitude (~7.6 km/s), transit across 31' Moon takes ~0.5 s near zenith. |
| **LEO Satellite Transit** (Starlink, etc.) | 0.5 s – 3.0 s | 15 – 180 frames | Fast silhouette or specular solar glint. |
| **Commercial Aircraft Transit** | 0.5 s – 4.0 s | 15 – 240 frames | Silhouette, exhaust turbulence, and contrails. Distant or slow aircraft can take up to 4–5 s. |
| **Birds / Bats** | 0.1 s – 1.0 s | 3 – 60 frames | Fast silhouette with rapidly shifting geometry. |
| **Impact Ejecta / Vapor Plume** | 1.0 s – 8.0 s | 30 – 480 frames | Expanding dust/vapor cloud scattering sunlight or obscuring surface features. |
| **Stellar Occultation** (Ingress / Egress) | < 50 ms (diffraction limit) | Instantaneous edge | Step-function disappearance/reappearance. Requires 5–10 s window for baseline calibration. |
| **Grazing Stellar Occultation** | 1 – 3 minutes | 1,800 – 10,000 frames | Star blinks multiple times through lunar topography. (Planned ephemeris event). |
| **Planetary Occultation** (e.g. Jupiter, Mars) | 60 – 120 s (ingress alone) | 1,800 – 7,200 frames | Extended planetary disc occultation. (Planned ephemeris event). |

#### Capture window conclusions:
- **Transient / Unplanned Events (Flashes, Meteors, Transits, Plumes):** An overall capture window of **5 to 10 seconds** is optimal. A configurable pre-trigger history of **2 to 5 seconds** guarantees recording the object's entry into the field of view even if detection occurs midway across the disk. A post-trigger duration of **3 to 5 seconds** captures the transit exit or plume dissipation.
- **Extended / Ephemeris Events (Occultations):** Long multi-minute events are best served by continuous manual or scheduled streaming to SER, rather than maintaining an impractically large RAM ring buffer.

### 2. Multi-bit-depth data representation

The storage formats are the ones Feature-6 defines:
- `PixelFormat::Mono8` (1 byte per pixel, 0–255)
- `PixelFormat::Mono16` (stored in memory as 16-bit little-endian `uint16_t`, 2 bytes per pixel)
- `PixelFormat::RGB888` (3 bytes per pixel)

There is no `Mono12` format. Twelve-bit data is a Mono16 frame that records 12 significant bits and whether they sit at the top or the bottom of the word, stored exactly as the source delivered it (Evidence rule 2). The display stretch and the SER header read those two fields.

#### Memory budgeting and buffer sizing:
For a 10-second ring buffer:
- **640 × 480 Mono16 @ 60 fps:** ~368 MB
- **1920 × 1080 Mono16 @ 30 fps:** ~1.24 GB
- **1920 × 1080 Mono16 @ 60 fps:** ~2.48 GB
- **3840 × 2160 (4K) Mono16 @ 30 fps:** ~4.98 GB

The ring buffer capacity must no longer be a hard-coded constant (150 frames). It will be configured by **target duration in seconds** (e.g. default 3–5 seconds) clamped by a **user-configurable maximum RAM ceiling** (e.g. default 2.0 GB).

The ceiling covers every live frame, not only the ring. Since Feature-7 frames are shared, and the frames held by the open clip and by clips waiting for the writer count against the same limit. The writer's queue limit, counted in frames in Feature-7, becomes a share of this ceiling in bytes. How large that share must be depends on how fast the SER writer drains it, and that is measured here.

### 3. Non-destructive display stretch pipeline

Standard monitors and Qt GUI widgets (`QImage`) display 8 bits per channel. If 12-bit or 16-bit data is simply shifted or clamped, faint features on the dark limb and earthshine appear completely black.

The display stretch converts high-bit-depth frames into an 8-bit `QImage` (`Format_Grayscale8` or `Format_RGB888`) strictly for GUI rendering:
- **Data integrity rule:** The raw frame data stored in `Frame`, buffered in `FrameRingBuffer`, and recorded to disk remains **100% linear, untouched, and unclipped**.
- **Display modes:**
  - *High-bit shift / Linear:* Fast bit-shift (`>> 4` or `>> 8`), zero CPU overhead, optimal for bright illuminated lunar maria and highlands.
  - *Window / Level (Min/Max):* Sliders for Black Point ($B$) and White Point ($W$) mapping linearly to 0–255.
  - *Auto-stretch (MTF / Percentile):* Automatically samples frame median and dispersion (e.g. median ± $k \times \text{MAD}$ or 0.1% / 99.9% percentiles) to automatically reveal earthshine and faint details.
  - *Invert mode:* Inverts brightness for high-contrast tracking of dark transit silhouettes.

### 4. Colour display

A raw Bayer mosaic is debayered for the screen only, with a simple bilinear interpolation in plain loops. The stored frame keeps the mosaic and its pattern (Evidence rule 2).

### Done when

1. `SyntheticFrameSource` can produce Mono8 and Mono16 test frames, the latter with 12 or 16 significant bits.
2. The preview window displays 12-bit and 16-bit feeds with interactive stretch controls (Min/Max sliders and Auto-stretch). On a 16-bit recording of the earthshine side, the auto-stretch shows surface detail that the fixed shift shows as black.
3. The ring buffer can be configured by duration in seconds with memory use tracked and capped against a RAM limit. The limit covers the ring, the open clip, and the clips waiting for the writer.
4. The SER writer's rate is measured on the largest frames in hand and shown as the write rate. The share of the RAM limit given to waiting clips is set from it.
5. A test shows that no stretch setting changes a stored frame or a written clip.

### Implementation and validation review — 14 November 2026

The Display and capture dock is tabbed beside Detection. Linear/high-bit, native-ADU black/white sliders and exact controls, percentile/midtones Auto-stretch, inversion, and bilinear Bayer colour are display-only. Changes also redraw a stopped recording from its retained raw frame. Settings persist. Capture controls apply at Start; synthetic input adds low/high Mono12, Mono16, and RGGB12 with a deliberately faint test half-disk.

History, quiet tail, and maximum post-trigger collection now use source seconds, defaulting to 5, 3, and 10. Playback speed does not change the window. Long events continue at time or RAM boundaries. Each clip records requested durations, available history, byte allowances, and its collection ending. Completed files and a completed requested tail are distinct. A RAM ending is visible in the window's counter/message.

One shared `MemoryBudget` follows numeric image/analysis allocations across pools, detector/sparse arrays, maps, differences, and display images, including old sources still held by the writer. Shared pixels count once, copies separately, and capacity/reallocation peaks/spares count. This is not a process-RSS ceiling: metadata, bounded diagnostic histories, indices, Qt caches, codec workspaces and allocator overhead are additional. A layout-dependent working reserve precedes the history/open-clip/writer shares; actual admission remains authoritative. The collection allowance leaves room for its difference and one incoming frame. Writer admission counts active plus queued bytes. Refused clips/continuations are logged. Optional-map refusal leaves detection unchanged; a required allocation failure stops acquisition visibly and hands off the available clip with the failure recorded. The complete ownership and admission policy is in [docs/feature-5-validation.md](docs/feature-5-validation.md).

Ten new core cases cover native low/high 12/16-bit contrast and evidence invariance, actual synthetic native layouts/faint detail, all Bayer patterns/edges, shared/copy/image lifetimes, optional-map refusal, time/byte-limited history, tails at different rates, long-event continuation, forced allocation failure, and RAM splitting with its difference. The actual window plays and stretches a generated 12-bit SER during a one-frame injection, checks unmodified saved frames and the original file byte-for-byte, restretches after Stop, and restores settings. All 800 frames of the pressure sequence are analysed even when a capped writer refuses a continuation. Qt offscreen rendering is used; no manual native-desktop check is claimed. Siril independently opens new Mono8/RGB/Mono16 samples; profiler/measurement output cannot name the input itself or its hardlink.

Release preview conversion means (linear/window/auto): **1296×964 Mono8: 2.583 / 5.147 / 6.367 ms**; **1280×1024 RGB: 7.909 / 23.318 / 25.249 ms**; **1296×964 Mono16, 13 significant high bits: 3.233 / 14.622 / 14.051 ms**. Reading and QWidget composition are excluded. Buffered 128-repeat SER writes measured **3,641.71 / 4,283.34 / 3,863.77 MB/s**, respectively, including Qt flush but excluding display previews. These generated TEST clips exercise roughly 160/503/320 MB and are not sustained physical-disk guarantees. The measured headroom on the available 22.5/39.3 MB/s inputs does not justify increasing the conservative writer share. Full measurements are in [docs/feature-5-measurements.json](docs/feature-5-measurements.json).

Acceptance **1, 3, 4, and 5 pass**. Item **2 is partial**: controls, native-bit handling, and constructed faint-detail visibility pass; a real earthshine recording remains unidentified in the survey. Auto-stretch of the sole Mono16 archive file shows noise, not usable lunar terrain. This shares Feature-9's field-data gap. No dependencies or license terms changed. The original proposal is preserved above; the feature has not moved into implemented history while that field check remains open.

### Optional live histogram — 15 November 2026

**View → Histogram** opens a dock below the preview. Its fixed bin array counts samples from the latest GUI frame up to five times per second when the dock is visible. The horizontal axis uses the declared native ADU range or the observed range with **Fit range** enabled. The vertical scale supports linear or logarithmic counts. The dock remembers visibility and the two scale options.

Counts are calculated before display conversion and use the declared sample alignment. A single distribution combines Bayer sample codes or the stored RGB channels. The readout includes the exact observed range and the fractions at zero and at the maximum code. The maximum code follows the stream's declared layout and can be 65535 for a camera that preserves full RAW16 words. Tests cover high/low alignment, Bayer and RGB, endpoint counts, range fitting, and the optional dock in Preview. A bench check used the histogram during RAW8 and RAW16 Preview and Monitoring on the ASI120MM Mini. [Preview and Monitoring](docs/preview-monitoring.md)

### Out of this step

- Hardware camera SDK integration (Feature-10).
- Advanced automated transit / silhouette tracking detectors (optical flow / Kalman filtering).
- Lunar limb sub-pixel registration.

### Order

After Feature-9 and before Feature-10. If the recordings used for Feature-9 are 16-bit and too dark to place a blip on with the fixed shift, the stretch moves ahead of Feature-9; nothing in it depends on the trigger.

---

## Feature-10 — First light: camera adapters (ZWO, then QHY) and manual save

> **15 November 2026 — ZWO, QHY and manual capture software are integrated.** The Mini has passed RAW8/RAW16, readback, evidence, injection, induced-loss and two 30-minute soak checks. QHY loads the user-installed runtime. QHY hardware validation and lunar observing acceptance are pending.

This is the step where SeleneWatch watches the real Moon. By now the path has run on recordings, the trigger has been measured on them, clips are SER, and the preview can show the earthshine. What is missing is the camera.

The observer has a ZWO camera and a QHY camera, and both SDKs are installed on the development machine (paths in [local environment.txt](local%20environment.txt)). ZWO comes first. QHY follows in the same feature, because the second adapter is what shows which code the two really share.

### The two cameras

| | ZWO ASI120MM | QHY ALccd5L-II colour (a QHY5L-II-C) |
| --- | --- | --- |
| Sensor | mono | colour, Bayer mosaic |
| Frame | 1280×960, 3.75 µm pixels | 1280×960, 3.75 µm pixels |
| Sample depth | 8 bits, or 12 bits in a 16-bit word | 8 bits, or 12 bits in a 16-bit word |
| GPS | none | none |
| SDK | ASICamera2 | qhyccd; its header lists the QHY5L-II-C |

The two have the same frame size and pixel size, and between them they cover the two cases that matter: mono and colour. That makes them a good pair to build the adapters on.

At this frame size the numbers are small. A frame is 1.2 MB at 8 bits and 2.5 MB at 16. At 30 fps that is 37 or 74 MB/s, and five seconds of history is 184 or 369 MB. The larger rows of the scaling table in the project description are for other observers' cameras. The frame rates these two actually reach over USB, at 8 and at 16 bits, are measured at first light and written down here.

### What an adapter is

A camera adapter is a frame source (Feature-6). It opens one camera, applies the settings, and on its worker thread fetches each frame straight into a writable frame from its `FramePool`. Both SDKs fill a buffer the caller supplies, so a frame is not copied on the way in.

An adapter reports, in `StreamInfo` and on each frame:

- camera model and serial number, and the SDK version read at run time;
- frame size, storage format, significant bits, where those bits sit in the word, and the Bayer pattern if the sensor is colour;
- exposure, gain, offset, binning, and ROI as the camera reports them back, not as they were requested;
- what its frame time means: the host's arrival time, or a GPS-stamped exposure where the camera has one;
- whether its frame index is a hardware counter, and how dropped frames are known.

An adapter that cannot answer one of these says "unknown". It does not guess (Evidence rules 2 and 4).

Nothing outside an adapter may include a vendor header or depend on a vendor's behaviour.

### Build

- Each adapter has its own CMake switch and SDK path: `SELENEWATCH_ZWO_SDK_DIR` and `SELENEWATCH_QHY_SDK_DIR`. With a path set, the adapter is built and the SDK's DLLs are copied next to the executable after the build. With none set, the application builds and runs on synthetic and recorded sources, as it does today.
- The SDKs stay outside the repository.
- Each SDK's license and redistribution terms are recorded in `THIRD_PARTY_LICENSES.md` before its adapter is merged.

### ZWO ASI (ASICamera2 SDK)

Read from the SDK installed on the development machine:

- **License.** The SDK folder carries a permissive license text (MIT wording, copyright ZWO 2015). Using and redistributing the DLL with its notice is allowed. It is recorded when the adapter is merged.
- **Capture.** Video mode: `ASIStartVideoCapture`, then `ASIGetVideoData` with a timeout, into the pool frame.
- **Formats.** The adapter offers `RAW8` and `RAW16` only. `RGB24` and `Y8` are processed inside the SDK and are not what the sensor delivered. A colour camera's raw output is stored as the Bayer mosaic with its pattern.
- **12-bit alignment.** The project description notes that ZWO's `RAW16` puts 12-bit data in the top bits. The adapter does not rely on that note. A test on the camera (the lowest bits of a real frame are zero or they are not) confirms it, and the frame records what was found.
- **Frame index and drops.** The SDK has no per-frame counter. It reports a running count of dropped frames (`ASIGetDroppedFrames`). The adapter reads it after each frame and advances the frame index by the frames delivered plus the frames dropped since the last one, so a drop shows as a gap in the index (Evidence rule 4). `StreamInfo` says the index is derived, not a hardware counter.
- **Frame time.** The host's clock when `ASIGetVideoData` returns. That is after the exposure, the readout, and the USB transfer, and `StreamInfo` says so.
- **GPS is left for later.** Some ZWO models have a GPS module and deliver a stamped exposure time through `ASIGetVideoDataGPS`. The observer's camera is not expected to have one, and a GPS path cannot be tested without such a camera. Nothing blocks it: `StreamInfo` already says what the frame time means, and an adapter that has a GPS time would report it there. It is added when a GPS camera is in hand.
- **Controls.** Exposure, gain, offset, binning, ROI, bit depth, high-speed mode, and USB bandwidth. Changing format, binning, or ROI stops and restarts the stream, because the frame layout changes.
- The SDK's guide-port calls (`ASIPulseGuideOn`) are not used. Guiding goes through the mount (Feature-8).

### QHYCCD (qhyccd SDK)

Read from the SDK installed on the development machine (version 25.06.16):

- **License.** The SDK folder carries no license text. Its use and redistribution terms must be found and recorded before the adapter is merged. Until then the adapter is built on the development machine only and nothing of the SDK is distributed.
- **Capture.** Live mode: `SetQHYCCDStreamMode`, `BeginQHYCCDLive`, then `GetQHYCCDLiveFrame`, which is polled and reports width, height, bits, and channels with each frame.
- **Formats.** 8 and 16 bits through `SetQHYCCDBitsMode`. Where the significant bits sit is checked on the camera in the same way as for ZWO.
- **Frame index, drops, and time.** To be established from the SDK and the camera before the adapter is written: whether live mode exposes a frame counter, and what the host can know about dropped frames. Where the answer is "the SDK does not say", `StreamInfo` says unknown. The SDK has GPS support (`CAM_GPS`) and precise exposure timing (`GetQHYCCDPreciseExposureInfo`) for the models that have them; like the ZWO GPS path, that is left for later.
- The SDK brings its own runtime DLLs (USB and threading libraries). They are copied with it and listed in `THIRD_PARTY_LICENSES.md`.

**Shared code.** When the QHY adapter is written, what it repeats from the ZWO adapter (the fetch loop with its timeout and stop check, the pool, the error report, the restart on a layout change) moves into one helper. It is not designed ahead of the second adapter.

### Mono and colour

Both kinds of camera are supported, and the difference is kept to three places.

- **Stored.** A colour camera's raw output is the Bayer mosaic, stored as it arrives in Mono8 or Mono16 with its pattern recorded (Evidence rule 2). The SDKs' processed colour formats are not used.
- **Shown.** The preview debayers for the screen only (Feature-5).
- **Analysed.** The trigger compares each pixel with its own history, so it runs on the mosaic unchanged (Feature-9). The feature lock bins a mosaic 2×2 before matching, so the colour pattern does not enter the match (Feature-8).

Everything else, including the ring, the clips, and the writer, does not know whether the sensor is mono or colour.

### Camera controls in the window

The source selector lists the cameras the built adapters find. The controls an adapter reports are shown with their ranges. Values are read back from the camera after each change, and the values in force are what `StreamInfo` records. A camera that is unplugged ends the source thread with a reported error, as any source failure does.

### Dropped frames and overload, shown

The status bar shows the dropped-frame count for the session beside the frame rate. Gaps inside a clip are written into its metadata (Evidence rule 4). If the frame path cannot keep pace with the camera, the SDK drops frames and the count rises; that is the overload signal the project description asks for, and it is visible, not silent.

### Manual save

**Save now** keeps the current ring and a short interval after it, as a clip marked manual, with a line in `candidates.txt`. It costs almost nothing since clips are lists of shared frames, and it matters from the first night: it is how the observer keeps something the detector did not fire on.

### Done when

1. With no SDK path set, the application builds, runs, and passes its tests.
2. With the ZWO SDK path set and the camera connected, the camera appears as a source, streams at 8 and at 16 bits, and its frames reach the same ring, detector, and writer as a recording's.
3. The same with the QHY camera.
4. For each camera: where the significant bits sit has been checked on the hardware, and frames carry the result.
5. For each camera: a clip saved from the live stream opens in SER Player, PIPP, or Siril with the right size, depth, frame count, and times, and `event.txt` holds the camera, its settings, the SDK version, and what the frame time means.
6. Loading the machine until frames are dropped raises the dropped count on screen, and a clip saved then records the gaps.
7. Unplugging the camera while streaming is reported, and the application stays usable.
8. **Save now** writes a clip marked manual and a line in `candidates.txt`.
9. A first session on the Moon of at least an hour with each camera: the frame rate held, the frames dropped, the detector's cost per frame, the candidates logged, and the write rate are written down here.

### Manual capture implementation review — 14 November 2026

Save now is available for both existing source types. A source-thread request marks the latest acquired frame, retains the current time/byte-bounded history, and collects the configured quiet tail. Requests join an open clip without interrupting analysis, and a manual tail can continue across its cap. The record distinguishes host request UTC, original applied source UTC/index, and continuation frames. Manual marks do not invent threshold measurements or increment the automatic-candidate count; repeated requests remain separate marks. Native SER output, the shared allocation ceiling, writer refusal, partial endings, and pending/outcome logging use the existing path.

Tests cover configured history/tail duration, overlapping automatic/manual capture, continuation, pending requests at Stop/EOF/failure/loop boundaries, explicit rejection when no frame exists, writer refusal logging, and exact Mono8/low-high 12-bit Bayer/RGB pixel and timestamp round-trips. An actual offscreen window plays a constructed native recording and saves it manually with the original file unchanged. Acceptance item **8 is implemented and checked** for synthetic/recorded sources. Items involving camera adapters, hardware formats, dropped frames, SDK terms, and hour-long first-light sessions remain open. Details are in [docs/reference-and-manual-validation.md](docs/reference-and-manual-validation.md). No camera SDK/dependency was added; this feature remains an open proposal.

### Out of this step

- Guiding through a camera's ST-4 port.
- GPS-stamped frame times, until a camera with GPS is in hand.
- Cooling control, filter wheels, and long single exposures.
- Cameras other than these two. Each later family is its own adapter, added when an observer has the hardware.
- Continuous recording and the full time model (the session record stage).

### Order

After Feature-5, so the preview can show what the camera is pointed at. ZWO first, then QHY, then the shared helper. Manual save can be built at any point after Feature-7.

### ZWO implementation and hardware review — 14 November 2026

The ZWO portion is implemented behind optional `SELENEWATCH_ZWO_SDK_DIR`; without it, synthetic and recorded sources still build and run without the ASI DLL. The only vendor-header consumer is `cameras/zwo_sdk.cpp`. `ZwoSource` owns the camera and worker, fills shared-budget pool buffers directly, and uses the existing Session/detector/ring/writer. A narrow API boundary is actually used by deterministic camera tests; no shared QHY helper is introduced ahead of the second adapter. The installed SDK reports 1.41.0.0. Its MIT-worded ZWO 2015 notice and exact DLL are copied by CMake, and its source/version/obligations are recorded in `THIRD_PARTY_LICENSES.md`.

The connected device identifies as **ZWO ASI120MM Mini**, USB2, 1280×960 mono, advertised 12-bit ADC, bins 1/2. RAW8 and RAW16 work through the real Session. The Camera tab discovers devices explicitly, shows supported ranges and reads back exposure, gain, offset, raw format, bin, ROI and bandwidth. Unsupported high-speed mode is absent. Every applied change restarts acquisition and finishes the old clip, keeping settings snapshots separate; invalid geometry does not stop a healthy stream. SDK dark subtraction and flip are disabled. Raw colour binning beyond 1 remains refused pending camera-specific Bayer verification.

**Packing decision, revising the proposal's fixed-depth expectation:** bin-1 RAW16 samples had zero low four bits (`word OR 0x3ff0`), but the bin-2 sequence used lower bits (`0x7ffc`, including bits 2 and 3). Sampled zeros do not establish a padding contract. Frames therefore retain and interpret all 16 delivered word bits, low aligned, and record the advertised ADC depth separately as 12. No native bits are removed or rescaled. New SER files declare the interpretation in the standard Observer text (`SeleneWatch;layout=16L`, with the existing TEST prefix where applicable), so live and replay detector units agree. Malformed/contradictory declarations are refused; foreign/legacy inference remains available. Existing clips are unchanged.

The source records host UTC immediately after the SDK read, an accompanying steady time, and cumulative SDK losses. Its index is delivery ordinal plus observed losses, explicitly derived with approximate gap placement because the SDK buffers frames. A failed/reset drop counter preserves the just-received frame with unknown telemetry, reports failure and closes. Stop polls at 100 ms; no-frame failure allows twice the actual exposure plus five seconds. Physical USB removal is not claimed as tested.

One-minute, full-frame Release runs at 1000 µs, gain 0, offset 9 and bandwidth 80 delivered **1662 RAW8 frames at 28.020 arrival fps** and **830 RAW16 frames at 14.001 fps**, with zero SDK drops and zero image/analysis memory refusals. Charged peaks were 647.6/630.4 MB; these are payload values, not process RSS. A 640×480 bin-2 RAW16 run at gain 20/offset 12 delivered 41 frames at 14.005 fps. A one-frame Gaussian test (FWHM 4, peak 8192 delivered-word units, K=6) produced a new candidate matched to an actually brightened sample and saved marked test data. This does not establish observing sensitivity.

A manual QA-only delayed consumer induced **343 real SDK drops** in 15 seconds; its TEST clips recorded frame gaps. The window test separately verifies the on-screen drop count and saved gap metadata. Up to 20 sampled preview-frame SHA256 hashes per hardware run matched saved native pixels. Siril 1.4.4 independently read every frame of new 31-frame RAW8, 30-frame RAW16 and 43-frame overload TEST files; independent checks matched sizes, storage depths, frame counts, exact UTC trailer ticks and sidecars. Overlapping clips intentionally repeat frames; write counters measure buffered output rather than durable disk speed.

All **nine CTest targets pass in Debug and Release, with and without the SDK**, without touching physical hardware. Cases cover cleanup, readback clamps, native buffers, geometry/Bayer constraints, timeout/stop, simulated disconnect, loss/reset/telemetry failure, immutable settings across window restarts, native manual evidence and declared-layout replay decisions. Offscreen window rendering is inspected; no manual desktop interaction is claimed. Full results and limits are in [docs/zwo-validation.md](docs/zwo-validation.md) and [docs/zwo-measurements.json](docs/zwo-measurements.json).

Acceptance items **1 and the ZWO portion of 2/5/8** are checked. Item 4 is addressed by preserving all delivered bits and explicitly reporting unverified ADC packing. Item 6 has real SDK induced-loss/gap evidence plus a deterministic window-count check; a loaded native desktop session is still a field check. **QHY, physical unplug, raw-colour hardware, manual real-camera desktop use and the hour-long Moon sessions remain open**, as do Feature-9/Feature-5 observing calibration. Feature-10 remains here; no historical record is moved or condensed.

### Unattended soak implementation and review — 14 November 2026

The manual camera probe now has a soak mode with periodic manual clips and flushed progress records. It uses the normal Session/detector/ring/writer and keeps automatic capture active. It records acquisition, SDK losses/unknown state, charged payload, sampled Windows working set/private memory, writer backlog/bytes, candidates and output size. Once-per-second output/free-space checks end acquisition visibly; pending and available partial evidence finishes, so output can exceed the stop threshold by those final writes. No clip is deleted to enforce a quota. A `stop.request` file ends an unattended run gracefully. The actual command is exercised with the deterministic SDK boundary, including native RAW8/RAW16 files, sampled byte checks, periodic gaps, output limits, disconnect partial endings, invalid configuration, existing-output protection and requested stops. All **10 CTest targets pass in Debug/Release with and without the ASI SDK**.

Two 30-minute runs used full-frame 1280×960, bin 1, exposure 1000 µs, gain 0, offset 9, bandwidth 80, and the normal 5/3/10-second capture windows/2 GiB allocation ceiling. RAW8 acquired **50,417 frames** and RAW16 **25,192**, with zero SDK-reported drops and zero memory refusals. After warm-up their progress-derived rates were **28.020/14.000 fps**. All **210 native byte samples per run** matched saved pixels; 3253/1633 frames were kept in periodic clips, with successful evidence outcomes and a drained writer. Charged peaks were 292.5/296.2 MB. Ten-second process-memory snapshots can miss brief collection/write peaks and do not establish leak-free behavior; post-warm-up private usage changed by 4.5/5.6 MB. Builds/tests ran concurrently, and neither illumination nor temperature was controlled. No candidates fired; this is not a lunar sensitivity or false-positive result.

Siril independently read the first and last clips from each run, with exact size/depth/count/tick checks and unchanged SER hashes. Initial capture controls and RAW8/full-frame/bin-1 settings were read back and restored before the camera was closed. Full measurements, sampling limits and reproduction are in [docs/zwo-soak-validation.md](docs/zwo-soak-validation.md), [docs/zwo-soak-measurements.json](docs/zwo-soak-measurements.json) and [docs/camera-soak.md](docs/camera-soak.md). **Physical unplug, desktop/hardware QHY qualification and the one-hour lunar sessions remain open**; these bench runs do not close acceptance item 9. The complete proposal and previous reviews remain intact.

### QHY development draft review — 14 November 2026

The original BSD-2-Clause QHY wrapper/source/controls are prepared on `feature-10-qhy-development` in an isolated worktree. The selected Windows SDK is 25.6.16.15, confirmed by header/file/runtime version, with all 26 used exports present. No SDK-wide Windows redistribution grant was found in the installed package or inspected vendor documentation/download pages; the compatibility header's copyright/license reference does not establish DLL rights. SDK terms and exact redistribution/notice scope remain unverified and are recorded in `THIRD_PARTY_LICENSES.md`. The draft stays unmerged, off by default, loads the installed DLL in place when explicitly configured, and copies/bundles no vendor files. No vendor contact, driver installation or firmware operation was performed.

`QhySource` uses the existing Session/detector/ring/writer and a narrow API boundary implemented by the SDK and deterministic TEST device. SDK-reported buffer length is allocated from the shared pool; excess capacity remains charged, while only checked native image bytes are published. Unexpected width/height/depth/channel counts fail visibly and finish available prior evidence. RAW8/RAW16, finite/ranged exposure/gain/offset/USB-traffic/transfer-speed controls and ROI are supported as reported/read back. SDK debayer is disabled, as are reported row-noise/automatic-exposure controls. RAW16 retains all word bits with ADC depth separate. The opaque SDK device ID is separate from an unknown serial. Transfer-speed units are separate from ZWO bandwidth/high-speed controls.

The draft intentionally limits binning to 1 because no independent bin getter was established and colour/bin behavior needs hardware qualification. It labels bin 1 as commanded and checks delivered geometry, rather than presenting a request as an independent readback. ROI origin adjusts the SDK-reported Bayer phase. Host arrival UTC/steady time are recorded; exposure timing is unknown. A hardware exposure counter has not been decoded, so indices are delivery ordinals and dropped frames remain unknown in metadata, the window and diagnostics. Arrival timing does not invent loss counts or prove adjacency for two-frame confirmation.

The SDK live read has no timeout argument in the installed API. Returned generic error/not-ready states are retried under a twice-exposure-plus-five-second deadline; this cannot bound a call that blocks inside the driver. Actual Stop/removal/reconnect behavior remains a required hardware check. A shared production worker abstraction was not introduced around these different error/telemetry contracts. Concrete shared code now consists of native-unit settings editing, Bayer phase, and the ASI/QHY diagnostic loop. Source selection uses kind data rather than assuming SDK entry positions.

All **11 CTest targets pass in six configurations** (Debug/Release without SDKs, QHY alone, both SDKs), using fake APIs without opening hardware. New cases cover cleanup, buffer tails/capacity accounting, exact native bytes and Bayer origin, clamped fractional controls, not-ready/disconnect/start/layout failures, manual native evidence and unknown loss/serial semantics. An actual offscreen window changes RAW8 to RAW16/ROI/gain, checks separate evidence settings and partial endings, and shows unknown losses. The shared soak runner checks QHY Bayer clips and sampled bytes too. Native-unit editing preserves values offset from a fractional step grid. The installed SDK's version-only command and DLL export/import policy are checked independently of cameras.

The observer-assisted checklist, SDK review, commands and limitations are in [docs/qhy-development.md](docs/qhy-development.md), with [matrix results](docs/qhy-development-measurements.json). **QHY hardware, colour orientation/packing, getters, Stop/unplug/reconnect, sustained throughput and lunar acceptance remain unverified**, alongside SDK redistribution terms. This is software preparation, not completion of Feature-10 or a hardware-qualified release. The full proposal and historical reviews remain preserved.

---

### Preview and Monitoring implementation review — 14 November 2026

The observer-approved workflow separates tuning from evidence capture. **Start preview** runs acquisition, bounded history and the same detector with capture OFF; it creates neither automatic/manual clips nor candidate records. **Start monitoring** enables automatic event clips and Save now. **Stop monitoring** finishes the current available clip with `monitoring stopped`, returns to Preview and keeps acquisition running. Previously queued immutable jobs finish writing independently. **Stop stream** ends acquisition and closes the camera. A prominent banner and explicit buttons expose the current state and pending transitions. Preview diagnostic decisions are labelled **Would trigger**, rather than appearing as observing candidates. Evidence rule 5 is explicitly scoped to Monitoring; analysis remains active in both modes.

In Preview, every supported writable control currently exposed by the ZWO adapter—exposure, gain, offset, USB bandwidth and high-speed mode—can be adjusted live. A coalesced request reaches the camera worker between SDK reads; actual controls/layout are read back and copied safely to the UI. Requested values remain separate from actual quantised values. Unsupported controls are absent, flip stays disabled for raw orientation, and format/bin/ROI changes explicitly restart Preview after preflight validation. Monitoring fixes acquisition settings; display stretch and detector thresholds remain editable and each trigger retains its effective thresholds.

Camera and Detection docks are visible together. Measurements include peak sigma versus K and percentage of threshold, patch size versus bounds, compactness versus its limit, native noise, persistence, active patches, maps and recent history. Live camera edits invalidate history/reference and show estimated **Settling** before threshold feedback resumes; charged arrays are reused during repeated reference initialisation. SDK buffer age and per-frame settings association remain unknown. Consequently camera Monitoring starts through a fresh open/readback with empty history/reference; no buffered tuning frame is labelled with the new fixed evidence snapshot. The short transition interruption and initially short history are deliberate. Synthetic/recorded streams retain valid same-configuration history/reference and reconfirm patches on arming, including an already-active Preview patch and two-frame persistence.

`Session` owns mode state and processes changes on the source thread before frames or at camera wait checkpoints. Stop monitoring therefore also works during a long exposure without a hardware restart. A live readback outside Preview fails before changed frames can reach evidence, and a camera change racing a pending arm request cannot enable capture without a reopen. Preview creates no candidate journal; entry to Monitoring queues the existing session-start record. Existing QA commands explicitly start in Monitoring, preserving their unattended capture behavior.

Tests cover Preview without output, Would-trigger feedback through a real offscreen window, native live controls/readback clamps on the acquisition worker without restart, invalid geometry, history and persistence boundaries, disarm-frame exclusion, a pending-arm race, long-exposure mode changes, fixed evidence metadata, partial clips and simulated disconnects. The connected Mini additionally passed automated real-SDK RAW8/RAW16 window checks of live exposure/gain/offset/bandwidth changes, fresh Monitoring starts, manual native clips, continuing acquisition after disarming and balanced connection cleanup. High-speed mode is tested with the SDK boundary because this Mini does not expose it. Detailed build/hardware results and the limits of settling estimates are in [Preview and Monitoring](docs/preview-monitoring.md).

This refines Feature-10 without closing QHY, physical unplug, manual desktop use, raw-colour hardware or lunar observing acceptance. No dependencies or licence terms change. The complete original proposal and all previous reviews remain above; `implemented features.md` is unchanged.

### QHY runtime discovery and Preview/Monitoring integration — 15 November 2026

The QHY adapter moved to `main` with the user's installed QHY All-In-One driver and DLL as its runtime. `SELENEWATCH_QHY_SDK_DIR` supplies headers for the optional build. Automatic lookup checks the application directory, the vendor's Windows installation directory and the Desktop SDK folder. The camera panel saves an explicit DLL path and reports an unusable path until the observer changes it or restores automatic discovery. The panel and inspection probe report the loaded version and path. The build uses dynamic lookup and includes no QHY runtime, import library, drivers or firmware. Any future bundling requires verifying redistribution terms.

QHY Preview supports reported exposure, gain, offset, USB traffic and transfer speed. The source worker reads back each change and notifies the Session, which resets the detector reference and history. Changes to and from exposures of at least one second restart SDK live video on the same open connection. Layout changes reopen Preview. Monitoring starts with a fresh camera open and an empty reference and history. The Session handles Stop monitoring at the next delivered frame or when the SDK returns without a frame. Bin 1, unknown loss counts and host arrival timestamps remain the QHY evidence contract.

All 11 CTest suites passed in each of eight build configurations. The real QHY runtime probe verified discovery, override and missing or incompatible DLL diagnostics with an empty device list. A combined Release run on the ASI120MM Mini matched all 10 saved native RAW8/RAW16 frames and verified restoration of the original settings. The physical QHY camera, unplug and reconnect, native Bayer validation and lunar sessions remain open. [Runtime details and qualification checklist](docs/qhy-development.md)

---

## Feature-8 — Feature lock and mount guiding

A long session only works if the Moon stays in the field. A mount's lunar rate follows the Moon's mean motion in right ascension. It does not follow the motion in declination, which reaches about a quarter of an arcsecond per second, or half the Moon's diameter in an hour. Polar misalignment, flexure, and refraction add their own drift. Without correction the watched region walks out of the frame, and the observer has to keep nudging the mount.

This feature lets the observer lock on a lunar feature, shows how far it has drifted, and, when asked, corrects the mount with small guide pulses.

Mount control was out of scope in Feature-4 and in the README. This feature reverses that on purpose. The reason is that field drift decides how long an unattended session can run, and the measurement needed to correct it (the image shift) is the same one registration needs later.

### What shapes the design

1. **A correction is a disturbance to the detector.** It moves the image under a held reference, and edges light up in the difference. So corrections are few, slow, and recorded. Guiding keeps the feature inside a tolerance box; it does not chase the seeing.
2. **A mount command is a physical action.** SeleneWatch sends guide pulses and nothing else. A pulse ends by itself, so a crash or a lost connection cannot leave the mount moving. Slew, sync, park, tracking on or off, axis rates, and rate offsets are never sent.
3. **A wrong measurement must not move the telescope.** No command is sent on a weak match, a lost lock, or a mount state the guider does not expect. When in doubt the guider stops and says why.
4. **The measurement is useful without a mount.** The lock and the drift readout stand alone. They tell the observer when to nudge by hand, and they put the field offset on record.
5. **It is testable without hardware.** The loop is closed in tests with a drifting synthetic scene and a simulated mount.

### Part A — lock and drift readout (no mount)

**Lock.** The observer drags a rectangle on the preview, or clicks to get a default box. `FeatureLock` keeps that region as a template, with the position it was taken from. A large region is binned by a whole factor so the template is at most 96 pixels on its longer side. A Bayer mosaic is always binned by an even factor, at least 2×2, so the colour pattern does not enter the match. The preview needs a mapping from widget to frame coordinates; the blip of Feature-6 adds it.

**The region must be lockable.** A flat or saturated region has nothing to match. A straight piece of limb or terminator fixes the position across the edge and not along it. At lock time the gradients of the template are checked in both directions. A region with too little contrast, or with structure in only one direction, is refused with that reason.

**Measurement.** A few times a second the guide thread takes the latest published frame and finds the template in a search window around the last found position. The match is a zero-mean normalised cross-correlation in plain loops, which does not depend on gain, exposure, or bit depth. A parabola through the peak and its neighbours gives the sub-pixel position. A measurement is valid when the peak score is above `kMinLockScore` (starting value 0.6) and the peak is not on the edge of the window.

**Smoothing.** Seeing moves a single frame by an arcsecond or two at random. The drift estimate is the median of the valid measurements of the last few seconds. Decisions use the estimate, never one measurement.

**Lock lost.** When valid measurements stop for longer than `kLockLostSeconds` (cloud, a bumped mount, the feature out of the window), the state is "Lock lost". The search widens for a while. If the feature is found again the lock resumes. Otherwise it stays lost until the observer locks again.

**Shown.** A box on the preview at the lock position and a second one at the found position. The status line shows the offset in pixels, the score, and the state. Once a calibration exists the offset is also shown in arcseconds.

**Recorded.** `events/guiding.txt` gets one line per second while a lock is held: UTC time, frame index, offset, score, state. The writer thread appends it; the guide thread never touches the disk.

### Part B — the guiding loop, on a simulated mount

**One narrow mount type.** It connects and disconnects, reports a state (connected, tracking, slewing, parked, side of pier, whether it can pulse-guide), sends one pulse (north, south, east, or west, for a number of milliseconds), and says whether a pulse is still running. It has no other motion. Two implementations exist from the start, the simulated mount and the ASCOM adapter of Part C, so the interface is justified.

**Simulated pointing.** `SimulatedPointing` is a small object holding where the telescope points relative to the Moon. A drift rate moves it. Pulses from `SimulatedMount` move it back at a guide rate, with an optional declination backlash. The synthetic source paints the disk at that offset, turned by a camera angle. The synthetic scene gains a few fixed crater-like features, because its present texture repeats every 78 pixels and a repeating pattern cannot test a lock.

**Calibration.** Guiding needs to know which way the image moves for each pulse direction, and how far per millisecond. That is measured, not entered. With the lock held and guiding off, the guider pulses west until the feature has moved a set distance, returns east, then does the same north and south, taking up the declination backlash first. The result is two image vectors, one per mount axis. Calibration fails, with the reason, if the feature does not move, if the two vectors are far from perpendicular, or if the lock is lost on the way. The result covers camera angle, mirror flips, plate scale, and guide rate in one measurement. It is kept for the run and is tied to the side of pier it was made on.

**Control.** The rule is a tolerance box with a pause.

- While the estimate is inside `kGuideTolerancePx` on both axes, nothing is sent.
- When it leaves the box, one pulse per axis is sent, sized from the calibration to remove `kGuideGain` of the error (starting value 0.7) and never longer than `kMaxPulseMs`. One axis moves at a time.
- After a pulse the guider waits for the pulse to end, for a settle time, and for a fresh estimate, before it decides again.
- Declination is not reversed for a small overshoot. The Moon's declination drift keeps one sign for days, and each reversal costs the backlash.

The decision is a plain function of the estimate, the mount state, and the time. It has no thread and no clock of its own, so a test can step it.

**Speed of a correction.** A pulse moves the image at the mount's guide rate. At half sidereal rate and 2 arcseconds per pixel that is about 0.13 pixels per frame at 30 fps, which is less than the seeing. The calibration measures this number, and the window shows it. If it is more than `kMaxGuideSpeedPxPerFrame`, guiding refuses to start and asks for a lower guide rate on the mount.

**Nothing in guiding assumes a telescope or a camera.** SeleneWatch is meant for many observers and many combinations, so the guider is given no focal length and no pixel size. Everything it needs it measures in the calibration, and its limits are checked against those measurements. The example below shows why the limits exist; it is a worked case, not a target configuration.

**A worked example at long focal length.** With 3.75 µm pixels on a 2350 mm telescope the scale is 0.33 arcseconds per pixel and the field is 7.0 by 5.3 arcminutes. That is a high-resolution setup, dominated by the seeing. Three things follow.

- The declination drift alone, up to 0.27 arcseconds per second, is 0.8 pixels per second. A feature in the middle of the frame reaches the edge in about ten minutes. Without guiding, a session at this focal length is short.
- At half sidereal rate a pulse moves the image 23 pixels per second, 0.76 pixels per frame at 30 fps. That is too fast for the detector. The mount's guide rate has to be set near a tenth of sidereal, which gives 0.15 pixels per frame. The check above is what catches this.
- A tolerance of a few pixels would be about one arcsecond, smaller than the seeing, and the guider would chase noise. At this scale the box is tens of pixels. This is why the tolerance is better stated in arcseconds or as a fraction of the field (see the open decisions).

A shorter telescope scales all three numbers down in proportion to its focal length.

### Part C — the ASCOM adapter

ASCOM is the driver layer almost every mount on Windows provides, so it is the first real mount. The adapter talks to the installed driver through late-bound COM (`IDispatch`). The driver is found at run time by its ProgID, chosen with ASCOM's own chooser dialog. No ASCOM code is copied into the repository or linked at build time.

- The adapter reads `Connected`, `CanPulseGuide`, `Tracking`, `Slewing`, `AtPark`, `SideOfPier`, `AlignmentMode`, the guide rates, and `IsPulseGuiding`. It writes `Connected` and calls `PulseGuide`. It uses nothing else.
- Some drivers block in `PulseGuide` until the pulse ends; others return at once. Both work, because all mount calls are on the guide thread and the guider waits for `IsPulseGuiding` to clear.
- The driver object is created and used on the guide thread only (COM apartment rules).
- It is built behind its own CMake switch, like a camera adapter. Without the ASCOM Platform installed the app still builds, and the mount list shows the simulated mount only.
- The ASCOM Platform's license and redistribution position is checked and recorded in `THIRD_PARTY_LICENSES.md` before the adapter is merged.
- **Known risk:** SeleneWatch is a 64-bit program, and a 32-bit in-process driver cannot be loaded into it. Connecting to one fails with a clear message. Drivers that run as their own process are not affected. If this turns out to be common, the second adapter is ASCOM Alpaca (HTTP), which reaches any driver through ASCOM Remote.
- **On the development machine the risk does not apply.** It has ASCOM Platform 7.1, and every telescope driver installed on it (EQMOD, GS Sky, Device Hub, and the simulators) is registered as its own process. The OmniSim telescope simulator is installed and is the first target for this part; the observer's mount driver follows.

### Evidence

Guiding changes no pixel, so Evidence rules 1 to 8 stand. It adds rule 9: **mount corrections are recorded.**

- Every pulse is a line in `events/guiding.txt`: UTC time, direction, duration, the estimate that caused it, and the mount state. Calibration pulses are logged the same way and marked as calibration.
- A clip's metadata records the guiding state at the trigger (off, locked, guiding, lock lost), the field offset nearest the trigger, and every pulse that falls inside the clip's span.
- A candidate whose trigger falls inside a pulse or its settle time is marked "during mount correction" in `candidates.txt` and in the clip. It is still a candidate and is still saved (Evidence rule 5). The mark tells the reviewer that the image was moving.
- The writer does the matching. It already receives clips, candidate lines, and guide lines in order, so it compares their times when it writes. `EventCapture` and the detector know nothing about the mount.

### Threads and ownership

| Thread | Owns and runs | Must never |
| --- | --- | --- |
| Guide thread | The template, the measurements, the estimate, the guiding decision, the calibration, and the mount connection | Sit on the frame path; it takes the latest frame and skips the rest |

- The source thread offers the latest `FramePtr` through a one-slot mailbox, exactly as it does for the preview. This is why the feature needs the shared frames of Feature-7.
- A driver call that hangs stalls the guide thread only. Acquisition, detection, and the window continue. The window shows "Mount not responding" when a call exceeds a time limit.
- The window sends requests to the guide thread (lock, unlock, calibrate, guide on or off, connect) and receives status. It holds no guiding state of its own.

### Safety rules

1. Guiding is off at every start. The observer turns it on, and only with a valid lock, a connected mount, and a calibration.
2. Only guide pulses are sent, each no longer than `kMaxPulseMs`.
3. No pulse is sent unless the mount reports that it is connected, tracking, not slewing, and not parked.
4. No pulse is sent on an invalid estimate or in "Lock lost".
5. A change of side of pier ends guiding and discards the calibration.
6. If `kMaxFruitlessCorrections` corrections in a row do not reduce the error, guiding stops with "Corrections have no effect". This catches a wrong calibration, a mount that ignores pulses, and a feature that is not what was locked.
7. The total pulse time per minute is capped. Reaching the cap stops guiding.
8. Stopping the stream, a source failure, or closing the window ends guiding. Because only pulses are sent, nothing has to be undone on the mount.
9. An alt-azimuth mount (`AlignmentMode`) is refused in this step; see the open decisions.

A stop by rules 5 to 7 is not resumed automatically. The observer looks, and turns guiding on again.

### Done when

1. A test locks on a feature of the synthetic scene and moves the scene by a known sub-pixel amount: the measured offset agrees within a tenth of a pixel. The same test passes on a Mono16 scene and on one with half the brightness.
2. A test offers a flat region and a straight edge: both are refused at lock time, each with its reason.
3. A test blanks the scene for longer than `kLockLostSeconds`: the state is "Lock lost" and the guiding decision sends nothing. Restoring the scene restores the lock.
4. A test runs calibration against the simulated mount with a camera angle of 30° and a mirror flip: the two vectors match the simulation. With a mount that ignores pulses, calibration fails with its reason.
5. A test runs the stepped loop with a constant drift for a simulated hour: the feature stays inside the tolerance box, the pulse count is close to what the drift and the box predict, and declination is never reversed.
6. Tests for the safety rules: slewing, parked, not tracking, a pier flip, a mount that moves the wrong way, and the per-minute cap each stop or withhold pulses as written above.
7. A test fires a flash during a pulse: the candidate is saved and marked "during mount correction", and the clip lists the pulse.
8. By hand, with the synthetic source and the simulated mount: lock, calibrate, turn guiding on, and watch the drift readout stay in the box while `guiding.txt` fills.
9. By hand, with the ASCOM telescope simulator: connect through the chooser, calibrate, and guide. Then the same on a real mount under the Moon, with the readout and the pulse log checked against what the mount did.

### Out of this step

- Slewing to the Moon, goto, plate solving, or any pointing model.
- Rate offsets or axis-rate commands. They would remove a steady drift without pulses, but they persist after a crash; see the open decisions.
- Guiding through a camera's ST-4 port. It gives no mount state, so safety rule 3 cannot be checked.
- Alt-azimuth mounts and field rotation.
- Using the measured shift to align the detector's reference. That is registration (Stage 12). The shift is recorded so that it can be added later.
- Saving a calibration between runs. It waits for the settings page.

### Open decisions

- **Template refresh.** Shadows near the terminator change over an hour, and the score will fall. The proposed rule is to replace the template with the current patch at the matched position when the score has stayed low but valid for some minutes, and to log each refresh, since each one can shift the lock point by a fraction of a pixel. It is added only if a real session shows the score decaying.
- **Rate offsets.** If pulses turn out to disturb the detector on real data, a mount that accepts tracking-rate offsets could be steered without pulses. That needs a watchdog on the mount side or an explicit decision to accept a rate that outlives a crash.
- **Alt-azimuth mounts.** The image axes turn against the mount axes through the night, so one calibration does not last. Either recalibrate on a timer or compute the rotation. Decided when someone needs it.
- **Tolerance in arcseconds.** The starting values are in pixels. The observer's setup already shows that pixels are the wrong unit: the same box is one arcsecond on the C9.25 and several on a short telescope. The calibration gives pixels per millisecond of pulse, not arcseconds per pixel, unless the mount reports its guide rate. The choice is between arcseconds (from the reported guide rate, or a focal length the observer enters) and a fraction of the field (which needs neither). Decided in Part B.

### Order

Part A needs the shared frames of Feature-7 and real recordings to try a lock on (Feature-6), and can start once those exist. It reuses the preview coordinate mapping that the blip of Feature-6 adds. Part B follows and needs no hardware. Part C needs the ASCOM Platform and is only meaningful with a camera (Feature-10). Parts A and B can run alongside Feature-9 and Feature-5.

### Part A implementation — 15 November 2026

Part A is built. Parts B and C are not started. No mount code exists yet.

**Where it lives.** `guiding/` has one job, the lock, and depends only on frames: `FeatureLock` (template, lockability check, search), `LockTracker` (the steppable state of one lock: last found position, the median estimate, the lost-lock rule), and `Guider` (the guide thread, its newest-frame mailbox, requests, and the guiding log lines). The `Session` owns the `Guider`, beside the writer it feeds. It offers each published frame after detection and does nothing more when no lock is held or requested. The window has a **Guiding** dock (View → Guiding), lock and found boxes on the preview, and a `Lock:` readout in the status bar. It holds no lock state.

**Choosing a region.** A click already selects a pixel for the test flash and the detection probe, so it stays that way. A **drag** of more than 5 screen pixels selects a region and locks on it at once. **Lock at selection** locks a 64 × 64 box centred on the selected pixel; this is the plan's "click for a default box". A lock taken while the window shows a slightly older preview frame uses the newest frame; the feature moves far less than a pixel in that time.

**Lockability, as built.** All are measured on the binned template and reported in the panel and in `guide_lock`.
- *Saturation:* refused when more than half the region's pixels are at the stream's highest code.
- *Contrast:* the template's standard deviation divided by the pixel noise. The noise comes from a 3 × 3 operator that is a second difference across times a second difference down. Smooth shading and ramps give it almost nothing and white noise gives six times its standard deviation; the median of its magnitude is used. A first try with neighbouring-pixel differences counted crater detail as noise and refused good regions. The threshold is 3: with noise in template and frame, a perfect match then scores about 1 − 1/c² = 0.89, safely above `kMinLockScore`; a flat region measures about 1.
- *Structure:* the eigenvalues of the summed gradient outer products, less the share the noise alone contributes. Refused when the weaker is below 0.1 of the stronger. Upright and tilted straight edges are refused with that reason.
- The region is clipped to the frame and must be at least 8 × 8 binned pixels on each side, so a long thin strip is refused.

**Searching, as built.** A Bayer mosaic is binned by an even factor of at least 2. The search base on a mosaic is kept at an even offset, so each block holds the same arrangement of colour sites as the template's blocks. Each search window's mean and variance come from integral images; the cross term is a plain loop over the template.

**Starting values.** Measuring interval 0.2 s of *source* time, so a recording played fast measures as it did at the telescope, and the CPU limit only skips more frames. Estimate window 3 s, at least 3 valid matches. Track radius 24 px; after `kLockLostSeconds` = 2 s without a valid match the state becomes *lock lost, searching* with a 64 px radius around the last found position. A single match while searching could be a look-alike, so the lock resumes only when two consecutive matches agree within 3 px. After `kSearchSeconds` = 60 s of searching the state becomes *lock lost* and nothing more is measured until the observer locks again.

**Across runs.** The lock survives Stop and Start, including the camera reopen when Monitoring starts. A restarted stream, a loop boundary, or a clock that goes back starts a fresh estimate and fresh timers. A frame of another size, format or colour pattern releases the lock with that reason.

**Recorded.** `guiding.txt` sits beside `candidates.txt`. Like the candidate record, it is written only while Monitoring; Preview writes nothing. Lines carry the frame's UTC time: `guide_lock` (region, binning, contrast, structure, saturated share), `guide_refused`, `guide_state` (each change, with the reason), `guide_offset` (one per second of source time: state, region and binning, so the line stands alone when the lock was taken in Preview; latest dx, dy, score, valid; the estimate or `estimate=none`; samples) and `guide_unlock`. The writer appends them in order with its other work. A batch that cannot be written is counted in `WriterUsage::guidingWriteFailures` and shown in the Guiding dock. It is not reported as a broken candidate record.

**Tests.** `SeleneWatchGuidingTests` (CTest `guiding`) uses a scene of 360 fixed pseudo-random crater-like blobs with noise. It does not use the synthetic source's texture, which repeats every 78 px.

| Done-when item | Result |
| --- | --- |
| 1. Sub-pixel shift, Mono8 / 12-bit Mono16 / half brightness | worst error 0.020 / 0.017 / 0.041 px over five shifts up to 7.3 px (limit 0.1) |
| also: 2 × 2 binned region; RGGB mosaic | 0.015 px; worst 0.053 px |
| 2. Flat region; upright and tilted straight edge | refused for contrast; refused as one direction |
| 3. Blank longer than `kLockLostSeconds`, then restored | Searching, no estimate; restored after two agreeing matches 36 px away; *lock lost* after 60 s of searching |

The tests also cover the median rejecting one outlier, old matches leaving the window, a match on the window edge being invalid, restart and layout change, and the guider writing nothing in Preview and once per second in Monitoring. A window test drags a lock on the synthetic source, unlocks it, then locks again through the selected pixel; Preview leaves the output folder empty.

**Real recordings (manual probe).** `SeleneWatchProbe lock INPUT OUTPUT.csv [X Y W H]` locks on the first frame and follows the feature at the guider's rate, reading only the frames it measures. Without a region, it picks the most lockable 64 × 64 box of a grid. Debug build:

| Recording | Frames, span | Searches valid | Score min / median | Drift (linear fit) | Scatter about the fit | Search time |
| --- | --- | --- | --- | --- | --- | --- |
| 2015.12.19 C9.25, `Moon_180548.avi`, 8-bit | 500, 27.6 s | 124 of 124 | 0.975 / 0.988 | +1.10, −0.82 px/s | 1.24, 1.49 px RMS | 24 ms |
| 2015.12.21 8" Newtonian, `Moon_165104.ser`, 8-bit | 714, 38.7 s | 177 of 177 | 0.986 / 0.991 | −1.29, −0.44 px/s | 1.89, 2.44 px RMS | 24 ms |

Findings for Part B:
- The field drifted 1.4 px/s on both nights. At 0.33″/px that is about 0.45″/s, more than the Moon's declination drift alone. A feature in mid-frame would leave a 1296 px frame in about 8 minutes. This confirms the reason for guiding.
- Single-frame seeing scatter is 1.2–2.4 px RMS. A tolerance box has to be several times this.
- The 3 s median lags a steady drift by about half its window, here about 2 px. The control rule should allow for that lag, or estimate the rate, rather than shorten the window.
- The widened 64 px search costs about seven times a tracking search. In a Debug build that is roughly 0.2 s per search, which is acceptable on the guide thread.

Still open for Part A: the earthshine side (no recording yet), a terminator region over an hour (template refresh, see the open decisions), and a check by hand at the telescope with the ZWO camera.

### Part B implementation — 15 November 2026

**Where it lives.** `guiding/mount.h` is the narrow mount type: connect, disconnect, state, and one guide pulse. `SimulatedPointing` and `SimulatedMount` are the simulation. `Calibrator` and `GuideController` are plain state machines, stepped with a `GuideObservation` (time, lock, estimate, mount state, frame interval). Each returns a `GuideDecision`: a pulse to send, and whether to restart the estimate. Neither has a thread or a clock. `Guider` (the guide thread) owns the mount, the calibration, and both machines; it reads the mount, steps the active machine, sends its pulse, and records it. The window's Guiding dock has Mount and Guiding sections and holds no guiding state.

**The simulation.** The offset is kept in arcseconds along RA and Dec. Defaults are 0.10″/s RA and 0.25″/s Dec drift, a guide rate of 3.75″/s (a quarter of sidereal), 1″ per pixel, and a 30° camera angle. Declination backlash and a mirror flip are optional; so is a mount that ignores pulses, as a test fault. Connecting the simulated mount activates the pointing and disconnecting freezes it, so the synthetic Moon moves only while that mount is in use. The synthetic scene gained 14 fixed crater-like features (dark floor, bright rim) in a ring between 0.25 and 0.85 of the radius, clear of the centre and of the fixed flash. The disk, its texture, the craters, and the flash are painted at the pointing's offset; the pacing marker stays fixed in the frame.

**Calibration, as built.**
- Settle 1 s, then a fresh estimate of at least 8 matches.
- West in 500 ms steps until the feature has moved 25 px (at most 30 s), then a fresh estimate. East for the same pulse time, then another fresh estimate.
- The two legs share the field's own drift, so their sum gives the drift and their difference the west vector, with the drift removed.
- The backlash is taken up by pulsing north until the feature moves 4 px beyond the drift (at most 10 s). Then a north leg is measured the same way, less the drift, and the mount returns south for the north and backlash time together.
- Fails, with the reason, when a leg moves the feature less than 8 px, when the two axes are more than 20° from perpendicular, when the lock is lost, when the pier side changes, when the mount may not be pulsed, or when a pulse overruns by 10 s.
- The result records both vectors, the drift, the axis angle, the backlash time, the pier side, and arcseconds per pixel when the mount reports its declination guide rate.

**Control, as built.**
- The estimate is split along the two calibrated mount axes. The tolerance box applies to each *mount* axis, not to the image's x and y, since corrections are made per mount axis.
- When an axis component passes the tolerance, a pulse removes 70 % of it. Pulses are 20–1500 ms; RA goes first and declination after it ends.
- After the pulses, the guider waits for each to end (time and `IsPulseGuiding`), settles 1 s, restarts the estimate, and waits for a fresh one.
- A declination pulse opposite to the last one is withheld unless the error exceeds twice the tolerance.
- A correction is fruitless when the fresh error is not smaller than before; three in a row stop guiding.
- Pulse time in any minute is capped at 20 s.
- A pulse is refused when it would move the image more than 0.25 px per frame, measured from the calibration and the frame interval.

**Open decision closed: the tolerance unit.** The observer sets the tolerance as a share of the frame's shorter side (default 3 %, so 14 px on the 480-line synthetic frame and 29 px on the 960-line cameras). The dock shows it in pixels, and in arcseconds when the calibration has an image scale. This needs neither a focal length nor a guide rate. The finding from the recordings (1.2–2.4 px of seeing scatter at 0.33″/px) puts the default well above the scatter.

**Evidence, as built.** Every pulse goes to the writer as a `GuidePulseRecord` (start, duration, settle, direction, calibration or guiding). Its `guide_pulse` line in `guiding.txt` is written only while Monitoring and gives the estimate, the last match, the RA and Dec error, and the mount state. The once-per-second `guide_offset` line also carries a `GuideStateRecord`. The writer keeps 15 minutes of both and does the matching itself, as the plan says:
- a candidate whose trigger falls inside a pulse or its settle time gets `during_mount_correction=yes` in `candidates.txt` and on its `event.txt` trigger line;
- `event.txt` gains `guiding_at_first_trigger` (the nearest state record within 2 s, or "no lock record") and `mount_pulses_in_clip` with one `mount_pulse` line per pulse overlapping the clip.

`EventCapture` and the detector know nothing about the mount. Calibration and guiding starts and stops, mount connects and disconnects, and failures are also logged.

**Ownership and threads.** The mount is created by a factory on the guide thread, used only there, and released there when the guider ends. While a mount is connected, the guide thread wakes at least every 100 ms to follow pulses. It reads the mount's state every step while calibrating or guiding, and once a second otherwise. A mount call running for more than 5 s shows "Mount not responding"; the window also polls once a second, because a hung call publishes nothing. `Session::start` and `Session::stop` end calibration and guiding (safety rules 1 and 8). The lock and the calibration survive a restart; a new lock or an unlock ends guiding. Disconnecting discards the calibration.

**Tests (`guiding`, 28 cases, about 4 s).**

| Done-when item | Result |
| --- | --- |
| 4. Calibration, 30° camera angle; mirrored with 20″ backlash | west −3.236, −1.903 px/s against −3.248, −1.875 simulated; mirrored 3.250, 1.822 against 3.248, 1.875; axes within 5° of 90°; 1.0″/px recovered; backlash 6.5 s taken up (20″ at 3.75″/s = 5.3 s) |
| 4. A mount that ignores pulses | fails: "does not respond to pulses" |
| 5. One hour of constant drift, 10 px tolerance, 0.5 px seeing noise | worst 10.4 px RA, 10.9 px Dec; 222 pulses against 180 predicted; no declination reversal |
| 6. Slewing, parked, not tracking, pier flip (calibration discarded), cannot pulse-guide, alt-azimuth | each stops with its reason and sends nothing |
| 6. A mount that moves the wrong way; the per-minute cap | "corrections have no effect" after three; "cap" |
| 6. Lost lock or invalid estimate | no pulse |
| 7. A flash during a pulse | saved, marked `during_mount_correction=yes`; its clip lists the pulse and the guiding state; a later clip lists none |

Also: the simulation's pulses and backlash; a guide rate too fast for the frame rate is refused. The window test connects the simulated mount, starts a calibration, disconnects, and finds the output folder empty in Preview.

**Through the real threads (done-when 8, by tool).** `SeleneWatchProbe guide-sim OUTPUT 180` runs a Monitoring session on the synthetic source with the simulated mount and the full frame path. It locks on the most lockable 64 × 64 box, calibrates, and guides for 180 s (Debug build):
- Calibration took 46.5 s. It measured west −3.247, −1.874 px/s against −3.248, −1.875 simulated, and north 1.872, −3.257 against 1.875, −3.248; axes 89.9° apart, 0.998″/px, 0.126 px per frame.
- The lock held in all 178 one-second samples.
- From where guiding started, the field stayed within 9.8 px in RA and 2.9 px in Dec (tolerance 14.4 px). Unguided, it would have drifted 18 and 45 px.
- Each correction was one 1500 ms north pulse about every 22 s. The pulse cap limited each correction to 5.6 px, so the field was held at the edge of the box rather than brought back to centre, as a tolerance box should.
- `guiding.txt` recorded every calibration and guiding pulse.

**Finding: field motion triggers the detector on the synthetic scene.** In that run the detector fired from 2.7 s after the simulated mount connected, before any pulse, and kept firing. It produced 26 candidates in chained clips, 9 of them correctly marked `during_mount_correction=yes`. The cause is the scene: the new craters have sharp rims on a noise-free render, so the per-pixel noise there is about 1.5 ADU, and 0.27 px/s of drift is many sigma against a 2 s reference. Real frames carry seeing motion and sensor noise in the noise model, but whether steady drift and guide pulses disturb the detector on real data is exactly design point 1 of this feature. It has to be checked on the first guided lunar session, before trusting unattended candidates during guiding. If they do disturb it, the open decision on rate offsets, or registration (Stage 12), is the remedy.

### Part C implementation — 15 November 2026

`AscomMount` (`guiding/ascom_mount.cpp`) uses late-bound COM by ProgID. It reads and writes only the members listed in Part C; DISPIDs are looked up once per member. The driver is created on the guide thread, in the multithreaded apartment. Errors carry the driver's own exception text. A driver that cannot be created gets the 32-bit in-process explanation. Optional members (`SideOfPier`, `AlignmentMode`, guide rates) stay unknown when a driver does not implement them. **Choose…** opens `ASCOM.Utilities.Chooser` on the GUI thread, and the chosen ProgID is remembered. The CMake option `SELENEWATCH_WITH_ASCOM` is on by default for Windows; it needs no SDK and links only `ole32` and `oleaut32`. Without the Platform, the dock offers the simulated mount only. License position: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) (Platform CC BY-SA 3.0; nothing distributed or linked).

Checked by hand with `SeleneWatchProbe ascom ASCOM.OmniSim.Telescope OUTPUT.txt`, from a 64-bit worker thread:
- Driver created in 335 ms; the first full state read took 34 ms.
- OmniSim reports German equatorial, pier east, `CanPulseGuide`, guide rates of 0.004178°/s (1× sidereal), and tracking off.
- `PulseGuide` returned within 12 ms each time (an asynchronous driver), and `IsPulseGuiding` cleared about 390 ms after each 300 ms pulse in all four directions.
- Safety rule 3 correctly reported "not tracking", so the guider would have refused to pulse.

Still open for Part C, by hand: connecting through the chooser in the window; EQMOD with the observer's mount; and calibrate and guide under the Moon. There, check the readout and `guiding.txt` against what the mount did. With OmniSim and a camera the image does not respond to the mount, so calibration correctly fails with "does not respond".

### Guide corrections and the detector on recordings — 16 November 2026

Part B's finding asked whether field motion and guide pulses disturb the detector on real data. Before the first guided session, `SeleneWatchProbe guide-pulses` measured it on five archive recordings: mare, terminator, crater rim, the largest RGB file, and a 104 s mare-and-limb file. It moves a crop of every frame the way corrections move the image, in whole pixels, every 6 s, at 0.05 to 1 px per frame for 0.3 or 1.5 s, and compares the candidates with an unmoved control in the same windows, at K 6, 12, 18 and 24. Method, tables and limits: [docs/guide-pulse-detector.md](docs/guide-pulse-detector.md); every count: [docs/guide-pulse-measurements.json](docs/guide-pulse-measurements.json).

- **At K 12 to 24, corrections at the allowed speeds add almost nothing.** In 648 corrections, no trial had more than one candidate above its control, each extra one fell inside a window the writer marks `during_mount_correction=yes`, and no trial or control had a candidate after a window. At K 18 and 24 nothing fired after start-up, even at four times the speed limit.
- **At K 6, every recording already fires unmoved** (35 to 435 candidates after start-up). Corrections add more, almost all inside the marked windows: 114 against 27 on the mare at the strongest allowed correction. After the windows, trials stay within the control's own range, so the 1 s settle time is long enough here.
- **The 0.25 px per frame limit stays.** At 1 px per frame, the mare and the crater rim fire at K 12.
- **The patch size and shape rules carry part of the protection.** On the RGB file the peak rose to 37.8 σ during corrections (control 13.3 σ) without a candidate.
- **The synthetic finding does not carry over.** The recordings' controls, with their own drift and seeing, are quiet at K 18 and 24. The synthetic scene's sharp, noise-free rims make it far more sensitive to motion than a real frame.

So for guiding, marking is enough on this evidence, and neither rate offsets nor registration is needed. Sunlit terrain, whole-pixel steps and a simulated cadence are its limits. The first guided session under the Moon, on earthshine, is still the check of Done-when item 9.

---

## Feature-12 — An overview of every clip

*Proposed on 16 November 2026, from a discussion with the observer.*

A night can leave dozens of clips, and each has to be judged by a person: a flash on the earthshine, a plane, a hot pixel, a guide pulse. The observer needs to see at a glance what triggered, where, how it changed, and when. The clip's previews do not show that today.

- `01_baseline.png` is the last frame before the onset. `02_event_peak.png` is the frame with the strongest patch, with no mark on it.
- `03_diff_annotated.png` is that frame's difference from the background, with a red box one pixel wide around the strongest patch and its sigma in the corner. A flash patch can be 3 × 3 pixels in a 1280 × 960 frame. The box disappears in a thumbnail and is hard to find at full size.
- `01` and `02` are converted linearly over the whole sample range. On a 12- or 16-bit frame the earthshine is almost black, and a faint flash hardly shows.
- Only the strongest patch is marked. Other triggers in the same clip are not.
- No image carries the time, the frame number, or the clip's flags (during a mount correction, dropped frames, test signal).
- Each image is one moment. A flash appearing and fading, or the path of a moving object, cannot be seen.
- A dark silhouette, such as a plane, the ISS or a bird in front of the sunlit Moon, does not show in a difference that keeps only rises.

### What shapes the design

1. **One image per clip, `00_overview.png`, readable as a thumbnail and at full size.** The existing three previews stay as they are.
2. **The same layout for every clip.** The contents adapt to what happened: a flash, a moving object, a silhouette, or a manual save in which nothing is found.
3. **It is made from the clip's own frames.** A manual save, a clip that continues a long event, and the history before a trigger are covered the same way. The overview does not depend on what the detector held in memory.
4. **It is a display image, not evidence** (Evidence rule 3). The writer makes it while the clip's frames are still in memory, with its own conversion beside the writer code, and charges its buffers to the memory budget.

### The picture

```
┌───────────────────────────────────────────────────────────────┐
│  Main view: the peak frame, stretched so the earthshine       │
│  shows. Rises orange, falls blue, over the image. Each        │
│  trigger in a thick box with ticks pointing at it, numbered.  │
├──────────┬──────────┬──────────┬──────────┬──────────┬────────┤
│ before   │  −0.03 s │   0.00 s │  +0.07 s │  after   │ change │
│ (crop)   │  (crop)  │  (peak)  │  (crop)  │  (crop)  │ (peak) │
├──────────┴──────────┴──────────┴──────────┴──────────┴────────┤
│ 2026-11-16 21:43:07.512 UTC · frame 18 342 · automatic · Moon │
│ 9.8 σ · 14 px · 3 frames, 0.10 s · during mount correction    │
└───────────────────────────────────────────────────────────────┘
```

**Activity.** What changed is measured from the clip's frames with the detector's own per-pixel model: a running mean and variance with the detector's time constant, and its noise floor. The first second of the clip only learns. After that, a pixel is active on a frame when it rises or falls by at least K times its noise, with K taken from the clip's first trigger (the detector's current K for a manual save). For each frame the overview keeps how many pixels rose and fell, where they are, and how far they spread. For each pixel it keeps the largest rise and the largest fall over the clip. A slow drift of the image, unguided, is learned by the running mean as it is by the detector, so it does not paint every crater edge.

**Main view.** The peak frame is the frame with the most activity, or the detector's peak frame when there is one. It is stretched with the display's automatic stretch, so the earthshine and the sky show, and scaled to at most 1280 pixels wide.

- Pixels that rose by at least K are drawn in orange, and pixels that fell by at least K in blue. The stronger the change, the more opaque the colour, so the frame shows through. A flash is an orange dot, a meteor an orange line, a plane against the Moon a blue band with orange dots for its lights, the ISS a thin blue line.
- Each trigger is marked after scaling, so the marks keep their size: a box at least 24 pixels across with a margin around the patch, four ticks pointing at it from outside, and its number when there are several.
- In Sky mode the masked area (Feature-11) is hatched.

**Film strip.** Up to six cells, each a crop enlarged without smoothing, so single pixels stay visible.

- The cells are the frame before the activity starts, up to three frames spread over the activity (always including the peak), and the frame after it ends. The last cell is the change at the peak, rises orange and falls blue.
- Each crop is centred where the activity is on that frame, so the strip follows a moving object. A crop covers the activity's spread with a margin, and is at least 32 pixels across. For a flash every crop is in the same place, and the strip shows it appear and fade.
- Each cell is labelled with its time from the first trigger and its frame number.

**Caption.**

- The UTC time and frame number of the first trigger, the trigger kind (automatic, manual, a continuation), the mode, and the number of triggers.
- The peak in sigma, the patch size in pixels, and how many frames the activity lasted and for how long.
- For activity that moved more than a few pixels: its direction and speed in pixels per second, and in arcseconds per second when the image scale is known.
- Flags: during mount correction, dropped frames inside the clip, test signal, a clip limited by RAM.
- When no activity is found, for example in a manual save, the caption says so, and the main view is the save's frame with no marks.

### Also for recordings and old clips

`SeleneWatchProbe overview <SER file> <PNG file>` makes the same picture for any SER file, without triggers. The tests use it, and the observer can use it on archive recordings and on clips written before this feature. It writes only where it is told, never into a clip folder (Evidence rule 7).

### Evidence

The overview changes no pixel and no evidence file. It is listed in the clip's metadata like the other previews, with the frame it shows as its main view and the K it used. Unlike the other three previews, whose failure fails the clip today, an overview that cannot be made (for example when the memory budget refuses its buffers) leaves a complete clip, with the reason in its metadata. A review aid must not cost the evidence.

### Done when

1. A test fires a one-frame 3 × 3 flash on a dim Mono16 scene. The overview exists, its mark is at least 24 pixels across at full size and still visible when the image is scaled to 320 pixels wide, the strip shows the flash appear and fade in the same place, and the caption gives the UTC time, frame number and sigma.
2. A test moves a bright point across the frame. The main view shows an orange line along its whole path, the strip's crops follow it, and the caption's speed is within 10% of the true speed.
3. A test moves a dark disk across a bright scene and saves it with Save now. The main view shows a blue path, and the strip shows the disk.
4. A test saves a clip with Save now while nothing happens. The overview says that no activity was found.
5. A test drifts the scene at 1 pixel per second with no event. The main view marks almost nothing: fewer than 0.1% of the pixels.
6. A test with two separate triggers in one clip marks and numbers both.
7. A test runs the writer under a tight memory budget. The overview's buffers are charged to it, and a refusal leaves a complete clip with the failure noted.
8. The probe command makes an overview of an archive recording.
9. Measured: the time the overview adds to writing a 10 s full-frame clip, in Release, recorded here.
10. By hand: the overviews of real lunar clips, reviewed by the observer.

### Out of this step

- A browser for the clips in the window (Stage 12).
- An index of a night's overviews in one page or contact sheet.
- Animated previews.

### Order

This feature comes before Feature-11 and Feature-13, so the clips they produce are reviewed with it from the start.

### Implementation — 16 November 2026

Built. Done-when items 1 to 9 pass; item 10, the observer's review of real lunar clips, is open.

**Where it lives.** `recording/clip_overview` measures the activity and draws the picture; it knows nothing of clips or triggers. `EventWriter::writeOverview` turns a clip into an `OverviewRequest` (the first trigger's K and model, its onset, the detector's peak frame, the trigger boxes, and the caption lines) and saves `00_overview.png` after `event.ser` is closed and before the frames are released. `event.txt` gets an `overview:` line: the main frame, K, the active frames and the changed pixels, or why it was not written. `SeleneWatchProbe overview` (`tools/overview_probe`) makes the picture of any recording. No dependency was added.

**Changes from the proposal, and why.**

- **Learning.** The overview learns for the first second of the clip, or until the first trigger's onset if that comes sooner. A clip whose trigger falls in its first second would otherwise show nothing.
- **A run stays in one place.** The first picture of an archive recording (the mare file, K 6) chained the seeing on 29 frames across 3.6 s into one run and reported a motion of 76 px/s. A frame now joins a run only when its activity is within 48 pixels, or four times its spread, of the run's nearest frame; activity elsewhere in between is passed over. A motion is reported only for a run of three frames or more, because two frames can be seeing on neighbouring edges (the RGB file showed this). The run on the mare is now one frame.
- **The change cell** shows the clip's change around the peak, not the peak frame's change alone. For a flash they are the same; for a moving object it shows the path through the crop.
- **No arcseconds per second.** The writer does not know the image scale. It stays open until the scale is known to the writer (the guiding calibration, or an entered focal length).
- **Scale.** A frame narrower than 640 pixels is enlarged by a whole factor; one wider than 1280 is reduced, and each picture pixel then takes the strongest change in the block it covers, so a one-pixel flash survives.
- **Colour.** The main view is debayered as the display does. The strip averages a Bayer mosaic over its 2 × 2 cells, so the filter pattern does not show when enlarged. The activity is measured on the mosaic, as the detector measures it.

**Tests.** A new suite, `overview` (8 tests): a one-frame 3 × 3 flash on a dim Mono16 scene, marked and visible in a 320-pixel thumbnail, with the strip before, at and after it in one place (item 1); a 3 × 3 point at 90 px/s, its path coloured end to end and its speed within 10% (item 2); a dark disk on a bright scene without a trigger, reported as a fall at 120 px/s (item 3); a Save now with nothing in it (item 4); a textured scene drifting 1 px/s, under 0.1% of pixels marked (item 5); two triggers, both marked (item 6); the writer saving the overview beside the evidence; and a budget that refuses the overview's buffers, leaving a complete clip that says `overview: not written: RAM limit` (item 7). Two older writer tests now count four PNG files in a clip folder instead of three. All 13 suites pass in Debug and Release.

**Measured** (Release, Ryzen 9 9955HX, one writer thread; item 9):

| Recording | Frames | Overview |
| --- | --- | --- |
| `2015.10.26 Moon/raw/Moon_200059.avi`, 1296 × 964 Mono8 | 150 / 300 / 450 | 238 / 465 / 636 ms |
| `2012.11.25 moon 25.11.2012/raw/alccd5_00023.AVI`, 1280 × 1024 RGB888 | 300 | 574 to 657 ms |

About 1.4 ms a frame at 1.25 megapixels, so a full-frame clip of 5 s history and 10 s after the trigger at 30 fps adds about 0.6 s to its write. Item 8: the probe made the pictures above, and `SeleneWatchProbe capture` on the crater-rim recording (`2015.12.21 Moon 8 Newt`, K 6) wrote three clips, each with its overview. Their 76, 29 and 32 numbered marks, scattered along the crater rims, show at a glance that those clips are seeing.

---

## Feature-13 — A trigger for darkening

*Proposed on 16 November 2026, from a discussion with the observer.*

The detector counts a pixel only when it gets brighter. That is right for an impact flash. But a plane, the ISS, a satellite or a bird in front of the sunlit Moon is a dark silhouette, and the pixels it covers get darker. Today such a transit is never a candidate. Only its bright parts can fire: a plane's lights against the earthshine, the sunlit ISS beside the Moon, or by chance the shimmer from a plane's exhaust along a crater edge. Transits are among the events the project means to record (PROJECT_DESCRIPTION.md, Section 3), and the observer can catch an unexpected one today only with Save now.

### What shapes the design

1. **The same test with the sign reversed.** A pixel counts as dark when it falls by at least K times its own noise, measured by the same per-pixel model. Dark pixels form their own patches. A rising patch and a falling patch are never merged, even when they touch, because a moving object makes both and each is reported for what it is.
2. **Silhouettes are larger than flashes.** At the observer's 0.33″ per pixel, the ISS at 400 km is about 170 pixels across. A plane 40 km away is several hundred pixels across, and one overhead is larger than the 7′ field. A bird near the telescope is far out of focus: a large, faint, soft shadow. Dark patches have their own maximum size and box ratio.
3. **They cross fast.** The ISS crosses a 7′ field in about a tenth of a second, three or four frames at 30 fps. A plane takes from a few frames to about a second. The persistence rule stays as it is: one frame, or two consecutive frames.
4. **It is measured on recordings before it changes Moon mode.** Seeing darkens a crater edge as often as it brightens it, so the dark side may fire as often as the bright side does on sunlit terrain. Darkening is off by default in both modes until the measurement in Part C decides.

### Part A — dark patches

- A checkbox, **Also trigger on darkening**, in the Detection panel, saved per mode (Feature-11).
- Falling pixels at or above K form 8-connected patches, tested against **Minimum patch** (shared with the bright side), **Maximum dark patch** (starting value 20 000 px) and **Maximum dark box / patch** (starting value 10×).
- The persistence rule, the one-frame option, and the following of a moving patch from frame to frame are those of bright patches.
- A firing dark patch is protected from learning like a bright one, so a silhouette that stops in the field is not learned as background at once. Feature-11's release limit applies to it in Sky mode.
- The search for dark patches has its own bound, like the bright one: four times the maximum dark patch, and at least 4096 pixels.
- The trigger is recorded as `darkening`, with the size, the box, the deepest fall in sigma and in ADU, and the position of the deepest pixel.
- The Detection panel's readouts show the dark side beside the bright side: the deepest fall in sigma, and the size of the largest dark patch.

### Part B — large darkenings

A silhouette larger than the maximum dark patch, such as a plane overhead or a bird filling the field, darkens a large share of the frame. Feature-11 Part B adds a trigger for a large share that rises; this part adds the same for a share that falls, `kDarkeningShare` of the unmasked frame. It is off by default in both modes, because a cloud edge crossing the field does the same, and the first measurements decide whether it is worth having.

### Part C — measurement on recordings

The probe that measured guide corrections (`docs/guide-pulse-detector.md`) runs the same five archive recordings with darkening on, at K 6, 12, 18 and 24.

- The count of dark candidates against bright candidates, per recording and per K, after start-up.
- A synthetic silhouette injected into the same recordings: a dark ellipse at the size of the ISS and of a distant plane, crossing in 3 and in 15 frames, at several depths. The share of crossings that fire, per K.
- The result is written in `docs/darkening-trigger.md`, with every count in a JSON file beside it, and summarised here. It decides whether darkening is on by default in Moon mode, and the starting values of the dark limits.

### Evidence

Darkening changes no pixel. A dark trigger is a candidate like any other: it is logged in `candidates.txt` with its kind, saved as a clip, and described in `event.txt` with the dark limits in force (rules 5 and 8).

### Done when

1. A test darkens a 5 × 5 patch for two frames on a bright synthetic scene. With darkening on, it fires as `darkening`; with it off, nothing fires. The existing flash tests pass unchanged.
2. A test moves a dark disk 40 pixels across a bright scene at 30 pixels per frame. One clip, with its triggers recorded as `darkening`.
3. A test puts a rise and a fall side by side on one frame. Two patches, one of each kind, never merged.
4. A test darkens 30% of the frame for three frames with the large-darkening trigger on: one trigger. A darkening over 30 s fires nothing.
5. A test fills the frame with dark pixels beyond the search bound. The search stops at the bound and says so, as the bright search does.
6. The overview (Feature-12) of a dark trigger shows the path in blue.
7. Part C is measured and recorded, and the defaults are set from it.
8. By hand: a real transit, a plane or a predicted ISS pass, recorded and reviewed.

### Out of this step

- Telling a plane, a satellite, a bird and a cloud apart.
- Predicting ISS and satellite transits, and recording around a predicted time. That is continuous recording (PROJECT_DESCRIPTION.md, Section 7).

### Order

After Feature-12, so dark triggers are reviewed with the overview. Part A and its tests first, then Part C on recordings, then the defaults. Part B shares its code with Feature-11 Part B and is built after it.

### Implementation — 16 November 2026

Parts A and B are built; Part C is measured below. Done-when items 1 to 6 pass; item 8, a real transit, is open.

**Where it lives.**

- `DetectionSettings` gains `darkening`, `maxDarkPixels`, `maxDarkBoxRatio` and `darkeningShare`, saved per target. All are off by default in both modes, so the detector is unchanged unless the observer turns them on.
- `FlashDetector`'s threshold pass also collects the pixels that fall by K, with their own search bound. One patch search now serves both signs: it is given a sign's pixels, its limits and its patches from the frame before, so a rising and a falling patch are never merged and each is followed only by its own kind. Rises go through exactly the steps they went through before; every earlier detector test passes unchanged.
- A dark patch is a `ChangeKind::Fall`. Its pixel count, `peakSigma` and `peakAdu` describe the fall: its size, its depth in sigma and its (negative) difference. It is protected from learning like a bright patch, and the release limit applies to it. The clip's peak preview stays the strongest rising patch, because `03_diff_annotated.png` shows a positive difference; a clip of falls gets its picture from the overview.
- The large brightening of Feature-11 and the large darkening share one rule, with a state each. Either one holds the whole frame from learning while it lasts.
- The frame reports the dark pixels, the deepest fall, the largest dark patch, and whether the dark search was stopped at its bound. The Detection panel shows these under its darkening controls, at the bottom of the panel, rather than beside the bright readouts as proposed; that keeps the bright readouts' reserved room unchanged.
- `event.txt` and `candidates.txt` record a dark patch as `kind=darkening fall_adu=…`, a large darkening as `kind=large_darkening share=… median_difference_adu=…`, and the dark settings when they are on (`darkening=yes max_dark_pixels=… max_dark_box_ratio=…`, `darkening_share=…`). The overview marks dark patches like bright ones and names the trigger in its caption.

**Tests.** A new suite, `darkening` (5 tests):

- **One dark patch (item 1):** a 5 × 5 patch darkened for two frames fires once, as `Fall`, with its size, box and depth, only when darkening is on.
- **A crossing (items 2 and 6):** a dark disk 40 pixels across crosses at 30 px a frame and makes one clip of `Fall` triggers. Its overview reports a fall that moved, with the path in blue, and `event.txt` records `kind=darkening` and the dark settings.
- **Rise and fall together (item 3):** a rise and a fall touching on one frame are two candidates, one of each kind.
- **Large darkening (item 4):** 30% of the frame darkened for three frames fires once as `LargeFall`. A darkening of 25% over 30 s fires nothing.
- **Search bound (item 5):** 5000 dark pixels with a 100-pixel maximum dark patch stop the dark search, and the frame says so. A flash elsewhere in the frame still fires.

The window test also checks that darkening is saved per target. All 15 suites pass in Debug and Release.

**Two findings from writing the tests.**

- In a small test frame a large darkening can also pass as a dark patch: 30% of 100 × 100 is 3000 pixels, under the 20 000-pixel limit. At full frame the 5% share is about 61 000 pixels, far above the limit, so one darkening does not fire twice there.
- When half the frame darkens, the frame's noise floor, a robust median, rises with it, and a flash elsewhere is no longer at K. That is the noise floor doing its job. It means a very large darkening deafens the detector for that frame.

### Part C, measured on recordings — 16 November 2026

`SeleneWatchProbe darkening` ran the five archive recordings of `docs/guide-pulse-detector.md` with darkening on, at K 6, 12, 18 and 24. It also injected elliptical silhouettes crossing the frame: 60 × 24 and 160 × 60 px, in 3 and in 15 frames, at depths of 30%, 60% and 90%. Method, tables and limits: [docs/darkening-trigger.md](docs/darkening-trigger.md); every count: [docs/darkening-measurements.json](docs/darkening-measurements.json).

- **At K 12 and above, darkening adds nothing to an unchanged field.** No control and no injected run had a dark candidate outside the crossings.
- **At K 6, the dark side fires less than the bright side, but not never:** 1 to 170 dark candidates against 77 to 514 rising ones. That is up to half again as many on the terminator and the long recording.
- **A near-black silhouette is caught at K 12.** At 90% depth every crossing fired on four of the five recordings, at both sizes and both speeds; the long one caught 52 of 68. At 60% depth K 12 still catches most; at 30% only the RGB recording caught any.
- **The defaults it sets.** Darkening stays off in both modes, because at the default K of 6 it adds candidates and Moon mode is to behave as before. An observer who wants transits turns it on, at K 12 or more on sunlit fields. The dark limits keep 20 000 px and 10×. Large darkening stays off, with nothing yet to measure it on.

Done-when item 7 is met. The archive holds no earthshine and no real transit, so a real transit (item 8) remains the test.

---

## Feature-14 — The full session record

*Proposed on 16 November 2026, from a discussion with the observer. This is Stage 11 of PROJECT_DESCRIPTION.md.*

Three things the record still lacks. A predicted event, such as an ISS transit, a lunar occultation or a graze, can only be caught today with Save now and a few seconds of history; Section 7 plans a continuous recording for them. A clip's time is a number without the evidence of how far to trust it (Section 5). And a clip does not say where it was observed from or with what (Evidence rule 8).

### What shapes the design

1. **A recording never loses a frame silently.** It takes every frame from its start to its end. If the disk cannot keep up or fills, the recording stops at once, finishes its file, and says why (Section 6).
2. **Nothing on the frame path waits.** The recording has its own writer thread and queue, like the clip writer. The clock check runs on its own thread.
3. **Detection keeps running during a recording.** A clip written meanwhile names the recording (Section 7).
4. **Contacting the internet is the observer's choice.** The clock check against an NTP server is off until turned on.

### Part A — continuous recording

- A **Record** button beside Save now, enabled while Monitoring. Every frame from the first press to the second goes into one SER file, unchanged, with its UTC time. Stopping monitoring or the stream, or the end of a recording being played, ends it too, with that reason.
- **A scheduled recording** for predicted events: **Record from** a UTC time **for** a number of seconds. Armed while Monitoring, it starts on the first frame whose time reaches the start and ends on the first frame at the end. It goes by frame times, so it also works, and is tested, on a recording being played.
- **Files:** `events/recordings/<UTC stamp>/` holds `recording.ser`; `frames.txt`, one line per frame with its index, UTC time and any test mark; and `recording.txt`, with what `event.txt` holds for a clip. That is the source, the camera settings, the layout, what the time means, the site and instrument, and the time record. It adds the start and end, the frames written, the dropped-frame gaps, the write rate, why it ended, and `complete: yes` last.
- **Its own writer.** Frames are shared, not copied, into a queue with a byte allowance of a quarter of the RAM ceiling. When the queue is full the disk is not keeping up: the recording stops with `disk too slow`. The disk reserve (Feature-11) is checked every second; below it the recording stops with `disk reserve`.
- **A crash leaves a readable file.** The SER header's frame count is brought up to date every second and `frames.txt` is flushed, so after a crash the file can be read up to the last update; without `complete: yes` it is known to be partial.
- `candidates.txt` gets `recording_start` and `recording_stop` lines; a clip written during a recording records `during_recording:` with its name.

### Part B — the time record

- **Frame intervals.** Each clip and recording records the mean, standard deviation, smallest and largest interval between frames: from the host's monotonic clock for a camera, from the frame times for a file. It also records the largest step between the UTC and monotonic clocks over its span, which shows a clock adjusted mid-clip.
- **The PC clock.** At each Start, the Windows time service's status is read when the service runs (its source, last synchronisation and phase offset). When the observer has turned it on, the PC clock is also compared with an NTP server (SNTP: four queries, the one with the shortest round trip kept): its offset and round trip. Both go on the `session_start` line and into every clip and recording. A failure is recorded as such; nothing waits for an answer.

### Part C — site and instrument

- **Session → Site and instrument…**: observer, site name, latitude, longitude and height; telescope, aperture, focal length and pixel size, from which the image scale is shown. Saved, and written on `session_start`, in every clip and recording, and in the SER header's Observer and Telescope fields (Instrument keeps the camera).
- With the image scale known, the overview (Feature-12) gives speeds in degrees per second as well as pixels per second.

### Evidence

The recording's SER file holds the frames unchanged (rules 1 to 3); `frames.txt` holds every frame's index and time and the gaps (rule 4); `recording.txt` describes it (rule 8). Detection continues (rule 5). The time record and the site add to rule 8 what it asked for once configured.

### Done when

1. A test records while Monitoring on a constructed source: the SER file holds every frame from the press to the stop, in order, unchanged; `frames.txt` matches it; `recording.txt` ends with `complete: yes`; `candidates.txt` has the start and stop; a clip during it names it.
2. A test schedules a recording on a played file: it starts and ends on the frames whose times reach the start and the end.
3. A test slows the recording's writer until its queue fills: the recording stops with `disk too slow`, its file is complete and holds a contiguous run of frames, and the window says so.
4. A test sets the free space below the reserve: the recording stops with `disk reserve`.
5. A test stops a recording's writer without finishing: the SER file can still be read up to its last header update.
6. A test gives frames known intervals with jitter: the recorded statistics match.
7. A test runs an SNTP responder on this computer with a known offset: the measured offset is within 5 ms of it. An unanswered query is recorded as such.
8. A test saves a site and instrument: they appear on `session_start`, in `event.txt`, in `recording.txt` and in the SER header, and the overview gives a speed in degrees per second.
9. By hand: a predicted ISS transit recorded with a schedule.

### Out of this step

- A GPS time reference (Section 5).
- Recording to a format other than SER.

### Implementation — 17 November 2026

Built. Done-when items 1 to 8 pass; item 9, a predicted ISS transit recorded with a schedule, is open.

**Where it lives.**

- `recording/recording_writer`: the continuous recording. It has its own thread and a queue of shared frames with a byte allowance. The source thread hands frames over and never waits. The writer thread makes the folder and files, so the source thread does not touch the disk either. A full queue stops the recording with `disk too slow`, and the disk reserve is checked once a second; in both cases what is queued is still written and the file finished. Once a second the SER header's frame count is brought up to date (`SerWriter::updateCount`) and `frames.txt` is flushed.
- `Session`: the source thread starts, feeds and ends the recording before each frame goes to detection. The causes are a request, a schedule, the end of Monitoring, a Stop, the end of a played file or a source failure. A recording's queue allowance is the ring's (half the share left after the working reserve, halved again): a backlog longer than the ring's history is a disk that is not keeping up. The writer logs `recording_start` and `recording_stop` lines in `candidates.txt` through the clip writer, so one thread writes that file. A clip that overlaps a recording's frames records `during_recording:`.
- `recording/frame_timing`: the frame intervals of each clip and recording, from the host's monotonic clock when the source is a camera and from the frame times otherwise, with the largest UTC-minus-monotonic step.
- `clock_check`: the Windows time service's status (`w32tm /query /status`, kept as the system prints it, in its language) and the SNTP query, through Winsock, which is part of Windows; no dependency was added. The session runs the check on its own thread at each Start when the window turns it on (the window always does; the NTP part only when the observer ticks it). Each clip, recording and `session_start` line carries the last finished check.
- `observing_site.h` and `SiteDialog` (**Session → Site and instrument…**); `recording/metadata_text` holds the stream, site and time text that clips and recordings share. **Session → Schedule a recording…** sets a UTC start and a length; **Record** sits beside Save now; the capture mode line shows a running or scheduled recording.

**Changes from the proposal, and why.**

- **The SER header's Observer field keeps SeleneWatch's mark.** It already holds the sample-layout and test mark that SeleneWatch's reader parses. So the observer's name is in the text records only; the telescope goes into the Telescope field as proposed.
- **No separate `clock_check` line in `candidates.txt`.** The check finishes some seconds after Start, at a moment nothing else is written, and a line then would land between other lines at random. The `session_start` line, the clips and the recordings carry the last finished check instead. The first `session_start` after the program starts says `not made`.
- **The window message on a stopped recording** comes from the session's notification; it is checked by the window test for a recording started and stopped by the observer, and by the writer tests for the reasons.

**Tests.** A new suite, `session_record` (8 tests):

- **Schedule (items 1 and 2):** from frame 40's time for 1.5 s, on a played file. The SER file holds frames 40 to 85 unchanged, with their times; `frames.txt` matches; `recording.txt` is complete; the log has the start and the stop; a clip of a flash inside names the recording.
- **Started by the observer (item 1):** at real-time speed, started and stopped by request, complete and without gaps.
- **Slow disk (item 3):** a writer slowed to 20 ms a frame with a ten-frame allowance stops with `disk too slow`. Its file is complete and holds exactly the frames it accepted.
- **Disk reserve (item 4):** a free space of 1 GiB against a 10 GiB reserve stops the recording within a second.
- **Unfinished file (item 5):** a recording still running is opened and read up to its header's count.
- **Intervals (item 6):** known intervals, and a clock put forward by 500 ms, give the expected statistics and step.
- **SNTP (item 7):** a server on this computer 2.5 s ahead is measured within 5 ms; a silent port is recorded as no answer.
- **Site (item 8):** the site appears on `session_start`, in `event.txt`, in `recording.txt` and in the SER header, and the overview gives degrees per second.

The window test checks the site dialog, the saved site, and Record offered only while Monitoring, with the readout while it runs. All 16 suites pass.

### Later change — 17 November 2026: the recordings folder

At the observer's request, recordings can go to a folder of their own, for example a larger or faster drive: **Session → Recordings folder…**, with **Default** for `events/recordings`. The session owns the setting (`setRecordingFolder`; empty means the default) and hands it to each recording's start, so `RecordingWriter::Start` now names the folder that holds the recordings instead of the events folder. The disk reserve was already checked on the recording's own folder, so it follows the recording to its drive. A folder that cannot be written is refused when it is chosen, not when a recording starts. Clips and `candidates.txt` stay in `events`, and the `recording_start` line gains `folder="…"`. The window test records once to the default and once to a chosen folder, and refuses a folder below a file.

---

## Feature-16 — A third camera family

*Proposed on 17 November 2026, from an investigation the observer asked for. The observer chose both Player One and ToupTek the same day; see the implementation notes at the end of this section. Nobody here has a camera of either family to test with.*

The observer asked about Player One Astronomy, ToupTek, Altair Astro, SVBONY and Atik. Each was checked for what it is used for, what its SDK looks like, and the terms its SDK comes with. The terms are the license files found with each SDK in the INDI project's `indi-3rdparty` repository, read on 17 November 2026; details in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

| Family | Used for | SDK | Terms found | Fit with SeleneWatch |
| --- | --- | --- | --- | --- |
| **Player One Astronomy** | Planetary and lunar first: fast small-pixel cameras such as the Neptune and Mars series, alongside deep-sky models | C API (`POAGetCameraCount`, `POAOpenCamera`, `POAStartExposure`, `POAGetImageData`), close to ZWO's | Use for developing products with their cameras; nothing explicit on redistribution | Close to the ZWO adapter's boundary |
| **ToupTek** (also sold under other names, among them **Altair Astro**) | Planetary, lunar and deep-sky, at lower prices | C API with callbacks and pulled frames (`Toupcam_EnumV2`, `Toupcam_Open`, `Toupcam_PullImage…`); Altair's `altaircam` is the same SDK renamed (same version, `Altaircam_` for `Toupcam_`) | A generic LGPL 2.1 text only; no vendor statement | One adapter could serve several brands, each through its renamed library; a different acquisition style from ZWO and QHY |
| **SVBONY** | Entry-level planetary and guiding | C API almost identical to ZWO's (`SVBGetNumOfConnectedCameras`, `SVBStartVideoCapture`, `SVBGetVideoData`) | No license file | The simplest adapter, for cameras with a smaller share of serious lunar work |
| **Atik** | Deep-sky and scientific imaging, with larger sensors and longer exposures | C API around single exposures (`ArtemisConnect`, `ArtemisStartExposure`, `ArtemisImageReady`) | No license file; a EULA was added in the 2025.11.24 SDK, not read | A poor fit: SeleneWatch needs a continuous stream of short frames |

**Recommendation: Player One.** For lunar and planetary work it is, after ZWO, the family most often met among the cameras built for it, and QHY is already supported. Its SDK follows ZWO's model: enumerate, open, set controls, start, and pull frames with a timeout, so the adapter can follow `cameras/zwo_source` and its test boundary closely. ToupTek is the alternative if the aim is the number of brands covered rather than planetary use: one adapter for ToupTek, Altair and others, at the cost of a different acquisition model. SVBONY would be cheap to add later on the same pattern as Player One. Atik is not recommended.

### Licensing

No SDK grants redistribution clearly, so Player One would follow the QHY pattern (Feature-10): the SDK's header is used at build time from outside the repository (`SELENEWATCH_PLAYERONE_SDK_DIR`), its DLL is never committed or shipped, and SeleneWatch loads the observer's copy, with a **Choose DLL…** override as for QHY. Where Player One's Windows installation puts `PlayerOneCamera.dll`, and whether its driver package installs it at all, has to be checked with their current download before the design is fixed.

### Proposed scope

- `cameras/playerone_api.h` (the boundary, as `ZwoApi`), `playerone_sdk.cpp` (the only file that includes the vendor header), `playerone_source.cpp`, and a Camera panel through the shared `CameraSettingsEditor`.
- RAW8 and RAW16 at bin 1 and 2, with ROI; exposure, gain, offset and USB bandwidth live in Preview, fixed in Monitoring; the camera's own frame drop count when the SDK gives one.
- A fake API in the tests, like `FakeZwoApi`, so CTest needs no DLL or camera.
- The exposure follower of Feature-15 works through the shared form as it does for ZWO and QHY.

### Done when

1. With no Player One SDK path the application builds, runs and passes its tests.
2. Tests with the fake API cover discovery, the controls, Preview and Monitoring boundaries, a disconnect, and timeouts, as for ZWO.
3. With the SDK path set, the adapter builds, and the version query finds the observer's DLL.
4. By hand, with a Player One camera, by an observer who has one: RAW8 and RAW16 stream, where the significant bits sit is checked, a clip opens in SER Player or Siril with the right size, depth and times, and drops are reported.

### Open questions for the observer

- Player One or ToupTek? *Answered: both.*
- Is there a Player One owner who could run item 4? Without it the adapter would ship marked "built; hardware test pending", as QHY is now.

### Implementation — 17 November 2026

Built for both families. Items 1 and 2 pass, for Player One and for ToupTek. Item 3 passes: the observer ran `SeleneWatchRuntimeProbe` against the vendors' DLLs on 17 November 2026 (results below). Item 4 needs cameras.

**The SDKs used to build.**
- Player One Windows camera SDK 3.10.1 (`PlayerOne_Camera_SDK_Windows_V3.10.1.zip`, sha256 `428cdf88…f2e7e5`), header `include/PlayerOneCamera.h`.
- ToupTek `toupcamsdk.20261024.zip` (sha256 `8845f9d7…cfc444`), header `inc/toupcam.h`, version 60.33015.20261024.
- Both are unpacked in `C:\dev\tools\SDKs`, outside the repository. `SELENEWATCH_PLAYERONE_SDK_DIR` and `SELENEWATCH_TOUPTEK_SDK_DIR` point at them. Without them, those families are not built and do not appear.

**Where it lives.**
- `cameras/playerone_*`: the boundary (`PlayerOneApi`), the binding, and `PlayerOneSource`. They follow ZWO's: index, properties, open, initialize, config attributes for exposure, gain, offset and USB bandwidth, then bin, raw format, ROI size and origin, video mode, a wait on `POAImageReady` with a bounded read, and the SDK's dropped-frame total. Flip is turned off; binned pixels are averaged, not summed. Colour stays at bin 1, as for ZWO. ROI widths are multiples of 4 and heights of 2, as the SDK requires.
- `cameras/touptek_*`: the boundary (`ToupTekApi`), the binding, and `ToupTekSource`, handle-based like QHY's.
  - The SDK turns the image upside down on Windows by default, which would also change the Bayer cell. The adapter turns that off as soon as the camera opens.
  - Raw mode is set at 8 or 16 bits, in video mode, with flips, automatic exposure and digital binning off. Bin is 1 only, and the ROI is even with sides of at least 8.
  - Frames are read with `Toupcam_WaitImageV4` and a bounded wait.
  - The controls are exposure, gain (ToupTek's percent, 100 for unity), black level, bandwidth and speed level. The black level's range grows with the raw depth (31 at 8 bits), so the source sets the raw depth before checking the requested controls.
  - Frame indices come from the camera's own sequence numbers when the SDK reports them. Each frame advances the index by its step from the one before, so a lost frame is counted where it happened, and the 32-bit counter's wrap is carried. A repeat or a step back stops the stream, since losses could no longer be attributed. Without sequence numbers, losses are unknown, as for QHY.
- `cameras/vendor_runtime`: loading a vendor DLL that the observer installs, by absolute path, with its dependencies from its own folder and System32. It was taken out of the QHY adapter, which now uses it too, and is shared by Player One and ToupTek. Their automatic location is beside `SeleneWatch.exe`. Otherwise the observer chooses the DLL with **Choose … DLL** (`RuntimeChooser`, which the QHY panel now uses as well). *See the later change below for the other programs' copies.*
- `camera_family_panel.h`: what the window needs from a camera family: its Source entry, discovery, the requested settings, the source it makes, readbacks, and live controls. With four families, the window keeps a list of them instead of a branch per family at every place that touched a camera (about twenty). The ZWO and QHY panels implement it with their behaviour unchanged.
- `tools/runtime_probe_main.cpp`, `SeleneWatchRuntimeProbe qhy|playerone|touptek [DLL]`: loads the runtime, prints its path, version and cameras, and opens nothing.

**Tests.**
- `playerone` (5 tests): settings read back at bin 2 and both depths; invalid ROI, binning and controls, and SDK failures, all leaving the camera closed; dropped frames counted and a disconnect reported; live controls on the same connection; a manual clip whose `event.txt` names the camera and whose SER holds the native words.
- `touptek` (6 tests): raw at 16 bits with a black level only valid there; losses from sequence gaps, through the counter's wrap, and a stream stopped on a step back; invalid settings and SDK failures; not-ready and disconnect; live controls; a manual clip.
- A window test drives both families through the window: the saved DLL choice reaching the SDK, discovery, Preview, a live gain change, Monitoring reopening the camera, the drop readout, and the docks following the Source choice.
- The refactored ZWO and QHY panels pass all their earlier window tests unchanged.

**The vendors' DLLs (item 3).** Run by the observer on 17 November 2026, with no camera of these families connected. Every export each binding uses resolved, and each SDK reported its version:

| Command | Runtime loaded | SDK version | Cameras |
| --- | --- | --- | --- |
| `SeleneWatchRuntimeProbe playerone "…\PlayerOne SDK\lib\x64\PlayerOneCamera.dll"` | the SDK's x64 DLL | 3.10.1 | 0 |
| `SeleneWatchRuntimeProbe touptek "…\ToupTek SDK\win\x64\toupcam.dll"` | the SDK's x64 DLL | 60.33015.20261024 | 0 |
| `SeleneWatchRuntimeProbe qhy` | `C:\Program Files\QHYCCD\AllInOne\sdk\x64\qhyccd.dll`, by automatic discovery | 25.6.16.15 | 0 |

The last row shows that the QHY adapter, which now loads its DLL through the shared loader, still finds the installed QHY runtime.

**Not verified.** Anything with a Player One or ToupTek camera (item 4).

### Later change — 17 November 2026: other programs' copies of the DLLs

At the observer's request, the automatic search also looks where other 64-bit astronomy programs install these DLLs for themselves, as it already did for QHY's All-In-One folder. Neither vendor's own driver installs the DLL in a documented place: Player One's manual on replacing its SDK DLL tells users to copy it from the SDK into each program.

**The order.**
1. The DLL beside `SeleneWatch.exe`, always first.
2. The copies found in the folders below, the newest file version first.

The loader already skips a copy without the exports SeleneWatch needs, for example a `toupcam.dll` too old to have `Toupcam_WaitImageV4`, and says why.

| Folder, under the 64-bit Program Files | DLL | Source |
| --- | --- | --- |
| `N.I.N.A. - Nighttime Imaging 'N' Astronomy\External\x64\PlayerOne` | `PlayerOneCamera.dll` | Player One's *Manually replace the SDK* manual (revision 3.9.0, screenshots of the installed folders) |
| `N.I.N.A. - Nighttime Imaging 'N' Astronomy\External\x64\ToupTek` | `toupcam.dll` | N.I.N.A.'s `nina.external` repository (folders `x64/ToupTek` and `x64/PlayerOne`), installed as the folder above |
| each `SharpCap … (64 bit)` folder, newest name first | both | the same manual (`SharpCap 4.1 (64 bit)`); SharpCap's forum: SharpCap installs its own `toupcam.dll` |
| `Common Files\ASCOM\x64`, under `Program Files (x86)` | `toupcam.dll` | an observer's report of ToupTek's ASCOM driver (astronomie-va.com forum, November 2023); not a ToupTek statement |

32-bit programs' copies, under `Program Files (x86)`, are not looked at, since 64-bit SeleneWatch cannot load them. On the development machine, APT and PHD2 hold 32-bit copies only, so the search finds nothing there yet.

**Where it lives.** `copiesInCapturePrograms` and `newestFirst` in `cameras/vendor_runtime` (file versions read with the Windows version API), used by the Player One and ToupTek bindings. The panels, the error messages and the Camera support window say where SeleneWatch looks.

**Windows' error boxes are off while candidates are tried.** The new test's first run under CTest hung for its five minutes. Loading a file that is not a DLL had made Windows raise a modal "Bad Image" box, waiting for a click, possibly on the observer's screen. The shell used for direct runs suppresses such boxes, which is why the direct run passed. The loader now sets the thread's error mode to `SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX` while it tries candidates, and restores it afterwards. So a damaged or wrong DLL among the automatic locations, or one the observer chose, is reported with its reason, as other failures are, and never waits on a click. The test checks that the mode is set during a load, and CTest stops it after 60 s should a box ever appear again.

**Tests.** A new `vendor_runtime` suite, using a fake Program Files folder and Windows' own DLLs, so that no vendor code is loaded:
- N.I.N.A.'s and 64-bit SharpCap's copies are found, and a 32-bit SharpCap folder is passed over;
- a file with a version comes before those without, and missing files are left out;
- the loader refuses a non-DLL and a DLL lacking an export, each with its reason, takes the first usable one, and names a chosen file that is missing.

---

## Feature-17 — One remembered output folder and Preview manual recording

*Proposed on 18 November 2026, from a discussion with the observer.*

SeleneWatch currently writes clips, `candidates.txt` and `guiding.txt` into `events/` next to `SeleneWatch.exe`, with only a separate override for continuous recordings (`Session → Recordings folder…`). Placing data inside the install folder breaks when upgrading a `.zip` install, fails if installed into a read-only location, and splits outputs across directories. In addition, the **Record** button currently works only during `Monitoring`, even though observers tuning a lunar or planetary camera in `Preview` often want to capture a raw `.ser` video immediately—either continuously or for a chosen number of frames—without running impact-flash monitoring.

### What shapes the design

1. **One configurable root for everything SeleneWatch writes.** Events, continuous/manual recordings, and time-lapses live in fixed subfolders of one user-chosen folder, remembered across restarts and version upgrades.
2. **Safe Windows defaults and cloud-sync awareness.** Never default to the program directory or silently fall back to another location if an external drive is unplugged. Avoid `OneDrive`-redirected folders by default, and warn if a cloud-synced directory is chosen.
3. **Manual SER capture belongs in Preview as well as Monitoring.** Tuning planetary or sunlit lunar captures happens in Preview (Evidence rule 5 separates Preview from Monitoring's candidate log). Manual recording in Preview must produce complete, unmodified `.ser` evidence without creating candidate records.
4. **Preserve scheduled recordings and existing disk-reserve protection.** The existing UTC schedule dialog stays intact for predicted events while Monitoring, and the existing free-space reserve continues to guard every writer.

### Part A — one remembered output folder

- **Settings > Output folder:** A folder picker showing the active path and available free space on its volume. Replaces the separate `Session → Recordings folder…` override so all outputs stay under one root (removing split-folder surprises).
- **Subfolder layout:**
  ```text
  <output folder>/
      events/
          moon/<UTC>/      clips, guiding.txt, and session candidates.txt
          sky/<UTC>/       clips, guiding.txt, and session candidates.txt
      recordings/<UTC>/    manual and scheduled SER recordings (recording.ser, frames.txt, recording.txt)
      timelapses/<UTC>/    time-lapse stacks, sharpened copies, MP4/GIF, and metadata (Feature-18)
  ```
  - Splitting `events/moon/` and `events/sky/` keeps both modes separated cleanly without needing extra folder settings.
  - Putting `candidates.txt` inside each session/mode folder rather than loose at the root keeps every session folder self-contained and archivable.
  - All `<UTC>` folder and file stamps use Windows-safe formatting without colons (matching `utcStamp()`).
- **First-run default:** A local non-synced folder under the user profile (for example `%USERPROFILE%\SeleneWatch`, or `Videos\SeleneWatch` when `Pictures`/`Documents` are redirected into OneDrive). If the observer explicitly picks a folder inside `OneDrive`, `Dropbox`, or `Google Drive`, Settings warns that high-rate FITS/SER writes can saturate uploads or cause file-locking conflicts.
- **Surviving upgrades:**
  - Persist the setting outside the install directory using `QSettings()` (already configured in `main.cpp` with `SeleneWatch` / `SeleneWatch` and `SELENEWATCH_SETTINGS_NAME` test isolation), or an INI file in `%APPDATA%\SeleneWatch\` (`QStandardPaths::AppConfigLocation`) if unified across settings, with a `settingsVersion` key for future migrations and optional priority for a portable settings file placed beside the executable.
  - Never key settings by release version or executable path.
- **Missing folder, permissions, and legacy data:**
  - Before starting Monitoring, a manual/scheduled Recording, or a Time-lapse, verify that the root exists (distinguishing an unplugged external drive from a deleted subfolder on a mounted drive), is writable (`QStorageInfo` + write probe), and meets `diskReserveBytes`. If missing or unwritable, warn clearly and offer to choose another folder or recreate it—never silently fall back to another location.
  - Handle mid-session USB drive disconnects gracefully by stopping writes visibly without crashing.
  - Changing the output folder applies to new output only; existing files are never moved automatically. On first run after upgrading, if legacy `events/` folders exist next to `SeleneWatch.exe`, show a one-time notice with an option to open the old folder rather than risking slow or UAC-blocked cross-drive moves.

### Part B — manual recording in Preview with optional frame limit

- **Available in Preview and Monitoring:** The **Record** button works whenever a stream is running (`Mode::Preview` or `Mode::Monitoring`), writing raw frames via `RecordingWriter` to `<output folder>/recordings/<UTC>/` (`recording.ser`, `frames.txt`, `recording.txt`).
- **Optional frame count limit:**
  - An optional frame count control next to `Record` (for example a numeric box where `0` / blank means **Unlimited**, i.e., record until stopped).
  - When a positive count `N` is set, the recording stops automatically after `N` frames with reason `reached frame limit` and `complete: yes` in `recording.txt`.
  - When left blank / `0`, recording continues until the observer presses Stop (`Record` again), stops the stream, or hits `disk too slow` / `disk reserve`.
- **Mode transitions and scheduled recording:**
  - Keeping `Session → Schedule a recording…` unchanged preserves UTC-scheduled captures while Monitoring.
  - If a manual recording is active in Preview and the observer clicks **Start monitoring** (which reopens camera devices to flush buffered frames) or edits a stream-restarting control (ROI, pixel format, binning), the active Preview recording finishes cleanly first (`monitoring started` / `stream restarted`).
  - `recording.txt` records the session mode (`preview` vs `monitoring` and target `moon`/`sky`) and `frame_limit` when set.

### Evidence

Manual recordings in Preview preserve Evidence rules 1–4 and 8 (`recording.ser` holds raw unaltered samples with native bit depth, `frames.txt` records every index/UTC/gap, and `recording.txt` records source, layout, site, clock check, and completion). In line with Evidence rule 5, recording in Preview never starts flash detection or writes entries into `candidates.txt`; only recordings active during Monitoring appear in `candidates.txt`.

### Done when

1. Choosing an output folder in Settings persists across restarts, `.zip`/`.msi` upgrades, and `SELENEWATCH_SETTINGS_NAME` isolation, and updates `events/moon/`, `events/sky/`, `recordings/`, and `timelapses/` under that root without touching old files.
2. First-run default avoids `OneDrive`-redirected folders and warns when a cloud-synced folder is selected.
3. Attempting to start Monitoring or Recording when the configured drive/folder is missing or read-only warns before starting and never silently writes elsewhere.
4. A one-time upgrade notice appears (without moving files) when old output folders exist beside the executable.
5. Clicking **Record** in Preview writes a valid, complete `recording.ser`, `frames.txt`, and `recording.txt` with no `candidates.txt` entries created.
6. Setting an optional frame count `N` stops the recording cleanly at `N` frames (`complete: yes`), while leaving it unset records continuously until Stop or `disk reserve` / `disk too slow`.
7. Existing scheduled recordings while Monitoring continue to pass their tests unchanged.

### Out of this step

- Moving or migrating historical `events/` archives in the background.
- Compressed video formats (MP4/GIF) for raw manual recordings (manual recordings stay lossless SER; time-lapse video encoding is Feature-18).

### Order

Feature-17 comes before Feature-18 so Time-lapse builds directly on the unified `<output folder>/timelapses/` root and updated `Session` output ownership.

---

### Feature-17 status & verification
- **Status:** Implemented (`recording/output_paths.h/.cpp`, `recording/event_writer.h/.cpp`, `recording/recording_writer.h/.cpp`, `main.cpp`, `mainwindow.h/.cpp`, `session.h/.cpp`).
- **Automated tests:** `core`, `session_record`, `display_capture`, `manual_capture`, `zwo`, `ui` (`outputFolderSafetyAndMigration` testing custom root persistence, `isCloudSyncedPath`, missing drive/folder refusal without fallback, legacy `events/` notice isolation, and Preview manual `Record` with optional frame count).

---

## Feature-18 — Time-lapse mode

*Proposed on 18 November 2026, from a discussion with the observer.*

Observers want to record multi-minute to multi-hour sessions of the Moon (e.g., crater shadows advancing along the terminator) or a planet (e.g., Jupiter rotating) and turn them into smooth, sharp animations. Impact-flash monitoring cannot serve this well: flash monitoring uses long exposures and high gain for faint earthshine, whereas bright lunar/planetary time-lapse capture uses short exposures, lower gain, high frame rates, and multi-frame registration/stacking.

### What shapes the design

1. **A separate mode, not part of Monitoring.** Time-lapse runs as a third `Session::Mode` alongside `Preview` and `Monitoring`. Impact-flash detection is off while it runs, keeping Monitoring's "never go blind" rule untouched.
2. **Bounded per-period frame selection and stacking (100–200 frames max per period).** Each period collects up to 100–200 frames max (selecting the sharpest `N%` from the period's capture window), keeping RAM usage, alignment latency, and `MemoryBudget` impact predictable for both lunar and planetary targets.
3. **Fixed-cadence parallel pipeline with overload protection.** Waiting sequentially *after* processing causes cadence drift (`interval + window + T_proc`), making planetary rotation or shadow movement stutter. Instead, acquisition/collection runs on fixed period timestamps while heavy alignment/stacking/FITS writing runs in parallel on a worker thread behind a **depth-1 pending queue** with a **pre-capture overload skip gate**.
4. **Scientific preservation and non-destructive re-sharpening.** Every finished stack is saved immediately as **linear 32-bit float FITS** (both unsharpened `stacks/` and sharpened `sharpened/`), so a crash or early stop never loses data and the entire sequence can be re-sharpened and re-encoded later without touching the unsharpened data.
5. **Respect existing SeleneWatch protections and compatible third-party packaging.** Honors `CaptureSettings::diskReserveBytes` (`freeBytesAt()`), integrates with `MemoryBudget` and `FeatureLock` / ASCOM guiding, and uses compatible permissive/LGPL libraries (**OpenCV**, **CFITSIO**, and **LGPL FFmpeg**) dynamically linked and shipped with their license texts alongside `SeleneWatch.exe` (just like ZWO's DLL).

### User flow and controls

1. **Set up in Preview:** Focus, frame, tune exposure/gain/ROI, inspect histogram, and optionally enable `FeatureLock` and ASCOM guiding.
2. **Configure Time-lapse panel:**
   - **Target preset:** `Moon` or `Planet` (fills editable defaults).
   - **Cadence:** `Interval` (default `60 s`) and `Capture window` (default `15 s`, validated `captureWindow <= interval`), showing the expected frame count at current FPS.
   - **Keep best & cap:** Percentage of sharpest frames to stack (Moon default `25%`, Planet default `15%`), capped at **100–200 frames max per period**.
   - **Region & Lunar constant anchor:** Full frame or crop around the locked feature; for `Moon`, an **optional user-selected constant sunlit area** (away from the terminator where shadows will not shift significantly over the session) to anchor multi-hour rotation/translation alignment.
   - **Sharpening:** `Off`, `Mild` (default), `Medium`, or a saved named profile.
   - **Stop condition:** Fixed duration, explicit date + UTC end time (with clear rollover preview), stack count, or "until I press Stop".
   - **Output options:** `MP4` and/or `GIF`, animation frame rate (`10–15 fps`, default `12–15 fps`), and optional raw `SER` burst saving (off by default, displaying estimated GB/hr).
3. **Start / Stop:** Pressing Stop early (or hitting a stop condition / low-disk stop) cleanly finishes any active stack and builds the animation from all completed stacks.

### Live feedback while running

- Progress bar (elapsed/remaining time for bounded runs, elapsed for manual stop), stacks completed, and countdown to next capture period.
- **Latest stack thumbnail** beside the live preview so improvement over raw frames is immediately visible.
- **Mini-filmstrip / live loop preview** backed by lightweight downscaled (≤512 px) 8-bit thumbnails of completed stacks.
- **Per-stack telemetry:** Frames captured, frames kept, brightness-normalized quality score, shift `(dx, dy)` and rotation angle.
- **Warnings:** Guiding lost, sudden cloud brightness/quality drop, processing falling behind the interval (period skipped), and low disk space approaching `diskReserveBytes`.

### Fixed-cadence parallel pipeline and overload protection

- **Cadence clock:** Periods open on fixed timestamps `t_k = t_0 + k × interval` (`steady_clock` for live cameras; source frame time during recorded playback).
- **Collector (source-side / lightweight):** During `[t_k, t_k + captureWindow)`, copies (and crops if ROI is active) incoming frames out of `FramePtr` immediately so `FramePool` slots are never held and acquisition never stalls. Frames are scored on arrival so at most the top `100–200` frames are retained for the period. At `t_k + captureWindow`, the burst (`BurstJob`) is handed to the stacker worker.
- **Stacker worker & depth-1 queue:** A dedicated worker thread processes one active `BurstJob` at a time behind a **single-slot (depth-1) pending queue**. Processing overlaps cleanly with idle time and even the next capture window as long as average processing time `T_proc <= interval` (only `captureWindow <= interval` is required, not `captureWindow + T_proc <= interval`).
- **Pre-capture overload skip gate:** Before opening period `k` at `t_k`, check:
  1. Is the depth-1 pending slot still occupied (worker has not yet started the previous queued burst), **or**
  2. Would admitting another burst exceed `MemoryBudget`?
  If either is true, **skip period `k` immediately** without collecting frames, increment the skipped counter, log the timestamp and reason (`processing overload` / `memory limit`) in `timelapse.txt`, and surface a warning in the UI (suggesting larger interval, tighter ROI, or lower frame cap if skips repeat). As a final safety backstop, if the pending slot or `MemoryBudget` rejects at window end, the newly collected burst is dropped non-blockingly and logged without stalling acquisition or interrupting the active stack.
- **Existing disk-reserve protection:** Before starting and before writing each stack (or optional SER burst), check `freeBytesAt(outputRoot)` against `CaptureSettings::diskReserveBytes`. If free space falls below the reserve, stop capture cleanly with reason `disk reserve` and immediately assemble the video from all completed stacks.

### Per-period image processing

1. **Color handling (v1):** If the stream is Bayer color, debayer frames before scoring and alignment; use luminance/green channel for quality scoring and alignment transforms. (Raw Bayer stacking / Bayer-drizzle stays out of v1.)
2. **Quality scoring & cloud tracking:** Measure sharpness on the illuminated target ROI (not dark sky) using a mildly smoothed gradient/Laplacian-of-Gaussian normalized by mean target intensity, so high-gain noise and thin cloud dimming do not distort sharpness ranks. Track mean target brightness separately to flag cloud-affected frames/stacks.
3. **Within-burst alignment & stacking:**
   - Two-pass apodized (Hann-windowed, mean-subtracted) subpixel phase correlation: align the sharpest subset to build a low-noise temporary mean reference, then align all kept frames (`<= 200`) to that reference.
   - Resample with bicubic/Lanczos interpolation and stack using floating-point sigma-clipped (or trimmed) averaging to remove hot pixels and brief transits, cropping/tracking valid pixel coverage cleanly.
4. **Inter-stack session alignment:**
   - **Planet:** Align finished stacks across the session using **planetary disc centroid / limb fit** so rotating surface features (e.g., Jupiter's belts / Great Red Spot) do not pull the planet across the frame over hours.
   - **Moon:** Best-effort **2D rigid alignment (translation + rotation)** against the session reference (handling Alt-Az field rotation and polar drift), prioritizing the **user-selected constant sunlit anchor region** when defined (or full lit region / sequential local keyframe fallback as shadows evolve).
5. **Sharpening & linear 32-bit float FITS output:**
   - Write **both** the unsharpened stack (`stacks/<idx>_<UTC>.fits`) and the sharpened copy (`sharpened/<idx>_<UTC>.fits`) immediately as **linear 32-bit IEEE float FITS** (`NAXIS=2` mono or plane-sequential `NAXIS=3` RGB).
   - Headers carry standard FITS keywords: `DATE-OBS`, `DATE-AVG`, `DATE-END`, `EXPTIME`, `GAIN`, camera/site/instrument metadata, frames captured and kept, quality score, shift `(dx, dy)` and rotation angle, and sharpening profile (`none` for `stacks/`).
   - **Limits to respect:** Warn in the UI if the Planet capture window exceeds ~60–120 s (planetary rotation smearing) or if expected kept frames per period at current FPS fall too low (e.g., `< 10` frames).

### End-of-session video assembly & Re-sharpen window

```text
<output folder>/timelapses/<UTC start>/
    stacks/                 0001_<UTC>.fits, 0002_<UTC>.fits ...  (unsharpened, linear 32-bit float)
    sharpened/              0001_<UTC>.fits ...                   (linear sharpened, current profile)
    sharpening_profile.json
    timelapse.mp4
    timelapse.gif           (optional)
    timelapse.txt           (settings, skipped periods, UTC times, per-stack stats)
```

- **Video encoding:**
  - Optionally filter out cloud-ruined stacks below a quality threshold, crop to the common valid overlap (rounded to even width/height for H.264 `yuv420p`), apply **target-masked multiplicative brightness levelling** (measuring foreground target intensity rather than whole-frame mean), and apply a **consistent session black/white stretch + gamma** solely during 8-bit video export (never altering the linear FITS files).
  - Encode high-quality `MP4` (low CRF / high bitrate so crater rims aren't crushed); when `GIF` is enabled, downscale to a safe maximum width (e.g., ≤800 px) with palette optimization so multi-hundred-frame GIFs remain practical.
- **Re-sharpen / Rebuild window:**
  - Opens any existing session folder, defaulting its preview to the **highest-quality stack** in `stacks/` (with any stack selectable).
  - Live before/after tuning split into:
    - **Linear sharpening:** method (`Unsharp Mask` in v1; wavelet layers later), radius, amount, noise threshold/floor, and optional slight denoise (applied to FITS + video).
    - **Video export & tone-mapping:** black/white point stretch, gamma, brightness levelling, quality-drop threshold, crop, and FPS (applied to MP4/GIF only).
  - Save/load named reusable profiles (sanitizing profile names for Windows paths) and optionally set a default profile for new sessions.
  - **Apply to all** writes non-destructively into a new `sharpened_<profile>_<UTC>/` folder (never overwriting `stacks/` or earlier runs), and provides a fast **"Rebuild video only"** action when only video/tone settings change.

### Architecture and dependencies

- All added dependencies follow [AGENTS.md](AGENTS.md), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), and [packaging/package.cmake](packaging/package.cmake), dynamically linked and distributed with their required license texts beside `SeleneWatch.exe`:
  - **OpenCV 4.14.0 (`Apache-2.0`):** `phaseCorrelate` (Hanning-windowed subpixel translation), `findTransformECC` / `estimateAffinePartial2D` (2D rigid translation + rotation alignment), `warpAffine` (Lanczos-4 / bicubic resampling), edge-aware Bayer debayering (`cvtColor`), LoG/Sobel quality metrics, and unsharp mask / float image processing.
  - **CFITSIO 4.7.0 (`NASA / BSD-like permissive`):** Reading and writing standard 2D (`NAXIS=2`) and plane-sequential RGB (`NAXIS=3`) 32-bit float (`BITPIX=-32`) FITS stacks with strict 2880-byte formatting and full astronomical headers (`DATE-OBS`, `DATE-AVG`, `DATE-END`, `EXPTIME`, `GAIN`, site/instrument, alignment, and sharpening metadata), while also allowing Re-sharpen to open external FITS stacks from tools like Siril or PixInsight.
  - **FFmpeg 9.0 (`win64-lgpl-shared`, `LGPL` without `--enable-gpl`):** High-quality `MP4` encoding (e.g., `libopenh264` / `h264_mf` via `libavcodec` / `libavformat` / `libswscale` or LGPL `ffmpeg.exe`) and two-pass palette-optimized `GIF` generation (`palettegen` / `paletteuse`) with no GPL components (`libx264` excluded).

### Evidence

Time-lapse never modifies raw frames from the `FramePool`, writes strictly linear unaltered float averages to `stacks/*.fits`, preserves full provenance and UTC timing in FITS headers and `timelapse.txt`, and keeps video stretch/brightness-levelling separate from FITS pixel data. Raw SER bursts remain available as an opt-in lossless archive setting.

### Done when

1. Starting Time-lapse runs independently of Monitoring (flash detector off) on synthetic, recorded, and camera streams, and stopping early or hitting `diskReserveBytes` still produces a playable video from completed stacks.
2. Each period enforces the `100–200` max kept frame cap and stream-scores frames without holding `FramePool` buffers or exceeding `MemoryBudget`.
3. Under an artificially slowed stacker (`T_proc > interval`), the pre-capture skip gate prevents queue/RAM growth, logs skipped periods in `timelapse.txt`, and triggers the overload UI warning.
4. Synthetic moving/rotating lunar test sequences align cleanly using rigid translation + rotation around a selected anchor region, while rotating synthetic planetary sequences stay centered via disc centroid alignment.
5. Output `stacks/*.fits` and `sharpened/*.fits` (both Mono 2D and RGB 3D) read back validly in tests and open cleanly in standard astronomical tools (such as Siril), with linear pixel values preserved and full FITS headers written.
6. End-of-session and Re-sharpen/Rebuild produce valid `MP4` (even dimensions, consistent stretch, target-masked brightness levelling) and `GIF` outputs into non-destructive `sharpened_<profile>_<UTC>/` folders without overwriting `stacks/`.

### Out of this step

- Multi-point (local/de-warping) lunar alignment and planetary surface map derotation (WinJUPOS-style).
- Raw Bayer-drizzle stacking without prior debayering.
- Wavelet layer sharpening in v1 (architecture leaves method extensible from Unsharp Mask to wavelets).
- Automatic PNG copies of every stack (optional later export).

### Order

After Feature-17 (which provides `<output folder>/timelapses/` and the unified output settings). Internal build order: FITS reader/writer (CFITSIO) & OpenCV stacking/alignment pipeline (tested synthetically) → Time-lapse session controller & parallel overload gate → Time-lapse UI & live filmstrip → FFmpeg MP4 / GIF encoder → Re-sharpen / Rebuild window.

### Feature-18 status & verification

- **Status:** Implemented (`timelapse/timelapse_settings.h/.cpp`, `timelapse/fits_io.h/.cpp`, `timelapse/stack_processor.h/.cpp`, `timelapse/video_exporter.h/.cpp`, `timelapse/timelapse_session.h/.cpp`, `timelapse_panel.h/.cpp`, `resharpen_dialog.h/.cpp`, `session.h/.cpp`, `mainwindow.h/.cpp`, `preview_widget.h/.cpp`).
- **Automated tests:** `timelapse` (`SeleneWatchTimelapseTests`, 11 test cases covering CFITSIO 2D/3D `BITPIX=-32` FITS + metadata & external 16-bit integer FITS loading, OpenCV Bayer RGGB/BGGR demosaic + brightness-normalized sharpness, 2-pass phase correlation + sigma clipping, Planet centroid + Moon rigid translation/rotation with `lunarAnchor` against moving shadows, linear sharpening, `100–200` per-period cap + parallel pipeline without skips, pre-capture overload skip gate + `MemoryBudget`, `diskReserveBytes` stop-and-build, `Session::startTimeLapse` MP4/GIF export, and non-destructive `resharpenTimelapseSession` + `rebuildVideoOnly`) and `ui` (`timelapsePanelAndResharpenWorkflow`).
- **Pending field verification:** overnight Moon and Planet hardware capture at the telescope.

---

## Feature-22 — Separate Moon and Sky controls

*Proposed on 21 November 2026, from the observer's first nights with the program on a laptop.*

The Detection panel shows **Also trigger on darkening** and **Large darkening** in Sky mode. Feature-13 made them for silhouettes against the sunlit Moon: against a night sky an object has nothing to darken. A control that does nothing useful in a mode, or that a value saved earlier keeps active without being seen, misleads the observer.

### What shapes the design

1. **Each target shows only the controls that act on it.** Sky-only controls (horizon mask, large brightening, release) already sit in their own group shown only in Sky mode. Moon-only ones get the same treatment.
2. **A hidden control is also off.** In Sky mode the detector runs with darkening off and **Large darkening** at zero, whatever an older saved Sky setting says. The Moon's darkening settings are untouched.
3. **Audit every dock.** Guiding is already disabled in Sky mode. The other docks are checked for controls that only make sense for one target.

### Done when

1. In Sky mode the darkening controls are hidden and the detector never fires on darkening, even with darkening saved on for Sky by an older version.
2. In Moon mode the controls and their saved values are as before.
3. A UI test switches between targets and checks which controls are shown and which settings reach the detector.


### Feature-22 status & verification

- **Implemented** in `detection_panel.cpp`: the darkening check box and its limits are hidden in Sky mode, and `DetectionPanel::settings()` gives darkening off and **Large darkening** zero there whatever was saved. The other docks were checked: Guiding is already disabled in Sky mode ("Guiding (Moon only)") and the lunar anchor is cleared; no other control acts on one target only.
- **Test:** `UiTest::darkeningIsForTheMoonOnly`.

---

## Feature-23 — A window that fits a small laptop screen

*Proposed on 21 November 2026.*

On a 1920 × 1200 laptop at 125 % scaling (1536 × 960 logical pixels) the text of the controls above the preview is squeezed: the row of buttons, frame-rate, speed and loop controls is wider than the space beside the docks.

### What shapes the design

1. **Wrap instead of squeeze.** The controls above the preview flow onto a second line when the window is narrow, and stay on one line when it is wide. Controls that belong together (a label and its box, Record and its frame limit) stay together.
2. **Measured, not eyeballed.** A UI test opens the window at 1366 × 768 and 1536 × 960, shows each dock tab in turn, and fails if any button, label, box or list is drawn smaller than its text needs.

### Done when

1. The test passes at both sizes with the testing tools on (the most controls).
2. On a wide screen the controls take the same single line as before.


### Feature-23 status & verification

- **Implemented:** `FlowLayout` (`flow_layout.h/.cpp`) wraps the stream buttons, Record, frame rate, speed and loop controls, and the test-flash controls; `MainWindow::arrangeTopControls()` groups controls that belong together. The recording name shortens with "…" instead of widening its row.
- **Measured cause:** the single row of controls needed 1082 px, the right-hand docks about 380 px and a camera dock about 300 px: about 1780 px against 1536 px on the observer's laptop.
- **Test:** `UiTest::everyControlFitsOnASmallLaptopScreen` (1366 × 700 and 1536 × 880, camera chosen, testing tools on, every dock tab). **Pending:** a look on the laptop itself.
- **Width (observer's report, 21 November 2026):** the window could be resized in height but hardly in width. Before this feature its minimum width was 1484 px on a 1536 px screen. With the stream controls wrapping it was 1270 px; the source row (Source, Connect at startup, Open…, the recording name, the testing badge) now wraps too, the recording name taking the rest of its line, and the minimum is 1108 px for either target. `FlowLayout` gives a line's spare width to its first expanding item and narrows an item wider than the line down to its minimum. The UI test fails if the window cannot be narrower than 1150 px. The real-window test (`tests\gui_e2e_test.ps1`) passed with these changes.

---

## Feature-24 — Help on every control

*Proposed on 21 November 2026.*

Many controls have tooltips, but not all, and a tooltip is easy to miss. The observer wants to click something next to a control's name to read what it affects.

### What shapes the design

1. **One text per control.** The same text is the tooltip and the clickable help, so the two never disagree.
2. **No extra column.** A small ⓘ at the end of the control's label opens the help; a separate button on every row would take the width that Feature-23 needs.
3. **A test keeps it complete.** Every input control in the window and docks has help text; a new control without it fails the UI test.

### Done when

1. Every input control has a tooltip, and labelled controls in the docks show a clickable ⓘ that opens the same text.
2. The UI test fails when a control without help is added.


### Feature-24 status & verification

- **Implemented:** `control_help.h/.cpp`. `installControlHelp()` copies each tool tip to What's This and adds a clickable ⓘ (in Segoe UI Symbol, which every Windows has; Segoe UI lacks the glyph) to the label naming a control: form row labels and labels with a buddy. **Help → What's This?** (`Shift+F1`) covers check boxes and buttons. About 75 controls that had no tool tip got one; camera controls get theirs from the name their SDK gives them.
- **Test:** `UiTest::everyControlHasHelp` fails for any control in the window or its docks without help, for both targets with a camera chosen.

---

## Feature-25 — Sliders for camera settings

*Proposed on 21 November 2026.*

Gain, offset and exposure are adjusted by eye while watching the preview and the histogram, which a slider suits better than typing.

### What shapes the design

1. **Slider beside the exact value.** Every writable camera control with a range of at least ten steps gets a slider in the same row as its box; the box stays for exact entry. On/off and few-valued controls keep only the box.
2. **Logarithmic when the range is wide.** A control whose range spans more than three decades (exposure: microseconds to seconds) gets a logarithmic slider, so short exposures are not crowded into the first pixel.
3. **Same live rules as the box.** In Preview a change reaches the camera after the existing short pause (a drag sends one request when it pauses, not one per pixel). During Monitoring the camera settings stay locked, as now.
4. **One place for all camera families.** The slider lives in the shared camera settings form, so ZWO, QHY, Player One and ToupTek all get it.

### Done when

1. Gain, offset and exposure have sliders for each camera family (checked with the fake cameras); moving one changes the box and reaches the camera once after the pause in Preview.
2. The exposure slider is logarithmic, and its ends are the camera's minimum and maximum.


### Feature-25 status & verification

- **Implemented** in `CameraSettingsEditor`, shared by all four camera families: a writable control with at least ten steps has a slider beside its box (one position per step up to 1000), logarithmic when its range spans more than three decades. The existing 180 ms pause sends one live request per drag pause.
- **Test:** `UiTest::cameraSlidersSetTheBoxAndReachTheCameraOnce`. **Pending:** a drag with a real camera in Preview.

---

## Feature-26 — Auto-tune and suggested camera settings

*Proposed on 21 November 2026. Designed and revised the same day, after the observer's decisions and two design reviews (all below). Implemented on 21 November 2026 at the observer's request; see the status section at the end.*

Tonight's settings are found by hand: the observer reads the histogram, compares it with [CAMERA_SETTINGS_GUIDE.md](CAMERA_SETTINGS_GUIDE.md), clicks pixels to read their noise, injects test flashes and adjusts K until Preview stops saying "Would trigger" on nothing. Auto-tune does that measurement for them. It watches the Preview for a short or a longer time and proposes camera and detection settings, separately for the Moon and the sky. The observer accepts the suggestions, or some of them, or none.

### What the observer does

Auto-tune has two steps, because a K measured with one exposure and gain is no longer valid after either changes: **first the camera, then K**.

1. Start Preview with a camera (or a recording: K only, see design point 10) and choose the target, the Moon or the sky.
2. Press **Auto-tune…**. It is a prominent full-width button at the top of the Detection dock, above Profile, visible without scrolling. A panel opens beside the preview; it is not a modal dialog, so the preview stays live.
3. **Where to measure:**
   - **Moon, impact flashes:** drag a box on the earthshine. Auto-tune then finds the whole earthshine area that the box belongs to and outlines it on the preview, so the observer can see what it will measure (design point 3). The box is remembered for the next run.
   - **Moon, transits (darkening on):** the brightest sunlit areas of the whole frame are measured, so no box is needed.
   - **Sky:** the sky outside the mask is measured, so there is nothing to draw.
4. **Step 1, camera.** Press **Measure camera** (a few seconds is enough for brightness). The panel shows the measured level and its aim, then the suggested exposure, gain, and the follower's limits (Sky), each with a tick box, plus any warnings. **Apply** sends the ticked rows to the camera together. The panel then waits until the camera reports the new values back and the detector has settled, and measures again to check: the brightness should now be in the aim. When it is, or when the camera cannot reach it (the panel says why), step 2 is offered.
5. **Step 2, K.** Choose **Quick (15 s)** or **Thorough (60 s)** and press **Watch**. Several real detectors run on the same frames at different K values (design point 4). The panel then shows a table of what each K would have done:

   | K | Events in 60 s | Per-pixel threshold in the earthshine |
   | --- | --- | --- |
   | 6.0 (now) | 4 | 31 ADU |
   | 7.0 | 1 | 36 ADU |
   | **8.0 (suggested)** | **0** | **41 ADU** |
   | 10.0 | 0 | 52 ADU |

   The table also lists the events found, each with its time, position, size and the K values that found it. An event that was real (a meteor, a plane, a satellite) can be unticked; it is then not counted, and one plane lasting many frames counts once. **Apply K** sets the ticked K in the Detection panel.
6. **Close** changes nothing that was not applied.

### What shapes the design

1. **Suggest, never impose.**
   - Nothing changes until the observer applies it, and only the ticked rows change.
   - Auto-tune runs only in Preview. Its button and Apply are disabled while Monitoring, like the camera settings, including when Watch from dusk to dawn starts Monitoring while the panel is open: that cancels a watch in progress.
   - During Monitoring the only thing that changes camera settings is the exposure follower (Feature-15), under its own fixed rules.

2. **Different aims for each target and workflow.** The aims are the ones [CAMERA_SETTINGS_GUIDE.md](CAMERA_SETTINGS_GUIDE.md) already gives, kept in one place in the code so the guide, the help and the suggestions agree:

   | Watching | What is measured | Aim | How brightness is corrected |
   | --- | --- | --- | --- |
   | **The Moon**, impact flashes (darkening off) | Median of the earthshine area (design point 3) | **3–10 %** of full scale, aiming at 6 %; nothing in the area at 0 | Gain. The exposure stays between 10 and 40 ms, because a flash lasts 15–100 ms and a longer exposure adds background but no flash light. An exposure outside that range is brought into it first. |
   | **The Moon**, transits and shadows (darkening on) | Brightest sunlit pixels (99.9th percentile) in the whole frame | **60–80 %**, aiming at 70 %; nothing at the maximum code | The exposure, at most 10 ms so a 0.5 s ISS pass stays sharp; gain as low as that allows. |
   | **The sky** | Median outside the mask, the follower's own measure | **8–30 %**, aiming at 15 %, the follower's own band | The follower's order. A darker sky lengthens the exposure first, up to the longest exposure for meteors (250 ms, editable in the panel), then raises gain. A brighter sky lowers gain first, then shortens the exposure. |

3. **The Moon's earthshine area, shown to the observer.** The observer's box says where the earthshine is. Auto-tune then grows it to the whole earthshine area:
   - **Working image:** the frame is binned 8 × 8, so single-pixel noise and stars do not matter.
   - **Off-limb sky level:** the darkest 2 % of the frame (or the camera's black level if no sky is in view).
   - **Area:** the cells connected to the box whose level is closer to the box's earthshine than to the off-limb sky, no more than three times as far above the sky as the box's median, and with no saturated pixel. This keeps the maria and highlands, which differ by about a factor of two, and leaves out the off-limb sky, the glare near the terminator and the sunlit side.
   - **Shown on the preview:** an outline in its own colour, with the share of the frame it covers.
   - **When it fails:** if the area cannot be separated (it is smaller than the box, or it covers nearly the whole frame), the panel says so and measures the box alone.

   Brightness and noise are measured over the whole area, which is more representative than one box; the box only says where to start. The observer sees at a glance whether auto-tune understood the Moon, and redraws the box if not.

4. **K from real detectors, not from a model.** Which K would have triggered cannot be read from one detector run at a low K:
   - At a lower K, two bright cores joined by a faint bridge can make one patch too large to pass, which at a higher K would be two patches that pass.
   - A dense frame can exceed the bounded patch search.
   - K also changes which pixels learn the background, and which patches reach confirmation.

   The archive validation already shows a flash missed at K = 6 and found at a higher K ([reference-and-manual-validation.md](docs/reference-and-manual-validation.md)).

   So during a K watch, the session runs **several complete detectors** on the same frames, one per K value:
   - **The K values:** the current K and up to three others (for example the current K − 1, + 1 and + 2, never below 5.0). Each detector has the current patch limits and the triggers that are switched on (large brightening, darkening), and learns exactly as Monitoring would.
   - **What is counted:** each detector counts what Monitoring would count, new candidates (events), not frames.
   - **When the computer cannot keep up:** the number of extra detectors depends on the measured processing time per frame. If not even one extra fits, the watch measures the current K only, and the panel offers **Try K …** for a second watch at another value.
   - **The suggestion:** the lowest tested K with no unticked event during the watch. If even the highest tested K had events, the panel says so and offers a watch at higher values.

5. **A threshold is not a detection.**
   - **What the table shows:** K × the measured noise is shown as the **per-pixel threshold** in ADU, never as "caught". A flash can exceed it at its centre and still be too small for the minimum patch size, clip on bright terrain, or vanish before confirmation.
   - **What counts as caught:** only a flash actually recovered. With the testing tools on, the panel offers **Check with a test flash**. It injects a marked flash of a chosen size into the measured area, through the existing test-flash path (marked as test data, as now), and reports whether a detector at the suggested K found it at that place and time.

6. **One settled configuration at a time.**
   - **Results that go stale:** a K watch belongs to the camera settings it was measured with. Changing exposure, gain or offset, the target, the mask, the earthshine box or the source discards pending results and says so; a stale result can never be applied. A change made after a watch ends but before Apply discards it too.
   - **Applying camera rows:** they go to the camera together through the editor's existing debounced live request. The next measurement starts only after the camera has reported the new values back and the detector has settled.
   - **The exposure follower:** it adjusts the camera in Preview too, so it is paused while auto-tune measures, and its one-minute hold starts afresh when it resumes.

7. **Measured on the source thread, every frame.**
   - **Why there:** the window sees only the latest frame, about every 50 ms, so a one-frame flash can reach the detector and never reach the window. Measurements are therefore taken in the session, on every frame, with bounded statistics. The session sends progress and results to the panel.
   - **What the panel reports:** how many frames were measured and how much source time they span.
   - **Recordings:** a watch ends at the end of the file or at a loop boundary, and the panel shows the time actually covered. A 10 s clip played six times does not count as a 60 s watch.

8. **Measure, apply, check.** Neither exposure nor gain is trusted to scale the image exactly:
   - **Exposure:** the camera's black level (the pedestal set by Offset) means doubling the exposure does not double the raw median. The suggested exposure is computed above that pedestal, and step 1's check measures the result.
   - **Gain:** its units differ between camera families and models (tenths of a decibel on some, arbitrary steps on others). A gain suggestion is one bounded step in the needed direction:
     - the follower's gain step when one is set, otherwise one twentieth of the camera's gain range;
     - at most a quarter of the range in one apply.

     The check then measures it.
   - **When the aim is out of reach** (the exposure is at its cap and the gain at its limit, or the camera's own range), the panel shows how far the camera can get, instead of suggesting the impossible.

9. **Warnings, not silent changes.** Findings with no single right setting are shown as warnings:
   - **No unmasked samples:** the mask covers the whole frame. No brightness suggestion is made. The exposure follower is fixed the same way: today it would read a fully masked sky as black and keep lengthening the exposure.
   - **Pixels at 0 in the measured area:** raise the Offset. A dark pixel at 0 cannot get darker, and its noise is under-estimated. When the camera has an Offset control, a value is suggested.
   - **8-bit stream with noise below 1 ADU in the area:** each ADU is a coarse step for faint flashes; raise the gain or use RAW16.
   - **Moon: much of the guiding lock box is saturated** (more than 5 %, when a lock box is set): guiding cannot follow a flat white area.
   - **Twilight.** When the site's position is known, the Sun's altitude is shown:
     - **Sky, Sun above −18°:** "The sky will get darker. Auto-tune is best run in full darkness. With Follow the sky's brightness on, the follower will lengthen the exposure as it darkens; otherwise run auto-tune again later."
     - **Moon, Sun above −12°:** "The sky around the Moon will get darker. Run auto-tune again once it is dark." A thin earthshine crescent is often watched in twilight, so this is a reminder, not a block.

10. **Only what tonight's noise decides.** Auto-tune proposes exposure, gain, an Offset warning, K, and (Sky) the follower's limits. It leaves the shape settings unchanged:
    - minimum and maximum patch size;
    - the box limit;
    - large brightening;
    - learn-after time;
    - the dark-patch limits;
    - the mask.

    Those settings describe what an event looks like, not how noisy tonight is. With a SER or AVI recording, only step 2 is offered, and the camera rows read "Not a camera". This makes the K suggestion testable on recorded nights.

11. **With the exposure follower (Sky).** Auto-tune chooses the starting point, and the follower keeps the sky there through the night. Step 1 offers the follower's limits as **their own rows**, separate from the exposure and gain:
    - **Exposure from … to …:** from the camera's shortest exposure (at least 0.1 ms, so a bright dusk can be followed down) to the longest exposure for meteors (250 ms unless changed in the panel).
    - **Gain up to … in steps of …:** the gain applied in step 1 plus a quarter of the camera's gain range (never above the camera's maximum), in steps of one twentieth of the range. The follower raises gain only once the exposure is at its longest, one step at a time, at least 120 s apart. The quarter-range ceiling bounds how far it can go.

    These rows are ticked when the follower is on (the observer's decision 2). The observer can untick them to keep their own limits.

12. **Evidence stays intact.**
    - The watches save nothing.
    - Applied values become ordinary settings, recorded as now: exposure, gain and offset on session_start, and K and the patch limits on every candidate.
    - *(As built.)* Each applied change is logged in `candidates.txt` at the moment it is applied, like the exposure follower's `exposure_change`, instead of one field on the next session_start, which may come hours later or never (Preview and Monitoring in one stream):
      - `autotune_camera`: the target, the measured level and the applied rows;
      - `autotune_readback`: the exposure, gain and offset the camera reported back;
      - `autotune_k`: the old and new K, each tested K with its remaining and found events, its time and its frames with a skipped search, the excluded events, the watch length and the measured noise.

### Considered and left out

- **Re-tuning K during Monitoring.** K is measured in noise units, and the detector re-learns the noise continuously. A brighter or darker sky therefore does not make K wrong, and changing the evidence threshold in the middle of a night without the observer would make the night's candidates harder to compare. Brightness during Monitoring is the follower's job.
- **Running auto-tune automatically at dusk** (with Watch from dusk to dawn). It would change K and camera settings while nobody looks at the result, which goes against design point 1.
- **A model of each camera's gain curve.** It would need per-model data that the SDKs do not give. Measuring again is simpler and always right.
- **Estimating every K from one low-K run.** This is wrong for the reasons in design point 4.

### Technical outline

- **`auto_tune.h/.cpp`** (no widgets). These are plain functions and small types, unit-testable on synthetic and recorded frames:
  - the aims table, with the Sky band from `SkyExposureRule`, so the two cannot disagree;
  - `earthshineArea(frame, box)` returns the area and the reason when it fails;
  - the brightness measurement, which returns no value when there are no samples;
  - the camera suggestion;
  - the choice of K from the per-K event counts.
- **Session.** A watch is a bounded measurement the session runs on the source thread. It runs the extra detectors (copies of `FlashDetector` with only K changed), accumulates brightness and noise statistics on every frame, and counts new candidates per K. It stops on a configuration change, at the end of a recording, at a loop boundary, or when Monitoring starts. Progress and results are sent to the panel, as the detection view is now.
- **`skyLevel`** returns no value for a fully masked frame. The follower then does nothing and says why.
- **`AutoTunePanel`** (widget) holds the two steps, the rows, the K table, the events list and the warnings. It is opened by the **Auto-tune…** button at the top of the Detection dock. `MainWindow` applies camera rows through `CameraSettingsEditor::setControlValue` and the live Preview path, which is shared by all four camera families, and pauses the follower during a measurement.
- **Preview overlay:** the earthshine outline, drawn like the mask but in its own colour.
- **Help:** every control in the panel has a tooltip and ⓘ (Feature-24), including the aims, the K values tested, and the twilight advice.

### Done when

1. In core tests on synthetic frames of known brightness, black level and noise:
   - the suggested exposure brings the level into the aim for each of the three workflows;
   - the Moon keeps its exposure within 10–40 ms and corrects with gain;
   - a fully masked sky gives no level and no follower change;
   - the earthshine area of a synthetic crescent (sunlit limb, earthshine, off-limb sky) covers the earthshine and none of the other two.
2. A replay test with two bright cores joined by a faint bridge:
   - each K's count equals that of a detector run alone at that K;
   - the suggested K is never one that had an event;
   - *(Revised after the second review.)* Excluding an event removes it and leaves other events, even in the same frame, counted.
3. On a recorded night from `test_data`, a separate check after the watched span:
   - the suggested K, applied to the same recording, gives no false triggers over a known quiet span;
   - a set of marked test flashes of stated widths, durations and positions is reported with how many were recovered.
4. UI tests with a fake camera:
   - Measure camera → Apply sends exposure and gain together, once, through the live path;
   - the K watch waits for the readback;
   - a setting changed after a watch but before Apply discards the result;
   - Apply and the button are disabled during Monitoring, including when the night schedule starts it;
   - the follower is paused during a watch;
   - with Follow on, the follower's limits are filled in;
   - *(As built.)* every applied change is logged in `candidates.txt`.
5. The real-window test (`tests\gui_e2e_test.ps1`) runs a quick auto-tune on the synthetic source.
6. MANUAL, README and the camera settings guide describe Auto-tune, its aims, its two steps, the twilight advice and its limits.

### The observer's decisions (21 November 2026)

1. **The Moon's measured area:** the observer draws a box on the earthshine. Auto-tune then finds the whole earthshine area and shows it on the preview as a visual confirmation (design point 3).
2. **The follower's gain after accepting:** auto-tune may raise it, within a bounded ceiling (design point 11). The observer's concern is that a lot of gain changes could cause problems. Hence the ceiling, gain only after the exposure is at its longest, the twilight advice to run auto-tune in full darkness (design point 9), and a separate row that can be unticked.
3. **Watch lengths:** 15 s and 60 s.
4. **The button:** in the Detection settings, prominent and visible at once, at the top of the dock.

### Feature-26 design review — 21 November 2026

I recommend keeping the Preview workflow and making two changes before approval. Tune the camera first, then assess K on frames taken at the accepted settings. Give the observer separate readings for the pixel threshold and for any flash actually recovered.

1. **A detector run at K=3 cannot tell us which higher K would have fired.** Two bright cores joined by a faint bridge can fail the maximum patch size at K=3 and pass as separate patches at K=6. A dense frame can also exceed the bounded patch search at the lower K. Across frames, K changes which pixels learn the background and which patches reach two-frame confirmation. The archive tests already contain a flash missed at the lower K and recovered at the higher one. For a first version, count actual Preview candidates at the current K, offer a trial change, then measure again at that K. Report the count and usable duration for each tested K. Include enabled dark and large-change triggers, and group detections of the same event so a plane lasting many frames can be excluded once. [Detector implementation](flash_detector.cpp) · [Measured archive recovery](docs/reference-and-manual-validation.md)

2. **Label K × noise as a per-pixel threshold in ADU.** A flash can exceed that threshold at its centre and still miss the minimum patch size, clip on bright terrain, or disappear before the confirmation frame. Reserve “caught” for a marked injection that produces a candidate at the injected place and time. The existing replay probe already checks this. For acceptance, use a known negative interval and a separate set of marked flashes with stated widths, durations and positions. Report recovery on that second set alongside the quiet-span result. [Replay validation](docs/reference-and-manual-validation.md)

3. **Measure one settled configuration at a time.** Applying exposure, gain or offset invalidates a K measured before the change. Allow camera rows to be applied together through the editor's existing debounced live request, wait for readback and settling, then offer the K watch. The sky follower already writes controls in Preview and can intervene during a 60 s watch. Suspend its adjustments while measuring and clear its hold timer on resuming. Invalidate pending results on a target, mask, box or source change. A scheduled transition into Monitoring should cancel the watch and disable Apply even if the panel is already open. Record the rows that took effect and their readbacks in the matching session's provenance. [Camera editor](camera_settings_editor.cpp) · [Sky follower and schedule](mainwindow.cpp)

4. **Take measurements on the source thread.** The window receives a replaceable latest frame, normally at 50 ms intervals, so a one-frame flash can reach the detector and never reach the widget. Let the session accumulate bounded statistics from the normal detector and publish progress and results to the panel. Report measured frames and elapsed source time. End a recording watch at EOF or a loop boundary and show the actual coverage. A repeated 10 s clip should never qualify as a 60 s observation. [Session frame path](session.cpp)

5. **Handle missing measurements and camera limits explicitly.** A fully masked sky currently gives the same numeric level as a black sky. Report “no unmasked samples” and withhold the brightness suggestion in that case. A black-level pedestal also means doubling the exposure need not double the raw median. Use a bounded trial step and remeasure, especially near clipping. Show when the camera cannot reach the aim within the exposure cap. Correct the example that keeps 50 ms despite the 40 ms flash limit. On a brightening sky the existing follower reduces gain first when it can, so align the proposed correction order with that behavior. [Sky measurement and exposure rule](sky_exposure.cpp)

For the open decisions, I would use the drawn box for earthshine only and skip it for sunlit transits. Keep 15 s and 60 s as convenient watch lengths and put the button beside Profile. Preserve the observer's follower limits by default and offer the expanded gain range as a separately selectable change. A fake-camera test that changes settings after Watch completes but before Apply, plus a replay test with the joined patches described above, would exercise the two easiest mistakes to miss.

### Response to the design review — 21 November 2026

All five points were checked against the code and accepted; the design above is revised accordingly.

1. **Low-K run:** accepted. The archive validation confirms it (a flash missed at K = 6, found higher). Instead of one detector at a low K, several complete detectors run at the K values tested, each exactly as Monitoring would (design point 4). A sequential **Try K …** is the fallback when the computer cannot run them together. Enabled large-change and dark triggers are included, and events are counted as new candidates, so one plane counts once.
2. **"Caught":** accepted. The table shows a per-pixel threshold. "Caught" is reserved for a marked test flash recovered at its place and time (design point 5).
3. **One settled configuration:** accepted. Camera first, then K. A watch waits for the readback and settling. Any change discards pending results. The follower is paused during measurement. The night schedule cancels a watch. The readbacks go into the `autotune` provenance (design points 6 and 12).
4. **Source thread:** accepted. The window's latest-frame path would miss one-frame flashes (design point 7).
5. **Missing samples and camera limits:**
   - **Fully masked sky:** accepted. `skyLevel` returns 0 then, which also affects today's follower, so that is fixed with this feature (design point 9).
   - **Pedestal:** the pedestal and the bounded step are covered by measure, apply, check (design point 8).
   - **Out of reach:** an unreachable aim is reported.
   - **My earlier mistakes:** the 50 ms example and the Sky correction order are corrected; the order is now the follower's in both directions.

On the open decisions the review and the observer agree:
- a box for earthshine only;
- 15 s and 60 s;
- the button in the Detection dock.

For the follower's limits, the review suggested keeping the observer's limits by default. The observer chose to let auto-tune raise the gain. The design does both: the limits are a separate row, ticked by default when the follower is on, and the gain ceiling is bounded.

### Feature-26 second design review — 21 November 2026

The two-step workflow and independent detector runs address the main findings from the first pass. I would keep the observer's four decisions. Before implementation, I would settle three details that determine whether the K recommendation is trustworthy.

1. **A zero count needs usable coverage.** At a low K, enough pixels can cross threshold that `FlashDetector` skips its compact-patch search and sets `patchSearchLimited` or `darkSearchLimited`. With large-change triggers off, that row can show zero candidates even when a higher K resolves two real patches. A fully masked sky also produces zero candidates at every K. Show the usable span and search-limited frames per row. Withhold a quiet-K recommendation when there is no searchable area or an enabled search was skipped during the comparison. Add a dense-frame replay that would otherwise select the lowest K for precisely this reason. [Detector search and limit flags](flash_detector.cpp)

2. **Give each K the same start and protect normal acquisition.** On a live camera, the existing Preview detector may already have minutes of learned history while a new trial detector starts with its first frame. Give every compared row the same initialization frames and count over the same subsequent frames. Budget each extra detector against the session's shared RAM ceiling as well as its frame-time allowance. If none fits, the proposed **Try K** still needs a way to run: offer an explicit change to the ordinary Preview K and label the second watch with its own time span. Test under a tight RAM ceiling that Preview continues and no row claims frames it missed. [Detector state](flash_detector.h) · [Shared memory budget](docs/feature-5-validation.md)

3. **Specify exactly what excluding an event does.** Step 2 says unticking a real event removes it, but design point 4 selects a K with “no unticked event.” Those rules give opposite answers. Use **Exclude from noise count** and choose the lowest eligible K with zero *remaining* candidates. Preserve raw and excluded counts per K in the table and provenance. The detector tracks adjacent patches across frames. A fast plane can move beyond its previous box and produce multiple candidates, and two independent candidates can occur in the same frame. Keep the individual candidates inspectable when grouping them. Test a real transit and a separate noise patch in the same frame: excluding the transit must leave the noise patch counted. [Patch matching](flash_detector.cpp)

Three smaller changes would make the camera and evidence behavior testable:

- **Make the pedestal optional.** The camera adapters expose Offset or Black level in vendor control units. The proposal needs a verified conversion before subtracting that number from a raw median in ADU. If it is unknown, take a bounded exposure step and remeasure. For the earthshine outline, test a tight crop with no off-limb sky: the darkest 2% can be lunar terrain. Fall back to the observer's box when the background estimate is unreliable. Let the observer proceed to K with their current settled settings even if they decline a camera suggestion and the brightness remains outside the aim. [Camera offset readback](cameras/qhy_source.cpp) · [Mode-dependent black-level range](cameras/touptek_sdk.cpp)
- **Cover manual recording in Preview.** An observer can press **Record** in Preview, and the session applies a test flash before passing the frame to the recorder. A recovery check started then would put the flash in the observer's SER. Keep camera Apply and the injection check unavailable during a manual recording, including when Record starts with the panel open. Verify that cancelling a pending check leaves the recorded frames untouched. [Session frame order](session.cpp) · [Manual recording workflow](MANUAL.md)
- **Invalidate on detector edits as well as camera edits.** Changing minimum patch size, one-frame confirmation, noise floor or the reference time changes what the K table means. Bind each result to the complete detection settings used for that watch and check them again at Apply. A UI test can finish a watch, switch one-frame confirmation, and verify that the old K suggestion can no longer be applied. [Detector settings handling](flash_detector.cpp)

### Response to the second design review — 21 November 2026

All six points were checked against the code and accepted. Two are met in a slightly different way, noted below.

1. **A zero count needs usable coverage.** Accepted. Each row shows how much time it measured and on how many frames its patch search was skipped. A row with a skipped search, or one that stopped early or covered less than 80 % of the watch, is never suggested. A fully masked sky stops the watch with "there is nothing to watch". Test: `aSkippedSearchIsNeverAQuietK`.
2. **The same start, within the budgets.**
   - **Same start:** accepted, met differently. Rather than re-learning every copy from the same first frames (about 5 s of a 15 s watch with the Moon's 2 s learning time), every row is a copy of the live detector taken at one frame, then re-armed with the existing `rearm()`. All rows start from the same learned background, and every row counts over the same later frames. The watch first waits until the live detector has learned for three of its time constants (at most 10 s).
   - **Budgets:** copies run only within seven tenths of the frame time and within the part of the RAM ceiling's processing reserve that no buffer is sized for. A copy refused by the RAM ceiling stops only its own row.
   - **No room:** with no room for a copy, the live detector's own candidates are counted at the current K, and **Try K** changes the ordinary Preview K visibly and watches again. Each row keeps its own time span.
3. **Excluding an event.** Accepted. The list offers each event to exclude from the noise count. One listed event is the candidates of different K rows at the same place and time. The suggestion is the lowest eligible K with no remaining events. Raw and remaining counts are shown and logged. Test: `excludingAnEventLeavesOthersCounted`.
4. **The pedestal.** Accepted. No vendor offset is converted.
   - **Black level:** two brightness measurements at the same gain and different exposures give it in ADU. Without them, an exposure change is bounded to a factor of four, and the check measures it.
   - **Tight crop:** the earthshine area falls back to the box. It does so when the darkest cells are not much darker than the box (below 60 %), or when they do not reach the frame's border. **Measure only my box** forces the fallback.
   - **Step 2 any time:** the K watch can be run without accepting a camera suggestion.
5. **Manual recording.** Accepted. Camera Apply and the flash check are unavailable during a recording. The session also cancels a pending check flash, before the flash reaches a frame, when a recording starts or is requested. The cancelled flash is never drawn, so the recorded frames stay untouched.
6. **Detection setting changes.** Accepted. A K result is bound to the complete detection settings and the camera values it was measured with. Any detection change withdraws it, and Apply K checks both again. Test: `autoTuneMeasuresAppliesAndChecks` switches one-frame confirmation after a watch.

### Feature-26 status & verification

- **Implemented** on 21 November 2026:
  - `auto_tune.h/.cpp`: aims, brightness over an area, the earthshine area, the camera suggestion, K values, event groups and the choice of K;
  - `auto_tune_watch.h/.cpp`: the measurement on the source thread, with the K rows, the flash check, and stopping on any change;
  - the `Session` hooks;
  - `auto_tune_panel.h/.cpp` and `mainwindow_autotune.cpp`;
  - the preview's earthshine outline;
  - the **Auto-tune…** button above the Detection dock's settings.
- **Follower fixes:** `skyLevel` returns no value for a fully masked frame, and the follower then waits. The follower also pauses while auto-tune measures.
- **Tests:**
  - **Core:** the 17-test `auto_tune` suite.
  - **Window:** `autoTuneMeasuresAppliesAndChecks` and `autoTuneOutlinesTheEarthshineBox` run the whole flow with a fake ZWO camera. Small-screen fit and help on every control are checked with the panel open.
  - **Real window:** `tests\gui_e2e_test.ps1` runs a quick K watch on the synthetic sky and applies the suggestion; its run gave K 5.0 with a 9 ADU per-pixel threshold.
  - **Suites:** all 23 CTest suites pass.
- **Pending:**
  - **Done-when 3:** a replay on a recorded earthshine night of the suggested K's quiet span and of marked flashes recovered. It needs the representative earthshine recording that Feature-27's field validation also asks for.
  - **Real cameras:** checks on a real camera of the gain step and the readback check.
  - **Night sky:** a run under a real night sky.

---

## Feature-28 — Auto-tune's own window and explicit preview drawing

*Proposed and implemented on 21 November 2026, from the observer's remarks after using Feature-26.*

1. **Auto-tune in its own window.** Pressing **Auto-tune…** expanded the panel at the top of the Detection dock and pushed every setting down. Auto-tune now opens in its own window beside the main one, with a scroll area inside. Its size and place are remembered. Closing it ends a box being drawn and hides the earthshine outline; a measurement in progress goes on and its result is shown when the window is opened again.
2. **A drag draws only what was chosen.** On the Moon every drag locked guiding, so drawing the earthshine box and locking guiding could be confused. A drag on the preview now draws only after the observer chooses what to draw:
   - **Draw lock box** (new, in Guiding): the guiding lock box, cyan, labelled *Guiding lock*;
   - **Draw earthshine box** (Auto-tune): purple, labelled *Earthshine*, its area tinted purple;
   - **Select lunar anchor** (Time-lapse): pink, as before;
   - **Draw mask** (the sky): red, as before.

   Choosing one turns the others off, and each turns itself off after one box. While a box is being drawn, its rubber band has the colour of what it will become. With nothing chosen, a drag draws nothing and the status line says how to choose. **Lock at selection** (a clicked pixel) is unchanged.
3. **Dusk to dawn needs the site's position.** With **Position known** unticked, **Watch from dusk to dawn** stayed ticked and did nothing. Without a position it is now turned off and disabled, as is **Sun below**, and the panel says why. With the position back the observer ticks it again.

### Feature-28 status & verification

- **Implemented:**
  - `MainWindow::setupAutoTune` (the window);
  - the `GuidingPanel` **Draw lock box**;
  - `MainWindow::chooseDrawing` (one drawing at a time, its colour);
  - `PreviewWidget` box labels and the rubber band colour;
  - `DetectionPanel::setSitePositionKnown`.
- **Tests:**
  - `aDragDrawsOnlyWhatWasChosen`: the window opens without changing the dock; a drag with nothing chosen draws nothing and says so; the two drawings are exclusive; an earthshine box is not a lock; closing the window ends drawing.
  - `dragLocksOnAFeatureAndClickStillSelects` now locks through **Draw lock box**.
  - `nightScheduleAndExposureFollower` checks the schedule is turned off, disabled and saved off without a position.
  - Help and small-screen tests cover the auto-tune window.
- **Pending:** `tests\gui_e2e_test.ps1` was updated to find the auto-tune window, but it was not run: SeleneWatch was observing from the Release build. A look on the observer's laptop is also pending. *Later the same day:* the real-window test found the auto-tune window and passed, before the Feature-30 commit.

---

## Feature-29 — Site and instrument presets

*Proposed and implemented on 21 November 2026.*

An observer who observes from more than one place, or with more than one telescope, retyped the site and instrument each time. **Session → Site and instrument…** now has a **Presets** row:
- **Choosing a preset** fills in the observer, the site, its position and the instrument. Nothing is used until **OK**.
- **Save as preset** stores the values in the form under the name typed beside it, replacing a preset of the same name.
- **Delete** removes the chosen preset.

The NTP clock check belongs to the computer, not to a site, so it is not part of a preset. Presets are kept in QSettings under `site/presets/` (`site_presets.h/.cpp`).

### Feature-29 status & verification

- **Implemented** in `site_presets.h/.cpp` and `SiteDialog`.
- **Test:** `sitePresetsSaveLoadAndDelete` saves two presets (one without a position), loads each back exactly, replaces one by name and deletes one.

---

## Feature-30 — Minimum patch from auto-tune

*Proposed on 21 November 2026. Under discussion; not approved for implementation.* *Approved and implemented the same day: see the observer's decisions and the status below.*

The observer asked whether auto-tune can also find the minimum patch, which decides how small an object can be detected.

### What the minimum patch trades

A candidate must cover at least **Minimum patch** pixels at K (8 by default):
- **Too large:** a faint flash covers few pixels above K and is rejected. A flash's footprint at K is set by the optics and seeing (its FWHM) and by how far its peak stands above K × the noise: a flash just above the threshold covers only its core.
- **Too small:** noise, single hot pixels, cosmic-ray hits (one to four pixels) and twinkling stars pass.

So the minimum patch is not a property of the night's noise alone, as K is: it depends on the image scale and seeing (stable for one instrument) and on the faintest flash the observer wants. It is worth measuring rather than guessing, but from two sides.

### Proposal

1. **The signal side: the footprint of a just-detectable flash.**
   - **Moon:** a marked test flash with the seeing's FWHM, at an amplitude just above the threshold (for example 1.5 × K × the measured noise), is injected into the measured area, as the flash check does. The size of the patch it makes at K is the footprint to protect: the minimum patch must not be larger. FWHM is the observer's Test flash setting, or for the sky the measured FWHM of stars outside the mask.
   - **Sky:** meteors are streaks, far larger than a star, so there the minimum patch matters mainly for noise.
2. **The noise side: what smaller minimum patches would trigger.** The K watch's complete detectors also run at smaller minimum patches, for example the current value, two thirds of it and a half. The table then becomes K × minimum patch, with the events of each. This is affordable only within the same frame-time and RAM budgets, so pairs are tested only where there is room. The thorough watch is the natural place.
3. **The suggestion:** the smallest tested minimum patch, never larger than the footprint, whose row at the suggested K has no remaining events, and never below 3 pixels (single hot pixels and most cosmic rays). It is shown together with the faintest flash it keeps.

### Questions for the observer

1. Should the minimum patch be suggested at all, or only measured and shown (footprint and noise at each size), leaving the choice to the observer?
2. Is the Test flash's FWHM the right seeing for the Moon, or should auto-tune measure the sharpness of the earthshine (for example the edge of a crater rim) instead?
3. Is a longer thorough watch acceptable when pairs of K and minimum patch are compared (the copies share the same frame-time budget, so fewer pairs fit at a high frame rate)?

### The observer's decisions (21 November 2026) and the approved design

1. **A value is suggested**, as for K, for the flashes watch only (Feature-31). The shadows keep their fixed 20-pixel default.
2. **The flash width.**
   - **With an image scale:** a real flash is a point source, as wide as a star: the FWHM in pixels is the typical seeing divided by the image scale (″ per pixel, from **Site and instrument**). The typical seeing is one field in the auto-tune window, 2.5″ by default, and is remembered.
   - **Without an image scale** (no focal length or pixel size): the **Test flash** FWHM (3 px by default) is used and labelled as an assumption, with a one-line hint to enter the focal length and pixel size. A narrower assumed flash keeps the minimum patch small, which errs towards catching faint flashes; the noise watch still rules out any value that triggers on noise.
   - **The pixel size** will be filled in from the connected camera's SDK when the observer has not entered it (times binning), so usually only the focal length is needed.
   - **Later:** measuring the blur on the Moon's limb.
3. **A separate step, after K.**
   - **3. Minimum patch (flashes):** the footprint is measured first. One marked test flash of the assumed width is injected with its peak at 1.5 × K × the measured noise, and a copy of the live detector with a minimum patch of 1 counts the pixels it covers at K.
   - **The noise watch** then runs for 60 s at the applied K, with complete detector copies at the current minimum patch and about ⅔, ½ and ⅓ of it (never below 3).
   - **The suggestion** is the smallest tested value with no remaining events and a fully searched watch, no larger than the footprint and never below 3. When noise needs a larger value than the footprint allows, the panel says that flashes of this width cannot be told from noise at this K, and suggests the smallest quiet value.
4. **The MANUAL explains** why a test flash is injected into real frames (section 2.6).

### Implementation plan

1. **Core** (`auto_tune.h/.cpp`): `minPatchValuesToTest`, `flashWidth` (seeing, image scale, fallback), `footprintAmplitude`, `chooseMinPatch`, and the row's `minPixels`; tests in `auto_tune_tests.cpp`. **Done** (21 November 2026): two new tests, all 21 auto-tune tests pass.
2. **Watch** (`auto_tune_watch`):
   - rows that vary the minimum patch at a fixed K;
   - a footprint kind, injecting through the flash path into a copy with minimum patch 1, which reports the patch size at the flash.
3. **Panel and window:** step 3, with the seeing field, the width readout and its source, the 60 s watch, the table, the footprint and Apply; logged as `autotune_min_patch`.
4. **Camera pixel size** from the four SDKs.
5. **Tests:** window and real-window tests, then the MANUAL step.

**Status (21 November 2026): implemented.**
- **Steps 2 to 4:**
  - footprint and minimum-patch rows in `AutoTuneWatch`;
  - step 3 in the panel, with the seeing remembered as `autotune/seeing`;
  - the window flow, logged as `autotune_min_patch`, and Apply bound to the settings it was measured with;
  - the pixel size from the ZWO, QHY and Player One SDKs, and ToupTek's as an optional export.
- **Tests:**
  - `theFootprintOfAJustDetectableFlashIsMeasured`: a 3 px flash covers a few pixels, a 6 px one more;
  - `minimumPatchRowsCountWhatEachValueWould`;
  - `autoTuneSuggestsTheMinimumPatchForFlashes`: the window flow, the Test-flash fallback, the camera's pixel size, and the scale from the seeing.
- **Not in the real-window test:** its 60 s watch is too long there.
- **Pending:**
  - a run on a real earthshine recording or the real sky;
  - measuring the blur on the Moon's limb (later).

---

## Feature-31 — The Moon's flashes and shadows watched apart

*Proposed and implemented on 21 November 2026, at the observer's request.*

Flashes on the earthlit side need a sensitive detector and the earthshine at 3–10 %, which saturates the sunlit side. Shadows on the sunlit side need it unsaturated, and a detector insensitive to seeing on bright terrain. One set of settings cannot serve both.

**Watching** now offers three choices:
- **The Moon: flashes on the earthlit side.** It keeps the Moon's settings and profiles from before, with darkening always off. **Ignore brighter than** (30 % of full scale by default) leaves out pixels whose learned brightness is above it: the sunlit side and the glare by the terminator, where no flash can be seen. It follows the Moon's drift. The test-flash readout says when a chosen pixel is above it.
- **The Moon: shadows on the sunlit side.** It has its own settings and profiles, with darkening always on. Its defaults favour no false triggers: K 12, shadows of at least 20 pixels confirmed over two frames, dark patches up to 20000 px and 10 ×, and large darkening off.
- **The sky**, as before.

The **Also trigger on darkening** checkbox is gone. Both Moon watches save under `events/moon/`, share the Moon's capture settings and guiding, and run one at a time. Auto-tune follows the watch: earthshine aims for the flashes, sunlit aims for the shadows.

**Two cameras, two copies of SeleneWatch** (checked in the code, not with hardware):
- **Files:** no conflict. Session, clip and recording folders are created atomically, with a `-2` suffix on a clash.
- **Settings:** shared unless each copy is started with its own `SELENEWATCH_SETTINGS_NAME`.
- **Hardware:** each copy must open a different camera. Only one may guide the mount. CPU, RAM ceiling, USB and disk add up.

**Tests:**
- `theSunlitSideIsLeftOutForFlashes` (detector).
- `eachTargetKeepsItsOwnSettings` and `darkeningIsForTheMoonOnly` (window), plus the end-to-end window and real-window tests, which now flash in one watch and shadow in another.

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
- **The sky:** **Watch from dusk to dawn** is removed (the observer's decision below). Its stop at dawn becomes the **At dawn** condition, ticked for anyone who had the schedule on, and it can be combined with an earlier local time. The automatic start at dusk goes; it may come back later as an extension of **Ends**.
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
7. **The sky:** no mount choice is shown. **Watch from dusk to dawn** is gone, and a saved one becomes **At dawn**.
8. **Window:** every new control has help (Feature-24) and fits the small screen (Feature-23).
9. **Documentation:** the MANUAL and README describe **Ends**, the mount choice and the grace period.
10. **By hand:** EQMOD stops tracking at a scheduled end and after guiding is lost.

### Out of this step

- Parking, slewing or unparking the mount (the observer's decision).
- Ending a session because tracking stopped while not guiding.
- Starting a Moon session when the Moon rises above an altitude: a natural follow-up, cheap once the Moon's altitude exists.
- Starting a sky session at dusk (removed with **Watch from dusk to dawn**). It can come back as a start condition beside **Ends**.
- A mount for the sky.

### The observer's decisions (21 November 2026)

1. **One shared Ends row** beside the start buttons, with the choices proposed.
2. **No parking.** At the end the mount keeps tracking or stops tracking. The observer must choose one, and a warning makes sure keeping it tracking is not forgotten.
3. **The choice applies to every end SeleneWatch makes by itself**: the schedule, lost guiding, a lost camera, a full disk.
4. **The sky needs no mount.** It is watched with a fixed camera and lens.
5. **Lost guiding ends the session**, for Monitoring and for a time-lapse (a Moon or planet time-lapse is guided too), after a grace period longer than today's for passing clouds.
6. **Watch from dusk to dawn is removed**, with every reference in the code and the documentation. A start condition can be added later by extending **Ends**.

### Feature-32 status & verification

**Implemented** on 21 November 2026:
- **The conditions:** `session_end.h/.cpp` (the settings, `endTime`, `plannedEnd`, their texts) and the Moon's altitude in `sky_schedule.h/.cpp`: the Astronomical Almanac's low-precision position with its parallax. Dawn is now a rising crossing of the Sun.
- **The window:** `mainwindow_session_end.cpp` (the Ends row, the session's start, the checks every second, the end, the first-start question, the records) and `session_end_dialog.h/.cpp` (**Change…**).
- **The mount:** `Mount::stopTracking` and `MountState::canSetTracking`; `AscomMount` writes `Tracking` false after reading `CanSetTracking` at connect; `SimulatedMount` stops the shared pointing's tracking.
- **The guider:** `requestStopTracking` (ends guiding first, then stops tracking and reads it back), `setLostAfter` (the lock's search time, **Lock lost after** in the Guiding dock, 2 min by default, 30 s to 10 min), and `GuideStatus::unplannedStops`, which counts guiding stops that were not the observer's or the stream's.
- **Removed:** **Watch from dusk to dawn**, **Sun below** and the schedule's reading in the Detection dock, the window's night schedule and `nightAction`; the time-lapse's **Fixed duration** and **UTC end time** (its **Stops** choice is now **When the session ends** or **After a number of stacks**).

**As built, beside the proposal:**
- **The record.** There is no separate `session_end` line. The reason goes on the `monitoring_stop` line of `session.txt` (`reason=session end: …`) and on `stop_reason:` in `timelapse.txt`. What the mount did is a `mount_at_end action=… result="…"` line in `session.txt`, shown in the nightly report, which is written again for it, and a `mount_at_end:` line in `timelapse.txt` once the time-lapse has written that file.
- **The report rewrite** is tried again, a second apart, up to five times. On Windows a report written moments before can still be held open by another program (a virus scan), and replacing it is refused (found by the window test).
- **The status line** keeps why the session ended and what the mount did, also when the nightly report's message arrives after it.
- **A short gap is not an end.** A session counts as stopped by itself only after three seconds without Monitoring or the time-lapse, so the sky follower's restart of Monitoring does not end it.
- **The Moon's altitude** agrees with JPL Horizons within 0.04° at 14 times in 2026 and 2030, against the 0.3° proposed.

**Tests:**
- **Core:** `theMoonsAltitudeMatchesJplHorizons`, `aSessionEndsAtTheFirstConditionMet` and the Sun's rising crossing (`sky_extras`); the lock's search time (`guiding`).
- **Window:**
  - `aSessionEndsByItselfAndStopsTheMountsTracking`: the first-start question (Cancel and Stop tracking), Monitoring ended an hour later with the stream closed, tracking stopped and recorded in `session.txt` and the report; the observer's Stop leaving the mount tracking; a time-lapse ended the same way with its record; no mount choice for the sky.
  - `theDiskReserveEndsMonitoring`.
  - `lostGuidingEndsTheSession`: a real calibration of the simulated mount, guiding, the observer's stop of guiding ending nothing, then the mount's tracking stopping and ending the session at once.
  - `theEndsDialogShowsWhenEachConditionIsMet`, with the migration of a saved **Watch from dusk to dawn**.
- **Suites:** all 23 pass. The window suite takes about 110 s instead of 55 s, most of it the calibration.
- **Real window:** `tests\gui_e2e_test.ps1` passed on the Release build, with the **Ends** row in place and the sky group without **Watch from dusk to dawn**.

**Pending:** EQMOD with the observer's mount stopping its tracking at a scheduled end and after guiding is lost.

## Feature-33 — A Record button that becomes Stop

*Requested and built on 22 November 2026, from a discussion with the observer.*

The observer asked for manual recording to work like a recorder: a **Record** button that becomes a **Stop** button while recording, and stops the recording when clicked. The button already toggled since Feature-14 / Feature-17, but only its text changed ("Record" / "Stop recording") and it looked the same in both states, so the change was easy to miss at the telescope.

### What changed

- **Not recording:** the button shows a red dot and **Record**, in the normal button style.
- **Recording:** the button turns red with white bold text, shows a white square and **Stop** followed by the recording's length, for example **Stop 0:42** (m:ss, h:mm:ss from an hour on). Clicking it stops the recording, as before.
- **The length** is the time between the recording's first and last frames, the same figure the `● RECORDING` line in the mode banner shows, refreshed with it once a second.
- **The button's width** is fixed wide enough for **Stop 0:00:00**, so the controls row does not move as the time grows.
- The tooltip while recording says the button stops it and what the time is.
- What starts, stops and ends a recording (frame limit, disk guard, a scheduled recording) is unchanged.

### Design notes

- `MainWindow::updateRecordButton()` owns the button's look. `updateControls()` calls it and keeps deciding whether the button is enabled; `updateStreamReadout()` calls it each second while recording. The icon, tooltip and style sheet are changed only when the state changes (`recordButtonStops_`), not every second.
- The icons are drawn with `QPainter`, with no new image files. A disabled **Record** uses Qt's greyed version of the red dot.

### Feature-33 status & verification

**Tests:** `UiTest::siteAndRecordingControls` checks that the button reads **Stop m:ss** with the red style while recording in Monitoring, and **Record** with the normal style after it stops. The `ui`, `preview`, `session_record` and `manual_capture` suites pass on the Debug build. Both states were checked in offscreen screenshots.

**Pending:** a look at the real window on the observing laptop (1920×1200 at 125 %).

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

### Stage 2 — the camera's best gain from its measured noise

*As proposed:* "In the aim" is a stand-in. What decides the faintest flash that can be caught is the threshold, K × the noise, counted in photons rather than ADU. While the loop steps through gains, it could measure, per gain, the variance of each pixel over time against its mean (a photon-transfer measurement). This gives the camera's conversion factor (ADU per electron) and read noise at each gain, from the camera itself. The loop would then choose, within the aim, the gain whose threshold in photons is lowest. It would be a measured optimum for the observer's camera and sky on the night, not a rule of thumb. It needs stage 1 first, and the per-pixel temporal variance that the detector already keeps.

*As built (22 November 2026; the observer asked for both stages without waiting for review):*

1. **No conversion factor is needed.** At a fixed exposure the background puts the same number of electrons into a pixel whatever the gain. The threshold in photons, K × noise ÷ (ADU per electron), is therefore proportional to noise ÷ signal, both measured in ADU. The gain with the highest **background signal-to-noise** has the lowest threshold. A full photon-transfer fit would give the same ranking with more measurements and more assumptions, so it was left out.
2. **What is measured.** Each brightness measurement now also gives the area's temporal noise: for up to about 4096 pixels spread over the area, the standard deviation of each pixel over the measurement's frames, and the median over the pixels (`TemporalNoise`). It is never taken below half a code, the detector's own noise floor (0.5 ADU by default) and about the rounding of each sample. The signal is the level above the black level. A **dark round** measures the black level at a gain: one round at a hundredth of the tuned exposure, with the hundredth of light it still holds taken out. It is skipped, and the black level taken as 0, when the camera cannot go that short.
3. **Which gains are compared:** two, at the exposure stage 1 reached.
   - **The reached gain:** the gain stage 1 found in the aim.
   - **The top gain:** the highest gain measured in the aim. When none was measured above 70 % of the top of the aim, a probe aims at 85 % of it, from the response measured at this exposure above the black level. A probe never goes past a gain already found above the aim; after an overshoot, it halves the interval.
   - **The order:** the reached gain's dark round comes first, so the probe aims at the signal; then the probe; then the top gain's dark round.
   - Comparing only these two keeps stage 2 to about three or four rounds. While building, comparing every gain measured in the aim needed a dark round each and ran out of rounds when the black level alone was in the aim.
4. **The choice.** The higher gain is chosen only when its signal-to-noise is more than 5 % better. A little more gain that changes nothing keeps its headroom, and fewer hot pixels are amplified. Then the chosen settings are set at the tuned exposure and measured once more. After 8 rounds of stage 2 without a choice, the reached gain is kept.
5. **Where it applies:** the Moon's flashes and the sky. For transits, the gain stays as low as the exposure allows (Feature-26).
6. **What to expect.** On most cameras the read noise in electrons is already near its floor at the gain that puts faint earthshine or a dark sky in the aim. The background's own shot noise then dominates, and stage 2 keeps the reached gain. It changes the gain where the read noise still falls inside the aim. The clearest case is a camera with a high-conversion-gain mode (for example the IMX462 and IMX585 sensors around gain 252 on ZWO), where the read noise drops sharply at one gain. It also matters for 8-bit streams, where half a code is a large part of the noise.
7. **The record.**
   - The panel lists each round of stage 2: the level and noise measured, or the dark level, and what comes next.
   - The rounds are logged as `autotune_camera` lines with `stage=refine`, and the chosen settings with `stage=chosen`.
   - The choice is logged as an `autotune_gain_choice` line with the exposure, the gain and, for each gain compared, its level, black level, noise and signal-to-noise.
   - Each `autotune_camera` line carries `measured_noise`.
8. **Stopping.** Cancel, a change of the measured area or of a detection setting, or a readback that does not come during stage 2 returns the camera to the settings stage 1 reached, never leaving it at a dark or probe round's. That return is logged with `stage=stopped`. A change the observer makes to a camera setting is left as it is.
9. **The measured area is fixed for the whole loop**, stages 1 and 2. A dark round's frame would otherwise move the earthshine's outline.

### Stage 1 status & verification

Built on 22 November 2026.

- **Core:** `nextCameraStep` in `auto_tune.cpp` is a plain function over the loop's measurements, like the rest of auto-tune. `suggestCamera` still gives the final verdict, warnings and the Offset and follower rows; the loop removes its exposure and gain rows.
- **Window:** `mainwindow_autotune.cpp` runs the rounds: measure, `nextCameraStep`, set through the camera settings editor, wait for the readback, measure again. **Measure camera** and the check after Apply are replaced by **Tune camera**; the panel lists the rounds and has **Restore previous settings**.
- **Rounds on model cameras** (counting the final measurement in the aim): dark earthshine 4, bright earthshine 3, an exposure outside 10–40 ms 3, a saturated Moon for transits 5, the night sky 1, the dusk sky 3; gain curves from 5 to 60 units per stop on ranges of 100 and 600, 2 to 5. The fake camera of the window test, a dark sky under an 8-bit black level, takes 6: three exposure moves to 250 ms, then two gain moves.
- **Tests:** `auto_tune` (`theLoopReachesEachAimInAFewRounds`, `theLoopLearnsAnyGainCurve`, `saturationOnlyBoundsTheLoopsMove`, `theLoopStopsWhenTheAimIsOutOfReach`); `ui` (`autoTuneTunesTheCameraLiveThenWatchesK` with a fake camera whose frames follow exposure and gain, including Restore; `autoTuneOutlinesTheEarthshineBox` ending out of reach).
- **Not run:** the real-window test `tests\gui_e2e_test.ps1` (its element name was updated).
- **Pending:** a night with a real camera on the earthshine and on the sky.

### Stage 2 status & verification

Built on 22 November 2026.

- **Core:** `TemporalNoise` and `refineGain` in `auto_tune.cpp`; `AutoTuneWatch` measures the noise with the brightness.
- **Window:** `mainwindow_autotune.cpp` runs stage 2 after stage 1 reaches the aim (`refineCameraGain`, `leaveRefining`, `finishCameraTune`).
- **Model cameras** (`auto_tune`): a 12-bit camera whose read noise drops from 6 to 1.2 electrons at gain 340 reaches the aim at gain 282, and stage 2 chooses gain 358, with background signal-to-noise 4.3 against 2.6, in 7 rounds in all. A shot-noise-limited camera and the night sky keep the reached gain (70.1 against 70.1; 28.4 against 28.5). The dark rounds give the black level to within 0.05 % of full scale.
- **Window test** (`ui`, `autoTuneTunesTheCameraLiveThenWatchesK`): the fake camera now has Gaussian noise from a read noise that drops at gain 85. The loop reaches the aim at gain 77 in 6 rounds. Stage 2 then takes a dark round, probes gain 89, takes its dark round, and chooses gain 89 (6.9 against 5.5). It sets it back at 250 ms and confirms, 10 rounds in all.
- **Tests added:** `temporalNoiseIsEachPixelsSpreadOverTime`, `theRefinedGainHasTheFaintestThreshold`, `aShotNoiseLimitedSceneKeepsItsHeadroom`, `theDarkMeasurementGivesTheBlackLevel`.
- **Not run:** the real-window test `tests\gui_e2e_test.ps1`.
- **Pending:** a night with a real camera, ideally one with a high-conversion-gain mode, on the earthshine and on the sky.

### Done when (stage 1)

- A model camera with a black level and an unknown gain curve reaches the aim in each workflow in a few rounds, from far too dark, from far too bright and from saturation, and never beyond the camera's or the workflow's limits.
- The loop stops with the reason when the aim is out of reach, and when the aim lies between two settings.
- A window test with a fake camera whose brightness follows exposure and gain: one press of **Tune camera** brings it into the aim through live changes, logs each round, and **Restore previous settings** sets the earlier values back.
- MANUAL, the camera settings guide and this record describe the loop.

### Done when (stage 2)

- On a model camera whose read noise drops inside the aim, the chosen gain is in the lower-noise range, and its signal-to-noise is within 10 % of the best any gain in the aim gives.
- A shot-noise-limited camera keeps the gain stage 1 reached.
- The dark rounds give the black level.
- In the window, stage 2 runs after stage 1, ends at the tuned exposure, and logs its choice.

## Feature-35 — Two ways to watch the sky

*Discussed with the observer and built on 22 November 2026, at the observer's request without a separate review.*

**The question.** The observer asked what longest exposure auto-tune sets for the sky, and whether the sky's two uses should be told apart: a high frame rate to catch movement, and longer exposures to catch fainter detail.

**What it was.** One sky watch. Auto-tune's **Longest exposure for meteors** was 250 ms by default (10 ms to 10 s), not remembered between launches. The exposure follower's own longest was 2 s by default.

**What decides the split.** Whether the light moves across pixels during the exposure.
- **A moving source** (a meteor, a satellite) crosses a pixel in a few milliseconds on a wide-field lens. Once the exposure is longer than that, each pixel holds the same light from it however long the exposure is; the sky's background grows with the exposure, and so does the noise each pixel is tested against. SeleneWatch's detector tests each pixel against K × its noise, so for meteors a longer exposure does not reach fainter ones, and past the point where sky noise passes read noise it makes them harder to catch. Short exposures also give timing and trajectory frame by frame; meteor networks run at 25–30 frames a second.
- **A flash in place** (lightning, a sprite, a fireball's flare) gains light with the exposure up to the flash's own length, tens of milliseconds.
- **Slow or still light** (aurora, airglow, slow satellites, long glows) keeps adding up, so longer exposures show fainter detail. Faint static detail (the Milky Way) is imaging rather than catching a change.

**What was built.**
1. **`Watch::SkySlow`** beside `Watch::Sky`. In the choice box they are **The sky: fast events (meteors, lightning)** and **The sky: slow and faint (long exposures)**. Both are the sky's target: its folder, mask behaviour, large brightening and release, no guiding.
2. **Each keeps its own settings.**
   - Detection settings and profiles: the fast sky keeps the sky's keys from before (`sky/detection/`, profiles under `sky`), so an observer's saved sky settings are the fast sky's; the slow sky uses `skySlow/`.
   - Capture settings: `skySlow/capture/` for the slow sky; the fast sky keeps `sky/capture/`.
   - The exposure follower's limits: `skySlow/follow/` and `sky/follow/`.
   - Auto-tune's **Longest exposure**, now remembered per watch.
   - Which sky watch was chosen last (`sky/watch`).
3. **Defaults.**
   - Longest exposure (auto-tune and the follower's upper limit): 40 ms for fast events, 2000 ms for slow and faint (`defaultLongestExposureMs`). Values saved before are kept.
   - Slow and faint detection: the sky's, with the background learned over 10 s (about five frames of 2 s) and a patch learned after 60 s of firing.
   - Slow and faint capture: 20 s of history, 10 s after a trigger, clips up to 60 s.
   - These are first values, to be checked on the first nights.
4. **The record.** `monitoring_start` lines in `session.txt` now name the watch: `target=sky watch=sky_slow`.
5. **Wording.** The tooltips no longer say that longer exposures reach fainter meteors; the camera settings guide gives 20–40 ms for fast events and 0.25–2 s for slow and faint light.

**Tests.** `ui`: `eachTargetKeepsItsOwnSettings` now has four watches and checks the slow sky's own detection, capture and follower settings and that each sky watch keeps its own; `autoTuneTunesTheCameraLiveThenWatchesK` runs the fast sky, whose exposure now stops at 40 ms.

**Pending:** a night outside with each sky watch, to check the slow watch's detection defaults on real frames.

## Feature-36 — A time-lapse beside the slow sky watch

*Requested by the observer and built on 22 November 2026, without a separate review.*

**The request.** When watching the sky at longer exposures, optionally make a time-lapse alongside detection: save an image every 30 or 60 s and make a video at the end.

**What was built.**
1. **The setting.** **Time-lapse while monitoring, every … s** in the slow sky's group of the Detection dock (1 s to 1 h, 30 s by default), off by default, remembered (`skySlow/timelapse/`). It is shown only for the slow sky and can be changed only outside Monitoring.
2. **`SkyTimelapse`** (`timelapse/sky_timelapse.h`), owned by the session.
   - **When:** it starts when a watch folder opens with the setting on, spans the watch's automatic restarts (the exposure follower's), and ends when the folder closes. The session offers it every frame in Monitoring; it keeps one every interval of source time, and a recording that loops starts the cadence again.
   - **The source thread never waits.** A kept frame is copied, with its own memory so the camera's frame returns to its pool, into a queue of at most two; the time-lapse's own thread writes it. A frame due while two wait is skipped and counted.
   - **What is kept, in `<watch>/timelapse/`:**
     - `frame_00001.png` …: the camera's samples unchanged, 16-bit words for a 16-bit stream, the Bayer mosaic as delivered;
     - `frames.txt`: a header with the format, significant bits, alignment and Bayer pattern, then each frame's file, UTC time, frame index, and `contains_test_signal=yes` when it held a test signal;
     - `timelapse.mp4`: 25 frames a second, at most 1920 px on its long edge, stretched for display like the preview's auto-stretch, its levels eased from frame to frame (70 % the previous levels, 30 % the frame's) so the video does not flicker;
     - `timelapse.txt`: start, end, the reason, the interval, frames kept and skipped, the video, and any problem.
   - **The disk reserve:** before each frame, the free space must leave the **Disk reserve** and two frames; otherwise no more frames are kept and the reason is recorded.
   - **The record:** `session.txt` gets `timelapse_start interval_s=… folder=timelapse` and `timelapse_stop frames=… skipped=… video=… reason="…"`.
3. **The video is built as the night goes** rather than at the end. `Mp4Stream` (in `video_exporter`) keeps FFmpeg running for the whole watch and is given each frame as it is kept. Stopping only closes the file, so the video is ready at once and closing the program never waits for an encoding.
   - **Crash safety.** It is a fragmented MP4 with a keyframe every 10 frames, and FFmpeg flushes every packet. While building, a first version without the flush left a 28-byte file after 35 frames; with it, a copy of the file taken while FFmpeg still ran decoded 30 of the 35 frames. A crash or power cut therefore loses at most the last few frames of video, and none of the PNGs.
   - **Why per-frame stretch rather than one stretch for the night:** one fixed stretch chosen at the end needs all frames first, and a night from dusk to full dark would be black for most of it. The PNGs keep the true samples for any other rendering.
4. **The window.** The mode banner shows `Time-lapse: N frames kept, one every 30 s.` while Monitoring, with any problem. At the end, a message says where the frames and video are.

**Tests.**
- `timelapse`: `skyTimelapseKeepsAFrameEveryIntervalAndItsVideo` (a frame every 30 s of source time over 200 s, a 12-bit stream's words unchanged in the PNG, `frames.txt`, a loop restarting the cadence, the MP4 decoding, `timelapse.txt`); `skyTimelapseStopsAtTheDiskReserve`; `anUnclosedVideoStillPlays`.
- `ui`: `theSlowSkyKeepsATimelapseWhileMonitoring` (the setting only for the slow sky, locked during Monitoring, three frames at 1 s in the banner, the folder's PNGs, MP4 and record, and the `session.txt` lines).

**Not done.** The real-window test `tests\gui_e2e_test.ps1` was not run.

**Pending:** a night outside with the slow sky and the time-lapse on, ideally from dusk, with the exposure follower on.

## Feature-37 — Transit watch between lunar time-lapse bursts

The observer wants one lunar time-lapse that also preserves unexpected silhouettes crossing the sunlit Moon. A short crossing can fall between sampled bursts or disappear during frame selection and stacking. In an isolated benchmark the Mini's measured 1280 × 960 RAW8 rate of 28 fps supported both jobs. At 2560 × 1920 RAW16, concurrent capture reached the 2 GiB RAM limit at 30 and 60 fps. Alternating completed the bursts. A 10-second burst plus stacking occupied about 13 seconds at the smaller size and about 24–36 seconds at the larger size. The observer chose explicit best-effort transit coverage and priority for the time-lapse.

- **Also watch for transits** is an opt-in on the Moon time-lapse panel, off initially. It uses the saved **The Moon: shadows on the sunlit side** detector settings and Moon clip settings. Built-in defaults work on a new installation. **Configure transit detection…** opens the existing controls in Preview so the observer can check or adjust them. The time-lapse uses the exposure and gain already set on its stream.
- The one camera stream and guiding continue through both activities. The transit detector and ring work in the gaps. They pause during burst collection and while a stack is queued, processed, or written. Reset the reference and history at the pause so a moved image or an old clip lead-up cannot cross the gap. The normal Monitoring mode keeps its continuous coverage rule.
- When a burst is due while a transit clip is collecting or saving, let the clip finish its tail and disk write. Give that burst up to five seconds of grace. Run a delayed burst for its full capture window if it fits before the next scheduled burst. If it cannot begin within grace or fit, log the skip and keep the next scheduled time. The frame path never waits on the writer. A failed write becomes a recorded outcome and releases the wait.
- Show whether transit watching is active, saving, or paused. Record pause and resume times alongside the saved and unsaved candidate outcomes. Keep timestamped native clips under the time-lapse session, and offer **Open transit clips** when the run finishes. Stop closes the active clip and hands off the evidence even if a tail is incomplete.

**Acceptance.** An injected crossing in a gap makes a native SER clip with its lead-up and tail. An injected crossing during a burst makes no candidate and sits inside a logged pause. A clip crossing the scheduled start delays the burst until its writer finishes. A clip occupying the grace interval skips just that period. Verify the Moon-only control, saved/default settings, and normal plain time-lapse and Monitoring behaviour. Field-check the coverage and throughput with a real sunlit lunar stream.

**Status & verification (22 November 2026).** Implemented across `Session`, `TimelapseSession`, `TimelapseSettings`, `EventWriter`, `watch_settings`, `MainWindow`, and `TimelapsePanel`. Debug build and full 25/25 CTest suite passed (`manual_capture`, `timelapse`, `ui`, `dependency_rules`). Not run: Release build and real-window GUI E2E (`tests\gui_e2e_test.ps1`). Field check with a real sunlit lunar stream remains open in `features.md`.

## Feature-38 — Switch observing watches during Preview

Allow **Watching** to switch between Moon flashes, Moon shadows, and both sky watches while a live Preview is running. Stay in Preview on the same camera or playback stream. Apply the chosen watch's saved detector and display settings, clear stale detection and history, and offer the Moon time-lapse controls again when a Moon watch is selected. Starting the time-lapse from a Moon Preview remains a separate action.

Keep the selection locked in Monitoring and Time-lapse, through pending transitions, and during a Preview recording so its provenance and the current guide and camera workflow stay consistent. A Preview watch change invalidates an in-progress Auto-tune measurement. Choosing a watch keeps the camera's active controls and readback; each target's stored camera profile remains available when reopening a stream. Crossing between Moon and sky applies the existing target-specific guiding and pointing controls.

**Acceptance.** Switch Moon → sky → Moon shadows on a running Preview, check the loaded settings, source continuity and control availability, and confirm the next Monitoring run uses the chosen watch. Verify the locks during recording, Monitoring and time-lapse.

**Status & verification (22 November 2026).** Implemented in `Session::requestPreviewWatch` / `applyPreviewWatch` and `MainWindow::onWatchRequested` / `updateControls`. Debug build and full 25/25 CTest suite passed (`UiTest::watchesCanChangeInPreviewAndTransitControlsFollowTheMoon`). Not run: Release build and real-window GUI E2E (`tests\gui_e2e_test.ps1`).
