#include "frame_pool.h"

#include <mutex>
#include <utility>
#include <vector>

// The free list and the counters. Frames in flight hold a weak pointer to
// this, so a frame dropped after the pool is gone finds nothing and is freed.
struct FramePool::Shared
{
    std::mutex mutex;
    std::vector<std::unique_ptr<Frame>> free;
    size_t allocatedFrames{0};
    size_t allocatedBytes{0};
};

FramePool::FramePool()
    : shared_(std::make_shared<Shared>())
{
}

std::shared_ptr<Frame> FramePool::acquire(size_t pixelBytes)
{
    std::unique_ptr<Frame> frame;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (!shared_->free.empty())
        {
            frame = std::move(shared_->free.back());
            shared_->free.pop_back();
        }
    }

    const bool isNew = !frame;
    if (isNew)
    {
        frame = std::make_unique<Frame>();
    }

    // Resizing a recycled buffer of the same size does nothing.
    const size_t bytesBefore = frame->data.capacity();
    try
    {
        frame->data.resize(pixelBytes);
    }
    catch (...)
    {
        // The frame is freed as this unwinds. A recycled one was being counted.
        if (!isNew)
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            --shared_->allocatedFrames;
            shared_->allocatedBytes -= bytesBefore;
        }
        throw;
    }
    const size_t bytesAfter = frame->data.capacity();

    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (isNew)
        {
            ++shared_->allocatedFrames;
        }
        shared_->allocatedBytes += bytesAfter - bytesBefore;
    }

    // The deleter runs on whichever thread drops the last pointer.
    std::weak_ptr<Shared> weak = shared_;
    return std::shared_ptr<Frame>(frame.release(), [weak](Frame* released) {
        std::unique_ptr<Frame> owned(released);
        const std::shared_ptr<Shared> shared = weak.lock();
        if (!shared)
        {
            return;
        }

        const size_t bytes = owned->data.capacity();
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->free.size() < kMaxSpareFrames)
        {
            try
            {
                shared->free.push_back(std::move(owned));
                return;
            }
            catch (...)
            {
                // No room to remember the frame. It is freed below.
            }
        }

        // Enough spares already. The frame is freed, so stop counting it.
        --shared->allocatedFrames;
        shared->allocatedBytes -= bytes;
    });
}

size_t FramePool::allocatedFrames() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->allocatedFrames;
}

size_t FramePool::allocatedBytes() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->allocatedBytes;
}
