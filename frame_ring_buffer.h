#pragma once

#include "frame.h"

#include <cstddef>
#include <mutex>
#include <vector>

// Thread-safe bounded circular RAM buffer holding recent Frame history.
// Pushing recycles pixel buffer storage via std::swap to eliminate runtime allocations.
class FrameRingBuffer
{
public:
    explicit FrameRingBuffer(size_t capacity = 150);
    ~FrameRingBuffer() = default;

    FrameRingBuffer(const FrameRingBuffer&) = delete;
    FrameRingBuffer& operator=(const FrameRingBuffer&) = delete;
    FrameRingBuffer(FrameRingBuffer&&) = delete;
    FrameRingBuffer& operator=(FrameRingBuffer&&) = delete;

    // Pushes the frame into the circular buffer and swaps out the slot's
    // existing pixel buffer into 'frame.data' for reuse by the caller.
    void push(Frame& frame);

    // Resets the buffer head and stored count.
    void clear();

    // Returns the number of valid frames currently stored.
    [[nodiscard]] size_t size() const;

    // Returns the maximum number of frames the buffer can store.
    [[nodiscard]] size_t capacity() const;

    // Returns an approximation of total RAM (in bytes) occupied by the buffer and its slots.
    [[nodiscard]] size_t memoryUsageBytes() const;

    // Copies stored frames oldest-first. Holds the mutex for the copy.
    [[nodiscard]] std::vector<Frame> copyStored() const;

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<Frame> slots_;
    size_t head_{0};
    size_t count_{0};
};
