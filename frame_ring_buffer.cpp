#include "frame_ring_buffer.h"

#include <algorithm>
#include <utility>

FrameRingBuffer::FrameRingBuffer(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity))
{
    slots_.resize(capacity_);
}

void FrameRingBuffer::push(FramePtr frame)
{
    // The frame that leaves the ring is released after the lock is dropped,
    // so returning its buffer to the pool does not happen under this mutex.
    FramePtr leaving;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        leaving = std::exchange(slots_[head_], std::move(frame));
        head_ = (head_ + 1) % capacity_;
        if (count_ < capacity_)
        {
            ++count_;
        }
    }
}

std::vector<FramePtr> FrameRingBuffer::stored() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<FramePtr> frames;
    frames.reserve(count_);

    const size_t oldest = (head_ + capacity_ - count_) % capacity_;
    for (size_t i = 0; i < count_; ++i)
    {
        frames.push_back(slots_[(oldest + i) % capacity_]);
    }
    return frames;
}

void FrameRingBuffer::clear()
{
    std::vector<FramePtr> leaving(capacity_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.swap(leaving);
        head_ = 0;
        count_ = 0;
    }
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
