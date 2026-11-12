# Implemented features

Historical record of completed features in the program. Open proposals stay in [features.md](features.md). When moving completed features from `features.md`, append them here in full detail without removing or condensing specifications, review notes, or design decisions. This file is an append-only history.

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
