#include "blip_injector.h"
#include "event_capture.h"
#include "flash_detector.h"
#include "recording/event_writer.h"
#include "recording/ser_reader.h"
#include "recording/ser_writer.h"
#include "session.h"
#include "sources/recording_source.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <random>

namespace {

Frame flat(uint64_t index, int value = 100)
{
    Frame frame;
    frame.width = 64; frame.height = 48;
    frame.frameIndex = index;
    frame.timestamp = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{static_cast<int64_t>(index) * 33};
    frame.data.assign(64 * 48, static_cast<uint8_t>(value));
    frame.testSource = true;
    return frame;
}

void patch(Frame& frame, int x, int y, int side, int addition)
{
    for (int row = y; row < y + side; ++row)
        for (int col = x; col < x + side; ++col)
            frame.data[static_cast<size_t>(row) * frame.width + col] += static_cast<uint8_t>(addition);
}

Frame noisy(uint64_t index, std::mt19937& random)
{
    Frame frame = flat(index);
    std::normal_distribution<double> noise(0, 1);
    for (int y = 0; y < frame.height; ++y)
        for (int x = 0; x < frame.width; ++x)
            frame.data[static_cast<size_t>(y) * frame.width + x] = static_cast<uint8_t>(
                std::clamp(static_cast<int>(std::round(100 + noise(random) * (x < 32 ? 1 : 5))), 0, 255));
    return frame;
}

QString contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

class DetectionTest : public QObject
{
    Q_OBJECT
private slots:
    void oneFrameAndExactThreshold();
    void rejectsHotPixelsLargeAndScatteredChanges();
    void anIsolatedPixelCannotVetoACompactFlash();
    void twoFrameReferenceIsHeld();
    void nativeMono16AndBayerKeepSubPreviewSignals();
    void localNoiseNormalizesTwoDifferentHalves();
    void settingsAffectTheNextFrame();
    void overlaysChangeNoPixelsOrDecisions();
    void capturedSettingsAndPeakRemainAtTheirOriginalValues();
    void aOneFrameBlipThroughSessionIsSaved();
    void aLoopFlushesTheClipAtItsBoundary();
};

void DetectionTest::oneFrameAndExactThreshold()
{
    for (int addition : {2, 3})
    {
        FlashDetector detector;
        detector.observe(flat(0));
        Frame frame = flat(1);
        patch(frame, 20, 20, 3, addition);
        QCOMPARE(detector.observe(frame), addition == 3); // 3 / 0.5 = exactly 6 sigma.
        QCOMPARE(detector.summary().settings.sigma, 6.0);
        QCOMPARE(detector.summary().peakSigma, addition * 2.0);
        if (addition == 3)
        {
            QCOMPARE(detector.summary().brightPixels, 9);
            QVERIFY(!detector.observe(frame));
            QVERIFY(detector.firing());
            QVERIFY(!detector.observe(flat(2)));
            QVERIFY(detector.observe(frame));
        }
    }
}

void DetectionTest::rejectsHotPixelsLargeAndScatteredChanges()
{
    FlashDetector detector;
    detector.observe(flat(0));
    Frame hot = flat(1); patch(hot, 4, 4, 1, 100);
    QVERIFY(!detector.observe(hot));
    Frame broad = flat(2, 200);
    QVERIFY(!detector.observe(broad));
    detector.reset(); detector.observe(flat(0));
    Frame scattered = flat(1);
    for (int y : {2, 20, 40}) for (int x : {2, 30, 60}) patch(scattered, x, y, 1, 100);
    QVERIFY(!detector.observe(scattered));
    QCOMPARE(detector.summary().brightPixels, 1);
    QVERIFY(!detector.summary().passed);
}

void DetectionTest::twoFrameReferenceIsHeld()
{
    FlashDetector detector({6, 8, 400, false});
    detector.observe(flat(0));
    Frame first = flat(1); patch(first, 20, 20, 3, 20);
    QVERIFY(!detector.observe(first));
    const double peak = detector.summary().peakSigma;
    first.frameIndex = 2;
    QVERIFY(detector.observe(first));
    QCOMPARE(detector.summary().peakSigma, peak);
    Frame shifted = flat(3, 101);
    QVERIFY(!detector.observe(shifted, true));
    QCOMPARE(detector.difference(shifted).front(), 1.0f);
    for (int i = 0; i < 20; ++i) detector.observe(shifted, true);
    QCOMPARE(detector.difference(shifted).front(), 1.0f);
}

void DetectionTest::anIsolatedPixelCannotVetoACompactFlash()
{
    FlashDetector detector;
    detector.observe(flat(0));
    Frame frame = flat(1);
    patch(frame, 20, 20, 3, 20);
    patch(frame, 1, 1, 1, 50);
    QVERIFY(detector.observe(frame));
    QCOMPARE(detector.summary().brightPixels, 9);
    QCOMPARE(detector.summary().peakSigma, 40.0);
    QCOMPARE(detector.summary().boxX, 20);
    QCOMPARE(detector.summary().boxY, 20);
}

void DetectionTest::nativeMono16AndBayerKeepSubPreviewSignals()
{
    for (const auto alignment : {BitAlignment::Low, BitAlignment::High})
    {
        Frame frame = flat(0);
        frame.format = PixelFormat::Mono16; frame.significantBits = 12;
        frame.alignment = alignment; frame.bayer = BayerPattern::BGGR;
        const int shift = alignment == BitAlignment::High ? 4 : 0;
        frame.data.clear();
        for (int i = 0; i < 64 * 48; ++i)
        {
            const int sample = 1000 << shift;
            frame.data.push_back(static_cast<uint8_t>(sample & 255));
            frame.data.push_back(static_cast<uint8_t>(sample >> 8));
        }
        FlashDetector detector;
        detector.observe(frame);
        for (int y = 20; y < 23; ++y) for (int x = 20; x < 23; ++x)
        {
            const size_t at = static_cast<size_t>(y * 64 + x) * 2;
            const int sample = 1003 << shift;
            frame.data[at] = static_cast<uint8_t>(sample & 255);
            frame.data[at + 1] = static_cast<uint8_t>(sample >> 8);
        }
        const Frame before = frame;
        QVERIFY(detector.observe(frame));
        QCOMPARE(detector.summary().peakAdu, 3.0);
        QCOMPARE(detector.summary().peakSigma, 6.0);
        QCOMPARE(frame.data, before.data);
        QVERIFY(frame.bayer == BayerPattern::BGGR);
    }
}

void DetectionTest::localNoiseNormalizesTwoDifferentHalves()
{
    for (const bool inNoiseUnits : {false, true})
    {
        for (const int x : {10, 44})
        {
            FlashDetector detector;
            std::mt19937 random(47);
            for (int i = 0; i < 180; ++i) detector.observe(noisy(static_cast<uint64_t>(i), random));
            Frame probe = flat(180);
            detector.observe(probe, false, {DetectionOverlay::None, x, 20, false});
            const double localNoise = detector.summary().probeNoise;
            QVERIFY(localNoise > 0);
            Frame flash = flat(181);
            patch(flash, x, 20, 4, inNoiseUnits ? static_cast<int>(std::ceil(8 * localNoise)) : 18);
            QCOMPARE(detector.observe(flash), inNoiseUnits || x < 32);
        }
    }
}

void DetectionTest::settingsAffectTheNextFrame()
{
    FlashDetector detector;
    detector.observe(flat(0));
    Frame flash = flat(1); patch(flash, 20, 20, 3, 3);
    detector.setSettings({8, 8, 400, true});
    QVERIFY(!detector.observe(flash, true));
    detector.setSettings({5, 8, 400, true});
    QVERIFY(detector.observe(flash, true));
    QCOMPARE(detector.summary().settings.sigma, 5.0);
    detector.observe(flat(2));
    detector.setSettings({5, 12, 400, true});
    QVERIFY(!detector.observe(flash));

    // Two-frame confirmation cannot combine frames tested at different K.
    FlashDetector persistent({6, 8, 400, false});
    persistent.observe(flat(0));
    QVERIFY(!persistent.observe(flash));
    persistent.setSettings({5, 8, 400, false});
    flash.frameIndex = 2;
    QVERIFY(!persistent.observe(flash));
    flash.frameIndex = 3;
    QVERIFY(persistent.observe(flash));
    QCOMPARE(persistent.onsetFrameIndex(), uint64_t{2});
    persistent.setSettings({4, 8, 400, true});
    flash.frameIndex = 4;
    QVERIFY(!persistent.observe(flash));
    QVERIFY(persistent.firing());
}

void DetectionTest::overlaysChangeNoPixelsOrDecisions()
{
    FlashDetector plain, decorated;
    std::mt19937 random(79);
    for (int i = 0; i < 60; ++i)
    {
        Frame frame = noisy(static_cast<uint64_t>(i), random);
        if (i == 40) patch(frame, 10, 20, 4, 50);
        const auto bytes = frame.data;
        const bool expected = plain.observe(frame);
        const bool actual = decorated.observe(frame, false,
            {i % 2 ? DetectionOverlay::Sigma : DetectionOverlay::Noise, 10, 20, true});
        QCOMPARE(actual, expected);
        QCOMPARE(decorated.summary().brightPixels, plain.summary().brightPixels);
        QCOMPARE(decorated.summary().noiseFloor, plain.summary().noiseFloor);
        QCOMPARE(decorated.summary().peakSigma, plain.summary().peakSigma);
        QCOMPARE(frame.data, bytes);
        if (i > 0) QVERIFY(decorated.view().map);
    }
}

void DetectionTest::capturedSettingsAndPeakRemainAtTheirOriginalValues()
{
    std::vector<EventClip> clips;
    EventCapture capture(64, [] {}, [&](EventClip clip) { clips.push_back(std::move(clip)); });
    capture.onFrame(std::make_shared<const Frame>(flat(0)));
    Frame flash = flat(1); patch(flash, 20, 20, 3, 20);
    capture.onFrame(std::make_shared<const Frame>(flash));
    capture.setDetectionSettings({100, 64, 900, false});
    for (int i = 2; i < 34; ++i) capture.onFrame(std::make_shared<const Frame>(flat(static_cast<uint64_t>(i))));
    QCOMPARE(clips.size(), size_t{1});
    QCOMPARE(clips[0].triggers.size(), size_t{1});
    QCOMPARE(clips[0].triggers[0].settings.sigma, 6.0);
    QVERIFY(clips[0].triggers[0].settings.oneFrame);
    QCOMPARE(clips[0].peak.frameIndex, uint64_t{1});
    QCOMPARE(clips[0].peakDifference[20 * 64 + 20], 20.0f);

    QTemporaryDir directory;
    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, directory.path(), 100, [&](const auto& result) { results.push_back(result); });
    writer.submit(std::move(clips[0]));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), size_t{1}, 5000);
    QVERIFY(results[0].notSaved.isEmpty());
    const QString text = contents(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.contains("sigma=6 min_pixels=8 max_pixels=400 one_frame=yes"));
    QVERIFY(text.contains("peak_preview_frame_index: 1\n"));
    QVERIFY(text.contains("peak_settings: sigma=6 min_pixels=8 max_pixels=400 one_frame=yes"));
    QVERIFY(QFile::exists(QDir(results[0].folder).filePath("03_diff_annotated.png")));
}

