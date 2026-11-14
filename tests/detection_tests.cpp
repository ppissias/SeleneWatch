#include "tests/detector_observation.h"
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
    void aLaterPatchTriggersWhileAnotherStaysActive();
    void unrelatedPatchesCannotConfirmEachOther();
    void aClipTailDoesNotFreezeQuietBackgroundLearning();
    void repeatedFlashesRemainIndependentDuringSixMinutesOfActivity();
    void simultaneousCandidatesAreAllRetained();
    void differencesAreCopiedBeforeLearning();
    void patchMergesAndSplitsDoNotDuplicateEvents();
    void aDroppedFrameBreaksPendingConfirmation();
    void aNewWeakerCandidateDoesNotReplaceTheStrongestPeak();
    void aFullyProtectedFieldDoesNotAdvanceLearning();
};

void DetectionTest::aLaterPatchTriggersWhileAnotherStaysActive()
{
    FlashDetector detector;
    observe(detector,flat(0));
    Frame first=flat(1); patch(first,5,5,3,50);
    QVERIFY(observe(detector,first));
    Frame later=flat(2); patch(later,5,5,3,50); patch(later,40,25,3,20);
    QVERIFY(observe(detector,later));
    QCOMPARE(detector.summary().peakX,40);
    QCOMPARE(detector.onsetFrameIndex(),uint64_t{2});
    QVERIFY(!observe(detector,later));
}

void DetectionTest::unrelatedPatchesCannotConfirmEachOther()
{
    FlashDetector detector({6,8,400,false});
    observe(detector,flat(0));
    Frame first=flat(1); patch(first,5,5,3,20);
    QVERIFY(!observe(detector,first));
    Frame next=flat(2); patch(next,40,25,3,20);
    QVERIFY(!observe(detector,next));
    next.frameIndex=3;
    QVERIFY(observe(detector,next));
    QCOMPARE(detector.onsetFrameIndex(),uint64_t{2});
}

void DetectionTest::aClipTailDoesNotFreezeQuietBackgroundLearning()
{
    EventCapture capture(CaptureSettings{},[]{},[](EventClip){});
    capture.onFrame(std::make_shared<const Frame>(flat(0)));
    Frame first=flat(1); patch(first,5,5,3,20);
    capture.onFrame(std::make_shared<const Frame>(first));
    for (int i=2;i<50;++i)
    {
        Frame frame=flat(i);
        patch(frame,40,25,8,i%2?2:0);
        capture.onFrame(std::make_shared<const Frame>(frame),{DetectionOverlay::None,42,27,false});
    }
    QVERIFY(capture.detectionSummary().probeNoise>0.8);
}

void DetectionTest::repeatedFlashesRemainIndependentDuringSixMinutesOfActivity()
{
    int candidates=0, clips=0;
    EventCapture capture({.5,.3,1},[&]{++candidates;},[&](EventClip){++clips;});
    capture.onFrame(std::make_shared<const Frame>(flat(0)));
    for (int i=1;i<3600;++i)
    {
        Frame f=flat(i); f.timestamp=std::chrono::system_clock::time_point{}+std::chrono::milliseconds{i*100};
        patch(f,5,5,3,50);
        const bool injected=i==1 || i%37==0;
        if (injected) patch(f,40,25,3,20);
        const auto bytes=f.data;
        capture.onFrame(std::make_shared<const Frame>(f));
        QCOMPARE(f.data,bytes);
        const auto& s=capture.detectionSummary();
        QCOMPARE(s.newCandidates,static_cast<size_t>((i==1?1:0)+(injected?1:0)));
        QVERIFY(s.protectedPixels<=50);
        if (injected && i>1) QCOMPARE(s.peakX,40);
    }
    capture.finishPartial();
    QCOMPARE(candidates,99); QVERIFY(clips>300);
    QCOMPARE(capture.detectionSummary().learningFrames,size_t{3599});
    QVERIFY(capture.detectionSummary().longestActiveSeconds>359);
}

void DetectionTest::simultaneousCandidatesAreAllRetained()
{
    EventClip saved; int candidates=0;
    EventCapture capture(CaptureSettings{},[&]{++candidates;},[&](EventClip clip){saved=std::move(clip);});
    capture.onFrame(std::make_shared<const Frame>(flat(0)));
    Frame f=flat(1); patch(f,5,5,3,20); patch(f,40,25,3,30);
    capture.onFrame(std::make_shared<const Frame>(f)); capture.finishPartial();
    QCOMPARE(candidates,2); QCOMPARE(saved.triggers.size(),size_t{2});
    QCOMPARE(saved.triggers[0].peakX,5); QCOMPARE(saved.triggers[1].peakX,40);
    QCOMPARE(saved.frames.size(),size_t{2});
    QCOMPARE(saved.peakDifference[25*64+40],30.0f);
}

