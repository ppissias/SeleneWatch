#include "guiding/calibrator.h"
#include "guiding/feature_lock.h"
#include "guiding/guide_controller.h"
#include "guiding/guider.h"
#include "guiding/lock_tracker.h"
#include "mounts/simulated_mount.h"
#include "recording/event_writer.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

namespace {

// A crater-like blob of the synthetic test scene, in scene pixels.
struct Blob
{
    double x, y, sigma, amplitude;
};

// Fixed pseudo-random blobs, so every run sees the same scene. They do not
// repeat, unlike the synthetic source's texture.
const std::vector<Blob>& blobs()
{
    static const std::vector<Blob> scene = [] {
        std::vector<Blob> out;
        uint32_t state = 20261115u;
        auto next = [&] {
            state = state * 1664525u + 1013904223u;
            return static_cast<double>(state >> 8) / static_cast<double>(1u << 24);
        };
        // Bright rims and dark floors, from a few ADU to about twice the background.
        for (int i = 0; i < 360; ++i)
            out.push_back({next() * 400 - 20, next() * 300 - 20, 1.5 + next() * 3.0, (next() - 0.4) * 120});
        return out;
    }();
    return scene;
}

double brightness(double x, double y)
{
    double value = 100;
    for (const Blob& blob : blobs())
    {
        const double dx = x - blob.x;
        const double dy = y - blob.y;
        const double r2 = dx * dx + dy * dy;
        if (r2 < 25 * blob.sigma * blob.sigma) value += blob.amplitude * std::exp(-r2 / (2 * blob.sigma * blob.sigma));
    }
    return value;
}

// Repeatable noise of about one unit standard deviation.
double noise(int x, int y, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ seed * 83492791u;
    double sum = 0;
    for (int i = 0; i < 4; ++i)
    {
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        sum += static_cast<double>(h & 0xffff) / 65535.0;
    }
    return (sum - 2.0) * std::sqrt(3.0);
}

std::chrono::system_clock::time_point at(double seconds)
{
    return std::chrono::system_clock::time_point{} + std::chrono::seconds{1'790'000'000}
        + std::chrono::round<std::chrono::system_clock::duration>(std::chrono::duration<double>(seconds));
}

struct Render
{
    int width{240}, height{180};
    double shiftX{0}, shiftY{0};
    double gain{1};
    PixelFormat format{PixelFormat::Mono8};
    BayerPattern bayer{BayerPattern::None};
    uint32_t noiseSeed{1};
    double noiseSigma{1};
    // A featureless frame of the same background.
    bool blank{false};
};

// The scene moved by (shiftX, shiftY): a feature at scene x appears at frame x + shiftX.
// Mono16 holds 12-bit samples in the high bits.
FramePtr render(const Render& r, double seconds = 0, uint64_t index = 0)
{
    auto frame = std::make_shared<Frame>();
    frame->width = r.width;
    frame->height = r.height;
    frame->format = r.format;
    frame->bayer = r.bayer;
    frame->significantBits = r.format == PixelFormat::Mono16 ? 12 : 8;
    frame->alignment = r.format == PixelFormat::Mono16 ? BitAlignment::High : BitAlignment::Low;
    frame->frameIndex = index;
    frame->timestamp = at(seconds);
    frame->data.resize(frameBytes(r.width, r.height, r.format));
    for (int y = 0; y < r.height; ++y)
    {
        for (int x = 0; x < r.width; ++x)
        {
            double value = r.blank ? 100.0 : brightness(x - r.shiftX, y - r.shiftY);
            if (r.bayer != BayerPattern::None)
            {
                // RGGB: red and blue respond less than green.
                const bool red = x % 2 == 0 && y % 2 == 0;
                const bool blue = x % 2 == 1 && y % 2 == 1;
                value *= red ? 0.7 : blue ? 0.45 : 1.0;
            }
            value = value * r.gain + r.noiseSigma * noise(x, y, r.noiseSeed);
            const size_t pixel = static_cast<size_t>(y) * r.width + x;
            if (r.format == PixelFormat::Mono16)
            {
                const auto sample = static_cast<uint16_t>(std::clamp(std::round(value * 16.0), 0.0, 4095.0));
                const auto word = static_cast<uint16_t>(sample << 4);
                std::memcpy(frame->data.data() + pixel * 2, &word, sizeof(word));
            }
            else
            {
                frame->data[pixel] = static_cast<uint8_t>(std::clamp(std::round(value), 0.0, 255.0));
            }
        }
    }
    return frame;
}

// A frame cut at a whole-pixel offset from one large rendering, for tests
// that need many frames.
class Canvas
{
public:
    Canvas()
        : full_(render(Render{.width = kSide, .height = kSide, .shiftX = kMargin, .shiftY = kMargin}))
    {
    }

    FramePtr frame(int shiftX, int shiftY, double seconds, uint64_t index = 0) const
    {
        auto frame = std::make_shared<Frame>();
        frame->width = 240;
        frame->height = 180;
        frame->frameIndex = index;
        frame->timestamp = at(seconds);
        frame->data.resize(240 * 180);
        for (int y = 0; y < 180; ++y)
            std::memcpy(frame->data.data() + static_cast<size_t>(y) * 240,
                full_->data.data() + static_cast<size_t>(y + kMargin - shiftY) * kSide + kMargin - shiftX, 240);
        return frame;
    }

    static FramePtr blank(double seconds, uint64_t index = 0)
    {
        return render(Render{.noiseSeed = static_cast<uint32_t>(index) + 7, .blank = true}, seconds, index);
    }

private:
    static constexpr int kSide = 320;
    static constexpr int kMargin = 40;
    FramePtr full_;
};

const LockRegion kRegion{88, 58, 64, 64};

} // namespace

// Feature-8 Part A: the lock, its refusals, the drift estimate, and the lost-lock rule.
class GuidingTest : public QObject
{
    Q_OBJECT
private slots:
    void measuresSubPixelShifts_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<double>("gain");
        QTest::newRow("mono8") << static_cast<int>(PixelFormat::Mono8) << 1.0;
        QTest::newRow("mono16 12-bit high") << static_cast<int>(PixelFormat::Mono16) << 1.0;
        QTest::newRow("half brightness") << static_cast<int>(PixelFormat::Mono8) << 0.5;
    }

