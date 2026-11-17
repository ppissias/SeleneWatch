#pragma once

#include "blip_injector.h"
#include "event_capture.h"
#include "guiding/guider.h"
#include "observing_site.h"
#include "recording/event_writer.h"
#include "recording/recording_writer.h"
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
#include <thread>

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
        // The guider has a new status. Read it with guider().status().
        std::function<void()> lockChanged;
        // A continuous recording started or ended. Read recordingStatus().
        std::function<void()> recordingChanged;

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

    // The feature lock and mount guiding. The window sends requests through
    // guider() and reads guider().status() when lockChanged arrives.
    [[nodiscard]] Guider& guider() { return guider_; }

    // Feature-14. Context thread. Copied into each clip and recording from now on.
    void setSite(const ObservingSite& site);
    // Context thread. At each Start, on its own thread: the Windows time
    // service's status and, with a server given, an SNTP check. The result
    // goes into the clips and recordings that follow, and into session_start.
    void setClockCheck(bool enabled, std::string ntpServer);
    [[nodiscard]] std::string clockRecord() const;

    // Feature-14 continuous recording. Context thread. Starts or stops one
    // while Monitoring; applied before the next frame. False when not Monitoring.
    bool requestRecording(bool record);
    // A scheduled recording: from start, for seconds, by frame time. It runs
    // while Monitoring; a new schedule replaces the last.
    struct Schedule
    {
        std::chrono::system_clock::time_point start{};
        std::chrono::system_clock::time_point end{};
    };
    void scheduleRecording(Schedule schedule);
    void cancelSchedule();
    [[nodiscard]] std::optional<Schedule> schedule() const;
    [[nodiscard]] RecordingWriter::Status recordingStatus() const;
    // Any thread. Lines for candidates.txt that are not about a clip, such as
    // an exposure change (Feature-15).
    void logRecord(const QString& lines) { writer_.logRecord(lines); }
    // Tests: the recording writer's drive query and pace.
    [[nodiscard]] RecordingWriter& recordingWriter() { return recorder_; }

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
    // Source thread: starts, feeds and ends the continuous recording.
    void updateRecording(const FramePtr& frame);
    void beginRecording(const FramePtr& first, const std::string& startedBy);
    void endRecording(const std::string& reason);
    void startClockCheck();

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

    // Feature-14. The site, the clock check and the requests are under
    // requestMutex_; the recording's state is the source thread's.
    QString outputRoot_;
    ObservingSite site_;
    std::string clockRecord_;
    bool clockCheck_{false};
    std::string ntpServer_;
    std::atomic<bool> clockBusy_{false};
    std::jthread clockThread_;
    std::optional<bool> recordRequest_;
    std::optional<Schedule> schedule_;
    RecordingWriter recorder_;
    bool recording_{false};
    bool scheduledRecording_{false};
    std::string recordingName_;
    uint64_t recordingFirstIndex_{0}, recordingLastIndex_{0};
    bool recordingKnown_{false};
    size_t recordingQueueBytes_{0};

    // Declared last, so it is stopped and destroyed before what it feeds.
    std::unique_ptr<FrameSource> source_;
};
