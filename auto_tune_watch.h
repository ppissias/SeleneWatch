#pragma once

#include "auto_tune.h"
#include "blip.h"
#include "flash_detector.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Feature-26: what one auto-tune measurement is asked to do.
struct TuneWatchRequest
{
    enum class Kind
    {
        Brightness,   // The camera step: the area's brightness.
        Sensitivity,  // The K step: what each K would have triggered.
        FlashCheck    // Whether the live detector finds a marked test flash.
    };
    Kind kind{Kind::Brightness};
    double seconds{2};
    TuneArea area;
    bool brightest{false};
    // The detection settings the watch is bound to: a change cancels it.
    DetectionSettings settings;
    // Sensitivity: the K values to compare, the most wanted first.
    std::vector<double> kValues;
    // FlashCheck: the flash the session injects once the stream has settled.
    BlipSettings flash;
};

// What a measurement found so far, or at its end.
struct TuneWatchStatus
{
    enum class State { Idle, Waiting, Measuring, Finished, Cancelled };
    State state{State::Idle};
    uint64_t id{0};
    TuneWatchRequest::Kind kind{TuneWatchRequest::Kind::Brightness};
    double requestedSeconds{0};
    // Why it waits, why it stopped, or why fewer K values were compared.
    std::string note;
    double progress{0};
    uint64_t frames{0};
    double seconds{0};
    // The median over the measured frames; none without samples.
    std::optional<BrightnessSample> brightness;
    // Sensitivity: the median per-pixel noise in the area, as the detector
    // uses it, and the samples' format.
    double areaNoiseAdu{0};
    PixelFormat format{PixelFormat::Mono8};
    int significantBits{8};
    std::vector<KRowResult> rows;
    // Only the live detector ran (no room for copies): one row, at the current K.
    bool liveOnly{false};
    // FlashCheck.
    std::optional<bool> recovered;
    std::string checkText;
};

// Feature-26: one auto-tune measurement, run on the source thread.
//
// The window starts, cancels and reads it on its own thread. The session
// feeds it each frame in Preview, after the live detector has analysed and
// learned from that frame. It never changes a frame or a setting:
// - Brightness measures the area's brightness on every frame.
// - Sensitivity runs complete copies of the live detector, one per K value,
//   all starting from the same learned background (a copy, re-armed), and
//   counts the candidates each would have produced. How many copies run
//   depends on the frame time left over and on the processing reserve of the
//   RAM ceiling; with no room, the live detector's own candidates are counted
//   at the current K.
// - FlashCheck asks the session to inject one marked test flash, and watches
//   whether the live detector finds it at its place and time.
// It waits for the camera to settle and the detector to learn before it
// measures, and stops when its settings, the mode or the camera change.
class AutoTuneWatch
{
public:
    // Context thread. Replaces any measurement in progress; returns its id.
    uint64_t start(TuneWatchRequest request);
    // Any thread. Cancels the current measurement, if there is one.
    void cancel(const std::string& reason);
    [[nodiscard]] TuneWatchStatus status() const;
    // A measurement is pending, waiting or running.
    [[nodiscard]] bool busy() const { return busy_.load(); }

    // Source thread: what the session knows about this frame.
    struct Context
    {
        DetectionSettings settings;
        bool preview{true};
        bool settling{false};
        // Bytes of the processing reserve that the copies may use.
        size_t spareBytes{0};
    };
    // Source thread, after the live detector learned from frame. Returns a
    // flash for the session to inject (FlashCheck), once.
    std::optional<BlipSettings> onFrame(const Frame& frame, const FlashDetector& live,
        const std::vector<DetectionSummary>& liveCandidates, const Context& context);
    // Source thread: the source ended or jumped (a loop). A measurement ends
    // with what it has, and says why.
    void sourceInterrupted(const std::string& reason);
    // Source thread, or while it is joined: the stream stopped.
    void stopped(const std::string& reason);
    // Source thread: whether the window should hear about a change now
    // (a new state, or a quarter of a second of progress).
    bool takeChanged(std::chrono::steady_clock::time_point now);
    // Source thread: a flash check has asked for its flash and not finished.
    [[nodiscard]] bool injecting() const { return injecting_; }

private:
    struct Row
    {
        std::unique_ptr<FlashDetector> detector;
        KRowResult result;
    };
    void begin(const Frame& frame, const FlashDetector& live, const Context& context);
    void measure(const Frame& frame, const FlashDetector& live, const std::vector<DetectionSummary>& liveCandidates);
    void finish(TuneWatchStatus::State state, const std::string& note);
    void publish();
    static void record(KRowResult& result, const FlashDetector& detector, const std::vector<DetectionSummary>& candidates,
        bool darkening);

    mutable std::mutex mutex_;
    std::optional<TuneWatchRequest> pending_;
    std::optional<std::string> cancelRequest_;
    TuneWatchStatus published_;
    uint64_t nextId_{1};
    std::atomic<bool> busy_{false};

    // Source thread only.
    std::optional<TuneWatchRequest> request_;
    TuneWatchStatus working_;
    std::vector<Row> rows_;
    std::vector<double> levels_, atZero_, atMax_;
    std::chrono::system_clock::time_point started_{}, lastTime_{};
    uint64_t lastIndex_{0};
    bool haveLast_{false};
    double interval_{0};
    bool settledOnce_{false};
    bool changed_{false};
    std::chrono::steady_clock::time_point lastNotice_{};
    // FlashCheck.
    bool injecting_{false};
    std::optional<uint64_t> flashFirst_, flashLast_;
    std::chrono::system_clock::time_point flashAsked_{};
};
