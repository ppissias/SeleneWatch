#pragma once

#include "autotune/auto_tune_watch.h"
#include "session/blip_injector.h"
#include "session/frame_stabilizer.h"
#include "capture/event_capture.h"
#include "guiding/guider.h"
#include "core/observing_site.h"
#include "recording/event_writer.h"
#include "recording/recording_writer.h"
#include "core/frame.h"
#include "sources/frame_source.h"
#include "session/latest_frame_slot.h"
#include "core/stream_info.h"
#include "timelapse/sky_timelapse.h"
#include "timelapse/timelapse_session.h"

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
    // TimeLapse stacks bursts at fixed intervals and can watch for transits in the gaps.
    enum class Mode { Stopped, Preview, Monitoring, TimeLapse };
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
        // Time-lapse progress or finished (Feature-18).
        std::function<void(const TimelapseStatus&)> timeLapseChanged;
        std::function<void(const TimelapseFinishResult&)> timeLapseFinished;
        // Feature-26: an auto-tune measurement changed. Read autoTuneStatus().
        std::function<void()> autoTuneChanged;
        std::function<void(QString path, QString error)> reportReady;
        // Feature-36: the time-lapse beside the slow sky watch ended.
        std::function<void(const SkyTimelapse::Status&)> watchTimelapseEnded;

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
    // Context thread. Switch detector and limits on the current Preview stream.
    [[nodiscard]] bool canChangePreviewWatch() const;
    [[nodiscard]] bool previewWatchChangePending() const;
    bool requestPreviewWatch(Watch watch, const DetectionSettings& detection, CaptureSettings capture);
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] bool modeChangePending() const;

    // Stops the source and hands off a clip still being collected. Safe when stopped.
    void stop(std::string reason = "stopped", bool resumeWatch = false);
    // Feature-32: the folder of the last Monitoring session that ended, or empty.
    [[nodiscard]] QString lastWatchFolder() const;
    // Feature-32: a line added to a finished session's record by the writer
    // (what the mount did at its end), after everything queued before it.
    // With rewriteReport, that session's nightly report is written again.
    void addToRecord(const QString& folder, const QString& fileName, const QString& line, bool rewriteReport);
    void finishInterruptedWatch() { if (!running_) closeWatch("camera restart failed"); }

    // Context thread. One request at a time, while running only. Settings
    // are copied; later control edits do not change an active injection.
    bool requestBlip(const BlipSettings& settings);
    [[nodiscard]] bool blipBusy() const;
    bool requestManualSave();
    [[nodiscard]] bool manualSavePending() const;

    // Context thread. Copied by the source before the next frame.
    void setDetectionSettings(const DetectionSettings& settings);
    // While stopped. Feature-52 learning period is enabled by default for all sessions.
    void setDetectorWarmupEnabled(bool enabled);

    // Feature-26: one auto-tune measurement at a time, in Preview only. It
    // reads frames and never changes them, except that a flash check has one
    // marked test flash injected, never into a recording. Context thread.
    // Returns the measurement's id, or 0 when it cannot start now.
    uint64_t startAutoTune(TuneWatchRequest request);
    void cancelAutoTune(const std::string& reason);
    [[nodiscard]] TuneWatchStatus autoTuneStatus() const { return tuneWatch_.status(); }
    [[nodiscard]] bool autoTuneBusy() const { return tuneWatch_.busy(); }
    void setDetectionDisplay(DetectionOverlay overlay, int probeX, int probeY);

    // The feature lock and mount guiding. The window sends requests through
    // guider() and reads guider().status() when lockChanged arrives.
    [[nodiscard]] Guider& guider() { return guider_; }

    // Feature-14. Context thread. Copied into each clip and recording from now on.
    void setSite(const ObservingSite& site);
    // Context thread. Whether the window's testing tools (synthetic source,
    // test flash) are on; recorded on each session_start line.
    void setTestingTools(bool on);
    // Feature-35: which watch, recorded on monitoring_start. Context thread.
    void setWatch(Watch watch);
    // Feature-36: while Monitoring, keep a frame every this many seconds of
    // source time for a time-lapse in the watch folder; 0 is off. Context
    // thread; taken when a watch folder opens.
    void setWatchTimelapse(double intervalSeconds);
    [[nodiscard]] SkyTimelapse::Status watchTimelapseStatus() const { return skyTimelapse_.status(); }
    // Feature-17. Context thread, while stopped only. Updates the single
    // output root used for events/<mode>/, recordings/, and timelapses/.
    void setOutputRoot(const QString& outputRoot);
    // Standalone Preview recordings. Monitoring uses the active watch.
    [[nodiscard]] QString recordingFolder() const;
    // Parent of the monitoring watch folders for the active target.
    [[nodiscard]] QString eventsRoot() const;
    // Where time-lapses go (<outputRoot>/timelapses).
    [[nodiscard]] QString timelapsesFolder() const;
    // The unified output root (<outputRoot>).
    [[nodiscard]] QString outputRoot() const { return outputRoot_; }
    // Context thread. At each Start, on its own thread: the Windows time
    // service's status and, with a server given, an SNTP check. The result
    // goes into the clips and recordings that follow, and into session_start.
    void setClockCheck(bool enabled, std::string ntpServer);
    [[nodiscard]] std::string clockRecord() const;

    // Feature-14 / Feature-17 manual continuous recording. Context thread.
    // Starts or stops one while Preview or Monitoring; maxFrames > 0 stops
    // automatically after that many frames. False when stopped.
    bool requestRecording(bool record, uint64_t maxFrames = 0);
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
    // an exposure change (Feature-15). Ignored outside Monitoring so no
    // top-level candidates.txt is created outside session folders.
    void logRecord(const QString& lines);
    // Any thread. Auto-tune entries logged during Preview (held in memory per
    // target and flushed into <watchFolder>/autotune.txt when Monitoring opens).
    void logAutoTune(const QString& lines);
    // Tests: the recording writer's drive query and pace.
    [[nodiscard]] RecordingWriter& recordingWriter() { return recorder_; }

    // Feature-18 time-lapse mode. Starts from Preview (or starts Preview if
    // Stopped). The optional detector settings belong to the Moon shadows watch.
    bool startTimeLapse(const TimelapseSettings& settings, QString& error,
        FreeSpaceQuery freeSpace = {}, std::chrono::milliseconds stackDelay = {},
        std::optional<DetectionSettings> transitDetection = {});
    void stopTimeLapse(const std::string& reason = "stopped by the observer");
    void waitTimeLapseFinished();
    // Abandons MP4/GIF assembly of every finalizing time-lapse (closing the
    // program); their FITS stacks stay on disk.
    void cancelTimeLapseExport();
    [[nodiscard]] bool timeLapseFinalizing() const;
    [[nodiscard]] TimelapseStatus timeLapseStatus() const;

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
    [[nodiscard]] bool canChangePreviewWatchLocked() const;
    bool applyPreviewWatch();
    void prepareCaptureBudget(const Frame& frame);
    bool openWatch(QString& error);
    void retryReport(const QString& folder, const QString& error, int attemptsLeft);
    void closeWatch(const QString& reason, bool resume = false);
    // Owned by the source thread during acquisition, otherwise by its caller.
    QString watchFolder_;
    // Feature-32: written on the source or context thread, read by the window.
    mutable std::mutex lastWatchMutex_;
    QString lastWatchFolder_;
    bool watchIntervalOpen_{false};
    std::chrono::steady_clock::time_point watchStart_{};
    // Source thread: per-monitoring-interval stream health and active detector config.
    uint64_t watchFrames_{0};
    uint64_t watchFrameGaps_{0};
    bool watchHasFrameIndex_{false};
    uint64_t watchLastFrameIndex_{0};
    std::optional<uint64_t> watchSourceDropsStart_;
    std::optional<uint64_t> watchSourceDropsLast_;
    uint64_t watchStabActiveFrames_{0};
    uint64_t watchStabFallbackFrames_{0};
    DetectionSettings watchLoggedDetection_;
    bool watchDetectionLogged_{false};
    // Under requestMutex_: Preview auto-tune entries flushed to <watch>/autotune.txt.
    Target pendingAutoTuneTarget_{Target::Moon};
    QStringList pendingAutoTune_;
    // Source thread: starts, feeds and ends the continuous recording.
    void updateRecording(const FramePtr& frame);
    void beginRecording(const FramePtr& first, const std::string& startedBy, uint64_t maxFrames = 0);
    void endRecording(const std::string& reason);
    void startClockCheck();
    // Source thread: computes optional sub-pixel shift and handles reset/recenter/logging.
    FrameShift stepStabilizer(const Frame& frame, bool enabled, const QString& logFolder);
    void resetStabilizer();

    QObject* context_;
    Notifications notifications_;

    // Changed at start or by source-thread Preview readbacks. Context queries
    // copy it under requestMutex_; clips receive it on the source thread.
    StreamInfo streamInfo_;

    MemoryBudget memory_{size_t{2} * 1024 * 1024 * 1024};
    CaptureSettings captureSettings_;
    bool budgetPrepared_{false};
    // Feature-26: the part of the processing reserve no buffer is sized for,
    // which auto-tune's detector copies may use. Set with the budget.
    std::atomic<size_t> tuneSpareBytes_{0};
    AutoTuneWatch tuneWatch_;

    EventWriter writer_;
    EventCapture capture_;
    // Source thread only (Feature-50): shift estimator for stabilized detection.
    FrameStabilizer stabilizer_;
    bool prevStabilizeEnabled_{false};
    std::chrono::system_clock::time_point lastStabilizerTime_{};
    LatestFrameSlot previewSlot_;
    BlipInjector blip_;
    mutable std::mutex requestMutex_;
    DetectionSettings detectionSettings_;
    DetectionDisplayRequest displayRequest_;
    std::optional<std::chrono::system_clock::time_point> manualRequest_;
    std::optional<bool> monitoringRequest_;
    // Whole watch selection, copied at the next source-thread boundary.
    struct PreviewWatch
    {
        Watch watch;
        DetectionSettings detection;
        CaptureSettings capture;
    };
    std::optional<PreviewWatch> previewWatchRequest_;
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
    bool testingTools_{false};
    Watch watch_{Watch::MoonFlashes};
    double watchTimelapseSeconds_{0};
    ObservingSite site_;
    std::string clockRecord_;
    bool clockCheck_{false};
    std::string ntpServer_;
    std::atomic<bool> clockBusy_{false};
    std::jthread clockThread_;
    std::optional<bool> recordRequest_;
    bool recordStarting_{false}; // Under requestMutex_, between request and writer admission.
    uint64_t recordMaxFrames_{0};
    std::optional<Schedule> schedule_;
    RecordingWriter recorder_;
    // Feature-36. After the recorder and the writer, which record its end;
    // finished before they shut down.
    SkyTimelapse skyTimelapse_;
    bool recording_{false};
    bool scheduledRecording_{false};
    std::string recordingName_;
    uint64_t recordingFirstIndex_{0}, recordingLastIndex_{0};
    uint64_t recordingFrames_{0}, recordingMaxFrames_{0};
    bool recordingKnown_{false};
    size_t recordingQueueBytes_{0};

    // Declared last, so it is stopped and destroyed before what it feeds.
    void openTransitWatch(const QString& parent);
    void updateTransitWatch(const FramePtr& frame, TimelapseWork work);
    void setTransitState(TransitWatchState state, std::chrono::system_clock::time_point utc);
    void closeTransitWatch(const std::string& reason);
    void finishTimeLapse(uint64_t id, TimelapseFinishResult result);

    // The source thread owns the folder and detector while running. The
    // writer acknowledges closed clips and the last queued record in order.
    QString transitFolder_;
    std::atomic<TransitWatchState> transitState_{TransitWatchState::Off};
    std::atomic<unsigned> transitWrites_{0};
    std::chrono::system_clock::time_point transitFrameUtc_{};
    std::atomic<bool> captureResetRequested_{false};
    mutable std::mutex timelapseMutex_;
    std::optional<DetectionSettings> transitDetection_;
    bool transitDraining_{false};
    std::optional<std::pair<uint64_t, TimelapseFinishResult>> transitFinish_;
    QObject timeLapseCallbacks_;
    uint64_t nextTimelapseId_{0};
    uint64_t activeTimelapseId_{0};
    std::shared_ptr<TimelapseSession> timelapse_;
    std::vector<std::pair<uint64_t, std::shared_ptr<TimelapseSession>>> finishingTimelapses_;
    std::unique_ptr<FrameSource> source_;
};
