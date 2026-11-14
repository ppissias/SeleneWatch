#pragma once

#include "frame.h"

#include <cstddef>
#include <mutex>
#include <vector>
#include <deque>

struct RingUsage
{
    size_t frames{0}, bytes{0};
    double seconds{0};
    bool memoryClamped{false};
};

// Thread-safe time and byte bounded history of published frames.
// It holds pointers. Pushing drops the oldest pointer; it never copies pixels.
class FrameRingBuffer
{
public:
    explicit FrameRingBuffer(size_t maxBytes = 512 * 1024 * 1024, double seconds = 5);
    ~FrameRingBuffer() = default;

    FrameRingBuffer(const FrameRingBuffer&) = delete;
    FrameRingBuffer& operator=(const FrameRingBuffer&) = delete;
    FrameRingBuffer(FrameRingBuffer&&) = delete;
    FrameRingBuffer& operator=(FrameRingBuffer&&) = delete;

    // Stores the newest frame; drops history older than the duration or
    // over the byte allowance. Keeps the current frame even at a tiny quota.
    void push(FramePtr frame);

    // Lets go of every stored frame.
    void clear();

    // Returns the number of frames currently stored.
    [[nodiscard]] size_t size() const;

    void configure(size_t maxBytes, double seconds);
    [[nodiscard]] RingUsage usage() const;

    // The stored frames, oldest first. Copies pointers under the lock, not pixels.
    [[nodiscard]] std::vector<FramePtr> stored() const;

private:
    mutable std::mutex mutex_;
    std::deque<FramePtr> frames_;
    size_t maxBytes_, bytes_{0};
    double seconds_;
    bool clamped_{false};
};
