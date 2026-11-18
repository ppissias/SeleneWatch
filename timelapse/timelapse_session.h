#pragma once

#include "frame.h"
#include "guiding/guider.h"
#include "memory_budget.h"
#include "observing_site.h"
#include "recording/output_paths.h"
#include "stream_info.h"
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

struct TimelapseStatus
{
    bool active{false};
    bool finalizing{false};
    QString sessionDir;
    int stacksCompleted{0};
    int periodsSkipped{0};
    double elapsedSeconds{0};
    double remainingSeconds{0}; // < 0 when open-ended (manual stop)
    double nextBurstInSeconds{0};
    double lastProcessingSeconds{0};

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
};

struct TimelapseFinishResult
{
    bool ok{false};
    QString sessionDir;
    QString stopReason;
    int stacksCompleted{0};
    int periodsSkipped{0};
    TimelapseExportResult exportResult;
};

// Runs a lucky-imaging time-lapse session:
// - Source thread collects up to 100-200 best frames per fixed-cadence period,
//   never holding FramePool slots or blocking acquisition.
// - Worker thread aligns, stacks, sharpens, writes 32-bit float FITS immediately,
//   and exports MP4/GIF when the session stops (or hits diskReserveBytes).
// - Depth-1 pending burst queue + pre-capture overload skip gate prevent RAM
//   blowup if processing falls behind the interval.
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
        std::function<void(const TimelapseStatus&)> statusChanged;
        std::function<void(const TimelapseFinishResult&)> finished;
    };

    explicit TimelapseSession(QObject* context, Config config);
    ~TimelapseSession();

    TimelapseSession(const TimelapseSession&) = delete;
    TimelapseSession& operator=(const TimelapseSession&) = delete;

    // Source thread. Offers a published frame to the active period collector.
    void onFrame(const FramePtr& frame);
    // Any thread. Updates guiding status flag for warnings.
    void setGuidingStatus(const GuiderStatus& status, bool guidingWanted);
    // Any thread. Requests session stop (also triggered by stop condition or disk reserve).
    void stop(const std::string& reason = "stopped by the observer");
    // Blocks until worker finishes active stack and builds MP4/GIF.
    void waitFinished();

    [[nodiscard]] TimelapseStatus status() const;
    [[nodiscard]] bool active() const { return accepting_.load(); }

    // Tests: inject processing delay or replace disk-space query.
    void setStackDelay(std::chrono::milliseconds delay);

private:
    struct BurstJob
    {
        int periodIndex{0};
        std::chrono::system_clock::time_point windowStartUtc{};
        std::chrono::system_clock::time_point windowEndUtc{};
        uint64_t framesCaptured{0};
        std::vector<PreparedBurstFrame> frames;
        std::vector<FramePtr> rawFrames; // Populated only when saveRawSer is true
        size_t chargedBytes{0};
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
    };

    void workerLoop(std::stop_token stopToken);
    void processBurst(BurstJob& job);
    void finalizeSession(const std::string& reason);
    void writeMetadataLog(const QString& reason, const TimelapseExportResult& exportRes) const;
    void postStatus();
    bool tryReserveBurstMemory(size_t bytes);
    void releaseBurstMemory(size_t bytes);
    void startNextWindowIfDue(double tSec, std::chrono::system_clock::time_point utcNow);
    void finishCurrentWindow();
    void discardCurrentBurst();

    QObject* context_;
    Config config_;
    bool useSourceTime_{false};
    std::optional<QRect> effectiveCropRoi_;

    // Source-thread collector state.
    bool clockStarted_{false};
    std::chrono::steady_clock::time_point startSteady_{};
    std::chrono::system_clock::time_point startSourceUtc_{};
    std::chrono::system_clock::time_point startWallUtc_{};
    int currentPeriodIndex_{-1};
    bool collecting_{false};
    double windowStartSec_{0};
    double windowEndSec_{0};
    std::chrono::system_clock::time_point windowStartUtc_{};
    std::chrono::system_clock::time_point windowEndUtc_{};
    uint64_t currentCaptured_{0};
    std::vector<PreparedBurstFrame> currentHeap_; // Min-heap by sharpness
    std::vector<FramePtr> currentRaw_;
    size_t currentBurstBytes_{0};

    // Shared queue (depth-1 pending slot) and status.
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::condition_variable doneCv_;
    std::optional<BurstJob> pendingJob_;
    std::atomic<bool> accepting_{true};
    bool workerFinished_{false};
    bool stopRequested_{false};
    std::string stopReason_;
    std::chrono::milliseconds stackDelay_{0};

    // Worker-only session state.
    QString sessionDir_;
    std::optional<FitsImage> referenceStack_;
    QRect commonOverlap_;
    double baselineQuality_{0};
    int consecutiveSkips_{0};
    std::vector<StackLogEntry> stackLogs_;
    std::vector<QString> skippedLogs_;

    TimelapseStatus status_;
    std::jthread worker_;
};