    void measuresSubPixelShifts()
    {
        QFETCH(int, format);
        QFETCH(double, gain);
        Render base{.gain = gain, .format = static_cast<PixelFormat>(format)};
        std::string refusal;
        const auto lock = FeatureLock::take(*render(base), kRegion, refusal);
        QVERIFY2(lock, refusal.c_str());
        QCOMPARE(lock->binning(), 1);
        double worst = 0;
        for (const auto [sx, sy] : {std::pair{0.0, 0.0}, {0.37, -0.81}, {-1.5, 0.25}, {4.62, 3.1}, {-7.3, -5.55}})
        {
            Render moved = base;
            moved.shiftX = sx;
            moved.shiftY = sy;
            moved.noiseSeed = 99;
            const LockMatch match = lock->find(*render(moved), 0, 0, LockTracker::kTrackRadius);
            QVERIFY2(match.valid, match.reason.c_str());
            QVERIFY2(std::abs(match.dx - sx) <= 0.1 && std::abs(match.dy - sy) <= 0.1,
                qPrintable(QString("shift %1, %2 measured %3, %4").arg(sx).arg(sy).arg(match.dx).arg(match.dy)));
            QVERIFY(match.score > 0.8);
            worst = std::max({worst, std::abs(match.dx - sx), std::abs(match.dy - sy)});
        }
        qInfo("contrast %.1f, structure %.2f, noise %.2f ADU; worst error %.3f px",
            lock->quality().contrast, lock->quality().structureRatio, lock->quality().noiseAdu, worst);
    }

    void binsLargeRegionsAndBayerMosaics()
    {
        std::string refusal;
        const auto large = FeatureLock::take(*render({}), {40, 30, 190, 140}, refusal);
        QVERIFY2(large, refusal.c_str());
        QCOMPARE(large->binning(), 2);
        QCOMPARE(large->region().width, 190);
        const auto moved = render({.shiftX = 1.3, .shiftY = -2.6, .noiseSeed = 5});
        LockMatch match = large->find(*moved, 0, 0, LockTracker::kTrackRadius);
        QVERIFY2(match.valid, match.reason.c_str());
        QVERIFY2(std::abs(match.dx - 1.3) <= 0.15 && std::abs(match.dy + 2.6) <= 0.15,
            qPrintable(QString("binned measured %1, %2").arg(match.dx).arg(match.dy)));

        const Render mosaic{.bayer = BayerPattern::RGGB};
        const auto colour = FeatureLock::take(*render(mosaic), {89, 57, 64, 64}, refusal);
        QVERIFY2(colour, refusal.c_str());
        QCOMPARE(colour->binning(), 2);
        double worst = 0;
        for (const auto [sx, sy] : {std::pair{1.0, 0.0}, {-2.4, 1.7}, {3.0, -3.0}})
        {
            Render shifted = mosaic;
            shifted.shiftX = sx;
            shifted.shiftY = sy;
            shifted.noiseSeed = 11;
            match = colour->find(*render(shifted), 0, 0, LockTracker::kTrackRadius);
            QVERIFY2(match.valid, match.reason.c_str());
            QVERIFY2(std::abs(match.dx - sx) <= 0.2 && std::abs(match.dy - sy) <= 0.2,
                qPrintable(QString("mosaic shift %1, %2 measured %3, %4").arg(sx).arg(sy).arg(match.dx).arg(match.dy)));
            worst = std::max({worst, std::abs(match.dx - sx), std::abs(match.dy - sy)});
        }
        qInfo("binned 2x2 region error %.3f, %.3f px; RGGB mosaic worst error %.3f px",
            std::abs(large->find(*moved, 0, 0, LockTracker::kTrackRadius).dx - 1.3),
            std::abs(large->find(*moved, 0, 0, LockTracker::kTrackRadius).dy + 2.6), worst);
    }

    void refusesRegionsWithoutTwoDimensionalDetail()
    {
        std::string refusal;
        QVERIFY(!FeatureLock::take(*render({.blank = true}), kRegion, refusal));
        QVERIFY2(refusal.find("contrast") != std::string::npos, refusal.c_str());

        // A straight edge, upright and tilted, like a piece of limb or terminator.
        for (const double angle : {0.0, 0.5})
        {
            auto edge = std::make_shared<Frame>(*render({.blank = true}));
            for (int y = 0; y < edge->height; ++y)
                for (int x = 0; x < edge->width; ++x)
                {
                    const double across = (x - 120) * std::cos(angle) + (y - 90) * std::sin(angle);
                    const double value = 90 + 60 * std::tanh(across / 2.0) + noise(x, y, 3);
                    edge->data[static_cast<size_t>(y) * edge->width + x] = static_cast<uint8_t>(std::clamp(std::round(value), 0.0, 255.0));
                }
            refusal.clear();
            QVERIFY(!FeatureLock::take(*edge, kRegion, refusal));
            QVERIFY2(refusal.find("one direction") != std::string::npos, refusal.c_str());
        }

        auto saturated = std::make_shared<Frame>(*render({}));
        std::fill(saturated->data.begin(), saturated->data.end(), uint8_t{255});
        refusal.clear();
        QVERIFY(!FeatureLock::take(*saturated, kRegion, refusal));
        QVERIFY2(refusal.find("saturated") != std::string::npos, refusal.c_str());

        refusal.clear();
        QVERIFY(!FeatureLock::take(*render({}), {-200, 10, 64, 64}, refusal));
        QVERIFY(!refusal.empty());
    }

