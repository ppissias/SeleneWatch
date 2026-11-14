#pragma once

#include "blip_injector.h"
#include "event_capture.h"
#include "recording/event_writer.h"
#include "frame.h"
#include "sources/frame_source.h"
#include "latest_frame_slot.h"
#include "stream_info.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// The frame path from a source to the capture, the writer, and the preview.
//
// Owns the selected source, the capture, and the writer. On the source
// thread it adds a requested test blip, publishes each frame, offers it to
// the preview slot, and runs the capture. When the source says the next
// frame does not follow the previous one, it restarts the capture as Start
// would. When a recording ends, it hands off the clip still being collected.
//
// The window owns the Session, gives it a source, starts and stops it, and
// reads its counters. It hears about events through Notifications. A test
// can run a Session with no window.
//
// start, stop, and setSource are called on the context object's thread.
class Session final : private FrameSourceListener
{
public:
    // Each one runs on the context object's thread, queued. Any may be empty.
    struct Notifications
    {
        // A frame and its detection snapshot wait in the preview slot.
        // Take them together with takePreview().
        std::function<void()> previewReady;
        std::function<void()> candidateStarted;
        std::function<void(const ClipWriteResult&)> clipWritten;

        // The recording has no more frames. Call stop() to finish.
        std::function<void()> ended;

        // The source failed. Call stop() to finish.
        std::function<void(const QString&)> failed;
    };

    // Clips and candidates.txt go under outputRoot. context must outlive the session.
    Session(QObject* context, QString outputRoot, Notifications notifications);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // While stopped only. The session owns the source from here on.
    void setSource(std::unique_ptr<FrameSource> source);
    // While stopped. Limits are applied at Start; writer-held old frames
    // remain charged and may prevent lowering the ceiling until they drain.
    void setCaptureSettings(CaptureSettings settings);

    // Opens the source and starts it. False, with the reason, when the
    // source cannot be opened; nothing runs then.
    bool start(QString& error);

    // Stops the source and hands off a clip still being collected. Safe when stopped.
    void stop();

    // Context thread. One request at a time, while running only. Settings
    // are copied; later control edits do not change an active injection.
    bool requestBlip(const BlipSettings& settings);
    [[nodiscard]] bool blipBusy() const;

    // Context thread. Copied by the source before the next frame.
    void setDetectionSettings(const DetectionSettings& settings);
    void setDetectionDisplay(DetectionOverlay overlay, int probeX, int probeY);

    // Started and not yet stopped. Stays true after the recording has
    // ended, until stop().
    [[nodiscard]] bool isRunning() const;

    // The source of this run has ended or failed and delivers nothing more.
    // Lets the window tell a late notification from an earlier run apart.
    [[nodiscard]] bool sourceFinished() const;

    // The stream of the current or last run. Context thread only.
    [[nodiscard]] const StreamInfo& streamInfo() const;

    // Safe from any thread.
    [[nodiscard]] uint64_t acquiredFrames() const;
    [[nodiscard]] size_t bufferSize() const;
    [[nodiscard]] RingUsage historyUsage() const;
    [[nodiscard]] MemoryUsage memoryUsage() const;
    [[nodiscard]] WriterUsage writerUsage() const;
    [[nodiscard]] PreviewFrame takePreview();

private:
    // Source thread.
    void onFrame(std::shared_ptr<Frame> frame) override;
    void onDiscontinuity() override;
    void onEnded() override;
    void onFailed(const std::string& message) override;

    void post(std::function<void()> call);

    QObject* context_;
    Notifications notifications_;

    // Set at start, before the source thread runs, and only read while it runs.
    StreamInfo streamInfo_;

    MemoryBudget memory_{size_t{2} * 1024 * 1024 * 1024};
    CaptureSettings captureSettings_;
    bool budgetPrepared_{false};

    EventWriter writer_;
    EventCapture capture_;
    LatestFrameSlot previewSlot_;
    BlipInjector blip_;
    mutable std::mutex detectionMutex_;
    DetectionSettings detectionSettings_;
    DetectionDisplayRequest displayRequest_;
    std::chrono::steady_clock::time_point lastPreviewTime_{};
    std::atomic<uint64_t> acquiredFrames_{0};
    std::atomic<bool> sourceFinished_{false};
    bool running_{false};

    // Declared last, so it is stopped and destroyed before what it feeds.
    std::unique_ptr<FrameSource> source_;
};
