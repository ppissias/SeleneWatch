#pragma once

#include "core/frame.h"
#include "guiding/guider.h"
#include "core/memory_budget.h"
#include "core/observing_site.h"
#include "recording/output_paths.h"
#include "core/stream_info.h"
#include "timelapse/fits_io.h"
#include "timelapse/stack_processor.h"
#include "timelapse/timelapse_settings.h"
#include "timelapse/video_exporter.h"

#include <QtCore/QObject>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Work on a frame boundary. The transit detector runs only when the stacker is idle.
enum class TimelapseWork { Idle, Capturing, Stacking };

// Coverage state for the optional lunar transit watch, published by Session.
enum class TransitWatchState { Off, Watching, Saving, Capturing, Stacking, Finished };

struct TimelapseStatus
{
    bool active{false};
    bool finalizing{false};
    QString sessionDir;
    int stacksCompleted{0};
    int periodsSkipped{0};
    double elapsedSeconds{0};
    double nextBurstInSeconds{0};
    double lastProcessingSeconds{0};

    // Video assembly progress while finalizing (0/0 until encoding starts).
    int exportDone{0};
    int exportTotal{0};

    // Latest completed stack telemetry and thumbnail.
    uint64_t lastCapturedFrames{0};
    uint64_t lastKeptFrames{0};
    double lastQuality{0};
    double lastShiftX{0};
    double lastShiftY{0};
    double lastRotationDeg{0};
    QImage latestThumbnail;

    // Warnings (empty when clear).
    bool overloadWarning{false};
    bool cloudWarning{false};
    bool guidingLostWarning{false};
    bool diskLowWarning{false};
    QString warningText;
    QString stopReason;
    TransitWatchState transitWatch{TransitWatchState::Off};
};

struct TimelapseFinishResult
{
    bool ok{false};
    QString sessionDir;
    QString stopReason;
    int stacksCompleted{0};
    int periodsSkipped{0};
    TimelapseExportResult exportResult;
    QString transitFolder;
};

// Runs a lunar or planetary time-lapse session:
// - Source thread collects up to 100-200 best frames per fixed-cadence period,
//   never holding camera FramePool frames or blocking acquisition. Every byte it
//   keeps (cropped copies and, with saveRawSer, full raw copies) is charged to the
//   session RAM budget and admitted only below kBudgetHeadroomPercent of the
//   ceiling, so a full burst refuses frames instead of starving the camera.
// - Worker thread aligns, stacks, sharpens, writes 32-bit float FITS immediately,
//   and exports MP4/GIF when the session stops (or hits diskReserveBytes).
// - Depth-1 pending burst queue + pre-capture overload skip gate prevent RAM
//   blowup if processing falls behind the interval.
// - Errors: a RAM refusal while stacking skips that period; any other exception
//   stops the session (never the program), is written to timelapse.txt, and the
//   video is still assembled from the stacks already on disk.
class TimelapseSession
{
public:
    struct Config
    {
        QString timelapsesRoot;
        TimelapseSettings settings;
        StreamInfo stream;
        ObservingSite site;
        std::string softwareVersion;
        uint64_t diskReserveBytes{0};
        MemoryBudget memory;
        std::optional<LockRegion> lockRegion;
        FreeSpaceQuery freeSpace;
        // Source thread, just before a transit-enabled burst claims RAM.
        std::function<void()> beforeBurst;
        std::function<void(const TimelapseStatus&)> statusChanged;
        std::function<void(const TimelapseFinishResult&)> finished;
    };

    explicit TimelapseSession(QObject* context, Config config);
    ~TimelapseSession();

    TimelapseSession(const TimelapseSession&) = delete;
    TimelapseSession& operator=(const TimelapseSession&) = delete;

    // Source thread. Offers a published frame to the active period collector.
    TimelapseWork onFrame(const FramePtr& frame, bool transitClipBusy = false);
    // Any thread. Updates guiding status flag for warnings.
    void setGuidingStatus(const GuiderStatus& status, bool guidingWanted);
    // Any thread. Requests session stop (also triggered by stop condition or disk reserve).
    void stop(const std::string& reason = "stopped by the observer");
    // Blocks until worker finishes active stack and builds MP4/GIF.
    void waitFinished();

    [[nodiscard]] TimelapseStatus status() const;
    [[nodiscard]] bool active() const { return accepting_.load(); }
    [[nodiscard]] bool valid() const { return initError_.isEmpty(); }
    [[nodiscard]] QString initError() const { return initError_; }
    [[nodiscard]] const QString& sessionDir() const { return sessionDir_; }

    // Any thread. Abandons MP4/GIF assembly during finalization (for closing the
    // program); FITS stacks stay on disk and Re-sharpen can rebuild the video.
    void cancelExport();

    // Tests: inject processing delay, or run code (which may throw) at the start
    // of each stack on the worker thread.
    void setStackDelay(std::chrono::milliseconds delay);
    void setBeforeStackForTests(std::function<void(int periodIndex)> hook);