void DetectionTest::differencesAreCopiedBeforeLearning()
{
    FlashDetector detector;
    observe(detector,flat(0));
    Frame f=flat(1,101);
    detector.analyze(f);
    BudgetVector<float> difference;
    detector.difference(f,difference); QCOMPARE(difference.front(),1.0f);
    QVERIFY_THROWS_EXCEPTION(std::logic_error,static_cast<void>(detector.analyze(f)));
    detector.updateReference(f);
    QVERIFY_THROWS_EXCEPTION(std::logic_error,detector.difference(f,difference));
}

void DetectionTest::patchMergesAndSplitsDoNotDuplicateEvents()
{
    FlashDetector detector; observe(detector,flat(0));
    Frame split=flat(1); patch(split,5,5,3,20); patch(split,10,5,3,20);
    QVERIFY(observe(detector,split)); QCOMPARE(detector.candidates().size(),size_t{2});
    Frame merged=split; merged.frameIndex=2;
    merged.data[6*64+8]=120; merged.data[6*64+9]=120;
    QVERIFY(!observe(detector,merged)); QVERIFY(detector.firing());
    split.frameIndex=3;
    QVERIFY(!observe(detector,split)); QVERIFY(detector.firing());
    QCOMPARE(detector.summary().activePatches,size_t{2});
}

void DetectionTest::aDroppedFrameBreaksPendingConfirmation()
{
    FlashDetector detector({6,8,400,false}); observe(detector,flat(0));
    Frame f=flat(1); patch(f,5,5,3,20); QVERIFY(!observe(detector,f));
    f.frameIndex=3; QVERIFY(!observe(detector,f));
    f.frameIndex=4; QVERIFY(observe(detector,f));
    QCOMPARE(detector.onsetFrameIndex(),uint64_t{3});
}

void DetectionTest::aNewWeakerCandidateDoesNotReplaceTheStrongestPeak()
{
    EventClip saved;
    EventCapture capture(CaptureSettings{},[]{},[&](EventClip clip){saved=std::move(clip);});
    capture.onFrame(std::make_shared<const Frame>(flat(0)));
    Frame first=flat(1); patch(first,5,5,3,20);
    capture.onFrame(std::make_shared<const Frame>(first));
    Frame second=flat(2); patch(second,5,5,3,40); patch(second,40,25,3,10);
    capture.onFrame(std::make_shared<const Frame>(second));
    QCOMPARE(capture.detectionSummary().peakX,40); // The new candidate is displayed.
    capture.finishPartial();
    QCOMPARE(saved.triggers.size(),size_t{2}); QCOMPARE(saved.triggers[1].peakX,40);
    QCOMPARE(saved.peak.frameIndex,uint64_t{2}); QCOMPARE(saved.peak.peakX,5);
    QCOMPARE(saved.peak.activePatches,size_t{2});
    QCOMPARE(saved.peakDifference[5*64+5],40.0f);
}

void DetectionTest::aFullyProtectedFieldDoesNotAdvanceLearning()
{
    FlashDetector detector({6,8,4000,true});
    observe(detector,flat(0));
    QVERIFY(observe(detector,flat(1,120)));
    observe(detector,flat(2,120));
    QCOMPARE(detector.summary().protectedPixels,size_t{64*48});
    QCOMPARE(detector.summary().learningFrames,size_t{1});
    observe(detector,flat(3)); observe(detector,flat(4));
    QCOMPARE(detector.summary().learningFrames,size_t{2});
}

void DetectionTest::oneFrameAndExactThreshold()
{
    for (int addition : {2, 3})
    {
        FlashDetector detector;
        observe(detector,flat(0));
        Frame frame = flat(1);
        patch(frame, 20, 20, 3, addition);
        QCOMPARE(observe(detector,frame), addition == 3); // 3 / 0.5 = exactly 6 sigma.
        QCOMPARE(detector.summary().settings.sigma, 6.0);
        QCOMPARE(detector.summary().peakSigma, addition * 2.0);
        if (addition == 3)
        {
            QCOMPARE(detector.summary().brightPixels, 9);
            QVERIFY(!observe(detector,frame));
            QVERIFY(detector.firing());
            QVERIFY(!observe(detector,flat(2)));
            QVERIFY(observe(detector,frame));
        }
    }
}

