# Implemented features

Historical record of completed features in the program. Open proposals stay in [features.md](features.md). When moving completed features from `features.md`, append them here in full detail without removing or condensing specifications, review notes, or design decisions. This file is an append-only history.

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