void DetectionTest::aOneFrameBlipThroughSessionIsSaved()
{
    QTemporaryDir directory;
    const QString input = QDir(directory.path()).filePath("quiet.ser");
    QFile file(input);
    QVERIFY(file.open(QIODevice::WriteOnly));
    SerWriter ser(file);
    QString error;
    Frame frame = flat(0);
    QVERIFY(ser.start(frame, "test", error));
    for (int i = 0; i < 40; ++i) { frame = flat(static_cast<uint64_t>(i)); QVERIFY(ser.append(frame, error)); }
    QVERIFY(ser.finish(error)); file.close();
    QObject context;
    bool requested = false, ended = false;
    std::vector<ClipWriteResult> results;
    Session* running = nullptr;
    Session::Notifications notices;
    notices.previewReady = [&] {
        const auto preview = running->takePreview();
        if (preview.frame && !requested) requested = running->requestBlip({20, 20, 4, 50, 1});
    };
    notices.ended = [&] { ended = true; };
    notices.clipWritten = [&](const auto& result) { results.push_back(result); };
    Session session(&context, QDir(directory.path()).filePath("events"), std::move(notices));
    running = &session;
    session.setSource(std::make_unique<RecordingSource>(std::filesystem::path(input.toStdWString())));
    QVERIFY2(session.start(error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(ended, 5000);
    QVERIFY(requested);
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), size_t{1}, 5000);
    const QString text = contents(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.contains("one_frame=yes"));
    QCOMPARE(text.count(" test_blip="), 1);
}

