#include "frame_ring_buffer.h"

#include <algorithm>
#include <utility>

FrameRingBuffer::FrameRingBuffer(size_t maxBytes, double seconds)
    : maxBytes_(std::max(size_t{1}, maxBytes)), seconds_(seconds) {}

void FrameRingBuffer::configure(size_t maxBytes, double seconds)
{
    clear();
    std::lock_guard lock(mutex_);
    maxBytes_ = std::max(size_t{1}, maxBytes);
    seconds_ = seconds;
}

void FrameRingBuffer::push(FramePtr frame)
{
    std::vector<FramePtr> leaving;
    std::lock_guard lock(mutex_);
    bytes_ += frame->data.capacity();
    frames_.push_back(std::move(frame));
    while (frames_.size() > 1)
    {
        const bool old = std::chrono::duration<double>(frames_.back()->timestamp - frames_.front()->timestamp).count() > seconds_;
        const bool full = bytes_ > maxBytes_;
        if (!old && !full) break;
        if (full && !old) clamped_ = true;
        bytes_ -= frames_.front()->data.capacity();
        leaving.push_back(std::move(frames_.front()));
        frames_.pop_front();
    }
}

void FrameRingBuffer::clear()
{
    std::deque<FramePtr> leaving;
    std::lock_guard lock(mutex_);
    leaving.swap(frames_);
    bytes_ = 0; clamped_ = false;
}

size_t FrameRingBuffer::size() const
{
    std::lock_guard lock(mutex_);
    return frames_.size();
}

RingUsage FrameRingBuffer::usage() const
{
    std::lock_guard lock(mutex_);
    return {frames_.size(), bytes_, frames_.empty() ? 0 : std::max(0.0,
        std::chrono::duration<double>(frames_.back()->timestamp - frames_.front()->timestamp).count()), clamped_};
}

std::vector<FramePtr> FrameRingBuffer::stored() const
{
    std::lock_guard lock(mutex_);
    return {frames_.begin(), frames_.end()};
}
