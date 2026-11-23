#include "detector_observation.h"
#include "capture/event_capture.h"
#include "recording/clip_overview.h"
#include "recording/event_writer.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImageReader>
#include <QtTest/QTest>

#include <cmath>
#include <functional>
#include <random>

namespace {

std::chrono::system_clock::time_point timeOf(uint64_t index)
{
    return std::chrono::system_clock::time_point{} + std::chrono::seconds{1'790'000'000}
        + std::chrono::microseconds{static_cast<int64_t>(index) * 33'333};
}

// A Mono8 frame of a bright scene, like the sunlit Moon, at 30 fps. change
// gives each pixel's difference from the level; noise is Gaussian.
std::shared_ptr<Frame> scene(int width, int height, uint64_t index, std::mt19937& random,
    const std::function<double(int x, int y)>& change = {}, double level = 200, double noise = 2)
{
    auto frame = std::make_shared<Frame>();
    frame->width = width; frame->height = height; frame->frameIndex = index;
    frame->timestamp = timeOf(index);
    frame->testSource = true;
    frame->data.resize(static_cast<size_t>(width) * height);
    std::normal_distribution<double> scatter(0, noise);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            frame->data[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(
                std::clamp(std::lround(level + (change ? change(x, y) : 0.0) + scatter(random)), 0L, 255L));
    return frame;
}

DetectionSettings withDarkening()
{
    DetectionSettings settings;
    settings.darkening = true;
    return settings;
}

QString contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

// Feature-13: a trigger for darkening.
class DarkeningTest : public QObject
{
    Q_OBJECT
private slots:
    void aDarkPatchFiresOnlyWithDarkeningOn();
    void aDarkDiskCrossingIsOneClipShownInBlue();
    void withDarkeningOnlyTheFallIsLookedFor();
    void aLargeDarkeningFiresOnceAndASlowOneNever();
    void tooManyDarkPixelsStopTheSearchAndSaySo();
};

// Done-when 1: a 5 x 5 patch darkened for two frames.
void DarkeningTest::aDarkPatchFiresOnlyWithDarkeningOn()
{
    for (const bool on : {false, true})
    {
        std::mt19937 random(31);
        DetectionSettings settings;
        settings.darkening = on;
        FlashDetector detector(settings);
        int fired = 0;
        DetectionSummary trigger;
        for (uint64_t i = 0; i < 60; ++i)
        {
            const bool dark = i == 40 || i == 41;
            const auto frame = scene(64, 48, i, random, [dark](int x, int y) {
                return dark && x >= 20 && x < 25 && y >= 20 && y < 25 ? -40.0 : 0.0;
            });
            if (observe(detector, *frame))
            {
                ++fired;
                trigger = detector.candidates().front();
            }
        }
        if (!on)
        {
            QCOMPARE(fired, 0);
            continue;
        }
        QCOMPARE(fired, 1);
        QCOMPARE(trigger.change, ChangeKind::Fall);
        QCOMPARE(trigger.frameIndex, uint64_t{40});
        QCOMPARE(trigger.brightPixels, 25);
        QCOMPARE(QRect(trigger.boxX, trigger.boxY, trigger.boxWidth, trigger.boxHeight), QRect(20, 20, 5, 5));
        QVERIFY(trigger.peakSigma > 10);
        QVERIFY(trigger.peakAdu < -30);
    }
}

// Done-when 2 and 6: a dark disk 40 pixels across crosses a bright scene at
// 30 pixels a frame, as a plane or the ISS crosses the sunlit Moon.
void DarkeningTest::aDarkDiskCrossingIsOneClipShownInBlue()
{
    std::mt19937 random(32);
    std::vector<EventClip> clips;
    int candidates = 0;
    EventCapture capture(CaptureSettings{}, [&] { ++candidates; }, [&](EventClip clip) { clips.push_back(std::move(clip)); },
        withDarkening());
    for (uint64_t i = 0; i < 200; ++i)
    {
        const double centreX = 20 + 30.0 * (static_cast<double>(i) - 60);
        const bool crossing = i >= 60 && i < 66;
        capture.onFrame(scene(200, 100, i, random, [crossing, centreX](int x, int y) {
            return crossing && std::hypot(x - centreX, y - 50.0) <= 20 ? -140.0 : 0.0;
        }));
    }
    capture.finishPartial();
    QCOMPARE(candidates, 1);
    QCOMPARE(clips.size(), size_t{1});
    for (const ClipTrigger& trigger : clips[0].triggers) QCOMPARE(trigger.change, ChangeKind::Fall);
    // The clip's peak preview is for rises; a darkening leaves it empty.
    QVERIFY(!clips[0].peak.ready);

    OverviewRequest request;
    request.onsetFrameIndex = clips[0].triggers.front().onsetFrameIndex;
    const ClipOverview overview = makeClipOverview(clips[0].frames, request);
    QVERIFY2(overview.activity.contains("fall") && overview.activity.contains("moved"), qPrintable(overview.activity));
    QVERIFY(overview.changedBounds.left() <= 2 && overview.changedBounds.right() >= 175);

    QTemporaryDir directory;
    QObject context;
    std::vector<ClipWriteResult> results;
    {
        EventWriter writer(&context, directory.path(), 256 * 1024 * 1024, [&](const auto& result) { results.push_back(result); });
        writer.submit(std::move(clips[0]));
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), size_t{1}, 10000);
    }
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    const QString text = contents(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.contains(" kind=darkening fall_adu="));
    QVERIFY(text.contains(" darkening=yes max_dark_pixels=20000 max_dark_box_ratio=10"));
    QVERIFY(text.contains("preview_gif: 04_event_preview.gif "));
    const QString gifPath = QDir(results[0].folder).filePath("04_event_preview.gif");
    QVERIFY(QFile::exists(gifPath));
    QImageReader gif(gifPath);
    QVERIFY(gif.imageCount() >= 1 && gif.imageCount() <= 10);
    QVERIFY(QFileInfo(gifPath).size() <= 512 * 1024);
    const QImage saved(QDir(results[0].folder).filePath("00_overview.png"));
    // The main view is 200 pixels wide, enlarged four times; the disk's path
    // runs along row 50.
    const QRgb pixel = saved.pixel(100 * 4, 50 * 4);
    QVERIFY2(qBlue(pixel) > qRed(pixel) + 60, qPrintable(QString::number(pixel, 16)));
}

// Feature-45 (Done-when 3 of Feature-13 before it): a rise and a fall
// touching on one frame. With darkening only the fall is looked for, and
// what the detector reports, its history and its sigma map are of the fall;
// without darkening only the rise.
void DarkeningTest::withDarkeningOnlyTheFallIsLookedFor()
{
    const auto touching = [](int x, int y) {
        if (y < 20 || y >= 23) return 0.0;
        if (x >= 20 && x < 23) return 40.0;
        if (x >= 23 && x < 26) return -40.0;
        return 0.0;
    };
    for (const bool darkening : {true, false})
    {
        std::mt19937 random(33);
        DetectionSettings settings;
        settings.darkening = darkening;
        FlashDetector detector(settings);
        for (uint64_t i = 0; i < 40; ++i) observe(detector, *scene(64, 48, i, random));
        const auto frame = scene(64, 48, 40, random, touching);
        QVERIFY(detector.analyze(*frame, {DetectionOverlay::Sigma, -1, -1, true}));
        QCOMPARE(detector.candidates().size(), size_t{1});
        const DetectionSummary& candidate = detector.candidates().front();
        QCOMPARE(candidate.change, darkening ? ChangeKind::Fall : ChangeKind::Rise);
        QCOMPARE(candidate.brightPixels, 9);
        QCOMPARE(candidate.boxX, darkening ? 23 : 20);
        QVERIFY(darkening ? candidate.peakAdu < -30 : candidate.peakAdu > 30);
        // The whole frame's measurement, as the panel and its history show it.
        const DetectionView view = detector.view();
        QCOMPARE(view.summary.peakSigma, candidate.peakSigma);
        QVERIFY(view.summary.peakSigma > settings.sigma);
        QCOMPARE(view.history.back().peakSigma, candidate.peakSigma);
        QVERIFY(view.map);
        const auto sigmaAt = [&view](int x) { return view.map->values[static_cast<size_t>(21) * view.map->width + x]; };
        const int rise = 21, fall = 24;
        QVERIFY(sigmaAt(darkening ? fall : rise) > settings.sigma);
        QCOMPARE(sigmaAt(darkening ? rise : fall), 0.0f);
        detector.updateReference(*frame);
    }
}

// Done-when 4: 30% of the frame darkened for three frames fires once. A slow
// darkening over 30 s fires nothing.
void DarkeningTest::aLargeDarkeningFiresOnceAndASlowOneNever()
{
    DetectionSettings settings = withDarkening();
    settings.darkeningShare = 0.05;
    // In this small frame 30% is 3000 pixels, small enough to pass as a dark
    // patch too. At full frame, 5% is far above the dark patch limit.
    settings.maxDarkPixels = 1000;
    {
        std::mt19937 random(34);
        FlashDetector detector(settings);
        std::vector<DetectionSummary> triggers;
        for (uint64_t i = 0; i < 90; ++i)
        {
            const bool dark = i >= 60 && i < 63;
            observe(detector, *scene(100, 100, i, random, [dark](int x, int) { return dark && x < 30 ? -60.0 : 0.0; }));
            for (const DetectionSummary& candidate : detector.candidates()) triggers.push_back(candidate);
        }
        QCOMPARE(triggers.size(), size_t{1});
        QCOMPARE(triggers[0].change, ChangeKind::LargeFall);
        QVERIFY(std::abs(triggers[0].share - 0.3) < 0.02);
    }
    {
        std::mt19937 random(35);
        FlashDetector detector(settings);
        int fired = 0;
        for (uint64_t i = 0; i < 900; ++i)
            fired += observe(detector, *scene(64, 48, i, random, {}, 200 - 50.0 * static_cast<double>(i) / 899)) ? 1 : 0;
        QCOMPARE(fired, 0);
    }
}

// Done-when 5: more dark pixels than four times the maximum dark patch (at
// least 4096) are not split into patches, as on the bright side. Since
// Feature-45 a flash elsewhere is not looked for either.
void DarkeningTest::tooManyDarkPixelsStopTheSearchAndSaySo()
{
    std::mt19937 random(36);
    DetectionSettings settings = withDarkening();
    settings.maxDarkPixels = 100;
    FlashDetector detector(settings);
    for (uint64_t i = 0; i < 40; ++i) observe(detector, *scene(140, 100, i, random));
    // 5000 pixels fall, about a third of the frame, beside a flash.
    const auto frame = scene(140, 100, 40, random, [](int x, int y) {
        if (x < 50) return -60.0;
        return x >= 100 && x < 103 && y >= 70 && y < 73 ? 60.0 : 0.0;
    });
    QVERIFY(!observe(detector, *frame));
    QVERIFY(detector.summary().patchSearchLimited);
    QCOMPARE(detector.summary().brightPixels, 5000);
    QVERIFY(detector.candidates().empty());
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    DarkeningTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "darkening_tests.moc"
