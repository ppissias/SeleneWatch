#pragma once

#include "guiding/guide_step.h"

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <vector>

// The guiding decision: a tolerance box with a pause.
//
// The drift estimate is split along the two mount axes using the
// calibration. While both parts are inside the tolerance nothing is sent.
// When one leaves it, a pulse for that axis removes kGain of its error,
// never longer than kMaxPulseMs; with both out, the RA pulse goes first and
// the declination pulse after it has ended. Then the controller waits for
// the pulses to end, for the settle time, and for a fresh estimate before
// it decides again. Declination is not reversed for an overshoot smaller
// than twice the tolerance, since its drift keeps one sign for days and
// each reversal costs the backlash.
//
// It stops, with the reason, when the mount may not be pulsed, when the
// mount changes pier side (the calibration no longer holds), when
// kMaxFruitlessCorrections corrections in a row do not reduce the error,
// when the pulse time of the last minute would pass its cap, or when a pulse
// does not end. A stop is final: the observer looks, and starts guiding
// again. A plain state machine stepped with observations.
class GuideController
{
public:
    struct Settings
    {
        double tolerancePixels{10};
        double gain{0.7};
        int maxPulseMs{1500};
        int minPulseMs{20};
        double settleSeconds{1.0};
        int maxFruitlessCorrections{3};
        int maxPulseMsPerMinute{20000};
        double maxGuideSpeedPixelsPerFrame{0.25};
    };

    enum class Phase { Watching, Pulsing, Settling, Measuring, Stopped };

    GuideController(GuideCalibration calibration, Settings settings);

    // Why guiding cannot start with this calibration and observation, or empty.
    [[nodiscard]] static std::string refusal(const GuideCalibration& calibration, const Settings& settings,
        const GuideObservation& observation);
    // Image pixels per frame that a pulse moves the image at, or 0 when the frame rate is unknown.
    [[nodiscard]] static double guideSpeedPixelsPerFrame(const GuideCalibration& calibration, double frameInterval);

    GuideDecision step(const GuideObservation& observation);

    // Changes the tolerance; applied at the next decision.
    void setTolerance(double pixels) { settings_.tolerancePixels = pixels; }

    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] bool stopped() const { return phase_ == Phase::Stopped; }
    [[nodiscard]] const std::string& stopReason() const { return stopReason_; }
    // The mount changed pier side; the calibration must be discarded.
    [[nodiscard]] bool calibrationInvalid() const { return calibrationInvalid_; }
    [[nodiscard]] int corrections() const { return corrections_; }
    [[nodiscard]] int pulses() const { return pulses_; }
    [[nodiscard]] int declinationReversalsWithheld() const { return reversalsWithheld_; }
    // The latest estimate split along the mount axes, in image pixels: the
    // RA part along the west vector, the declination part along the north vector.
    [[nodiscard]] double raErrorPixels() const { return raError_; }
    [[nodiscard]] double decErrorPixels() const { return decError_; }

private:
    void stop(std::string reason);
    GuideDecision sendNext(const GuideObservation& observation);
    GuideDecision decide(const GuideObservation& observation);

    GuideCalibration calibration_;
    Settings settings_;
    Phase phase_{Phase::Watching};
    std::string stopReason_;
    bool calibrationInvalid_{false};

    std::vector<GuidePulse> queue_;
    std::optional<std::chrono::steady_clock::time_point> pulseEnd_;
    std::chrono::steady_clock::time_point settleUntil_{};
    std::deque<std::pair<std::chrono::steady_clock::time_point, int>> recentPulses_;
    std::optional<GuideDirection> lastDecDirection_;
    double errorBefore_{0};
    int fruitless_{0};
    int corrections_{0};
    int pulses_{0};
    int reversalsWithheld_{0};
    double raError_{0}, decError_{0};
};