void DetectionTest::aLoopFlushesTheClipAtItsBoundary()
{
    QTemporaryDir directory;
    const QString input = QDir(directory.path()).filePath("late-flash.ser");
    QFile file(input);
    QVERIFY(file.open(QIODevice::WriteOnly));
    SerWriter ser(file);
    QString error;
    Frame frame = flat(0);
    QVERIFY(ser.start(frame, "test", error));
    for (int i = 0; i < 10; ++i)
    {
        frame = flat(static_cast<uint64_t>(i));
        if (i >= 8) patch(frame, 20, 20, 3, 20);
        QVERIFY(ser.append(frame, error));
    }
    QVERIFY(ser.finish(error)); file.close();
    QObject context;
    std::vector<ClipWriteResult> results;
    int candidates = 0;
    Session::Notifications notices;
    notices.candidateStarted = [&] { ++candidates; };
    notices.clipWritten = [&](const auto& result) { results.push_back(result); };
    Session session(&context, QDir(directory.path()).filePath("events"), std::move(notices));
    auto source = std::make_unique<RecordingSource>(std::filesystem::path(input.toStdWString()));
    source->setLoopSeconds(1);
    session.setSource(std::move(source));
    QVERIFY2(session.start(error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(candidates >= 3, 5000);
    session.stop();
    QTRY_VERIFY_WITH_TIMEOUT(results.size() >= 3, 5000);
    for (const auto& result : results)
    {
        QVERIFY(result.notSaved.isEmpty());
        const QString metadata = contents(QDir(result.folder).filePath("event.txt"));
        uint64_t first = 0, last = 0;
        bool haveFirst = false;
        for (const QString& line : metadata.split('\n'))
        {
            bool isFrame = false;
            line.section(' ', 0, 0).toInt(&isFrame);
            if (!isFrame) continue;
            const auto index = line.section(' ', 1, 1).toULongLong();
            if (!haveFirst) { first = index; haveFirst = true; }
            last = index;
        }
        QVERIFY(haveFirst);
        QCOMPARE(first / 10, last / 10);
    }
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    DetectionTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "detection_tests.moc"
