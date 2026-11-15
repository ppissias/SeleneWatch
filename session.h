#pragma once

#include "blip_injector.h"
#include "event_capture.h"
#include "guiding/guider.h"
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
#include <optional>
#include <string>

// The frame path from a source to the capture, the writer, the preview, and
// the feature lock.
//
// Owns the selected source, the capture, the writer, and the guider. On the
// source thread it adds a requested test blip, publishes each frame, offers
// it to the preview slot and the guider, and runs the capture. When the source says the next
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
    // Preview analyses the stream with capture off. Monitoring also collects evidence clips.
    enum class Mode { Stopped, Preview, Monitoring };
    // Each one runs on the context object's thread, queued. Any may be empty.
    struct Notifications
    {
        // A frame and its detection snapshot wait in the preview slot.
        // Take them together with takePreview().
        std::function<void()> previewReady;
        std::function<void()> candidateStarted;
        std::function<void()> modeChanged;
        std::function<void()> cameraSettingsChanged;
        std::function<void(bool)> manualSaveHandled;
        std::function<void(const ClipWriteResult&)> clipWritten;
        // The feature lock has a new status. Read it with lockStatus().
        std::function<void()> lockChanged;

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

    // Opens the source and starts Preview by default. Diagnostic tools may
    // explicitly start Monitoring. False, with the reason, when the source
    // cannot be opened; nothing runs then.
    bool start(QString& error, Mode mode = Mode::Preview);

    // Context thread. Applied before the next delivered frame, or at a camera
    // wait checkpoint when no frame is ready. Disarming ends the available
    // clip before that frame, without stopping the source.
    // A camera tuned live must be reopened before arming to exclude buffered
    // frames whose settings cannot be established by the SDK.
    bool requestMonitoring(bool monitoring);
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] bool modeChangePending() const;

    // Stops the source and hands off a clip still being collected. Safe when stopped.
    void stop(std::string reason = "stopped");

    // Context thread. One request at a time, while running only. Settings
    // are copied; later control edits do not change an active injection.
    bool requestBlip(const BlipSettings& settings);
    [[nodiscard]] bool blipBusy() const;
    bool requestManualSave();
    [[nodiscard]] bool manualSavePending() const;

    // Context thread. Copied by the source before the next frame.
    void setDetectionSettings(const DetectionSettings& settings);
    void setDetectionDisplay(DetectionOverlay overlay, int probeX, int probeY);

    // Context thread. The guider takes the lock on the next frame it is
    // offered; the outcome arrives through lockChanged.
    void requestLock(LockRegion region);
    void requestUnlock();
    [[nodiscard]] LockStatus lockStatus() const;

    // Started and not yet stopped. Stays true after the recording has
    // ended, until stop().
    [[nodiscard]] bool isRunning() const;

    // The source of this run has ended or failed and delivers nothing more.
    // Lets the window tell a late notification from an earlier run apart.
    [[nodiscard]] bool sourceFinished() const;

    // The stream of the current or last run. Context thread only.
    [[nodiscard]] StreamInfo streamInfo() const;

    // Safe from any thread.
    [[nodiscard]] uint64_t acquiredFrames() const;
    [[nodiscard]] std::optional<uint64_t> droppedFrames() const;
    [[nodiscard]] size_t bufferSize() const;
    [[nodiscard]] RingUsage historyUsage() const;
    [[nodiscard]] MemoryUsage memoryUsage() const;
    [[nodiscard]] WriterUsage writerUsage() const;
    [[nodiscard]] PreviewFrame takePreview();

private:
    // Source thread.
    void onFrame(std::shared_ptr<Frame> frame) override;
    void onDiscontinuity() override;
    void onSettingsChanged(const StreamInfo& info, std::chrono::duration<double> settling) override;
    void onIdle() override;
    void onEnded() override;
    void onFailed(const std::string& message) override;

    void post(std::function<void()> call);
    void applyManualSave();
    void applyModeRequest();

    QObject* context_;
    Notifications notifications_;

    // Changed at start or by source-thread Preview readbacks. Context queries
    // copy it under requestMutex_; clips receive it on the source thread.
    StreamInfo streamInfo_;

    MemoryBudget memory_{size_t{2} * 1024 * 1024 * 1024};
    CaptureSettings captureSettings_;
    bool budgetPrepared_{false};

    EventWriter writer_;
    EventCapture capture_;
    LatestFrameSlot previewSlot_;
    BlipInjector blip_;
    mutable std::mutex requestMutex_;
    DetectionSettings detectionSettings_;
    DetectionDisplayRequest displayRequest_;
    std::optional<std::chrono::system_clock::time_point> manualRequest_;
    std::optional<bool> monitoringRequest_;
    std::atomic<Mode> mode_{Mode::Stopped};
    bool cameraTuned_{false}; // protected by requestMutex_
    std::chrono::steady_clock::time_point settlingUntil_{};
    std::chrono::steady_clock::time_point lastPreviewTime_{};
    std::atomic<uint64_t> acquiredFrames_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<bool> dropsKnown_{false};
    std::atomic<bool> sourceFinished_{false};
    bool running_{false};

    // After the writer, which it feeds; stopped before the writer shuts down.
    Guider guider_;

    // Declared last, so it is stopped and destroyed before what it feeds.
    std::unique_ptr<FrameSource> source_;
};
