#include "event_capture.h"
#include "event_writer.h"
#include "flash_detector.h"
#include "frame_ring_buffer.h"
#include "synthetic_scene.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

Frame painted(uint64_t index, bool flash)
{
    Frame frame;
    paintSyntheticFrame(frame, kWidth, kHeight, index, flash);
    return frame;
}

void feed(EventCapture& capture, uint64_t& index, int count, bool flash)
{
    for (int n = 0; n < count; ++n)
    {
        Frame frame = painted(index, flash);
        capture.onFrame(frame);
        ++index;
    }
}

// Four quiet frames establish a reference. Three flash frames follow.
// The detector fires on the second flash frame, and the third is the first
// post-event frame, so `quietAfter` further quiet frames are post-event
// frames together with that third flash frame.
void runFlash(EventCapture& capture, uint64_t& index, int quietAfter)
{
    feed(capture, index, 4, false);
    feed(capture, index, 3, true);
    feed(capture, index, quietAfter, false);
}

} // namespace

class RingBufferTest : public QObject
{
    Q_OBJECT

private slots:
    void oldestFirstBeforeWrap();
    void oldestFirstAfterWrap();
    void clearDropsStoredFrames();
    void pushSwapsPixelBuffer();
};

class FlashDetectorTest : public QObject
{
    Q_OBJECT

private slots:
    void markerAloneNeverFires();
    void threeFrameSpotFiresOnSecondFrame();
    void oneFrameSpotDoesNotFire();
};

class EventCaptureTest : public QObject
{
    Q_OBJECT

private slots:
    void oneClipPerFlash();
    void lockoutUntilWriteFinishes();
    void partialClipOnStop();
    void stopWhileIdleSavesNothing();
};

class EventWriterTest : public QObject
{
    Q_OBJECT

private slots:
    void writesUtcMetadata();
};

void RingBufferTest::oldestFirstBeforeWrap()
{
    FrameRingBuffer buffer(3);
    Frame first;
    first.frameIndex = 1;
    first.data = {1};
    buffer.push(first);

    Frame second;
    second.frameIndex = 2;
    second.data = {2};
    buffer.push(second);

    const std::vector<Frame> stored = buffer.copyStored();
    QCOMPARE(stored.size(), static_cast<size_t>(2));
    QCOMPARE(stored[0].frameIndex, static_cast<uint64_t>(1));
    QCOMPARE(stored[1].frameIndex, static_cast<uint64_t>(2));
    QCOMPARE(stored[0].data, std::vector<uint8_t>({1}));
    QCOMPARE(stored[1].data, std::vector<uint8_t>({2}));
}

void RingBufferTest::oldestFirstAfterWrap()
{
    FrameRingBuffer buffer(3);
    for (uint64_t index = 1; index <= 4; ++index)
    {
        Frame frame;
        frame.frameIndex = index;
        frame.data = {static_cast<uint8_t>(index)};
        buffer.push(frame);
    }

    const std::vector<Frame> stored = buffer.copyStored();
    QCOMPARE(stored.size(), static_cast<size_t>(3));
    QCOMPARE(stored[0].frameIndex, static_cast<uint64_t>(2));
    QCOMPARE(stored[1].frameIndex, static_cast<uint64_t>(3));
    QCOMPARE(stored[2].frameIndex, static_cast<uint64_t>(4));
}

void RingBufferTest::clearDropsStoredFrames()
{
    FrameRingBuffer buffer(3);
    Frame frame;
    frame.frameIndex = 7;
    frame.data = {9};
    buffer.push(frame);
    buffer.clear();

    QCOMPARE(buffer.size(), static_cast<size_t>(0));
    QVERIFY(buffer.copyStored().empty());

    Frame again;
    again.frameIndex = 8;
    again.data = {4};
    buffer.push(again);
    const std::vector<Frame> stored = buffer.copyStored();
    QCOMPARE(stored.size(), static_cast<size_t>(1));
    QCOMPARE(stored[0].frameIndex, static_cast<uint64_t>(8));
}