    void edgeOfSearchWindowIsNotAMatch()
    {
        std::string refusal;
        const auto lock = FeatureLock::take(*render({}), kRegion, refusal);
        QVERIFY(lock);
        // Moved farther than the search radius: the best score lies on the window's edge.
        const LockMatch match = lock->find(*render({.shiftX = 12, .noiseSeed = 4}), 0, 0, 6);
        QVERIFY(!match.valid);
        QVERIFY2(match.reason.find("edge") != std::string::npos, match.reason.c_str());
    }

    void estimateIsTheMedianOfRecentMatches()
    {
        const Canvas canvas;
        LockTracker tracker;
        QVERIFY2(tracker.lock(*canvas.frame(0, 0, 0.0), {100, 70, 24, 24}), tracker.status().message.c_str());
        QCOMPARE(tracker.status().state, LockState::Locked);
        // Frames closer than the measuring interval are skipped.
        QVERIFY(!tracker.step(*canvas.frame(2, -1, 0.1)));
        QVERIFY(tracker.step(*canvas.frame(2, -1, 0.2)));
        QVERIFY(!tracker.status().estimateValid);
        QVERIFY(tracker.step(*canvas.frame(2, -1, 0.4)));
        QVERIFY(tracker.step(*canvas.frame(9, -1, 0.6))); // One outlier does not move the median.
        QVERIFY(tracker.status().estimateValid);
        QVERIFY(std::abs(tracker.status().estimateDx - 2) < 0.1);
        QVERIFY(std::abs(tracker.status().estimateDy + 1) < 0.1);
        // Old matches leave the window.
        for (int step = 4; step < 23; ++step) QVERIFY(tracker.step(*canvas.frame(5, 3, step * 0.2)));
        QVERIFY(std::abs(tracker.status().estimateDx - 5) < 0.1);
        QVERIFY(std::abs(tracker.status().estimateDy - 3) < 0.1);
    }

    void lockIsLostAndRestored()
    {
        const Canvas canvas;
        LockTracker tracker;
        // Feature-32: the search lasts the grace period the observer sets; 60 s here.
        tracker.setSearchSeconds(60);
        QCOMPARE(tracker.searchSeconds(), 60.0);
        QVERIFY2(tracker.lock(*canvas.frame(0, 0, 0.0), {100, 70, 24, 24}), tracker.status().message.c_str());
        // Steps of 0.2 s: the last valid match is at step 5 (1.0 s).
        for (int step = 1; step <= 5; ++step) tracker.step(*canvas.frame(2, -1, step * 0.2));
        QVERIFY(tracker.status().estimateValid);

        // Cloud: no valid match. After kLockLostSeconds (2 s, step 15) the
        // lock is lost and there is no estimate to act on.
        for (int step = 6; step <= 15; ++step)
        {
            tracker.step(*Canvas::blank(step * 0.2));
            QCOMPARE(tracker.status().state, LockState::Locked);
        }
        tracker.step(*Canvas::blank(16 * 0.2));
        QCOMPARE(tracker.status().state, LockState::Searching);
        QVERIFY(!tracker.status().estimateValid);
        QVERIFY(!tracker.status().message.empty());

        // The feature returns, farther than the tracking radius would reach.
        tracker.step(*canvas.frame(30, -20, 17 * 0.2));
        QCOMPARE(tracker.status().state, LockState::Searching); // one match is not enough
        tracker.step(*canvas.frame(30, -20, 18 * 0.2));
        QCOMPARE(tracker.status().state, LockState::Locked);
        QVERIFY(std::abs(tracker.status().foundDx - 30) < 0.1);
        QVERIFY(std::abs(tracker.status().foundDy + 20) < 0.1);

        // Gone for good: after the search time nothing more is measured.
        for (int second = 4; second <= 70; ++second)
        {
            tracker.step(*Canvas::blank(second));
            if (second < 66) QVERIFY(tracker.status().state != LockState::Lost);
        }
        QCOMPARE(tracker.status().state, LockState::Lost);
        QVERIFY(!tracker.step(*canvas.frame(30, -20, 71)));
        QVERIFY(!tracker.status().estimateValid);
    }

    void restartKeepsTheLockAndLayoutChangeReleasesIt()
    {
        const Canvas canvas;
        LockTracker tracker;
        QVERIFY2(tracker.lock(*canvas.frame(0, 0, 10.0), {100, 70, 24, 24}), tracker.status().message.c_str());
        tracker.step(*canvas.frame(1, 1, 10.2));
        // A new run whose clock is earlier: the lock stays, the estimate restarts.
        tracker.discontinuity();
        QVERIFY(tracker.step(*canvas.frame(1, 1, 2.0)));
        QCOMPARE(tracker.status().state, LockState::Locked);
        QCOMPARE(tracker.status().estimateSamples, 1);

        QVERIFY(!tracker.step(*render({.width = 200, .height = 150}, 2.4)));
        QCOMPARE(tracker.status().state, LockState::Off);
        QVERIFY(!tracker.status().message.empty());
    }

