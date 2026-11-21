#include "tests/detector_observation.h"
#include "recording/clip_overview.h"
#include "sky_exposure.h"
#include "sky_schedule.h"

#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <cmath>
#include <functional>
#include <random>

namespace {

using namespace std::chrono;

system_clock::time_point utc(int year, unsigned month, unsigned day, int hour, int minute, int second)
{
    return sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}}
        + hours{hour} + minutes{minute} + seconds{second};
}

// A 128 x 96 Mono8 frame at 30 fps: a level, noise, and what draw adds.
FramePtr frame(uint64_t index, std::mt19937& random, const std::function<double(int x, int y)>& draw = {}, double level = 100)
{
    auto made = std::make_shared<Frame>();
    made->width = 128; made->height = 96; made->frameIndex = index;
    made->timestamp = system_clock::time_point{} + seconds{1'790'000'000} + microseconds{static_cast<int64_t>(index) * 33'333};
    made->testSource = true;
    std::normal_distribution<double> noise(0, 2);
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 128; ++x)
            made->data.push_back(static_cast<uint8_t>(std::clamp(std::lround(level + (draw ? draw(x, y) : 0.0) + noise(random)), 0L, 255L)));
    return made;
}

// A square of side 3 centred near (cx, cy).
bool inSquare(int x, int y, double cx, double cy)
{
    return std::abs(x - cx) <= 1.0 && std::abs(y - cy) <= 1.0;
}

QString kindOf(const std::vector<FramePtr>& frames, OverviewRequest request = {})
{
    const ClipOverview overview = makeClipOverview(frames, request);
    return overview.suggestedKind + ": " + overview.kindReason;
}

} // namespace

// Feature-15: sky extras.
class SkyExtrasTest : public QObject
{
    Q_OBJECT
private slots:
    void theSunsAltitudeMatchesPublishedValues();
    void theScheduleStartsBelowTheLimitAndStopsAbove();
    void theExposureFollowsTheSky();
    void theSkyLevelLeavesTheMaskOut();
    void eachClipGetsASuggestedKind();
    void aPolygonMaskIsFilledSavedAndObeyed();
};

// Done-when 1.
void SkyExtrasTest::theSunsAltitudeMatchesPublishedValues()
{
    // NREL's solar position paper (Reda and Andreas, 2004): Golden, Colorado,
    // 2003-10-17 12:30:30 local time (UTC-7). Topocentric zenith 50.11162
    // degrees, which includes about 0.013 degrees of refraction.
    const double golden = sunAltitudeDegrees(39.742476, -105.1786, utc(2003, 10, 17, 19, 30, 30));
    QVERIFY2(std::abs(golden - (90 - 50.11162 - 0.013)) < 0.1, qPrintable(QString::number(golden)));
    // Athens at the June solstice: the highest the Sun gets is 90 - 37.98 + 23.44.
    double highest = -90;
    for (int minute = 0; minute < 180; ++minute)
        highest = std::max(highest, sunAltitudeDegrees(37.98, 23.73, utc(2026, 6, 21, 9, 0, 0) + minutes{minute}));
    QVERIFY2(std::abs(highest - (90 - 37.98 + 23.44)) < 0.1, qPrintable(QString::number(highest)));
    // Midnight in winter in Athens: far below the horizon.
    QVERIFY(sunAltitudeDegrees(37.98, 23.73, utc(2026, 12, 21, 22, 30, 0)) < -60);

    // The next dusk at -10 degrees after an Athens afternoon.
    const auto from = utc(2026, 11, 17, 12, 0, 0);
    const auto dusk = nextSunCrossing(37.98, 23.73, -10, from);
    QVERIFY(dusk && *dusk > from && *dusk < from + hours{12});
    QVERIFY(std::abs(sunAltitudeDegrees(37.98, 23.73, *dusk) + 10) < 0.01);
    QVERIFY(sunAltitudeDegrees(37.98, 23.73, *dusk - minutes{1}) > -10);
    // No crossing at the pole in polar night.
    QVERIFY(!nextSunCrossing(89.9, 0, -1, utc(2026, 12, 21, 0, 0, 0)));
}

// Done-when 1: the decision itself.
void SkyExtrasTest::theScheduleStartsBelowTheLimitAndStopsAbove()
{
    // Arguments: Sun, limit, watching, started tonight, owns what runs.
    QCOMPARE(nightAction(-15, -10, false, false, false), NightAction::Start);
    QCOMPARE(nightAction(-15, -10, true, true, true), NightAction::None);
    QCOMPARE(nightAction(-15, -10, true, false, false), NightAction::None);
    // Once a night: the observer's stop holds until the next dusk.
    QCOMPARE(nightAction(-15, -10, false, true, false), NightAction::None);
    QCOMPARE(nightAction(-5, -10, true, true, true), NightAction::Stop);
    // Whatever runs, if it is the schedule's own, stops: the stream, or
    // Monitoring on the observer's stream.
    QCOMPARE(nightAction(-5, -10, false, true, true), NightAction::Stop);
    // Never what the observer started.
    QCOMPARE(nightAction(-5, -10, true, false, false), NightAction::None);
    QCOMPARE(nightAction(-5, -10, false, false, false), NightAction::None);
}

