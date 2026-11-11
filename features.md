# SeleneWatch features

## Stage 3 — RAM ring buffer

Live preview is in place: a `Frame`, a threaded `SyntheticFrameSource`, and a window that starts and stops the stream, shows the lunar disk, and reports frame rate. That is stage 2 of the roadmap in [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md).

The next implementation step is the RAM ring buffer, fed by the synthetic source that already exists.

Stage 3 is the bounded memory history that a later trigger will copy from. A detector is not useful until those pre-event frames exist. A video-file source waits until a second source gives a shared frame-source interface a real reason to exist. Do not add that interface, a detector, or recording in this step.

### Behavior

Keep the buffer independent of the window. The source thread pushes every acquired frame into the buffer. The window keeps taking the latest frame for display and may keep dropping frames when it falls behind. Displayed frame rate and stored frame count are different numbers.

The buffer is bounded. When it is full, the oldest frame is discarded and the new frame is kept. Capacity is a fixed number of frames for this first version, chosen so the memory use is obvious at 640×480 and then at a heavier size.

### Readout

Add one readout for how many frames are stored and how much RAM they occupy. Use it to see the capacity at 640×480 and then at a heavier size.

### Out of this step

- A common frame-source interface. Only one source exists.
- Video or image-sequence playback.
- Detection, triggers, and saving pre-event or post-event clips.
- Telescope guiding or mount control.

---

### Review Notes & Technical Recommendations

#### 1. Scope & Architectural Alignment
- **Principle compliance:** The specification adheres closely to the guidelines in [AGENTS.md](AGENTS.md), particularly Rule 3 ("No abstraction or engineering without a real need"). Postponing the `IFrameSource` abstraction until a second physical/file source is added avoids premature engineering.
- **Pipeline boundary:** Decoupling the buffer from the UI window is the correct architectural choice. The acquisition callback pushes to the buffer at camera speed, while the UI polls or samples at display rate.

#### 2. Key Design Recommendations for Implementation

1. **Class Design & Responsibilities (`FrameRingBuffer`):**
   - **Name:** `FrameRingBuffer` in `frame_ring_buffer.h` / `frame_ring_buffer.cpp`.
   - **Header comment & clear API:**
     ```cpp
     class FrameRingBuffer {
     public:
         explicit FrameRingBuffer(size_t capacity);
         void push(Frame frame);
         void clear();
         [[nodiscard]] size_t size() const;
         [[nodiscard]] size_t capacity() const;
         [[nodiscard]] size_t memoryUsageBytes() const;
         [[nodiscard]] std::vector<Frame> snapshot() const;
     };
     ```
   - **Threading & Concurrency:** Protect internal storage with `std::mutex` and `std::scoped_lock` / `std::lock_guard`. Keep lock durations minimal (only while copying/moving into circular slot and advancing write index). Avoid complex lock-free queues until profiling indicates mutex contention.

2. **Memory Allocation Policy:**
   - **Avoid runtime heap thrashing:** Pushing raw `Frame` structs dynamically when wrapping around can cause heap allocations/deallocations on every frame if `std::vector<uint8_t>` is continually reallocated.
   - **Buffer reuse strategy:** Prefer pre-allocated frame slots of fixed size where possible, or move assignment into the pre-existing vector buffer in the circular slot (`std::move` or reusing allocated capacity).

3. **Buffer Sizing for 640×480:**
   - At 640×480 Mono8, each frame is ~307 KB (0.30 MB).
   - A default capacity of **150 frames** (~5.0 seconds at 30 FPS) will consume **~46.1 MB** of RAM.
   - At 1920×1080 Mono8 (future heavier size), 150 frames will consume **~311 MB**.
   - These numbers make the memory footprint immediately observable in the UI and Task Manager.

4. **UI Status Bar Readout:**
   - Add a permanent status bar readout widget:
     `Buffer: <stored>/<capacity> frames (<size_in_mb> MB)` (e.g., `Buffer: 150/150 (46.1 MB)`).
   - Update this readout periodically (e.g., once every 500ms or 1s during streaming) to prevent UI overhead from competing with frame rendering.

5. **Future Seam for Stage 4:**
   - Even though triggers and file saving are out of scope for this step, ensure `FrameRingBuffer` exposes a method to extract a sequence of stored frames (e.g. `snapshot()` or `getRecentFrames(count)`). This provides the essential test seam to verify FIFO ordering and boundary conditions under automated testing.

### Remarks on the review

Most of the review should stand. Two points need a tighter wording before implementation.

**Accepted**

- Keep the buffer off the window, and do not add a frame-source interface yet. One source does not justify one.
- `FrameRingBuffer` with `push`, `clear`, `size`, `capacity`, and `memoryUsageBytes` is the right surface.
- A `std::mutex` around the slots is the right concurrency. A lock-free queue is not justified.
- 150 frames is a sound first capacity. At 640×480 Mono8 that is about 46 MB and about 5 seconds at 30 fps. At 1920×1080 Mono8 the same count is about 311 MB, which is large enough to see in the readout.
- Refresh the status bar about once a second, not on every frame. `Buffer: 150/150 frames (46.1 MB)` is the right text.

**Changes**

- Moving a `Frame` into a slot does not stop allocation. `generateFrame` builds a new `std::vector` every time, and move-assigning it into the slot destroys the slot's previous buffer. Recycle the storage: swap the incoming pixel buffer with the buffer already in the slot being overwritten, and let the source fill that returned buffer on the next frame.
- `snapshot()` should not be the stage 4 recording method, and the window should not call it. Copying every pixel buffer while holding the mutex stalls acquisition. For this step, `size` and `memoryUsageBytes` are enough. A copy of the stored sequence can wait until a test or a trigger actually needs it.
