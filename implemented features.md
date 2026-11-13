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