void RingBufferTest::pushSwapsPixelBuffer()
{
    FrameRingBuffer buffer(2);

    // Each slot hands its previous buffer back. The first lap returns empty slots.
    Frame first;
    first.data = {1, 2, 3, 4};
    buffer.push(first);
    QVERIFY(first.data.empty());

    Frame second;
    second.data = {5, 6, 7, 8};
    buffer.push(second);
    QVERIFY(second.data.empty());

    Frame third;
    third.data = {9, 9, 9, 9};
    buffer.push(third);
    QCOMPARE(third.data, (std::vector<uint8_t>{1, 2, 3, 4}));

    const std::vector<Frame> stored = buffer.copyStored();
    QCOMPARE(stored.back().data, (std::vector<uint8_t>{9, 9, 9, 9}));
}

void FlashDetectorTest::markerAloneNeverFires()
{
    FlashDetector detector;
    int maxRising = 0;
    for (uint64_t index = 0; index < 200; ++index)
    {
        Frame frame = painted(index, false);
        QVERIFY(!detector.observe(frame));
        maxRising = std::max(maxRising, detector.lastBrightCount());
    }
    QVERIFY(maxRising < FlashDetector::kMinBrightPixels);
}

void FlashDetectorTest::threeFrameSpotFiresOnSecondFrame()
{
    FlashDetector detector;
    for (uint64_t index = 0; index < 4; ++index)
    {
        Frame frame = painted(index, false);
        QVERIFY(!detector.observe(frame));
    }

    Frame onset = painted(4, true);
    QVERIFY(!detector.observe(onset));
    QVERIFY(detector.lastBrightCount() >= FlashDetector::kMinBrightPixels);
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(4));

    Frame trigger = painted(5, true);
    QVERIFY(detector.observe(trigger));
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(4));
    QVERIFY(detector.lastBrightCount() >= FlashDetector::kMinBrightPixels);
}

void FlashDetectorTest::oneFrameSpotDoesNotFire()
{
    FlashDetector detector;
    for (uint64_t index = 0; index < 2; ++index)
    {
        Frame frame = painted(index, false);
        QVERIFY(!detector.observe(frame));
    }

    Frame one = painted(2, true);
    QVERIFY(!detector.observe(one));
    QVERIFY(detector.lastBrightCount() >= FlashDetector::kMinBrightPixels);

    Frame quiet = painted(3, false);
    QVERIFY(!detector.observe(quiet));

    Frame again = painted(4, true);
    QVERIFY(!detector.observe(again));
    Frame second = painted(5, true);
    QVERIFY(detector.observe(second));
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(4));
}

void EventCaptureTest::oneClipPerFlash()
{
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    // 29 quiet frames after the three-frame flash make 30 post-event frames.
    runFlash(capture, index, 29);

    QCOMPARE(clips.size(), static_cast<size_t>(1));
    QCOMPARE(clips[0].onsetFrameIndex, static_cast<uint64_t>(4));
    QCOMPARE(clips[0].triggerFrameIndex, static_cast<uint64_t>(5));
    QCOMPARE(clips[0].framesBefore, 5);
    QCOMPARE(clips[0].framesAfter, 30);
    QCOMPARE(clips[0].frames.size(), static_cast<size_t>(36));
    QCOMPARE(clips[0].deltaThreshold, FlashDetector::kDeltaThreshold);
    QCOMPARE(clips[0].minBrightPixels, FlashDetector::kMinBrightPixels);
    QVERIFY(clips[0].brightPixelCount >= FlashDetector::kMinBrightPixels);
    QCOMPARE(capture.writesInFlight(), 1);

    bool foundOnset = false;
    bool foundTrigger = false;
    for (const Frame& frame : clips[0].frames)
    {
        foundOnset = foundOnset || frame.frameIndex == clips[0].onsetFrameIndex;
        foundTrigger = foundTrigger || frame.frameIndex == clips[0].triggerFrameIndex;
    }
    QVERIFY(foundOnset);
    QVERIFY(foundTrigger);
}

void EventCaptureTest::lockoutUntilWriteFinishes()
{
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    runFlash(capture, index, 29);
    QCOMPARE(clips.size(), static_cast<size_t>(1));

    feed(capture, index, 4, false);
    feed(capture, index, 3, true);
    QCOMPARE(clips.size(), static_cast<size_t>(1));

    QCOMPARE(capture.onWriteFinished(), 0);
    runFlash(capture, index, 29);
    QCOMPARE(clips.size(), static_cast<size_t>(2));
    QCOMPARE(clips[1].framesAfter, 30);
    QVERIFY(clips[1].triggerFrameIndex > clips[0].triggerFrameIndex);
}

