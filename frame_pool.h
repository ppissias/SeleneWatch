#pragma once

#include "frame.h"

#include <cstddef>
#include <memory>

// Hands out writable frames and takes their pixel buffers back for reuse.
//
// acquire() returns a frame with one owner. When the last pointer to that
// frame is dropped, on any thread, the frame goes back to the pool and its
// pixel buffer is reused by a later acquire(). A warm stream therefore
// allocates no pixel buffer per frame.
//
// The pool keeps only a few spare frames. When a clip is written and lets
// go of hundreds of frames at once, the rest are freed, so the memory held
// follows what is in use.
//
// A frame that outlives the pool is simply freed.
class FramePool
{
public:
    // Frames kept waiting for reuse. A steady stream needs one.
    static constexpr size_t kMaxSpareFrames = 4;

    FramePool();

    // A frame whose data holds exactly pixelBytes bytes. The pixel values
    // and the other fields are whatever the previous user left there.
    [[nodiscard]] std::shared_ptr<Frame> acquire(size_t pixelBytes);

    // Frames that exist: handed out, or kept as spares.
    [[nodiscard]] size_t allocatedFrames() const;

    // Pixel bytes held by those frames.
    [[nodiscard]] size_t allocatedBytes() const;

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};