    void guiderLogsWhileRecordingOnly()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QObject context;
        EventWriter writer(&context, directory.path(), size_t{1} << 20, [](const ClipWriteResult&) {});
        std::atomic<int> notified{0};
        Guider guider(writer, [&] { ++notified; });
        const Canvas canvas;

        uint64_t index = 0;
        auto offer = [&](const FramePtr& frame, bool recording) {
            guider.offer(frame, recording);
            return QTest::qWaitFor([&] { return guider.status().lock.frameIndex == frame->frameIndex; }, 5000);
        };
        // Nothing is wanted yet, so the frame is not kept.
        guider.offer(canvas.frame(0, 0, 0.0, ++index), true);
        QTest::qWait(50);
        QCOMPARE(guider.status().lock.state, LockState::Off);

        guider.requestLock({100, 70, 24, 24});
        QVERIFY(offer(canvas.frame(0, 0, 0.2, ++index), false));
        QCOMPARE(guider.status().lock.state, LockState::Locked);
        // Preview: measured, not written.
        QVERIFY(offer(canvas.frame(2, -1, 0.4, ++index), false));
        for (int step = 3; step <= 10; ++step) QVERIFY(offer(canvas.frame(2, -1, step * 0.2, ++index), true));
        QVERIFY(guider.status().lock.estimateValid);
        QVERIFY(notified.load() > 0);
        guider.requestUnlock();
        QVERIFY(QTest::qWaitFor([&] { return guider.status().lock.state == LockState::Off; }, 5000));