void EventCaptureTest::partialClipOnStop()
{
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    // The third flash frame plus 9 quiet frames is a 10-frame tail.
    runFlash(capture, index, 9);
    capture.finishPartial();

    QCOMPARE(clips.size(), static_cast<size_t>(1));
    QCOMPARE(clips[0].framesAfter, 10);
    QCOMPARE(clips[0].triggerFrameIndex, static_cast<uint64_t>(5));
}

void EventCaptureTest::stopWhileIdleSavesNothing()
{
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    feed(capture, index, 3, false);
    capture.finishPartial();
    QVERIFY(clips.empty());
    QCOMPARE(capture.writesInFlight(), 0);
}

void EventWriterTest::writesUtcMetadata()
{
    using namespace std::chrono;

    const sys_days date = year{2020} / January / std::chrono::day{2};
    const system_clock::time_point triggerTime = date + hours{3} + minutes{4} + seconds{5} + milliseconds{6};
    const system_clock::time_point onsetTime = triggerTime - milliseconds{33};

    EventClip clip;
    clip.onsetFrameIndex = 4;
    clip.triggerFrameIndex = 5;
    clip.triggerTime = triggerTime;
    clip.deltaThreshold = 60;
    clip.minBrightPixels = 120;
    clip.brightPixelCount = 200;
    clip.framesBefore = 1;
    clip.framesAfter = 1;

    Frame onset;
    onset.width = 2;
    onset.height = 2;
    onset.format = PixelFormat::Mono8;
    onset.frameIndex = 4;
    onset.timestamp = onsetTime;
    onset.data = {10, 10, 10, 10};

    Frame trigger;
    trigger.width = 2;
    trigger.height = 2;
    trigger.format = PixelFormat::Mono8;
    trigger.frameIndex = 5;
    trigger.timestamp = triggerTime;
    trigger.data = {255, 255, 255, 255};

    clip.frames.push_back(std::move(onset));
    clip.frames.push_back(std::move(trigger));

    const qint64 triggerMs = duration_cast<milliseconds>(triggerTime.time_since_epoch()).count();
    const QString expectedTime = QDateTime::fromMSecsSinceEpoch(triggerMs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
    const QString expectedFolder = QDateTime::fromMSecsSinceEpoch(triggerMs, QTimeZone::UTC).toString("yyyyMMdd-HHmmss-zzz");
    QVERIFY(expectedTime.endsWith(QLatin1Char('Z')));

    QString writtenPath;
    QString writtenError;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);

    QObject context;
    EventWriter writer(&context, [&](const QString& path, const QString& error) {
        writtenPath = path;
        writtenError = error;
        loop.quit();
    });
    writer.submit(std::move(clip));
    loop.exec();

    QVERIFY2(!writtenPath.isEmpty(), "writer did not finish");
    QVERIFY(writtenError.isEmpty());
    QCOMPARE(QDir(writtenPath).dirName(), expectedFolder);

    QFile note(QDir(writtenPath).filePath("event.txt"));
    QVERIFY(note.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(note.readAll());
    note.close();

    QVERIFY(text.contains("onset_frame_index: 4"));
    QVERIFY(text.contains("trigger_frame_index: 5"));
    QVERIFY(text.contains("trigger_time: " + expectedTime));
    QVERIFY(text.contains("delta_threshold: 60"));
    QVERIFY(text.contains("min_bright_pixels: 120"));
    QVERIFY(text.contains("bright_pixel_count: 200"));
    QVERIFY(text.contains("0000.png 4 "));
    QVERIFY(text.contains("0001.png 5 " + expectedTime));
    QVERIFY(text.trimmed().endsWith("complete: yes"));
    QCOMPARE(QDir(writtenPath).entryList(QStringList() << "*.png").size(), static_cast<qsizetype>(2));

    QDir(writtenPath).removeRecursively();
}

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    int status = 0;
    {
        RingBufferTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        FlashDetectorTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        EventCaptureTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        EventWriterTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    return status;
}

#include "stage4_tests.moc"
