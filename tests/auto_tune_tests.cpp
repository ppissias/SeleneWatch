#include "auto_tune.h"
#include "auto_tune_watch.h"
#include "blip_injector.h"
#include "flash_detector.h"

#include <QtTest/QTest>

#include <cmath>
#include <functional>
#include <random>

namespace {

using namespace std::chrono;

constexpr int kWidth = 128, kHeight = 96;

// A Mono8 frame at 30 fps: a level, Gaussian noise of 2 ADU, and what draw adds.
std::shared_ptr<Frame> frame(uint64_t index, std::mt19937& random, const std::function<double(int x, int y)>& draw = {},
    double level = 50)
{
    auto made = std::make_shared<Frame>();
    made->width = kWidth; made->height = kHeight; made->frameIndex = index;
    made->timestamp = system_clock::time_point{} + seconds{1'790'000'000} + microseconds{static_cast<int64_t>(index) * 33'333};
    std::normal_distribution<double> noise(0, 2);
    made->data.resize(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            made->data[static_cast<size_t>(y) * kWidth + x] = static_cast<uint8_t>(std::clamp(
                std::lround(level + (draw ? draw(x, y) : 0.0) + noise(random)), 0L, 255L));
    return made;
}

// A frame of one value everywhere a draw does not say otherwise, without noise.
std::shared_ptr<Frame> flat(const std::function<double(int x, int y)>& draw)
{
    auto made = std::make_shared<Frame>();
    made->width = kWidth; made->height = kHeight;
    made->data.resize(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            made->data[static_cast<size_t>(y) * kWidth + x] = static_cast<uint8_t>(std::clamp(std::lround(draw(x, y)), 0L, 255L));
    return made;
}

DetectionSettings quickSettings()
{
    DetectionSettings settings;
    settings.sigma = 6;
    settings.minPixels = 8;
    settings.maxPixels = 20;
    settings.referenceTimeConstantSeconds = 0.5;
    return settings;
}

// Drives a watch as the session does: the live detector analyses and learns
// from each frame first. Frames with a draw for their index get it.
struct Replay
{
    FlashDetector live;
    AutoTuneWatch watch;
    AutoTuneWatch::Context context;
    std::mt19937 random{7};
    uint64_t index{0};
    BlipInjector blip;

    explicit Replay(const DetectionSettings& settings) : live(settings)
    {
        context.settings = settings;
        context.spareBytes = size_t{1} << 30;
    }
    void run(int frames, const std::function<double(uint64_t index, int x, int y)>& draw = {})
    {
        for (int i = 0; i < frames; ++i, ++index)
        {
            const uint64_t at = index;
            auto made = frame(at, random, draw ? std::function<double(int, int)>([&](int x, int y) { return draw(at, x, y); })
                : std::function<double(int, int)>{});
            blip.apply(*made);
            live.analyze(*made);
            live.updateReference(*made);
            if (const auto flash = watch.onFrame(*made, live, live.candidates(), context)) QVERIFY(blip.request(*flash));
            watch.takeChanged(steady_clock::now()); // As the session does: publishes state changes.
            if (watch.status().state == TuneWatchStatus::State::Finished || watch.status().state == TuneWatchStatus::State::Cancelled)
                if (!watch.busy()) return;
        }
    }
};

TuneWatchRequest sensitivity(const DetectionSettings& settings, std::vector<double> kValues, double seconds)
{
    TuneWatchRequest request;
    request.kind = TuneWatchRequest::Kind::Sensitivity;
    request.seconds = seconds;
    request.settings = settings;
    request.kValues = std::move(kValues);
    return request;
}

// A camera whose raw level is its black level plus a slope times the
// exposure, multiplied by 2 for every gainPerStop gain units (60 unless
// changed), clipped at full scale.
struct ModelCamera
{
    double pedestal{0.02}, perMillisecond{0.001};
    TuneControl exposure{30'000, 32, 2'000'000}, gain{100, 0, 500};
    double gainPerStop{60};
    [[nodiscard]] double level(double brightnessScale = 1) const
    {
        return std::min(1.0, pedestal + perMillisecond * brightnessScale * exposure.value / 1000
            * std::pow(2.0, gain.value / gainPerStop));
    }
};

// Feature-34: the camera loop as the window runs it. Each round measures,
// asks for the next step and sets it, as the live camera would.
struct LoopRun
{
    CameraStep::Outcome outcome{CameraStep::Outcome::Move};
    int rounds{0};
    ModelCamera camera;
    std::string text;
    // Every setting stayed within the camera's range, and the exposure within
    // the workflow's once it was brought there.
    bool withinLimits{true};
};
LoopRun runLoop(TuneWorkflow workflow, ModelCamera camera)
{
    const TuneAim aim = tuneAim(workflow);
    std::vector<TuneMeasurement> history;
    LoopRun run;
    for (int round = 1; round <= kMaxCameraRounds; ++round)
    {
        BrightnessSample measured;
        measured.level = camera.level();
        measured.atMax = measured.level >= 1 ? 0.01 : 0;
        measured.samples = 1000;
        history.push_back({camera.exposure.value, camera.gain.value, measured});
        const CameraStep step = nextCameraStep(workflow, TuneCamera{camera.exposure, camera.gain, std::nullopt}, history);
        run.rounds = round;
        run.text = step.text;
        if (step.outcome != CameraStep::Outcome::Move)
        {
            run.outcome = step.outcome;
            break;
        }
        const bool exposureWasInRange = camera.exposure.value >= aim.minExposureUs && camera.exposure.value <= aim.maxExposureUs;
        camera.exposure.value = step.exposureUs;
        camera.gain.value = step.gain;
        if (camera.gain.value < camera.gain.minimum || camera.gain.value > camera.gain.maximum
            || camera.exposure.value < camera.exposure.minimum || camera.exposure.value > camera.exposure.maximum
            || (exposureWasInRange && (camera.exposure.value < aim.minExposureUs || camera.exposure.value > aim.maxExposureUs)))
            run.withinLimits = false;
    }
    run.camera = camera;
    return run;
}

// Measure, apply the ticked rows, measure again: as the observer would.
bool reachesTheAim(TuneWorkflow workflow, ModelCamera camera, int rounds, double brightness = 1)
{
    const TuneAim aim = tuneAim(workflow);
    std::optional<TuneMeasurement> previous;
    for (int round = 0; round < rounds; ++round)
    {
        BrightnessSample measured;
        measured.level = camera.level(brightness);
        measured.atMax = measured.level >= 1 ? 0.01 : 0;
        measured.samples = 1000;
        CameraTuneInput input;
        input.workflow = workflow;
        input.camera.exposure = camera.exposure;
        input.camera.gain = camera.gain;
        input.measured = measured;
        input.previous = previous;
        const CameraSuggestion suggestion = suggestCamera(input);
        if (suggestion.inAim && (workflow != TuneWorkflow::MoonFlashes
            || (camera.exposure.value >= aim.minExposureUs && camera.exposure.value <= aim.maxExposureUs))) return true;
        previous = TuneMeasurement{camera.exposure.value, camera.gain.value, measured};
        for (const TuneRow& row : suggestion.rows)
        {
            if (!row.ticked) continue;
            if (row.kind == TuneRow::Kind::Exposure) camera.exposure.value = row.value;
            if (row.kind == TuneRow::Kind::Gain) camera.gain.value = row.value;
        }
    }
    return false;
}

} // namespace

class AutoTuneTest : public QObject
{
    Q_OBJECT
private slots:
    void brightnessIsMeasuredOverTheArea();
    void theEarthshineAreaGrowsFromTheBox();
    void aTightCropMeasuresTheBoxAlone();
    void eachWorkflowReachesItsAim();
    void theMoonKeepsItsExposureForFlashesAndUsesGain();
    void theSkyLengthensTheExposureThenRaisesGain();
    void aBrighterSkyLowersGainFirst();
    void twoMeasurementsGiveTheBlackLevel();
    void aFullyMaskedSkyGivesNoSuggestion();
    void theFollowersLimitsAreOffered();
    void theLoopReachesEachAimInAFewRounds();
    void theLoopLearnsAnyGainCurve();
    void saturationOnlyBoundsTheLoopsMove();
    void theLoopStopsWhenTheAimIsOutOfReach();
    void kValuesAreTestedAroundTheCurrentOne();
    void eachKRowCountsWhatThatKAloneWould();
    void aSkippedSearchIsNeverAQuietK();
    void excludingAnEventLeavesOthersCounted();
    void theWatchWaitsForTheDetectorAndStopsOnAChange();
    void aLoopEndsTheWatchWithItsCoverage();
    void aTestFlashIsCaughtOrMissed();
    void theFlashWidthComesFromSeeingOrTheTestFlash();
    void theMinimumPatchIsTheSmallestQuietValueWithinTheFootprint();
    void theFootprintOfAJustDetectableFlashIsMeasured();
    void minimumPatchRowsCountWhatEachValueWould();
};

// Feature-30: a flash just above K covers a few pixels at K; a wider flash
// of the same peak in K covers more. The peak is set from the noise there.
void AutoTuneTest::theFootprintOfAJustDetectableFlashIsMeasured()
{
    int previous = 0;
    for (const double fwhm : {3.0, 6.0})
    {
        const DetectionSettings settings = quickSettings();
        Replay replay(settings);
        replay.run(90);
        TuneWatchRequest request;
        request.kind = TuneWatchRequest::Kind::Footprint;
        request.settings = settings;
        request.flash = BlipSettings{64, 48, fwhm, 0, 2, BlipKind::BrightFlash};
        replay.watch.start(request);
        replay.run(20);
        const TuneWatchStatus status = replay.watch.status();
        QCOMPARE(status.state, TuneWatchStatus::State::Finished);
        QVERIFY2(status.footprintPixels, status.checkText.c_str());
        // About 0.46 × FWHM² for a peak of 1.5 K: 4 px at 3 px wide, 17 px at 6 px wide.
        const double expected = 0.46 * fwhm * fwhm;
        QVERIFY2(*status.footprintPixels >= expected * 0.4 && *status.footprintPixels <= expected * 2.5,
            qPrintable(QString("FWHM %1: %2 px").arg(fwhm).arg(*status.footprintPixels)));
        QVERIFY(*status.footprintPixels > previous);
        previous = *status.footprintPixels;
        // The peak was set from the noise at the flash: 1.5 × K × about 2 ADU.
        QVERIFY(status.footprintPeakAdu > 1.5 * 6 * 1.5 && status.footprintPeakAdu < 1.5 * 6 * 3);
    }
}

// Feature-30: rows at one K and different minimum patches each count what
// that minimum patch alone would: a 5-pixel patch passes at 3 and 5, not at 8.
void AutoTuneTest::minimumPatchRowsCountWhatEachValueWould()
{
    DetectionSettings settings = quickSettings();
    settings.minPixels = 8;
    Replay replay(settings);
    replay.run(90);
    TuneWatchRequest request = sensitivity(settings, {}, 1.0);
    request.minPixelValues = minPatchValuesToTest(8);
    replay.watch.start(request);
    // A plus-shaped patch of 5 pixels, 15 sigma, on one frame.
    replay.run(40, [](uint64_t index, int x, int y) {
        if (index != 100) return 0.0;
        const int dx = std::abs(x - 64), dy = std::abs(y - 48);
        return (dx == 0 && dy <= 1) || (dy == 0 && dx <= 1) ? 30.0 : 0.0;
    });
    const TuneWatchStatus status = replay.watch.status();
    QCOMPARE(status.state, TuneWatchStatus::State::Finished);
    QCOMPARE(status.rows.size(), size_t{4});
    for (const KRowResult& row : status.rows)
    {
        QCOMPARE(row.k, 6.0);
        QCOMPARE(row.events, row.minPixels <= 5 ? uint64_t{1} : uint64_t{0});
    }
    const auto groups = groupEvents(status.rows);
    const MinPatchChoice choice = chooseMinPatch(status.rows, groups, std::vector<bool>(groups.size(), false), 1.0, 6);
    QVERIFY(choice.minPixels);
    QCOMPARE(*choice.minPixels, 8);
}

// Feature-30: a point source is as wide as the seeing over the image scale;
// without an image scale, the Test flash FWHM is the assumption.
void AutoTuneTest::theFlashWidthComesFromSeeingOrTheTestFlash()
{
    const FlashWidth measured = flashWidth(2.5, 0.5, 3);
    QVERIFY(measured.fromSeeing);
    QCOMPARE(measured.pixels, 5.0);
    const FlashWidth assumed = flashWidth(2.5, std::nullopt, 3);
    QVERIFY(!assumed.fromSeeing);
    QCOMPARE(assumed.pixels, 3.0);
    QCOMPARE(minPatchValuesToTest(8), (std::vector<int>{8, 5, 4, 3}));
    QCOMPARE(minPatchValuesToTest(20), (std::vector<int>{20, 13, 10, 7}));
    QCOMPARE(minPatchValuesToTest(3), (std::vector<int>{3}));
    QCOMPARE(footprintAmplitude(6, 2), 18.0);
}

void AutoTuneTest::theMinimumPatchIsTheSmallestQuietValueWithinTheFootprint()
{
    const auto row = [](int minPixels, uint64_t events) {
        KRowResult r;
        r.k = 6;
        r.minPixels = minPixels;
        r.frames = 1800;
        r.seconds = 60;
        r.events = events;
        for (uint64_t e = 0; e < events; ++e)
            r.kept.push_back({100 + e * 10, {}, ChangeKind::Rise, 10 * static_cast<int>(e), 10, 2, 2, minPixels, 7});
        return r;
    };
    // 3 px triggers on noise, 4 px and more are quiet.
    std::vector<KRowResult> rows{row(8, 0), row(5, 0), row(4, 0), row(3, 2)};
    auto groups = groupEvents(rows);
    std::vector<bool> none(groups.size(), false);
    MinPatchChoice choice = chooseMinPatch(rows, groups, none, 60, 6);
    QVERIFY(choice.minPixels && *choice.minPixels == 4 && !choice.noiseLimited);
    // Excluding the real events makes 3 px quiet too.
    std::vector<bool> all(groups.size(), true);
    QCOMPARE(*chooseMinPatch(rows, groups, all, 60, 6).minPixels, 3);
    // A footprint smaller than the smallest quiet value: noise limits the choice, and the panel says so.
    choice = chooseMinPatch(rows, groups, none, 60, 3);
    QVERIFY(choice.minPixels && *choice.minPixels == 4 && choice.noiseLimited);
    QVERIFY(choice.reason.find("cannot be told from noise") != std::string::npos);
    // A skipped patch search or a short watch is never quiet.
    rows[2].limitedFrames = 1;
    rows[1].seconds = 20;
    groups = groupEvents(rows);
    QCOMPARE(*chooseMinPatch(rows, groups, std::vector<bool>(groups.size(), false), 60, 6).minPixels, 8);
}

void AutoTuneTest::brightnessIsMeasuredOverTheArea()
{
    // The left half at 200, the right half at 20.
    const auto made = flat([](int x, int) { return x < kWidth / 2 ? 200.0 : 20.0; });
    TuneArea right;
    right.width = kWidth; right.height = kHeight;
    right.inside.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    for (int y = 0; y < kHeight; ++y)
        for (int x = kWidth / 2; x < kWidth; ++x) right.inside[static_cast<size_t>(y) * kWidth + x] = 1;
    const auto measured = measureBrightness(*made, right, false);
    QVERIFY(measured);
    QCOMPARE(measured->level, 20.0 / 255);
    QCOMPARE(measured->atZero, 0.0);
    // The 99.9th percentile of the whole frame is the bright half.
    QCOMPARE(measureBrightness(*made, {}, true)->level, 200.0 / 255);
    // Pixels at 0 are counted.
    const auto dark = flat([](int x, int) { return x < 32 ? 0.0 : 30.0; });
    QCOMPARE(measureBrightness(*dark, {}, false)->atZero, 0.25);
    // An area with no pixels has no brightness.
    TuneArea none = right;
    std::fill(none.inside.begin(), none.inside.end(), uint8_t{0});
    QVERIFY(!measureBrightness(*made, none, false));
}

// A crescent: the sky beyond the Moon's edge at 4, earthshine at 16 with a
// darker mare at 11, and the sunlit limb saturated with glare beside it.
std::shared_ptr<Frame> crescent()
{
    return flat([](int x, int y) {
        const double r = std::hypot(x - 64.0, y - 48.0);
        if (r > 40) return 4.0;                       // Sky.
        if (x > 92) return 255.0;                      // The sunlit limb.
        if (x > 84) return 60.0 + (x - 84) * 20.0;     // Glare near the terminator.
        if (std::hypot(x - 50.0, y - 40.0) < 10) return 11.0; // A mare.
        return 16.0;                                  // Earthshine.
    });
}

void AutoTuneTest::theEarthshineAreaGrowsFromTheBox()
{
    const auto moon = crescent();
    const EarthshineArea area = findEarthshine(*moon, {40, 50, 16, 16});
    QVERIFY2(!area.boxOnly, area.problem.c_str());
    QVERIFY(area.cellCount > 40);
    QVERIFY(std::abs(area.boxLevel - 16.0 / 255) < 0.005);
    QVERIFY(std::abs(area.skyLevel - 4.0 / 255) < 0.005);
    // The area is earthshine and the mare. Its 8 x 8 cells along the Moon's
    // edge mix in a little sky or glare, but nothing sunlit.
    const TuneArea pixels = area.pixels(kWidth, kHeight);
    int inside = 0, earthshine = 0, sky = 0, glare = 0, sunlit = 0;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
        {
            if (!pixels.inside[static_cast<size_t>(y) * kWidth + x]) continue;
            ++inside;
            const double value = moon->data[static_cast<size_t>(y) * kWidth + x];
            if (value == 16 || value == 11) ++earthshine;
            else if (value == 4) ++sky;
            else if (value == 255) ++sunlit;
            else ++glare;
        }
    QVERIFY(inside > 2000);
    QCOMPARE(sunlit, 0);
    QVERIFY2(earthshine > 0.9 * inside && glare < 0.03 * inside && sky < 0.08 * inside,
        qPrintable(QString("earthshine %1, sky %2, glare %3 of %4").arg(earthshine).arg(sky).arg(glare).arg(inside)));
    // Most of the earthshine is in it.
    int allEarthshine = 0;
    for (const uint8_t value : moon->data) allEarthshine += value == 16 || value == 11;
    QVERIFY(earthshine > 0.8 * allEarthshine);
    const auto measured = measureBrightness(*moon, pixels, false);
    QVERIFY(measured && std::abs(measured->level - 16.0 / 255) < 0.005);
}

void AutoTuneTest::aTightCropMeasuresTheBoxAlone()
{
    // Earthshine and a mare fill the frame: no sky beyond the edge to compare with.
    const auto crop = flat([](int x, int y) { return std::hypot(x - 30.0, y - 30.0) < 20 ? 11.0 : 16.0; });
    const EarthshineArea area = findEarthshine(*crop, {64, 40, 16, 16});
    QVERIFY(area.boxOnly);
    QVERIFY(!area.problem.empty());
    QCOMPARE(area.cellCount, 4);
    // A box on the saturated limb is refused.
    const EarthshineArea sunlit = findEarthshine(*crescent(), {96, 44, 4, 4});
    QVERIFY(sunlit.boxOnly && sunlit.cellCount == 0);
}

void AutoTuneTest::eachWorkflowReachesItsAim()
{
    ModelCamera dark;
    dark.perMillisecond = 0.0002;
    QVERIFY(reachesTheAim(TuneWorkflow::MoonFlashes, dark, 6));
    ModelCamera bright;
    bright.perMillisecond = 0.004;
    QVERIFY(reachesTheAim(TuneWorkflow::MoonFlashes, bright, 6));
    // Transits: a sunlit Moon, saturated at first.
    ModelCamera sunlit;
    sunlit.perMillisecond = 0.2;
    sunlit.exposure.value = 20'000;
    QVERIFY(reachesTheAim(TuneWorkflow::MoonTransits, sunlit, 8));
    // The sky at night, and at dusk.
    ModelCamera night;
    night.perMillisecond = 0.0004;
    night.exposure.value = 50'000;
    QVERIFY(reachesTheAim(TuneWorkflow::Sky, night, 8));
    ModelCamera dusk;
    dusk.perMillisecond = 0.01;
    dusk.exposure.value = 100'000;
    QVERIFY(reachesTheAim(TuneWorkflow::Sky, dusk, 8));
}

void AutoTuneTest::theMoonKeepsItsExposureForFlashesAndUsesGain()
{
    CameraTuneInput input;
    input.workflow = TuneWorkflow::MoonFlashes;
    input.camera.exposure = TuneControl{30'000, 32, 2'000'000};
    input.camera.gain = TuneControl{100, 0, 500};
    input.measured = BrightnessSample{0.015, 0, 0, 1000};
    CameraSuggestion s = suggestCamera(input);
    QVERIFY(!s.inAim && s.reachable);
    QCOMPARE(s.rows[0].kind, TuneRow::Kind::Exposure);
    QVERIFY(!s.rows[0].ticked);
    QCOMPARE(s.rows[0].value, 30'000.0);
    QCOMPARE(s.rows[1].kind, TuneRow::Kind::Gain);
    QVERIFY(s.rows[1].ticked && s.rows[1].value > 100 && s.rows[1].value <= 100 + 500.0 / 4);
    // An exposure longer than 40 ms is brought back first.
    input.camera.exposure->value = 50'000;
    input.measured = BrightnessSample{0.06, 0, 0, 1000};
    s = suggestCamera(input);
    QVERIFY(s.rows[0].ticked);
    QCOMPARE(s.rows[0].value, 40'000.0);
    // Pixels at 0 warn, and the offset is raised when the camera has one.
    input.camera.offset = TuneControl{10, 0, 320};
    input.measured = BrightnessSample{0.06, 0.02, 0, 1000};
    s = suggestCamera(input);
    QVERIFY(!s.warnings.empty());
    QVERIFY(std::any_of(s.rows.begin(), s.rows.end(), [](const TuneRow& r) { return r.kind == TuneRow::Kind::Offset && r.ticked && r.value == 20; }));
}

void AutoTuneTest::theSkyLengthensTheExposureThenRaisesGain()
{
    CameraTuneInput input;
    input.workflow = TuneWorkflow::Sky;
    input.camera.exposure = TuneControl{100'000, 32, 2'000'000};
    input.camera.gain = TuneControl{200, 0, 600};
    input.measured = BrightnessSample{0.05, 0, 0, 1000};
    CameraSuggestion s = suggestCamera(input);
    QVERIFY(s.rows[0].ticked && s.rows[0].value > 100'000 && s.rows[0].value <= 250'000);
    QVERIFY(!s.rows[1].ticked);
    // At the longest exposure for meteors, gain rises.
    input.camera.exposure->value = 250'000;
    s = suggestCamera(input);
    QVERIFY(!s.rows[0].ticked);
    QVERIFY(s.rows[1].ticked && s.rows[1].value > 200);
    // At the gain's highest too, the aim is out of reach and the panel says so.
    input.camera.gain->value = 600;
    s = suggestCamera(input);
    QVERIFY(!s.reachable);
    QVERIFY(s.verdict.find("cannot reach") != std::string::npos);
}

void AutoTuneTest::aBrighterSkyLowersGainFirst()
{
    CameraTuneInput input;
    input.workflow = TuneWorkflow::Sky;
    input.camera.exposure = TuneControl{100'000, 32, 2'000'000};
    input.camera.gain = TuneControl{200, 0, 600};
    input.measured = BrightnessSample{0.6, 0, 0, 1000};
    CameraSuggestion s = suggestCamera(input);
    QVERIFY(!s.rows[0].ticked);
    QVERIFY(s.rows[1].ticked && s.rows[1].value < 200);
    input.camera.gain->value = 0;
    s = suggestCamera(input);
    QVERIFY(s.rows[0].ticked && s.rows[0].value < 100'000);
}

void AutoTuneTest::twoMeasurementsGiveTheBlackLevel()
{
    // A black level of 10 % with the sky at 12 %: proportion alone would
    // overshoot; two measurements land on the aim.
    ModelCamera camera;
    camera.pedestal = 0.05;
    camera.perMillisecond = 0.0004;
    camera.gain.value = 0;
    camera.exposure.value = 50'000;
    CameraTuneInput input;
    input.workflow = TuneWorkflow::Sky;
    input.camera.exposure = camera.exposure;
    input.camera.gain = camera.gain;
    input.measured = BrightnessSample{camera.level(), 0, 0, 1000};
    input.previous = TuneMeasurement{25'000, 0, BrightnessSample{0.05 + 0.0004 * 25, 0, 0, 1000}};
    const CameraSuggestion s = suggestCamera(input);
    QVERIFY(s.rows[0].ticked);
    camera.exposure.value = s.rows[0].value;
    QVERIFY2(std::abs(camera.level() - 0.15) < 0.002, qPrintable(QString::number(camera.level())));
    QVERIFY(s.rows[0].why.find("black level") != std::string::npos);
}

void AutoTuneTest::aFullyMaskedSkyGivesNoSuggestion()
{
    CameraTuneInput input;
    input.workflow = TuneWorkflow::Sky;
    input.camera.exposure = TuneControl{100'000, 32, 2'000'000};
    const CameraSuggestion s = suggestCamera(input);
    QVERIFY(s.rows.empty() && !s.reachable);
    QVERIFY(s.verdict.find("mask covers the whole frame") != std::string::npos);
    const TuneArea masked = skyArea(DetectionMask({DetectionMask::rectangle(0, 0, kWidth, kHeight)}), kWidth, kHeight);
    QVERIFY(!measureBrightness(*flat([](int, int) { return 40.0; }), masked, false));
}

void AutoTuneTest::theFollowersLimitsAreOffered()
{
    CameraTuneInput input;
    input.workflow = TuneWorkflow::Sky;
    input.camera.exposure = TuneControl{100'000, 32, 2'000'000};
    input.camera.gain = TuneControl{200, 0, 600};
    input.measured = BrightnessSample{0.15, 0, 0, 1000};
    QCOMPARE(suggestCamera(input).rows.size(), size_t{2});
    input.follow = true;
    const CameraSuggestion s = suggestCamera(input);
    QCOMPARE(s.rows.size(), size_t{4});
    const TuneRow& exposure = s.rows[2];
    QCOMPARE(exposure.kind, TuneRow::Kind::FollowExposure);
    QVERIFY(exposure.ticked);
    QCOMPARE(exposure.value, 0.1);
    QCOMPARE(exposure.value2, 250.0);
    const TuneRow& gain = s.rows[3];
    QCOMPARE(gain.kind, TuneRow::Kind::FollowGain);
    QCOMPARE(gain.value, 350.0);   // 200 + a quarter of 600.
    QCOMPARE(gain.value2, 30.0);   // A twentieth of 600.
}

void AutoTuneTest::theLoopReachesEachAimInAFewRounds()
{
    const auto check = [](TuneWorkflow workflow, const ModelCamera& camera, int maxRounds, const char* name) {
        const LoopRun run = runLoop(workflow, camera);
        QVERIFY2(run.outcome == CameraStep::Outcome::InAim && run.rounds <= maxRounds && run.withinLimits,
            qPrintable(QString("%1: %2 rounds, %3").arg(name).arg(run.rounds).arg(QString::fromStdString(run.text))));
    };
    // Earthshine far too dark, most of the level the black level: gain only.
    ModelCamera dark;
    dark.perMillisecond = 0.00002;
    check(TuneWorkflow::MoonFlashes, dark, 5, "dark earthshine");
    const LoopRun darkRun = runLoop(TuneWorkflow::MoonFlashes, dark);
    QCOMPARE(darkRun.camera.exposure.value, 30'000.0);
    // Earthshine too bright even at gain 0: the exposure comes down within 10-40 ms.
    ModelCamera bright;
    bright.perMillisecond = 0.004;
    check(TuneWorkflow::MoonFlashes, bright, 5, "bright earthshine");
    // An exposure outside 10-40 ms is brought into it first.
    ModelCamera longExposure;
    longExposure.exposure.value = 200'000;
    check(TuneWorkflow::MoonFlashes, longExposure, 5, "long exposure");
    // Transits: a sunlit Moon, saturated at first.
    ModelCamera sunlit;
    sunlit.perMillisecond = 0.2;
    sunlit.exposure.value = 20'000;
    check(TuneWorkflow::MoonTransits, sunlit, 8, "saturated Moon");
    // The sky at night, and at dusk.
    ModelCamera night;
    night.perMillisecond = 0.0004;
    night.exposure.value = 50'000;
    check(TuneWorkflow::Sky, night, 5, "night sky");
    ModelCamera dusk;
    dusk.perMillisecond = 0.01;
    dusk.exposure.value = 100'000;
    check(TuneWorkflow::Sky, dusk, 5, "dusk sky");
}

void AutoTuneTest::theLoopLearnsAnyGainCurve()
{
    // The loop's first guess is a stop per tenth of the range; the camera may
    // answer much faster or much slower. The earthshine's signal starts 80
    // times too faint, under a black level of 1 %.
    for (const double perStop : {5.0, 10.0, 25.0, 60.0, 400.0})
        for (const double range : {100.0, 600.0})
        {
            ModelCamera camera;
            camera.gainPerStop = perStop;
            camera.gain = TuneControl{0, 0, range};
            camera.pedestal = 0.01;
            camera.perMillisecond = 0.0006 / 30; // 0.06 % of signal at 30 ms and gain 0.
            const double needed = perStop * std::log2(0.05 / 0.0006);
            if (needed > range) continue; // Out of this camera's reach.
            const LoopRun run = runLoop(TuneWorkflow::MoonFlashes, camera);
            QVERIFY2(run.outcome == CameraStep::Outcome::InAim && run.rounds <= 7 && run.withinLimits,
                qPrintable(QString("%1 units per stop, range %2: %3 rounds, %4").arg(perStop).arg(range).arg(run.rounds)
                    .arg(QString::fromStdString(run.text))));
        }
}

void AutoTuneTest::saturationOnlyBoundsTheLoopsMove()
{
    // Transits at 10 ms and gain 0, saturated: a quarter of the exposure.
    ModelCamera camera;
    camera.perMillisecond = 0.5;
    camera.exposure.value = 10'000;
    camera.gain.value = 0;
    std::vector<TuneMeasurement> history{{10'000, 0, BrightnessSample{1.0, 0, 0.2, 1000}}};
    const CameraStep first = nextCameraStep(TuneWorkflow::MoonTransits, TuneCamera{camera.exposure, camera.gain, std::nullopt}, history);
    QCOMPARE(first.outcome, CameraStep::Outcome::Move);
    QCOMPARE(first.exposureUs, 2'500.0);
    QCOMPARE(first.gain, 0.0);
    QVERIFY(first.text.find("saturated") != std::string::npos);
    // Too dark at 2.5 ms: the next exposure stays below the saturated 10 ms.
    camera.exposure.value = 2'500;
    history.push_back({2'500, 0, BrightnessSample{0.3, 0, 0, 1000}});
    const CameraStep second = nextCameraStep(TuneWorkflow::MoonTransits, TuneCamera{camera.exposure, camera.gain, std::nullopt}, history);
    QCOMPARE(second.outcome, CameraStep::Outcome::Move);
    QVERIFY(second.exposureUs > 2'500 && second.exposureUs < 10'000);
}

void AutoTuneTest::theLoopStopsWhenTheAimIsOutOfReach()
{
    // A sky too dark at the longest exposure for meteors and the highest gain.
    ModelCamera camera;
    camera.perMillisecond = 0.0000001;
    camera.exposure.value = 100'000;
    const LoopRun run = runLoop(TuneWorkflow::Sky, camera);
    QCOMPARE(run.outcome, CameraStep::Outcome::OutOfReach);
    QVERIFY(run.withinLimits);
    QCOMPARE(run.camera.exposure.value, 250'000.0);
    QVERIFY2(run.camera.gain.value == 500.0, run.text.c_str());
    QVERIFY2(run.text.find("cannot reach") != std::string::npos, run.text.c_str());
    // Without a camera nothing moves.
    std::vector<TuneMeasurement> history{{30'000, 0, BrightnessSample{0.01, 0, 0, 1000}}};
    QCOMPARE(nextCameraStep(TuneWorkflow::Sky, TuneCamera{}, history).outcome, CameraStep::Outcome::OutOfReach);
}

void AutoTuneTest::kValuesAreTestedAroundTheCurrentOne()
{
    QCOMPARE(kValuesToTest(6, 4), (std::vector<double>{6, 7, 5, 8}));
    QCOMPARE(kValuesToTest(5.5, 4), (std::vector<double>{5.5, 6.5, 7.5}));
    QCOMPARE(kValuesToTest(4, 3), (std::vector<double>{4, 5, 6}));
    QCOMPARE(kValuesToTest(8, 1), (std::vector<double>{8}));
}

// Two bright cores joined by a faint bridge: at K 5 they form one patch too
// large to pass; at K 8 they are two patches that pass. Each row must count
// what a detector at that K alone, started from the same background, counts.
void AutoTuneTest::eachKRowCountsWhatThatKAloneWould()
{
    const DetectionSettings settings = quickSettings();
    Replay replay(settings);
    replay.run(90); // Learning.
    QVERIFY(replay.live.summary().ready);
    FlashDetector aloneAt5 = replay.live, aloneAt8 = replay.live;
    for (auto [detector, k] : {std::pair{&aloneAt5, 5.0}, std::pair{&aloneAt8, 8.0}})
    {
        DetectionSettings s = settings;
        s.sigma = k;
        detector->setSettings(s);
        detector->rearm();
    }
    replay.watch.start(sensitivity(settings, {5, 8}, 1.0));
    // One flash: repeated ones in one place would raise the learned noise there.
    const auto joined = [](uint64_t index, int x, int y) {
        if (index != 100) return 0.0;
        if (y < 39 || y > 41) return 0.0;
        if ((x >= 39 && x <= 41) || (x >= 47 && x <= 49)) return 30.0;
        return x >= 42 && x <= 46 ? 12.0 : 0.0;
    };
    // The watch starts at once: the copies are taken now, as aloneAt5 and aloneAt8 were.
    std::mt19937 twin = replay.random;
    replay.run(40, joined);
    const TuneWatchStatus status = replay.watch.status();
    QCOMPARE(status.state, TuneWatchStatus::State::Finished);
    QCOMPARE(status.rows.size(), size_t{2});

    // The same frames through the stand-alone detectors.
    uint64_t at5 = 0, at8 = 0;
    for (uint64_t i = 90; i < 90 + 40; ++i)
    {
        const auto made = frame(i, twin, [&](int x, int y) { return joined(i, x, y); });
        if (i == 90) continue; // The watch's rows start with the frame after the copy.
        for (auto [detector, count] : {std::pair{&aloneAt5, &at5}, std::pair{&aloneAt8, &at8}})
        {
            detector->analyze(*made);
            *count += detector->candidates().size();
            detector->updateReference(*made);
        }
        if (i - 91 + 1 >= status.rows[0].frames) break;
    }
    const auto& row5 = status.rows[0].k == 5 ? status.rows[0] : status.rows[1];
    const auto& row8 = status.rows[0].k == 8 ? status.rows[0] : status.rows[1];
    QCOMPARE(row5.events, at5);
    QCOMPARE(row8.events, at8);
    QVERIFY2(row8.events > 0, "the separated cores trigger at K 8");
    QCOMPARE(row5.events, uint64_t{0});
    // The suggestion never has events.
    const auto groups = groupEvents(status.rows);
    const KChoice choice = chooseK(status.rows, groups, std::vector<bool>(groups.size(), false), 1.0);
    QVERIFY(choice.k);
    QCOMPARE(*choice.k, 5.0);
}

void AutoTuneTest::aSkippedSearchIsNeverAQuietK()
{
    const DetectionSettings settings = quickSettings();
    Replay replay(settings);
    replay.run(90);
    replay.watch.start(sensitivity(settings, {5, 8}, 1.0));
    // On one frame, more than 4096 pixels (36 %) rise far above K: too many
    // to search into patches at either K. On the Moon nothing else fires, so
    // both rows count no events, and neither may be called quiet.
    replay.run(40, [](uint64_t index, int x, int) { return index == 100 && x < 46 ? 80.0 : 0.0; });
    TuneWatchStatus status = replay.watch.status();
    QCOMPARE(status.state, TuneWatchStatus::State::Finished);
    for (const KRowResult& row : status.rows)
    {
        QVERIFY(row.limitedFrames > 0);
        QCOMPARE(row.events, uint64_t{0});
    }
    auto groups = groupEvents(status.rows);
    const KChoice none = chooseK(status.rows, groups, std::vector<bool>(groups.size(), false), 1.0);
    QVERIFY(!none.k);
    QVERIFY(none.reason.find("skipped patch search") != std::string::npos);
    // Without the dense frame, the same watch has a quiet K.
    replay.watch.start(sensitivity(settings, {5, 8}, 1.0));
    replay.run(40);
    status = replay.watch.status();
    groups = groupEvents(status.rows);
    QVERIFY(chooseK(status.rows, groups, std::vector<bool>(groups.size(), false), 1.0).k);
}
void AutoTuneTest::excludingAnEventLeavesOthersCounted()
{
    // One row with a plane and, in the same frame, a separate noise patch.
    KRowResult row;
    row.k = 6;
    row.frames = 450;
    row.seconds = 15;
    row.events = 2;
    row.kept = {{100, {}, ChangeKind::Rise, 10, 10, 30, 6, 120, 20}, {100, {}, ChangeKind::Rise, 90, 70, 3, 3, 9, 7}};
    KRowResult higher = row;
    higher.k = 8;
    higher.events = 1;
    higher.kept = {{100, {}, ChangeKind::Rise, 12, 10, 26, 6, 100, 20}};
    const std::vector<KRowResult> rows{row, higher};
    const auto groups = groupEvents(rows);
    QCOMPARE(groups.size(), size_t{2});
    // The plane is one listed event found at both K values.
    QCOMPARE(groups[0].members.size() + groups[1].members.size(), size_t{3});
    const size_t plane = groups[0].members.size() == 2 ? 0 : 1;
    std::vector<bool> excluded(groups.size(), false);
    excluded[plane] = true;
    QCOMPARE(remainingEvents(rows, 0, groups, excluded), uint64_t{1});
    QCOMPARE(remainingEvents(rows, 1, groups, excluded), uint64_t{0});
    const KChoice choice = chooseK(rows, groups, excluded, 15);
    QVERIFY(choice.k);
    QCOMPARE(*choice.k, 8.0);
    // Without the exclusion, both K values had events.
    QVERIFY(!chooseK(rows, groups, std::vector<bool>(groups.size(), false), 15).k);
}

void AutoTuneTest::theWatchWaitsForTheDetectorAndStopsOnAChange()
{
    const DetectionSettings settings = quickSettings();
    Replay replay(settings);
    replay.watch.start(sensitivity(settings, {6, 7}, 1.0));
    replay.run(5);
    QCOMPARE(replay.watch.status().state, TuneWatchStatus::State::Waiting);
    replay.watch.takeChanged(steady_clock::now() + seconds{1});
    QVERIFY2(replay.watch.status().note.find("learn") != std::string::npos, replay.watch.status().note.c_str());
    // While the camera settles it waits too.
    replay.context.settling = true;
    replay.run(60);
    replay.watch.takeChanged(steady_clock::now() + seconds{2});
    QCOMPARE(replay.watch.status().state, TuneWatchStatus::State::Waiting);
    replay.context.settling = false;
    replay.run(10);
    QCOMPARE(replay.watch.status().state, TuneWatchStatus::State::Measuring);
    // A detection setting changed: the result would not match it.
    replay.context.settings.oneFrame = false;
    replay.run(1);
    QCOMPARE(replay.watch.status().state, TuneWatchStatus::State::Cancelled);
    QVERIFY(replay.watch.status().note.find("detection settings changed") != std::string::npos);
    // Monitoring starting cancels it too.
    replay.context.settings = settings;
    replay.watch.start(sensitivity(settings, {6}, 1.0));
    replay.context.preview = false;
    replay.run(1);
    QCOMPARE(replay.watch.status().state, TuneWatchStatus::State::Cancelled);
    QVERIFY(replay.watch.status().note.find("Monitoring") != std::string::npos);
}

void AutoTuneTest::aLoopEndsTheWatchWithItsCoverage()
{
    const DetectionSettings settings = quickSettings();
    Replay replay(settings);
    replay.run(90);
    replay.watch.start(sensitivity(settings, {6}, 10.0));
    replay.run(30);
    // The recording starts again from its first frame.
    replay.index = 0;
    replay.run(1);
    const TuneWatchStatus status = replay.watch.status();
    QCOMPARE(status.state, TuneWatchStatus::State::Finished);
    QVERIFY(status.note.find("Ended early") != std::string::npos);
    QVERIFY(status.seconds < 2);
    const auto groups = groupEvents(status.rows);
    QVERIFY(!chooseK(status.rows, groups, std::vector<bool>(groups.size(), false), 10.0).k);
}

void AutoTuneTest::aTestFlashIsCaughtOrMissed()
{
    for (const double amplitude : {60.0, 6.0})
    {
        const DetectionSettings settings = quickSettings();
        Replay replay(settings);
        replay.run(90);
        TuneWatchRequest request;
        request.kind = TuneWatchRequest::Kind::FlashCheck;
        request.settings = settings;
        request.flash = BlipSettings{64, 48, 3.0, amplitude, 2, BlipKind::BrightFlash};
        replay.watch.start(request);
        replay.run(20);
        const TuneWatchStatus status = replay.watch.status();
        QCOMPARE(status.state, TuneWatchStatus::State::Finished);
        QVERIFY(status.recovered);
        QCOMPARE(*status.recovered, amplitude > 30);
        QVERIFY(status.checkText.find(amplitude > 30 ? "Caught" : "Missed") == 0);
    }
}

QTEST_GUILESS_MAIN(AutoTuneTest)
#include "auto_tune_tests.moc"
