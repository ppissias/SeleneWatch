#include "guiding/feature_lock.h"
#include "guiding/guider.h"
#include "guiding/lock_tracker.h"
#include "recording/event_writer.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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

        // Gone for good: after kSearchSeconds of searching nothing more is measured.
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
            return QTest::qWaitFor([&] { return guider.status().frameIndex == frame->frameIndex; }, 5000);
        };
        // Nothing is wanted yet, so the frame is not kept.
        guider.offer(canvas.frame(0, 0, 0.0, ++index), true);
        QTest::qWait(50);
        QCOMPARE(guider.status().state, LockState::Off);

        guider.requestLock({100, 70, 24, 24});
        QVERIFY(offer(canvas.frame(0, 0, 0.2, ++index), false));
        QCOMPARE(guider.status().state, LockState::Locked);
        // Preview: measured, not written.
        QVERIFY(offer(canvas.frame(2, -1, 0.4, ++index), false));
        for (int step = 3; step <= 10; ++step) QVERIFY(offer(canvas.frame(2, -1, step * 0.2, ++index), true));
        QVERIFY(guider.status().estimateValid);
        QVERIFY(notified.load() > 0);
        guider.requestUnlock();
        QVERIFY(QTest::qWaitFor([&] { return guider.status().state == LockState::Off; }, 5000));

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
};

QTEST_GUILESS_MAIN(GuidingTest)
#include "guiding_tests.moc"