        guider.shutdown();
        writer.shutdown();
        QFile log(QDir(directory.path()).filePath("guiding.txt"));
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(log.readAll());
        QVERIFY2(!text.contains("guide_lock"), qPrintable(text)); // taken in Preview
        QVERIFY2(text.contains("guide_offset state=\"locked\""), qPrintable(text));
        QVERIFY2(text.contains("region=100,70,24,24"), qPrintable(text));
        const auto values = QRegularExpression(R"( dx=([-+][0-9.]+) dy=([-+][0-9.]+) .* estimate_dx=([-+][0-9.]+))").match(text);
        QVERIFY2(values.hasMatch(), qPrintable(text));
        QVERIFY(std::abs(values.captured(1).toDouble() - 2) < 0.1);
        QVERIFY(std::abs(values.captured(2).toDouble() + 1) < 0.1);
        QVERIFY(std::abs(values.captured(3).toDouble() - 2) < 0.1);
        QVERIFY2(text.contains("guide_unlock"), qPrintable(text));
        // One offset line per second of source time over 1.6 s, plus the first.
        QCOMPARE(text.count("guide_offset"), 2);
        QCOMPARE(writer.usage().guidingWriteFailures, uint64_t{0});
    }

    // Part B: the simulation itself.
    void simulatedPointingFollowsPulsesAndBacklash()
    {
        using namespace std::chrono_literals;
        auto now = SimulatedPointing::Clock::time_point{} + 1h;
        SimulatedPointing::Settings settings;
        settings.driftRa = 0;
        settings.driftDec = 0;
        settings.decBacklash = 5;
        settings.cameraAngleDegrees = 30;
        auto pointing = std::make_shared<SimulatedPointing>(settings, now);
        SimulatedMount mount(pointing, [&] { return now; });
        // Inactive until the mount connects: no pulse moves it, no offset.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mount.pulseGuide(GuideDirection::West, 100));
        mount.connect();
        mount.pulseGuide(GuideDirection::West, 1000);
        QVERIFY(mount.state().pulseGuiding);
        now += 1500ms;
        QVERIFY(!mount.state().pulseGuiding);
        const auto rate = pointing->imageRate(GuideDirection::West);
        const auto moved = pointing->imageOffset(now);
        QVERIFY(std::abs(moved.x - rate.x * 1000) < 1e-9 && std::abs(moved.y - rate.y * 1000) < 1e-9);
        QVERIFY(std::abs(std::hypot(rate.x, rate.y) * 1000 - 3.75) < 1e-9);
        // The first north pulse takes up 5 arcseconds of slack: 1.33 s at 3.75"/s.
        mount.pulseGuide(GuideDirection::North, 2000);
        now += 2000ms;
        QVERIFY(std::abs(pointing->skyOffset(now).y + (7.5 - 5.0)) < 1e-9);
        mount.pulseGuide(GuideDirection::North, 1000); // same direction: no slack
        now += 1000ms;
        QVERIFY(std::abs(pointing->skyOffset(now).y + (11.25 - 5.0)) < 1e-9);
        mount.disconnect();
        const auto frozen = pointing->imageOffset(now);
        now += 10s;
        QCOMPARE(pointing->imageOffset(now).x, frozen.x);
    }

    // Done-when 4: calibration against the simulated mount with a 30 degree
    // camera angle and a mirror flip measures the simulation's vectors.
    void calibrationMeasuresTheSimulation_data()
    {
        QTest::addColumn<bool>("mirrored");
        QTest::addColumn<double>("backlash");
        QTest::newRow("30 degrees") << false << 0.0;
        QTest::newRow("30 degrees, mirrored, 20 arcsec backlash") << true << 20.0;
    }

    void calibrationMeasuresTheSimulation()
    {
        QFETCH(bool, mirrored);
        QFETCH(double, backlash);
        SimulatedPointing::Settings settings;
        settings.mirrored = mirrored;
        settings.decBacklash = backlash;
        Loop loop(settings);
        Calibrator calibrator;
        while (!calibrator.finished() && loop.elapsed() < 600) loop.step(calibrator.step(loop.observe()));
        QVERIFY2(calibrator.finished() && !calibrator.failed(), calibrator.failure().c_str());
        const GuideCalibration& result = *calibrator.result();
        const auto west = loop.pointing->imageRate(GuideDirection::West);
        const auto north = loop.pointing->imageRate(GuideDirection::North);
        const double speed = std::hypot(west.x, west.y);
        QVERIFY2(std::hypot(result.west.x - west.x, result.west.y - west.y) < 0.05 * speed,
            qPrintable(QString("west %1,%2 expected %3,%4").arg(result.west.x * 1000).arg(result.west.y * 1000).arg(west.x * 1000).arg(west.y * 1000)));
        QVERIFY2(std::hypot(result.north.x - north.x, result.north.y - north.y) < 0.05 * speed,
            qPrintable(QString("north %1,%2 expected %3,%4").arg(result.north.x * 1000).arg(result.north.y * 1000).arg(north.x * 1000).arg(north.y * 1000)));
        QVERIFY(std::abs(result.axisAngleDegrees - 90) < 5);
        QVERIFY(result.arcsecPerPixel && std::abs(*result.arcsecPerPixel - 1.0) < 0.05);
        QCOMPARE(result.pierSide, PierSide::West);
        if (backlash > 0) QVERIFY(result.backlashMs >= 5000); // 20" at 3.75"/s
        qInfo("west %.3f,%.3f north %.3f,%.3f px/s, drift %.3f,%.3f px/s, backlash %d ms, %.0f s",
            result.west.x * 1000, result.west.y * 1000, result.north.x * 1000, result.north.y * 1000,
            result.drift.x, result.drift.y, result.backlashMs, loop.elapsed());
    }

    void calibrationFailsWhenTheMountIgnoresPulses()
    {
        SimulatedPointing::Settings settings;
        settings.ignorePulses = true;
        Loop loop(settings);
        Calibrator calibrator;
        while (!calibrator.finished() && loop.elapsed() < 600) loop.step(calibrator.step(loop.observe()));
        QVERIFY(calibrator.failed());
        QVERIFY2(calibrator.failure().find("does not respond") != std::string::npos, calibrator.failure().c_str());
        QVERIFY(!calibrator.result());
    }

    // Done-when 5: an hour of constant drift stays in the box, with about the
    // predicted number of pulses and no declination reversal.
    void guidingHoldsTheFieldForAnHour()
    {
        Loop loop({});
        GuideController::Settings settings;
        settings.tolerancePixels = 10;
        GuideController controller(loop.trueCalibration(), settings);
        QVERIFY2(GuideController::refusal(loop.trueCalibration(), settings, loop.observe()).empty(),
            GuideController::refusal(loop.trueCalibration(), settings, loop.observe()).c_str());
        double worstRa = 0, worstDec = 0;
        int north = 0, south = 0, east = 0, west = 0;
        while (loop.elapsed() < 3600)
        {
            const GuideDecision decision = controller.step(loop.observe());
            if (decision.pulse)
            {
                switch (decision.pulse->direction)
                {
                case GuideDirection::North: ++north; break;
                case GuideDirection::South: ++south; break;
                case GuideDirection::East: ++east; break;
                case GuideDirection::West: ++west; break;
                }
            }
            loop.step(decision);
            QVERIFY2(!controller.stopped(), controller.stopReason().c_str());
            const auto sky = loop.pointing->skyOffset(loop.now);
            worstRa = std::max(worstRa, std::abs(sky.x));
            worstDec = std::max(worstDec, std::abs(sky.y));
        }
        // Each correction removes kGain of an error just past the tolerance.
        const auto s = loop.pointing->settings();
        const double predicted = 3600 * (std::abs(s.driftRa) + std::abs(s.driftDec)) / s.arcsecPerPixel / (settings.gain * settings.tolerancePixels);
        const int pulses = north + south + east + west;
        qInfo("%d pulses (%d N, %d S, %d E, %d W), predicted %.0f; worst RA %.1f px, Dec %.1f px; %d reversals withheld",
            pulses, north, south, east, west, predicted, worstRa, worstDec, controller.declinationReversalsWithheld());
        QVERIFY(worstRa <= 1.3 * settings.tolerancePixels && worstDec <= 1.3 * settings.tolerancePixels);
        QVERIFY(pulses > 0.6 * predicted && pulses < 1.4 * predicted);
        QVERIFY(north == 0 || south == 0); // declination never reversed
    }

    // Feature-40: imperfect polar alignment (strong steady drift) and heavy
    // declination backlash after calibration are both handled without false
    // fruitless stops, while an unpulsed drifting axis never penalizes a good pulse.
    void guidingHandlesStrongPolarDriftAndDecBacklash()
    {
        // Strong polar-alignment drift: 1.2"/s Dec + 0.4"/s RA at 3.75"/s guide speed,
        // with 20" (5.3 s) of Dec backlash after calibration ends moving South.
        SimulatedPointing::Settings pointing;
        pointing.driftRa = 0.4;
        pointing.driftDec = -1.2; // image drifts South -> requires North pulses (against calibration's South return)
        pointing.decBacklash = 20.0;
        Loop loop(pointing);
        Calibrator calibrator;
        while (!calibrator.finished() && loop.elapsed() < 600) loop.step(calibrator.step(loop.observe()));
        QVERIFY2(calibrator.finished() && !calibrator.failed(), calibrator.failure().c_str());

        // Reset the lock origin to the post-calibration position, as an observer does.
        const auto origin = loop.pointing->imageOffset(loop.now);
        auto observeCentered = [&] {
            GuideObservation o = loop.observe();
            o.foundDx -= origin.x;
            o.foundDy -= origin.y;
            o.estimateDx -= origin.x;
            o.estimateDy -= origin.y;
            return o;
        };
        GuideController::Settings settings;
        settings.tolerancePixels = 10;
        GuideController controller(*calibrator.result(), settings);
        const double startTime = loop.elapsed();
        while (loop.elapsed() - startTime < 600)
        {
            const GuideDecision decision = controller.step(observeCentered());
            loop.step(decision);
            QVERIFY2(!controller.stopped(), controller.stopReason().c_str());
        }
        QVERIFY(controller.pulses() > 5);

        // Unpulsed-axis drift: RA is pulsed and improves (12 -> 9 px), while Dec
        // (unpulsed because it stayed inside tolerance) drifts 5 -> 9.5 px, so
        // total hypot(dx, dy) grows (13.0 -> 13.09). That must NOT count as fruitless.
        Loop axisLoop({});
        GuideCalibration axisCal = axisLoop.trueCalibration();
        axisCal.west = {0.01, 0.0};
        axisCal.north = {0.0, 0.01};
        GuideController::Settings axisSettings;
        axisSettings.tolerancePixels = 10;
        axisSettings.maxFruitlessCorrections = 2;
        axisSettings.settleSeconds = 0.0;
        GuideController axisCtrl(axisCal, axisSettings);
        auto now = SimulatedPointing::Clock::time_point{} + std::chrono::hours{1};
        auto obsAt = [&](double dx, double dy) {
            GuideObservation o;
            o.now = now;
            o.locked = true;
            o.estimateValid = true;
            o.estimateSamples = 5;
            o.estimateDx = dx;
            o.estimateDy = dy;
            o.mount.connected = true;
            o.mount.tracking = true;
            o.mount.canPulseGuide = true;
            o.mount.pierSide = PierSide::West;
            o.frameInterval = 1.0 / 30.0;
            return o;
        };
        for (int i = 0; i < 4; ++i)
        {
            const auto d = axisCtrl.step(obsAt(12.0, 5.0)); // RA out (>10), Dec in (<=10), hypot=13.0
            QVERIFY(d.pulse.has_value());
            QCOMPARE(d.pulse->direction, GuideDirection::East);
            now += std::chrono::milliseconds{d.pulse->milliseconds + 100};
            (void)axisCtrl.step(obsAt(9.0, 9.5)); // ends pulse -> enters Measuring
            // Measuring evaluates RA (12 -> 9, improved!) even though hypot grew (13.0 -> 13.09).
            (void)axisCtrl.step(obsAt(9.0, 9.5));
            QVERIFY2(!axisCtrl.stopped(), axisCtrl.stopReason().c_str());
        }
    }

    // Feature-41: LockTracker fits live drift speed over recent unpulsed matches
    // (for polar-alignment feedback) and GuideController uses it on the first pulse.
    void lockTrackerMeasuresDriftSpeedAndFeedsGuideController()
    {
        const Canvas canvas;
        LockTracker tracker;
        QVERIFY2(tracker.lock(*canvas.frame(0, 0, 0.0), {100, 70, 24, 24}), tracker.status().message.c_str());
        // Drift at +1.5 px/s in X and -0.5 px/s in Y (whole-pixel canvas steps).
        for (int step = 1; step <= 40; ++step)
        {
            const double t = step * 0.2;
            const int sx = static_cast<int>(std::lround(1.5 * t));
            const int sy = static_cast<int>(std::lround(-0.5 * t));
            QVERIFY(tracker.step(*canvas.frame(sx, sy, t)));
            if (t < LockTracker::kMinDriftSpanSeconds) QVERIFY(!tracker.status().driftRateValid);
        }
        QVERIFY(tracker.status().driftRateValid);
        QVERIFY(tracker.status().driftRateSpanSeconds >= LockTracker::kMinDriftSpanSeconds);
        QVERIFY2(std::abs(tracker.status().driftRateDx - 1.5) < 0.15,
            qPrintable(QString("driftRateDx=%1").arg(tracker.status().driftRateDx)));
        QVERIFY2(std::abs(tracker.status().driftRateDy + 0.5) < 0.15,
            qPrintable(QString("driftRateDy=%1").arg(tracker.status().driftRateDy)));
        tracker.restartEstimate();
        QVERIFY(!tracker.status().driftRateValid);

        // GuideController uses live drift speed immediately on its first pulse
        // even when calibration.drift is zero.
        GuideCalibration cal;
        cal.west = {0.01, 0.0};  // 10 px/s
        cal.north = {0.0, 0.01}; // 10 px/s
        cal.pierSide = PierSide::West;
        GuideController::Settings settings;
        settings.tolerancePixels = 10;
        GuideController withoutDrift(cal, settings);
        GuideController withDrift(cal, settings);
        GuideObservation o;
        o.now = SimulatedPointing::Clock::time_point{} + std::chrono::hours{1};
        o.locked = true;
        o.estimateValid = true;
        o.estimateSamples = 5;
        o.estimateDx = 12.0;
        o.mount.connected = true;
        o.mount.tracking = true;
        o.mount.canPulseGuide = true;
        o.mount.pierSide = PierSide::West;
        o.frameInterval = 1.0 / 30.0;
        const auto d0 = withoutDrift.step(o);
        o.driftRateValid = true;
        o.driftRateDx = 3.0; // +3 px/s opposing East correction
        const auto d1 = withDrift.step(o);
        QVERIFY(d0.pulse && d1.pulse);
        QCOMPARE(d0.pulse->direction, GuideDirection::East);
        QCOMPARE(d1.pulse->direction, GuideDirection::East);
        QVERIFY2(d1.pulse->milliseconds > d0.pulse->milliseconds + 500,
            qPrintable(QString("without=%1 ms with=%2 ms").arg(d0.pulse->milliseconds).arg(d1.pulse->milliseconds)));
    }

    // Done-when 6: each safety rule stops guiding or withholds pulses.
    void safetyRulesStopOrWithholdPulses_data()
    {
        QTest::addColumn<int>("rule");
        QTest::addColumn<QString>("reason");
        QTest::newRow("slewing") << 0 << "slewing";
        QTest::newRow("parked") << 1 << "parked";
        QTest::newRow("not tracking") << 2 << "not tracking";
        QTest::newRow("pier flip") << 3 << "side of pier";
        QTest::newRow("cannot pulse-guide") << 4 << "cannot pulse-guide";
        QTest::newRow("alt-azimuth") << 5 << "alt-azimuth";
    }

    void safetyRulesStopOrWithholdPulses()
    {
        QFETCH(int, rule);
        QFETCH(QString, reason);
        Loop loop({});
        GuideController controller(loop.trueCalibration(), {});
        // Let the drift carry the field out of the box, with the mount nominal.
        while (loop.elapsed() < 20) loop.step({});
        SimulatedMount::Condition& condition = loop.mount.condition;
        switch (rule)
        {
        case 0: condition.slewing = true; break;
        case 1: condition.parked = true; break;
        case 2: condition.tracking = false; break;
        case 3: condition.pierSide = PierSide::East; break;
        case 4: condition.canPulseGuide = false; break;
        case 5: condition.altAzimuth = true; break;
        }
        GuideObservation o = loop.observe();
        o.estimateDx = 50; // far outside the tolerance
        const GuideDecision decision = controller.step(o);
        QVERIFY(!decision.pulse);
        QVERIFY(controller.stopped());
        QVERIFY2(QString::fromStdString(controller.stopReason()).contains(reason), controller.stopReason().c_str());
        QCOMPARE(controller.calibrationInvalid(), rule == 3);
        QVERIFY(GuideController::refusal(loop.trueCalibration(), {}, loop.observe()).size() > 0);
    }

    void noPulseOnALostLockOrInvalidEstimate()
    {
        Loop loop({});
        GuideController controller(loop.trueCalibration(), {});
        GuideObservation o = loop.observe();
        o.estimateDx = 50;
        o.locked = false;
        QVERIFY(!controller.step(o).pulse);
        o.locked = true;
        o.estimateValid = false;
        QVERIFY(!controller.step(o).pulse);
        QVERIFY(!controller.stopped());
        o.estimateValid = true;
        QVERIFY(controller.step(o).pulse);
    }

    void mountThatMovesTheWrongWayStopsGuiding()
    {
        SimulatedPointing::Settings settings;
        settings.driftDec = 0;
        settings.driftRa = 0.5;
        Loop loop(settings);
        GuideCalibration wrong = loop.trueCalibration();
        wrong.west = wrong.west * -1.0; // as if RA were reversed after calibrating
        GuideController controller(wrong, {});
        while (!controller.stopped() && loop.elapsed() < 600) loop.step(controller.step(loop.observe()));
        QVERIFY(controller.stopped());
        QVERIFY2(controller.stopReason().find("no effect") != std::string::npos, controller.stopReason().c_str());
        QCOMPARE(controller.corrections(), GuideController::Settings{}.maxFruitlessCorrections);
    }

    void pulseTimeCapStopsGuiding()
    {
        SimulatedPointing::Settings settings;
        settings.driftRa = 2;
        Loop loop(settings);
        GuideController::Settings guide;
        guide.maxPulseMsPerMinute = 1000;
        GuideController controller(loop.trueCalibration(), guide);
        while (!controller.stopped() && loop.elapsed() < 120) loop.step(controller.step(loop.observe()));
        QVERIFY(controller.stopped());
        QVERIFY2(controller.stopReason().find("cap") != std::string::npos, controller.stopReason().c_str());
    }

    void guidingRefusesAGuideRateTooFastForTheDetector()
    {
        Loop loop({});
        GuideObservation o = loop.observe();
        o.frameInterval = 0.5; // 3.75 px/s at 2 fps is 1.9 px per frame
        const std::string refusal = GuideController::refusal(loop.trueCalibration(), {}, o);
        QVERIFY2(refusal.find("lower guide rate") != std::string::npos, refusal.c_str());
    }

    // Done-when 7: a flash during a pulse is saved, marked, and the clip lists the pulse.
    void candidateDuringAPulseIsMarked()
    {
        QTemporaryDir directory;
        QObject context;
        std::vector<ClipWriteResult> results;
        {
            EventWriter writer(&context, directory.path(), size_t{64} << 20, [&](const ClipWriteResult& r) { results.push_back(r); });
            const auto start = at(100);
            GuidePulseRecord pulse;
            pulse.start = start;
            pulse.milliseconds = 400;
            pulse.settleSeconds = 1.0;
            pulse.direction = "north";
            writer.logGuiding("pulse line\n", pulse, GuideStateRecord{start, "locked, guiding", 3.5, -12.0});
            writer.submit(clipAt(start + std::chrono::milliseconds{300}, 1));
            // Two seconds after the pulse ended and settled: not marked.
            writer.submit(clipAt(start + std::chrono::seconds{4}, 50));
            writer.shutdown();
        }
        QCoreApplication::processEvents();
        QCOMPARE(static_cast<int>(results.size()), 2);
        QFile log(QDir(directory.path()).filePath("candidates.txt"));
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const QStringList lines = QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts);
        QStringList outcomes;
        for (const QString& line : lines)
            if (!line.contains("_pending")) outcomes << line;
        QCOMPARE(outcomes.size(), 2);
        QVERIFY2(outcomes[0].contains("during_mount_correction=yes") && outcomes[0].contains("clip="), qPrintable(outcomes[0]));
        QVERIFY2(!outcomes[1].contains("during_mount_correction"), qPrintable(outcomes[1]));
        QFile event(QDir(results[0].folder).filePath("event.txt"));
        QVERIFY(event.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(event.readAll());
        QVERIFY2(text.contains("mount_pulses_in_clip: 1"), qPrintable(text));
        QVERIFY2(text.contains("mount_pulse: start=") && text.contains("direction=north ms=400"), qPrintable(text));
        QVERIFY2(text.contains("guiding_at_first_trigger: state=\"locked, guiding\" offset_dx=3.50"), qPrintable(text));
        QFile second(QDir(results[1].folder).filePath("event.txt"));
        QVERIFY(second.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(QString::fromUtf8(second.readAll()).contains("mount_pulses_in_clip: 0"));
    }

private:
    // A stepped simulation: the simulated mount and pointing, and an
    // estimate built the way the tracker builds it (median of the matches
    // since the last restart, over its window), from the true image offset
    // plus seeing noise. One step is one measurement, 0.2 s.
    struct Loop
    {
        explicit Loop(SimulatedPointing::Settings settings)
            : now(SimulatedPointing::Clock::time_point{} + std::chrono::hours{1})
            , pointing(std::make_shared<SimulatedPointing>(settings, now))
            , mount(pointing, [this] { return now; })
            , start(now)
        {
            mount.connect();
        }

        GuideObservation observe()
        {
            const auto offset = pointing->imageOffset(now);
            const double nx = offset.x + 0.5 * noise(static_cast<int>(count), 1, 5);
            const double ny = offset.y + 0.5 * noise(static_cast<int>(count), 2, 5);
            if (count != lastSampled)
            {
                samples.push_back({now, nx, ny});
                lastSampled = count;
            }
            while (!samples.empty() && std::chrono::duration<double>(now - samples.front().time).count() > LockTracker::kEstimateSeconds)
                samples.pop_front();
            GuideObservation o;
            o.now = now;
            o.locked = true;
            o.foundDx = nx;
            o.foundDy = ny;
            o.estimateSamples = static_cast<int>(samples.size());
            o.estimateValid = samples.size() >= LockTracker::kMinEstimateSamples;
            if (o.estimateValid)
            {
                std::vector<double> xs, ys;
                for (const Sample& sample : samples)
                {
                    xs.push_back(sample.x);
                    ys.push_back(sample.y);
                }
                std::sort(xs.begin(), xs.end());
                std::sort(ys.begin(), ys.end());
                o.estimateDx = xs[xs.size() / 2];
                o.estimateDy = ys[ys.size() / 2];
            }
            o.mount = mount.state();
            o.frameInterval = 1.0 / 30.0;
            return o;
        }

        void step(const GuideDecision& decision)
        {
            if (decision.restartEstimate) samples.clear();
            if (decision.pulse) mount.pulseGuide(decision.pulse->direction, decision.pulse->milliseconds);
            now += std::chrono::milliseconds{200};
            ++count;
        }

        [[nodiscard]] double elapsed() const { return std::chrono::duration<double>(now - start).count(); }

        [[nodiscard]] GuideCalibration trueCalibration() const
        {
            const auto west = pointing->imageRate(GuideDirection::West);
            const auto north = pointing->imageRate(GuideDirection::North);
            GuideCalibration calibration;
            calibration.west = {west.x, west.y};
            calibration.north = {north.x, north.y};
            calibration.pierSide = PierSide::West;
            return calibration;
        }

        struct Sample
        {
            SimulatedPointing::Clock::time_point time;
            double x, y;
        };

        SimulatedPointing::Clock::time_point now;
        std::shared_ptr<SimulatedPointing> pointing;
        SimulatedMount mount;
        SimulatedPointing::Clock::time_point start;
        std::deque<Sample> samples;
        uint64_t count{0};
        uint64_t lastSampled{~uint64_t{0}};
    };

    static EventClip clipAt(std::chrono::system_clock::time_point trigger, uint64_t index)
    {
        EventClip clip;
        ClipTrigger first;
        first.onsetFrameIndex = index;
        first.triggerFrameIndex = index;
        first.triggerTime = trigger;
        first.brightPixelCount = 9;
        clip.triggers.push_back(first);
        clip.framesBefore = 1;
        for (int i = 0; i < 2; ++i)
        {
            auto frame = std::make_shared<Frame>();
            frame->width = 2;
            frame->height = 2;
            frame->frameIndex = index - 1 + static_cast<uint64_t>(i);
            frame->timestamp = trigger - std::chrono::milliseconds{33} * (1 - i);
            frame->data.assign(4, static_cast<uint8_t>(i ? 200 : 10));
            clip.frames.push_back(frame);
        }
        return clip;
    }
};

// The clip writer draws its preview images with fonts, which need a GUI application.
int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    GuidingTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "guiding_tests.moc"
