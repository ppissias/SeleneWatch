#pragma once

#include "capture_settings.h"
#include "detection_result.h"
#include "frame.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Feature-26: auto-tune's measurements and suggestions. No widgets and no
// threads: plain functions over frames and numbers, so they can be tested on
// synthetic and recorded frames. The session measures (AutoTuneWatch); the
// window shows and applies.

// What the observer is watching for. Each has its own aim (CAMERA_SETTINGS_GUIDE.md).
enum class TuneWorkflow { MoonFlashes, MoonTransits, Sky };
[[nodiscard]] TuneWorkflow tuneWorkflow(Target target, const DetectionSettings& settings);

// Where the measured brightness should be, as shares of full scale, and the
// exposure range the workflow allows, in microseconds.
struct TuneAim
{
    double low{0}, high{0}, aim{0};
    double minExposureUs{0}, maxExposureUs{0};
    // The brightest sunlit pixels (transits), or the area's median.
    bool brightest{false};
};
// skyMaxExposureUs is the longest exposure for meteors, the observer's choice.
[[nodiscard]] TuneAim tuneAim(TuneWorkflow workflow, double skyMaxExposureUs = 250'000);

// Which pixels are measured: inside[y * width + x] != 0. Empty measures the
// whole frame.
struct TuneArea
{
    int width{0}, height{0};
    std::vector<uint8_t> inside;
    [[nodiscard]] bool wholeFrame() const { return inside.empty(); }
    [[nodiscard]] bool fits(const Frame& frame) const;
};
// The sky outside the mask; whole frame without a mask.
[[nodiscard]] TuneArea skyArea(const DetectionMask& mask, int width, int height);

// One frame's brightness in the area, as shares of full scale.
struct BrightnessSample
{
    double level{0};    // The median, or the 99.9th percentile with brightest.
    double atZero{0};   // Share of the area's samples at 0.
    double atMax{0};    // Share at the maximum code.
    size_t samples{0};
    // Feature-34: the area's temporal noise over the measurement's frames
    // (TemporalNoise), as a share of full scale; 0 when not measured.
    double noise{0};
};
// None when the area has no samples in this frame (a fully masked sky).
[[nodiscard]] std::optional<BrightnessSample> measureBrightness(const Frame& frame, const TuneArea& area, bool brightest);

// Feature-34: the area's temporal noise. Each sampled pixel's standard
// deviation over the frames added, and the median over the pixels, as a
// share of full scale. At most about 4096 pixels, spread evenly over the
// area. Never below half a code: the detector's own noise floor (0.5 ADU by
// default), which is also about the rounding of each sample.
class TemporalNoise
{
public:
    void add(const Frame& frame, const TuneArea& area);
    // None before three frames.
    [[nodiscard]] std::optional<double> median() const;

private:
    std::vector<size_t> pixels_;
    std::vector<double> mean_, m2_;
    double maxCode_{255};
    int frames_{0};
};

// Feature-26: the earthshine area grown from the observer's box, on a grid of
// cells. A cell is in it when it is connected to the box, closer to the box's
// earthshine than to the sky beyond the Moon's edge, no more than three times
// as far above that sky as the box, and has no saturated pixel. When the sky
// cannot be told from the earthshine, or the area is smaller than the box or
// covers nearly the whole frame, only the box is measured and problem says why.
struct EarthshineArea
{
    static constexpr int kCell = 8;
    int columns{0}, rows{0};
    std::vector<uint8_t> cells;   // columns * rows, 1 when in the area.
    int cellCount{0};
    double shareOfFrame{0};
    double boxLevel{0}, skyLevel{0}; // Shares of full scale.
    bool boxOnly{false};
    std::string problem;
    [[nodiscard]] TuneArea pixels(int width, int height) const;
};
struct PixelBox
{
    int x{0}, y{0}, width{0}, height{0};
};
[[nodiscard]] EarthshineArea findEarthshine(const Frame& frame, const PixelBox& box);

// The camera's controls in their native units (exposure in microseconds).
struct TuneControl
{
    double value{0}, minimum{0}, maximum{0};
    bool operator==(const TuneControl&) const = default;
};
struct TuneCamera
{
    std::optional<TuneControl> exposure, gain, offset;
    bool operator==(const TuneCamera&) const = default;
};

// One suggested change, shown as a row the observer can tick.
struct TuneRow
{
    enum class Kind { Exposure, Gain, Offset, FollowExposure, FollowGain };
    Kind kind{Kind::Exposure};
    std::string name, now, suggested, why;
    // The values to apply: one control's native value, or the follower's
    // limits (exposure from and to in ms; gain up to and its step).
    double value{0}, value2{0};
    bool ticked{false};
};

// A brightness measured at known camera settings. Two of them at different
// exposures and the same gain give the camera's black level (Feature-26,
// design point 8).
struct TuneMeasurement
{
    double exposureUs{0}, gain{0};
    BrightnessSample brightness;
};

struct CameraTuneInput
{
    TuneWorkflow workflow{TuneWorkflow::MoonFlashes};
    TuneCamera camera;
    std::optional<BrightnessSample> measured;
    std::optional<TuneMeasurement> previous;
    double skyMaxExposureUs{250'000};
    // The exposure follower (Sky): whether it is on, and its current limits.
    bool follow{false};
    double followFromMs{1}, followToMs{2000}, followMaxGain{0}, followGainStep{0};
};

struct CameraSuggestion
{
    std::vector<TuneRow> rows;
    std::vector<std::string> warnings;
    std::string verdict;
    bool inAim{false};
    // The aim can be reached within the camera's range and the workflow's limits.
    bool reachable{true};
};
[[nodiscard]] CameraSuggestion suggestCamera(const CameraTuneInput& input);

// Feature-34: one round of the camera loop. Auto-tune sets exposure and gain
// live, measures, and moves again until the brightness is in the aim.
//
// history holds the loop's measurements, the last one taken at the camera's
// current settings. One control moves at a time, in the workflow's order
// (as suggestCamera). Its next value comes from the measurements taken while
// only it moved: exposure on a logarithmic scale, gain from the measured
// response of the last two, and never past a measurement on either side of
// the aim. Saturated measurements only bound the move.
struct CameraStep
{
    enum class Outcome { Move, InAim, OutOfReach };
    Outcome outcome{Outcome::Move};
    // The settings to apply next; for InAim and OutOfReach, the current ones.
    double exposureUs{0}, gain{0};
    // What was measured and what the round does, for the panel and the log.
    std::string text;
};
[[nodiscard]] CameraStep nextCameraStep(TuneWorkflow workflow, const TuneCamera& camera,
    const std::vector<TuneMeasurement>& history, double skyMaxExposureUs = 250'000);
// Rounds after which the loop gives up: the brightness may be changing.
constexpr int kMaxCameraRounds = 12;

// Feature-34, stage 2: the gain that lets the faintest flash or meteor through.
//
// At a fixed exposure the background puts the same number of electrons in a
// pixel whatever the gain. The threshold in photons, K × noise / (ADU per
// electron), is therefore proportional to noise / signal, both in ADU: the
// gain with the highest background signal-to-noise has the lowest threshold,
// with no need to know the camera's conversion factor. The signal is the
// level above the black level, which a very short ("dark") exposure at that
// gain measures. Only the Moon's flashes and the sky are refined; for
// transits the gain stays as low as the exposure allows.
[[nodiscard]] bool refinesGain(TuneWorkflow workflow);

// One gain compared at the tuned exposure, levels and noise as shares of full scale.
struct GainCandidate
{
    double gain{0}, level{0}, black{0}, noise{0};
    [[nodiscard]] double signalToNoise() const { return noise > 0 ? std::max(0.0, level - black) / noise : 0; }
};

// What refining does next, from the loop's measurements. Two gains are
// compared at the exposure stage 1 reached: the gain it reached, and the
// highest gain measured in the aim. Dark measurements are taken at a
// hundredth of that exposure.
//  - MeasureDark: a compared gain has no black level yet; the reached gain's
//    comes first, so the probe aims at the signal above it.
//  - ProbeGain: no gain was measured near the top of the aim, so a higher one
//    is measured (never past a gain already found above the aim).
//  - Choose: the lower gain unless the higher one's signal-to-noise is more
//    than 5 % better; a little more gain that changes nothing keeps headroom.
struct GainRefinement
{
    enum class Action { ProbeGain, MeasureDark, Choose };
    Action action{Action::Choose};
    // The settings for the next round, or the chosen ones.
    double exposureUs{0}, gain{0};
    // Every candidate with its black level, by gain (Choose).
    std::vector<GainCandidate> candidates;
    std::string text;
};
[[nodiscard]] GainRefinement refineGain(TuneWorkflow workflow, const TuneCamera& camera, double tunedExposureUs,
    const std::vector<TuneMeasurement>& history, double skyMaxExposureUs = 250'000);
// Rounds of refining after which the best so far is chosen.
constexpr int kMaxRefineRounds = 8;

// What one K would have done during a watch.
struct TuneEvent
{
    uint64_t frameIndex{0};
    std::chrono::system_clock::time_point time{};
    ChangeKind change{ChangeKind::Rise};
    int x{0}, y{0}, width{0}, height{0};
    int pixels{0};
    double peakSigma{0};
};
struct KRowResult
{
    double k{0};
    // Feature-30: a minimum-patch watch varies this at one K; 0 in a K watch.
    int minPixels{0};
    uint64_t frames{0};
    double seconds{0};
    // Frames whose patch search was skipped because too many pixels were at
    // K: a quiet count there means nothing.
    uint64_t limitedFrames{0};
    uint64_t events{0};              // Every candidate, also past the kept list.
    std::vector<TuneEvent> kept;     // The first kMaxKept candidates.
    static constexpr size_t kMaxKept = 200;
    std::string stopped;             // Why the row stopped early, if it did.
};

// The K values a watch compares, the most wanted first: the current K, then
// one above, one below and two above, never below kMinimumK, at most count.
constexpr double kMinimumK = 5.0;
[[nodiscard]] std::vector<double> kValuesToTest(double centre, int count);

// One listed event: the candidates of different K rows at the same place and
// time (frames at most one apart, boxes touching, the same kind).
struct TuneEventGroup
{
    TuneEvent first;
    std::vector<std::pair<size_t, size_t>> members; // (row, kept index)
};
[[nodiscard]] std::vector<TuneEventGroup> groupEvents(const std::vector<KRowResult>& rows);

// A row's candidates left after the excluded groups (the observer's real events).
[[nodiscard]] uint64_t remainingEvents(const std::vector<KRowResult>& rows, size_t row,
    const std::vector<TuneEventGroup>& groups, const std::vector<bool>& excluded);

// Feature-30: the minimum patch for flashes on the earthlit side.
//
// A real flash is a point source, as wide as a star: its FWHM in pixels is
// the seeing over the image scale. Without an image scale, the Test flash
// FWHM is used and said to be an assumption.
struct FlashWidth
{
    double pixels{3};
    bool fromSeeing{false};
};
[[nodiscard]] FlashWidth flashWidth(double seeingArcsec, std::optional<double> arcsecPerPixel, double fallbackPixels);

// The values a minimum-patch watch compares: the current one, then about
// two thirds, a half and a third of it, never below kMinimumPatch.
constexpr int kMinimumPatch = 3;
[[nodiscard]] std::vector<int> minPatchValuesToTest(int current);

// The peak of the footprint flash: just above K, so it covers what a faint
// flash covers at K.
[[nodiscard]] inline double footprintAmplitude(double k, double noiseAdu) { return 1.5 * k * noiseAdu; }

// The smallest tested minimum patch whose row covered the watch, was fully
// searched and has no remaining events. With a footprint, a value above it
// would drop faint flashes: the panel then says noise limits the choice.
struct MinPatchChoice
{
    std::optional<int> minPixels;
    bool noiseLimited{false};
    std::string reason;
};
[[nodiscard]] MinPatchChoice chooseMinPatch(const std::vector<KRowResult>& rows, const std::vector<TuneEventGroup>& groups,
    const std::vector<bool>& excluded, double watchSeconds, std::optional<int> footprint);

// The lowest K whose row covered the watch, had no limited frames, and has
// no remaining events. None, with the reason, otherwise.
struct KChoice
{
    std::optional<double> k;
    std::string reason;
};
[[nodiscard]] KChoice chooseK(const std::vector<KRowResult>& rows, const std::vector<TuneEventGroup>& groups,
    const std::vector<bool>& excluded, double watchSeconds);
