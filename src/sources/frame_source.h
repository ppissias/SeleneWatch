#pragma once

#include "core/frame.h"
#include "core/frame_pool.h"
#include "core/stream_info.h"

#include <cstddef>
#include <memory>
#include <string>

// Receives what a frame source delivers. Every call comes on the source's
// worker thread, in order.
class FrameSourceListener
{
public:
    // A frame with one owner, still writable. The receiver publishes it.
    virtual void onFrame(std::shared_ptr<Frame> frame) = 0;

    // The next frame does not follow on from the previous one, as when a
    // looped playback starts again.
    virtual void onDiscontinuity() = 0;

    // Live camera readbacks changed. Buffered frame association is unknown
    // in Preview; the duration is a conservative settling estimate, not a
    // per-frame settings guarantee. Recording requires a fresh camera open.
    virtual void onSettingsChanged(const StreamInfo&, std::chrono::duration<double>) { onDiscontinuity(); }

    // Camera wait returned without a frame. A listener may service mode
    // requests here, on the same worker, even during a long exposure.
    virtual void onIdle() {}

    // The source has no more frames, as at the end of a recording. Nothing
    // follows, and the worker thread ends.
    virtual void onEnded() = 0;

    // An error ended the source. Nothing follows. The listener must not call
    // stop() from here; that would join the worker from itself.
    virtual void onFailed(const std::string& message) = 0;

protected:
    ~FrameSourceListener() = default;
};

// Common frame source for the synthetic generator, recordings and live cameras.
//
// open() prepares the source and says what it will deliver. start() runs a
// worker thread that delivers frames to the listener until stop(), the end
// of a recording, or a failure. Frames come from the source's own pool.
class FrameSource
{
public:
    virtual ~FrameSource() = default;

    // Opens what the source reads and works out its StreamInfo. False, with
    // the reason, when the source cannot be used. Called before every start.
    virtual bool open(std::string& error) = 0;

    // The stream the opened source delivers. Valid after open().
    [[nodiscard]] virtual StreamInfo streamInfo() const = 0;

    // Starts the worker. The listener must outlive the run, until stop() returns.
    virtual void start(FrameSourceListener& listener) = 0;

    // Stops and joins the worker. Safe to call when it is not running.
    virtual void stop() = 0;

    // While stopped. Frame allocations from this source use the supplied budget.
    void setMemoryBudget(MemoryBudget budget) { pool_.setMemoryBudget(std::move(budget)); }
protected:
    FramePool pool_;
};
