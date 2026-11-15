#pragma once

#include "frame.h"
#include "guiding/calibrator.h"
#include "guiding/guide_controller.h"
#include "guiding/lock_tracker.h"
#include "guiding/mount.h"

#include <QtCore/QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

class EventWriter;

enum class GuideMode
{
    Idle,
    Calibrating,
    Guiding
};

[[nodiscard]] const char* guideModeName(GuideMode mode);

// The mount half of what the guider knows. A copy goes to the window.
struct GuideStatus
{
    std::string mountName;
    bool mountConnected{false};
    MountState mount;
    // The last failure to connect or to talk to the mount. Empty when none.
    std::string mountError;
    // A mount call has been running for longer than kMountCallLimitSeconds.
    bool mountNotResponding{false};

    GuideMode mode{GuideMode::Idle};
    std::string calibrationProgress;
    std::optional<GuideCalibration> calibration;
    double guideSpeedPixelsPerFrame{0};
    double tolerancePixels{0};
    double frameInterval{0};
    // The latest estimate along the mount axes while guiding, image pixels.
    double raErrorPixels{0}, decErrorPixels{0};
    // Pulses of the current or last calibration or guiding run.
    int pulses{0};
    std::optional<GuidePulse> lastPulse;
    std::chrono::system_clock::time_point lastPulseTime{};
    // Why calibration or guiding last ended, or why a request was refused.
    std::string message;
};

struct GuiderStatus
{
    LockStatus lock;
    GuideStatus guide;
};

// Runs the feature lock, the calibration, and the guiding on one thread.
//
// The source thread offers each published frame. The guider keeps only the
// newest and skips the rest, so it never holds up the frame path; with no
// lock and no lock request it does not even keep that one. The window sends
// requests (lock, unlock, connect, calibrate, guide on or off) and reads a
// copy of the status; it holds no guiding state of its own.
//
// The mount is created, used, and destroyed on this thread (COM apartment
// rules). A driver call that hangs stalls this thread only; the status then
// says the mount is not responding. Guiding is off at every start: the
// session ends it when the stream stops or starts, and the observer turns
// it on again.
//
// While the stream is Monitoring, the guider hands the writer its lines for
// guiding.txt: one offset line per second of source time, each lock change,
// each pulse, and each calibration and guiding start and stop. The writer
// keeps every pulse, recorded or not, to mark candidates that fall inside a
// correction. The guide thread never touches the disk.
//
// The lock survives Stop and Start: the first frame of a new run begins a
// fresh estimate. A frame of another size or format releases it.
class Guider
{
public:
    using MountFactory = std::function<std::unique_ptr<Mount>()>;

    static constexpr double kMountCallLimitSeconds = 5.0;
    // The mount's state is read this often while idle; every step otherwise.
    static constexpr double kIdleStateSeconds = 1.0;

    // statusChanged runs on the guide thread and must only post. It is called
    // again only after status() has been read, so a slow reader is not flooded.
    Guider(EventWriter& writer, std::function<void()> statusChanged);
    ~Guider();

    Guider(const Guider&) = delete;
    Guider& operator=(const Guider&) = delete;

    // Source thread. recording is true while the stream is Monitoring.
    void offer(const FramePtr& frame, bool recording);
    // Any thread. The next frame does not follow the previous one.
    void discontinuity();

    // Any thread. The lock is taken on the next frame offered.
    void requestLock(LockRegion region);
    void requestUnlock();

    // Any thread. The factory runs on the guide thread.
    void requestConnect(MountFactory factory);
    void requestDisconnect();
    void requestCalibration();
    void requestGuiding(bool on);
    void setTolerance(double pixels);
    // Any thread. Ends calibration or guiding with the reason (safety rules 1 and 8).
    void endGuiding(std::string reason);

    // Any thread.
    [[nodiscard]] GuiderStatus status() const;

    // Ends the thread and disconnects the mount. Called before the writer shuts down.
    void shutdown();

private:
    struct Requests
    {
        std::optional<LockRegion> lock;
        bool unlock{false};
        bool discontinuity{false};
        std::optional<MountFactory> connect;
        bool disconnect{false};
        bool calibrate{false};
        std::optional<bool> guide;
        std::optional<double> tolerance;
        std::optional<std::string> end;
    };

    void run(std::stop_token stopToken);
    void handleMountRequests(Requests& requests, bool recording);
    // One step of calibration or guiding, and the idle state reads.
    bool stepMount(bool recording);
    void execute(const GuideDecision& decision, bool calibration, bool recording);
    void stopActivity(const std::string& reason, bool recording);
    void connectMount(const MountFactory& factory, bool recording);
    void disconnectMount(bool recording);
    [[nodiscard]] GuideObservation observation(const MountState& state) const;
    void noteFrame(const Frame& frame);

    // Guide thread. Runs a mount call with the not-responding watch.
    template <typename Call>
    auto mountCall(Call&& call);

    // Guide thread. Hands the writer the lines for guiding.txt that this lock
    // step produced, with the once-per-second state record.
    void logLock(const LockStatus& before, const LockStatus& after, bool locked, bool unlocked, bool measured, bool recording);
    void log(const QString& lines, bool recording);
    void publish();

    EventWriter& writer_;
    std::function<void()> statusChanged_;

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    FramePtr pending_;
    bool recording_{false};
    Requests requests_;
    // A lock is held or requested, so frames are worth keeping.
    std::atomic<bool> wanted_{false};

    mutable std::mutex statusMutex_;
    GuiderStatus status_;
    mutable std::atomic<bool> notifyPending_{false};
    // Steady-clock ticks when the running mount call began; 0 when none runs.
    std::atomic<int64_t> callStarted_{0};

    // Guide thread only.
    LockTracker tracker_;
    std::optional<std::chrono::system_clock::time_point> lastLogTime_;
    std::unique_ptr<Mount> mount_;
    MountState mountState_;
    std::string mountError_;
    std::chrono::steady_clock::time_point lastStateRead_{};
    GuideMode mode_{GuideMode::Idle};
    std::optional<Calibrator> calibrator_;
    std::optional<GuideController> controller_;
    std::optional<GuideCalibration> calibration_;
    double tolerance_{10};
    std::string message_;
    int pulses_{0};
    std::optional<GuidePulse> lastPulse_;
    std::chrono::system_clock::time_point lastPulseTime_{};
    std::optional<std::pair<uint64_t, std::chrono::system_clock::time_point>> lastFrame_;
    std::deque<double> frameIntervals_;
    double frameInterval_{0};

    std::jthread thread_;
};