// Done-when 2: the rule.
void SkyExtrasTest::theExposureFollowsTheSky()
{
    SkyExposureRule rule;
    rule.minExposureUs = 1000; rule.maxExposureUs = 100'000;
    rule.minGain = 0; rule.maxGain = 300; rule.gainStep = 50;
    const auto t0 = steady_clock::time_point{} + hours{1};
    SkyExposureState state;
    // Within the bounds: nothing, ever.
    QVERIFY(!decideSkyExposure(0.15, 10'000, 0, t0, state, rule));
    QVERIFY(!decideSkyExposure(0.15, 10'000, 0, t0 + minutes{5}, state, rule));
    // Too dark: nothing before 60 s outside, then a longer exposure (at most twice).
    QVERIFY(!decideSkyExposure(0.02, 10'000, 0, t0, state, rule));
    QVERIFY(!decideSkyExposure(0.02, 10'000, 0, t0 + seconds{59}, state, rule));
    auto change = decideSkyExposure(0.02, 10'000, 0, t0 + seconds{61}, state, rule);
    QVERIFY(change);
    QCOMPARE(change->exposureUs, 20'000.0);
    QCOMPARE(change->gain, 0.0);
    // Not again within two minutes of a change, however long it stays dark.
    QVERIFY(!decideSkyExposure(0.02, 20'000, 0, t0 + seconds{62}, state, rule));
    QVERIFY(!decideSkyExposure(0.02, 20'000, 0, t0 + seconds{150}, state, rule));
    // At the longest exposure, the gain goes up instead.
    change = decideSkyExposure(0.02, 100'000, 0, t0 + seconds{250}, state, rule);
    QVERIFY(change);
    QCOMPARE(change->exposureUs, 100'000.0);
    QCOMPARE(change->gain, 50.0);
    // Too bright: the gain comes down first, then the exposure, by the ratio.
    SkyExposureState bright;
    QVERIFY(!decideSkyExposure(0.60, 50'000, 100, t0, bright, rule));
    change = decideSkyExposure(0.60, 50'000, 100, t0 + seconds{61}, bright, rule);
    QVERIFY(change && change->gain == 50 && change->exposureUs == 50'000);
    SkyExposureState noGain;
    QVERIFY(!decideSkyExposure(0.60, 50'000, 0, t0, noGain, rule));
    change = decideSkyExposure(0.60, 50'000, 0, t0 + seconds{61}, noGain, rule);
    QVERIFY(change && change->exposureUs == 25'000 && change->gain == 0);
    QVERIFY(change->reason.find("60.0% of full scale") != std::string::npos);
    // At every limit, nothing is left to change.
    SkyExposureState stuck;
    QVERIFY(!decideSkyExposure(0.01, 100'000, 300, t0, stuck, rule));
    QVERIFY(!decideSkyExposure(0.01, 100'000, 300, t0 + seconds{61}, stuck, rule));
}

void SkyExtrasTest::theSkyLevelLeavesTheMaskOut()
{
    // The top half of the frame is bright ground lights; the sky below is dark.
    std::mt19937 random(51);
    const auto lit = frame(0, random, [](int, int y) { return y < 48 ? 150.0 : 0.0; }, 20);
    QVERIFY(skyLevel(*lit, {}).value_or(0) > 0.3);
    const DetectionMask ground({DetectionMask::rectangle(0, 0, 128, 48)});
    const auto sky = skyLevel(*lit, ground);
    QVERIFY2(sky && std::abs(*sky - 20.0 / 255) < 0.02, qPrintable(QString::number(sky.value_or(-1))));
    // Feature-26 review: a mask over the whole frame leaves no sky to
    // measure, which is not the same as a black sky.
    const DetectionMask everything({DetectionMask::rectangle(0, 0, 128, 96)});
    QVERIFY(!skyLevel(*lit, everything));
}

// Done-when 3.
void SkyExtrasTest::eachClipGetsASuggestedKind()
{
    std::mt19937 random(52);
    std::vector<FramePtr> frames;
    // A one-frame flash.
    for (uint64_t i = 0; i < 60; ++i)
        frames.push_back(frame(i, random, [i](int x, int y) { return i == 40 && inSquare(x, y, 60, 40) ? 80.0 : 0.0; }));
    OverviewRequest flash;
    flash.onsetFrameIndex = 40;
    QVERIFY2(kindOf(frames, flash).startsWith("flash:"), qPrintable(kindOf(frames, flash)));

    // A fast straight streak: 4 px a frame for half a second.
    frames.clear();
    for (uint64_t i = 0; i < 70; ++i)
        frames.push_back(frame(i, random, [i](int x, int y) {
            return i >= 40 && i < 55 && inSquare(x, y, 20 + 4.0 * static_cast<double>(i - 40), 30 + 2.0 * static_cast<double>(i - 40)) ? 90.0 : 0.0;
        }));
    QVERIFY2(kindOf(frames).startsWith("meteor:"), qPrintable(kindOf(frames)));

    // A steady light crossing slowly, 0.5 px a frame for 3 s.
    frames.clear();
    for (uint64_t i = 0; i < 130; ++i)
        frames.push_back(frame(i, random, [i](int x, int y) {
            return i >= 35 && i < 125 && inSquare(x, y, 20 + 0.5 * static_cast<double>(i - 35), 50) ? 60.0 : 0.0;
        }));
    QVERIFY2(kindOf(frames).startsWith("satellite:"), qPrintable(kindOf(frames)));

    // The same light with a strobe twice a second.
    frames.clear();
    for (uint64_t i = 0; i < 130; ++i)
        frames.push_back(frame(i, random, [i](int x, int y) {
            if (i < 35 || i >= 125 || !inSquare(x, y, 20 + 0.5 * static_cast<double>(i - 35), 50)) return 0.0;
            return (i - 35) % 15 < 2 ? 150.0 : 40.0;
        }));
    QVERIFY2(kindOf(frames).startsWith("aircraft:"), qPrintable(kindOf(frames)));

    // A dark disk crossing a bright scene.
    frames.clear();
    for (uint64_t i = 0; i < 70; ++i)
        frames.push_back(frame(i, random, [i](int x, int y) {
            const double cx = 10 + 5.0 * (static_cast<double>(i) - 40);
            return i >= 40 && i < 60 && std::hypot(x - cx, y - 48.0) <= 6 ? -150.0 : 0.0;
        }, 200));
    QVERIFY2(kindOf(frames).startsWith("dark transit:"), qPrintable(kindOf(frames)));

    // A large brightening, whatever else.
    OverviewRequest lightning;
    lightning.largeBrightening = true;
    QVERIFY(kindOf(frames, lightning).startsWith("lightning:"));

    // Many triggers scattered over the frame.
    OverviewRequest seeing;
    for (int m = 0; m < 10; ++m) seeing.marks.push_back(QRect(10 + 11 * m, 5 + 8 * m, 3, 3));
    QVERIFY(kindOf(frames, seeing).startsWith("seeing:"));
}

// Done-when 4.
void SkyExtrasTest::aPolygonMaskIsFilledSavedAndObeyed()
{
    // A rectangle masks exactly its pixels; a triangle about half its box.
    std::vector<uint8_t> pixels(20 * 10, 0);
    DetectionMask({DetectionMask::rectangle(2, 3, 5, 4)}).fill(20, 10, pixels.data());
    QCOMPARE(std::count(pixels.begin(), pixels.end(), uint8_t{1}), 20);
    QCOMPARE(pixels[3 * 20 + 2], uint8_t{1});
    QCOMPARE(pixels[3 * 20 + 7], uint8_t{0});
    const MaskShape triangle{{0, 96}, {128, 96}, {64, 40}};
    const DetectionMask mask({triangle});
    std::vector<uint8_t> big(128 * 96, 0);
    mask.fill(128, 96, big.data());
    const auto inside = std::count(big.begin(), big.end(), uint8_t{1});
    QVERIFY(std::abs(static_cast<double>(inside) - 128 * 56 / 2.0) < 128);

    // Saved and read back, and a rectangle saved before polygons still reads.
    QCOMPARE(mask.text(), std::string("0,96 128,96 64,40"));
    QVERIFY(DetectionMask::fromText(mask.text()) == mask);
    QVERIFY(DetectionMask::fromText("0,400,640,80").shapes().front() == DetectionMask::rectangle(0, 400, 640, 80));
    QVERIFY(DetectionMask::fromText("1,2 3,4").empty());
    QVERIFY(!DetectionMask().with({{0, 0}, {1, 1}}).count());

    // A flash under the triangle does not fire; one above it does.
    DetectionSettings settings;
    settings.mask = mask;
    FlashDetector detector(settings);
    std::mt19937 random(53);
    for (uint64_t i = 0; i < 40; ++i) observe(detector, *frame(i, random));
    QVERIFY(!observe(detector, *frame(40, random, [](int x, int y) { return inSquare(x, y, 64, 80) ? 80.0 : 0.0; })));
    QVERIFY(observe(detector, *frame(41, random, [](int x, int y) { return inSquare(x, y, 64, 20) ? 80.0 : 0.0; })));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    SkyExtrasTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "sky_extras_tests.moc"
