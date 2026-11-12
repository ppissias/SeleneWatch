#include "frame_ring_buffer.h"

#include <algorithm>
#include <utility>

FrameRingBuffer::FrameRingBuffer(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity))
{
    slots_.resize(capacity_);
}

void FrameRingBuffer::push(Frame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);

    Frame& slot = slots_[head_];
    slot.width = frame.width;
    slot.height = frame.height;
    slot.format = frame.format;
    slot.frameIndex = frame.frameIndex;
    slot.timestamp = frame.timestamp;
    slot.steadyTimestamp = frame.steadyTimestamp;

    // The slot takes the new pixels. The caller gets the slot's previous buffer
    // back, which may be empty until the ring has filled once.
    std::swap(slot.data, frame.data);

    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_)
    {
        ++count_;
    }
}

std::vector<Frame> FrameRingBuffer::copyStored() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Frame> frames;
    frames.reserve(count_);
    if (count_ == 0)
    {
        return frames;
    }

    const size_t oldest = (head_ + capacity_ - count_) % capacity_;
    for (size_t i = 0; i < count_; ++i)
    {
        frames.push_back(slots_[(oldest + i) % capacity_]);
    }
    return frames;
}

void FrameRingBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    count_ = 0;
}

size_t FrameRingBuffer::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

size_t FrameRingBuffer::capacity() const
{
    return capacity_;
}

size_t FrameRingBuffer::memoryUsageBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    size_t total = sizeof(*this);
    total += slots_.capacity() * sizeof(Frame);
    for (const auto& slot : slots_)
    {
        total += slot.data.capacity();
    }
    return total;
}