    // New burst bytes are admitted only while the RAM budget stays below this
    // share of its ceiling, keeping room for the camera's own frames.
    static constexpr size_t kBudgetHeadroomPercent = 90;

private:
    struct BurstJob
    {
        int periodIndex{0};
        std::chrono::system_clock::time_point windowStartUtc{};
        std::chrono::system_clock::time_point windowEndUtc{};
        uint64_t framesCaptured{0};
        std::vector<RawBurstFrame> frames;
        size_t chargedBytes{0};
        uint64_t ramRefused{0}; // Frames not admitted because of the RAM budget
    };

    struct StackLogEntry
    {
        int index{0};
        QString fileName;
        std::chrono::system_clock::time_point dateObs{};
        std::chrono::system_clock::time_point dateEnd{};
        uint64_t captured{0};
        uint64_t kept{0};
        double quality{0};
        double shiftX{0};
        double shiftY{0};
        double rotationDeg{0};
        double processSeconds{0};
        uint64_t ramRefused{0};
    };

    void workerLoop(std::stop_token stopToken);
    void processBurst(BurstJob& job);
    void processBurstGuarded(BurstJob& job);
    void skipBurstAfterError(const BurstJob& job, const QString& reason);
    void stopAfterInternalError(const BurstJob* job, const QString& what);
    void requestStopLocked(const std::string& reason, bool discardPending);
    bool writeRawSer(BurstJob& job, QString& error);
    void finalizeSession(const std::string& reason);
    void writeMetadataLog(const QString& reason, const TimelapseExportResult& exportRes,
        const std::vector<QString>& skippedLogs) const;
    void postStatus();
    void postStatusSnapshot(TimelapseStatus snap) const;
    void refreshWarningTextLocked();
    [[nodiscard]] bool budgetHasRoomFor(size_t bytes) const;
    bool tryReserveBurstMemory(size_t bytes);
    [[nodiscard]] FramePtr copyRawFrame(const Frame& frame) const;
    void releaseBurstMemory(size_t bytes);
    void startNextWindowIfDueLocked(double tSec, std::chrono::system_clock::time_point utcNow, bool transitClipBusy);
    void skipPeriod(int period, std::chrono::system_clock::time_point utc, const QString& reason);
    [[nodiscard]] bool stackingOrQueued() const;
    void finishCurrentWindowLocked(bool allowAfterStop);
    void discardCurrentBurstLocked();

    QObject* context_;
    Config config_;
    bool useSourceTime_{false};
    std::optional<QRect> effectiveCropRoi_;
    QString initError_;

    // Collector state (protected by collectorMutex_; never touched by worker thread).
    // Lock order: collectorMutex_ before mutex_.
    std::mutex collectorMutex_;
    bool clockStarted_{false};
    std::chrono::steady_clock::time_point startSteady_{};
    std::chrono::system_clock::time_point startSourceUtc_{};
    std::chrono::system_clock::time_point startWallUtc_{};
    int currentPeriodIndex_{-1};
    int deferredPeriod_{-1}; // A due period waiting for the current transit clip.
    bool collecting_{false};
    double windowStartSec_{0};
    double windowEndSec_{0};
    std::chrono::system_clock::time_point windowStartUtc_{};
    std::chrono::system_clock::time_point windowEndUtc_{};
    uint64_t currentCaptured_{0};
    uint64_t currentRamRefused_{0};
    std::vector<RawBurstFrame> currentHeap_; // Min-heap by sharpness
    size_t currentBurstBytes_{0};

    // Shared queue (depth-1 pending slot) and status.
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::condition_variable doneCv_;
    std::optional<BurstJob> pendingJob_;
    bool stacking_{false}; // Under mutex_, from dequeue through release of burst memory.
    std::atomic<bool> accepting_{true};
    bool workerFinished_{false};
    bool stopRequested_{false};
    // Set with stopRequested_ when a queued burst must not be stacked (target
    // reached, disk reserve, write failure, internal error).
    bool discardPending_{false};
    std::string stopReason_;
    std::chrono::milliseconds stackDelay_{0};
    std::function<void(int)> beforeStackForTests_;
    std::atomic<bool> exportCancel_{false};

    // Worker-only session state.
    QString sessionDir_;
    std::optional<FitsImage> referenceStack_;
    double referenceQuality_{0};
    QRect commonOverlap_;
    double baselineQuality_{0};
    std::vector<double> qualityHistory_;
    int consecutiveSkips_{0};
    std::vector<StackLogEntry> stackLogs_;
    std::vector<QString> skippedLogs_;  // Protected by mutex_ (collector also appends)
    std::vector<QString> problemLogs_;  // Worker-only: raw SER failures, discarded bursts, errors

    // Warning detail messages protected by mutex_.
    QString fatalWarningText_;
    QString diskLowWarningText_;
    QString overloadWarningText_;

    TimelapseStatus status_;
    std::jthread worker_;
};