void DetectionTest::rejectsHotPixelsLargeAndScatteredChanges()
{
    FlashDetector detector;
    observe(detector,flat(0));
    Frame hot = flat(1); patch(hot, 4, 4, 1, 100);
    QVERIFY(!observe(detector,hot));
    Frame broad = flat(2, 200);
    QVERIFY(!observe(detector,broad));
    detector.reset(); observe(detector,flat(0));
    Frame scattered = flat(1);
    for (int y : {2, 20, 40}) for (int x : {2, 30, 60}) patch(scattered, x, y, 1, 100);
    QVERIFY(!observe(detector,scattered));
    QCOMPARE(detector.summary().brightPixels, 1);
    QVERIFY(!detector.summary().passed);
}

void DetectionTest::twoFrameReferenceIsHeld()
{
    FlashDetector detector({6, 8, 400, false});
    observe(detector,flat(0));
    Frame first = flat(1); patch(first, 20, 20, 3, 20);
    QVERIFY(!observe(detector,first));
    const double peak = detector.summary().peakSigma;
    first.frameIndex = 2;
    QVERIFY(observe(detector,first));
    QCOMPARE(detector.summary().peakSigma, peak);
    Frame shifted = flat(3, 101);
    BudgetVector<float> differences;
    QVERIFY(!detector.analyze(shifted));
    QCOMPARE((detector.difference(shifted, differences), differences.front()), 1.0f);
    detector.updateReference(shifted);
    for (int i = 0; i < 20; ++i) observe(detector,shifted);
    QVERIFY(detector.summary().peakAdu<1); // Background learns while no patch passes.
}

void DetectionTest::anIsolatedPixelCannotVetoACompactFlash()
{
    FlashDetector detector;
    observe(detector,flat(0));
    Frame frame = flat(1);
    patch(frame, 20, 20, 3, 20);
    patch(frame, 1, 1, 1, 50);
    QVERIFY(observe(detector,frame));
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
        observe(detector,frame);
        for (int y = 20; y < 23; ++y) for (int x = 20; x < 23; ++x)
        {
            const size_t at = static_cast<size_t>(y * 64 + x) * 2;
            const int sample = 1003 << shift;
            frame.data[at] = static_cast<uint8_t>(sample & 255);
            frame.data[at + 1] = static_cast<uint8_t>(sample >> 8);
        }
        const Frame before = frame;
        QVERIFY(observe(detector,frame));
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
            for (int i = 0; i < 180; ++i) observe(detector,noisy(static_cast<uint64_t>(i), random));
            Frame probe = flat(180);
            observe(detector,probe, {DetectionOverlay::None, x, 20, false});
            const double localNoise = detector.summary().probeNoise;
            QVERIFY(localNoise > 0);
            Frame flash = flat(181);
            patch(flash, x, 20, 4, inNoiseUnits ? static_cast<int>(std::ceil(8 * localNoise)) : 18);
            QCOMPARE(observe(detector,flash), inNoiseUnits || x < 32);
        }
    }
}

void DetectionTest::settingsAffectTheNextFrame()
{
    FlashDetector detector;
    for (uint64_t i=0;i<32;++i) observe(detector,flat(i));
    Frame flash = flat(1); patch(flash, 20, 20, 3, 3);
    detector.setSettings({8, 8, 400, true});
    QVERIFY(!observe(detector,flash));
    detector.setSettings({5, 8, 400, true});
    patch(flash,20,20,3,1);
    QVERIFY(observe(detector,flash));
    QCOMPARE(detector.summary().settings.sigma, 5.0);
    observe(detector,flat(2));
    detector.setSettings({5, 12, 400, true});
    QVERIFY(!observe(detector,flash));

    // Two-frame confirmation cannot combine frames tested at different K.
    flash=flat(1); patch(flash,20,20,3,3);
    FlashDetector persistent({6, 8, 400, false});
    observe(persistent,flat(0));
    QVERIFY(!observe(persistent,flash));
    persistent.setSettings({5, 8, 400, false});
    flash.frameIndex = 2;
    QVERIFY(!observe(persistent,flash));
    flash.frameIndex = 3;
    QVERIFY(observe(persistent,flash));
    QCOMPARE(persistent.onsetFrameIndex(), uint64_t{2});
    persistent.setSettings({4, 8, 400, true});
    flash.frameIndex = 4;
    QVERIFY(!observe(persistent,flash));
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
        const bool expected = observe(plain,frame);
        const bool actual = observe(decorated,frame,
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
    EventCapture capture(CaptureSettings{5, 0.99, 10}, [] {}, [&](EventClip clip) { clips.push_back(std::move(clip)); });
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
    EventWriter writer(&context, directory.path(), 128 * 1024 * 1024, [&](const auto& result) { results.push_back(result); });
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
