#pragma once

#include "frame.h"

#include <cstddef>
#include <mutex>
#include <vector>

// Thread-safe bounded circular buffer holding the most recent published frames.
// It holds pointers. Pushing drops the oldest pointer; it never copies pixels.
class FrameRingBuffer
{
public:
    explicit FrameRingBuffer(size_t capacity = 150);
    ~FrameRingBuffer() = default;

    FrameRingBuffer(const FrameRingBuffer&) = delete;
    FrameRingBuffer& operator=(const FrameRingBuffer&) = delete;
    FrameRingBuffer(FrameRingBuffer&&) = delete;
    FrameRingBuffer& operator=(FrameRingBuffer&&) = delete;

    // Stores the frame as the newest. When the buffer is full, the oldest
    // frame is let go.
    void push(FramePtr frame);

    // Lets go of every stored frame.
    void clear();

    // Returns the number of frames currently stored.
    [[nodiscard]] size_t size() const;

    // Returns the maximum number of frames the buffer can store.
    [[nodiscard]] size_t capacity() const;

    // The stored frames, oldest first. Copies pointers under the lock, not pixels.
    [[nodiscard]] std::vector<FramePtr> stored() const;

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<FramePtr> slots_;
    size_t head_{0};
    size_t count_{0};
};
