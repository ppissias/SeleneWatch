#include "event_capture.h"
#include "event_writer.h"
#include "flash_detector.h"
#include "frame_pool.h"
#include "frame_ring_buffer.h"
#include "latest_frame_slot.h"
#include "synthetic_scene.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtTest/QTest>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// A frame with one pixel, for tests that only care about order and identity.
FramePtr tiny(uint64_t index, uint8_t value)
{
    auto frame = std::make_shared<Frame>();
    frame->frameIndex = index;
    frame->data = {value};
    return frame;
}

FramePtr painted(uint64_t index, bool flash)
{
    auto frame = std::make_shared<Frame>();
    paintSyntheticFrame(*frame, kWidth, kHeight, index, flash);
    return frame;
}

void feed(EventCapture& capture, uint64_t& index, int count, bool flash)
{
    for (int n = 0; n < count; ++n)
    {
        capture.onFrame(painted(index, flash));
        ++index;
    }
}

// Four quiet frames establish a reference. Three flash frames follow: the
// onset, the trigger, and one more frame of the same run. Then `quietAfter`
// quiet frames. A clip ends after kPostEventFrames of those.
void runFlash(EventCapture& capture, uint64_t& index, int quietAfter)
{
    feed(capture, index, 4, false);
    feed(capture, index, 3, true);
    feed(capture, index, quietAfter, false);
}

system_clock::time_point testTime(int millisecond)
{
    const sys_days date = year{2020} / January / day{2};
    return date + hours{3} + minutes{4} + seconds{5} + milliseconds{millisecond};
}

QString utcText(system_clock::time_point time)
{
    const qint64 ms = duration_cast<milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QString utcStamp(system_clock::time_point time)
{
    const qint64 ms = duration_cast<milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString("yyyyMMdd-HHmmss-zzz");
}

FramePtr smallFrame(uint64_t index, system_clock::time_point time, std::vector<uint8_t> pixels,
    int width = 2, int height = 2)
{
    auto frame = std::make_shared<Frame>();
    frame->width = width;
    frame->height = height;
    frame->format = PixelFormat::Mono8;
    frame->frameIndex = index;
    frame->timestamp = time;
    frame->data = std::move(pixels);
    return frame;
}

// Two 2x2 frames around one candidate: onset at frame 4, trigger at frame 5.
EventClip smallClip(system_clock::time_point triggerTime)
{
    EventClip clip;
    clip.triggers.push_back(ClipTrigger{4, 5, triggerTime, 200, false});
    clip.deltaThreshold = 60;
    clip.minBrightPixels = 120;
    clip.framesBefore = 1;
    clip.framesAfter = 0;
    clip.frames.push_back(smallFrame(4, triggerTime - milliseconds{33}, {10, 10, 10, 10}));
    clip.frames.push_back(smallFrame(5, triggerTime, {255, 255, 255, 255}));
    return clip;
}

QString readText(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QStringList candidateLines(const QString& root)
{
    return readText(QDir(root).filePath("candidates.txt")).split('\n', Qt::SkipEmptyParts);
}

QStringList clipFolders(const QString& root)
{
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

} // namespace

class FramePoolTest : public QObject
{
    Q_OBJECT

private slots:
    void acquireGivesRequestedSize();
    void warmRingAllocatesNoMoreBuffers();
    void clipKeepsItsFramesWhileRingWraps();
    void droppedClipFreesAllButAFewSpares();
    void frameMayOutliveThePool();
};

class RingBufferTest : public QObject
{
    Q_OBJECT

private slots:
    void oldestFirstBeforeWrap();
    void oldestFirstAfterWrap();
    void storedSharesTheFrames();
    void clearLetsGoOfStoredFrames();
};

class LatestFrameSlotTest : public QObject
{
    Q_OBJECT

private slots:
    void newerFrameReplacesUnreadOne();
};

class FlashDetectorTest : public QObject
{
    Q_OBJECT

private slots:
    void markerAloneNeverFires();
    void runFiresOnceOnItsSecondFrame();
    void oneFrameSpotDoesNotFire();
};

class EventCaptureTest : public QObject
{
    Q_OBJECT

private slots:
    void oneClipPerFlash();
    void secondFlashJoinsTheOpenClip();
    void flashWhilePreviousClipWaitsStartsNewClip();
    void endlessTriggerSplitsAtTheCap();
    void partialClipOnStop();
    void stopWhileIdleSavesNothing();
};

class EventWriterTest : public QObject
{
    Q_OBJECT

private slots:
    void writesMetadataAndCandidateLog();
    void pngRoundTripIsExact();
    void clipBeyondQueueLimitIsLoggedNotSaved();
    void badClipDoesNotStopTheWriter();
    void sameTriggerTimeGetsTwoFolders();
    void unwritableRootIsARecordFailure();
    void submitAfterShutdownIsRefused();
    void captureToWriterEndToEnd();
};

// ---------------------------------------------------------------- FramePool

void FramePoolTest::acquireGivesRequestedSize()
{
    FramePool pool;
    const std::shared_ptr<Frame> frame = pool.acquire(100);
    QCOMPARE(frame->data.size(), static_cast<size_t>(100));
    QCOMPARE(pool.allocatedFrames(), static_cast<size_t>(1));
    QVERIFY(pool.allocatedBytes() >= 100);
}

void FramePoolTest::warmRingAllocatesNoMoreBuffers()
{
    FramePool pool;
    FrameRingBuffer ring(5);

    auto pushFrames = [&](int count) {
        for (int n = 0; n < count; ++n)
        {
            std::shared_ptr<Frame> frame = pool.acquire(64);
            ring.push(std::move(frame));
        }
    };

    // The new frame is acquired before the ring lets the oldest go, so a
    // warm ring of 5 keeps 6 frames in circulation.
    pushFrames(12);
    const size_t warm = pool.allocatedFrames();
    QVERIFY(warm <= ring.capacity() + 1);

    pushFrames(200);
    QCOMPARE(pool.allocatedFrames(), warm);
}

void FramePoolTest::clipKeepsItsFramesWhileRingWraps()
{
    FramePool pool;
    FrameRingBuffer ring(3);

    auto pushValue = [&](uint8_t value) {
        std::shared_ptr<Frame> frame = pool.acquire(4);
        std::fill(frame->data.begin(), frame->data.end(), value);
        ring.push(std::move(frame));
    };

    pushValue(1);
    pushValue(2);
    pushValue(3);
    std::vector<FramePtr> clip = ring.stored();
    QCOMPARE(clip.size(), static_cast<size_t>(3));

    // The ring wraps twice. The clip's frames are held, so new buffers are made.
    for (uint8_t value = 10; value < 16; ++value)
    {
        pushValue(value);
    }
    QCOMPARE(clip[0]->data, std::vector<uint8_t>(4, 1));
    QCOMPARE(clip[1]->data, std::vector<uint8_t>(4, 2));
    QCOMPARE(clip[2]->data, std::vector<uint8_t>(4, 3));

    const std::vector<FramePtr> inRing = ring.stored();
    for (const FramePtr& held : clip)
    {
        for (const FramePtr& current : inRing)
        {
            QVERIFY(held.get() != current.get());
        }
    }

    // Dropping the clip returns its three buffers. Further frames reuse them.
    const size_t beforeDrop = pool.allocatedFrames();
    clip.clear();
    for (uint8_t value = 20; value < 40; ++value)
    {
        pushValue(value);
    }
    QCOMPARE(pool.allocatedFrames(), beforeDrop);
}

void FramePoolTest::droppedClipFreesAllButAFewSpares()
{
    FramePool pool;

    // A clip holding many frames, as while it waits for the writer.
    std::vector<FramePtr> clip;
    for (int n = 0; n < 100; ++n)
    {
        clip.push_back(pool.acquire(1000));
    }
    QCOMPARE(pool.allocatedFrames(), static_cast<size_t>(100));
    QVERIFY(pool.allocatedBytes() >= 100 * 1000);

    // Writing it lets all of them go at once. The pool keeps a few spares.
    clip.clear();
    QCOMPARE(pool.allocatedFrames(), FramePool::kMaxSpareFrames);
    QVERIFY(pool.allocatedBytes() < 100 * 1000);
}

void FramePoolTest::frameMayOutliveThePool()
{
    std::shared_ptr<Frame> frame;
    {
        FramePool pool;
        frame = pool.acquire(8);
        frame->data[0] = 42;
    }
    QCOMPARE(frame->data[0], static_cast<uint8_t>(42));
    frame.reset();
}

// -------------------------------------------------------------- RingBuffer

void RingBufferTest::oldestFirstBeforeWrap()
{
    FrameRingBuffer buffer(3);
    buffer.push(tiny(1, 1));
    buffer.push(tiny(2, 2));

    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(2));
    QCOMPARE(stored[0]->frameIndex, static_cast<uint64_t>(1));
    QCOMPARE(stored[1]->frameIndex, static_cast<uint64_t>(2));
    QCOMPARE(stored[0]->data, std::vector<uint8_t>({1}));
    QCOMPARE(stored[1]->data, std::vector<uint8_t>({2}));
}

void RingBufferTest::oldestFirstAfterWrap()
{
    FrameRingBuffer buffer(3);
    for (uint64_t index = 1; index <= 4; ++index)
    {
        buffer.push(tiny(index, static_cast<uint8_t>(index)));
    }

    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(3));
    QCOMPARE(stored[0]->frameIndex, static_cast<uint64_t>(2));
    QCOMPARE(stored[1]->frameIndex, static_cast<uint64_t>(3));
    QCOMPARE(stored[2]->frameIndex, static_cast<uint64_t>(4));
}

void RingBufferTest::storedSharesTheFrames()
{
    FrameRingBuffer buffer(3);
    std::vector<FramePtr> pushed;
    for (uint64_t index = 1; index <= 5; ++index)
    {
        pushed.push_back(tiny(index, static_cast<uint8_t>(index)));
        buffer.push(pushed.back());
    }

    // The ring holds the last three. stored() hands out the same objects.
    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(3));
    QVERIFY(stored[0].get() == pushed[2].get());
    QVERIFY(stored[1].get() == pushed[3].get());
    QVERIFY(stored[2].get() == pushed[4].get());
}

void RingBufferTest::clearLetsGoOfStoredFrames()
{
    FrameRingBuffer buffer(3);
    const FramePtr frame = tiny(7, 9);
    buffer.push(frame);
    QCOMPARE(frame.use_count(), 2L);

    buffer.clear();
    QCOMPARE(buffer.size(), static_cast<size_t>(0));
    QVERIFY(buffer.stored().empty());
    QCOMPARE(frame.use_count(), 1L);

    buffer.push(tiny(8, 4));
    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(1));
    QCOMPARE(stored[0]->frameIndex, static_cast<uint64_t>(8));
}

// --------------------------------------------------------- LatestFrameSlot

void LatestFrameSlotTest::newerFrameReplacesUnreadOne()
{
    LatestFrameSlot slot;
    QVERIFY(slot.take() == nullptr);

    // Only the first put into an empty slot asks for a wake-up.
    QVERIFY(slot.put(tiny(1, 1)));
    QVERIFY(!slot.put(tiny(2, 2)));

    const FramePtr taken = slot.take();
    QVERIFY(taken != nullptr);
    QCOMPARE(taken->frameIndex, static_cast<uint64_t>(2));
    QVERIFY(slot.take() == nullptr);

    QVERIFY(slot.put(tiny(3, 3)));
}

// ----------------------------------------------------------- FlashDetector

void FlashDetectorTest::markerAloneNeverFires()
{
    FlashDetector detector;
    int maxRising = 0;
    for (uint64_t index = 0; index < 200; ++index)
    {
        QVERIFY(!detector.observe(*painted(index, false)));
        QVERIFY(!detector.firing());
        maxRising = std::max(maxRising, detector.lastBrightCount());
    }
    QVERIFY(maxRising < FlashDetector::kMinBrightPixels);
}

void FlashDetectorTest::runFiresOnceOnItsSecondFrame()
{
    FlashDetector detector;
    for (uint64_t index = 0; index < 4; ++index)
    {
        QVERIFY(!detector.observe(*painted(index, false)));
    }

    QVERIFY(!detector.observe(*painted(4, true)));
    QVERIFY(!detector.firing());
    QVERIFY(detector.lastBrightCount() >= FlashDetector::kMinBrightPixels);
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(4));

    QVERIFY(detector.observe(*painted(5, true)));
    QVERIFY(detector.firing());
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(4));

    // Later frames of the same run are the same candidate.
    QVERIFY(!detector.observe(*painted(6, true)));
    QVERIFY(detector.firing());
    QVERIFY(!detector.observe(*painted(7, true)));
    QVERIFY(detector.firing());

    // A quiet frame ends the run. The next run is a new candidate.
    QVERIFY(!detector.observe(*painted(8, false)));
    QVERIFY(!detector.firing());
    QVERIFY(!detector.observe(*painted(9, true)));
    QVERIFY(detector.observe(*painted(10, true)));
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(9));
}

void FlashDetectorTest::oneFrameSpotDoesNotFire()
{
    FlashDetector detector;
    for (uint64_t index = 0; index < 2; ++index)
    {
        QVERIFY(!detector.observe(*painted(index, false)));
    }

    QVERIFY(!detector.observe(*painted(2, true)));
    QVERIFY(detector.lastBrightCount() >= FlashDetector::kMinBrightPixels);

    QVERIFY(!detector.observe(*painted(3, false)));
    QVERIFY(!detector.firing());
}

// ------------------------------------------------------------ EventCapture

void EventCaptureTest::oneClipPerFlash()
{
    std::vector<EventClip> clips;
    int candidates = 0;
    EventCapture capture(50, [&candidates] { ++candidates; }, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    runFlash(capture, index, kPostEventFrames - 1);
    QVERIFY(clips.empty());
    feed(capture, index, 1, false);

    QCOMPARE(candidates, 1);
    QCOMPARE(clips.size(), static_cast<size_t>(1));
    const EventClip& clip = clips[0];
    QCOMPARE(clip.triggers.size(), static_cast<size_t>(1));
    QCOMPARE(clip.triggers[0].onsetFrameIndex, static_cast<uint64_t>(4));
    QCOMPARE(clip.triggers[0].triggerFrameIndex, static_cast<uint64_t>(5));
    QVERIFY(!clip.triggers[0].continuesPreviousClip);
    QVERIFY(clip.triggers[0].brightPixelCount >= FlashDetector::kMinBrightPixels);
    QCOMPARE(clip.deltaThreshold, FlashDetector::kDeltaThreshold);
    QCOMPARE(clip.minBrightPixels, FlashDetector::kMinBrightPixels);

    // Frames 0-4 come before the trigger frame. After it: the third flash
    // frame, then the quiet tail.
    QCOMPARE(clip.framesBefore, 5);
    QCOMPARE(clip.framesAfter, 1 + kPostEventFrames);
    QCOMPARE(clip.frames.size(), static_cast<size_t>(5 + 1 + 1 + kPostEventFrames));
    for (size_t i = 0; i < clip.frames.size(); ++i)
    {
        QCOMPARE(clip.frames[i]->frameIndex, static_cast<uint64_t>(i));
    }
}

void EventCaptureTest::secondFlashJoinsTheOpenClip()
{
    std::vector<EventClip> clips;
    int candidates = 0;
    EventCapture capture(100, [&candidates] { ++candidates; }, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    // First flash: onset 4, trigger 5, last flash frame 6.
    // Second flash ten frames later: onset 14, trigger 15, last flash frame 16.
    uint64_t index = 0;
    feed(capture, index, 4, false);
    feed(capture, index, 3, true);
    feed(capture, index, 7, false);
    feed(capture, index, 3, true);

    // The tail is counted from the end of the second flash.
    feed(capture, index, kPostEventFrames - 1, false);
    QVERIFY(clips.empty());
    feed(capture, index, 1, false);

    QCOMPARE(candidates, 2);
    QCOMPARE(clips.size(), static_cast<size_t>(1));
    const EventClip& clip = clips[0];
    QCOMPARE(clip.triggers.size(), static_cast<size_t>(2));
    QCOMPARE(clip.triggers[0].onsetFrameIndex, static_cast<uint64_t>(4));
    QCOMPARE(clip.triggers[0].triggerFrameIndex, static_cast<uint64_t>(5));
    QCOMPARE(clip.triggers[1].onsetFrameIndex, static_cast<uint64_t>(14));
    QCOMPARE(clip.triggers[1].triggerFrameIndex, static_cast<uint64_t>(15));

    const uint64_t lastIndex = 16 + kPostEventFrames;
    QCOMPARE(clip.frames.back()->frameIndex, lastIndex);
    QCOMPARE(clip.framesAfter, static_cast<int>(lastIndex - 5));
    QCOMPARE(clip.frames.size(), static_cast<size_t>(lastIndex + 1));
}

void EventCaptureTest::flashWhilePreviousClipWaitsStartsNewClip()
{
    // The clips collected here stand for clips waiting in the writer's queue.
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    runFlash(capture, index, kPostEventFrames);
    QCOMPARE(clips.size(), static_cast<size_t>(1));

    // Nothing has been "written". A second flash is still detected and kept.
    runFlash(capture, index, kPostEventFrames);
    QCOMPARE(clips.size(), static_cast<size_t>(2));
    QVERIFY(clips[1].triggers[0].triggerFrameIndex > clips[0].triggers[0].triggerFrameIndex);

    // The second clip's history overlaps the first clip's tail. The frames
    // they have in common are the same objects, not copies.
    int shared = 0;
    for (const FramePtr& first : clips[0].frames)
    {
        for (const FramePtr& second : clips[1].frames)
        {
            if (first->frameIndex == second->frameIndex)
            {
                QVERIFY(first.get() == second.get());
                ++shared;
            }
        }
    }
    QVERIFY(shared > 0);
}

void EventCaptureTest::endlessTriggerSplitsAtTheCap()
{
    std::vector<EventClip> clips;
    int candidates = 0;
    EventCapture capture(50, [&candidates] { ++candidates; }, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    // The flash never ends: onset 4, trigger 5, and every later frame fires.
    uint64_t index = 0;
    feed(capture, index, 4, false);
    feed(capture, index, 2, true);
    feed(capture, index, kMaxPostEventFrames - 1, true);
    QVERIFY(clips.empty());

    feed(capture, index, 1, true);
    QCOMPARE(clips.size(), static_cast<size_t>(1));
    QCOMPARE(clips[0].framesAfter, kMaxPostEventFrames);
    QCOMPARE(clips[0].frames.back()->frameIndex, static_cast<uint64_t>(5 + kMaxPostEventFrames));

    // A new clip started at once and carries on. It is not a new candidate.
    feed(capture, index, 10, true);
    capture.finishPartial();
    QCOMPARE(clips.size(), static_cast<size_t>(2));
    QCOMPARE(candidates, 1);

    const EventClip& next = clips[1];
    QCOMPARE(next.triggers.size(), static_cast<size_t>(1));
    QVERIFY(next.triggers[0].continuesPreviousClip);
    QCOMPARE(next.triggers[0].onsetFrameIndex, static_cast<uint64_t>(4));
    QCOMPARE(next.triggers[0].triggerFrameIndex, static_cast<uint64_t>(5 + kMaxPostEventFrames));
    QCOMPARE(next.framesAfter, 10);

    // Its history is the ring at that moment, so it overlaps the first clip.
    QVERIFY(next.frames.front()->frameIndex < clips[0].frames.back()->frameIndex);
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
    QVERIFY(clips.empty());
    capture.finishPartial();

    QCOMPARE(clips.size(), static_cast<size_t>(1));
    QCOMPARE(clips[0].framesAfter, 10);
    QCOMPARE(clips[0].triggers[0].triggerFrameIndex, static_cast<uint64_t>(5));
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
}

// ------------------------------------------------------------- EventWriter

void EventWriterTest::writesMetadataAndCandidateLog()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const system_clock::time_point triggerTime = testTime(6);
    QVERIFY(utcText(triggerTime).endsWith(QLatin1Char('Z')));

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(smallClip(triggerTime));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);

    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    QVERIFY(!results[0].recordFailed);
    QCOMPARE(QDir(results[0].folder).dirName(), utcStamp(triggerTime));

    const QString text = readText(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.contains("triggers: 1\n"));
    QVERIFY(text.contains("trigger: onset_frame_index=4 trigger_frame_index=5 bright_pixel_count=200 trigger_time="
        + utcText(triggerTime) + "\n"));
    QVERIFY(text.contains("delta_threshold: 60\n"));
    QVERIFY(text.contains("min_bright_pixels: 120\n"));
    QVERIFY(text.contains("frames_before: 1\n"));
    QVERIFY(text.contains("0000.png 4 " + utcText(triggerTime - milliseconds{33}) + "\n"));
    QVERIFY(text.contains("0001.png 5 " + utcText(triggerTime) + "\n"));
    QVERIFY(text.trimmed().endsWith("complete: yes"));
    QCOMPARE(QDir(results[0].folder).entryList(QStringList() << "*.png").size(), static_cast<qsizetype>(2));

    const QStringList lines = candidateLines(root.path());
    QCOMPARE(lines.size(), static_cast<qsizetype>(1));
    QVERIFY(lines[0].startsWith(utcText(triggerTime) + " onset_frame_index=4 trigger_frame_index=5 "));
    QVERIFY(lines[0].contains("delta_threshold=60 min_bright_pixels=120"));
    QVERIFY(lines[0].endsWith("clip=" + utcStamp(triggerTime)));
}

void EventWriterTest::pngRoundTripIsExact()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    // Every 8-bit value, in an image whose width is not a multiple of four.
    const int width = 17;
    const int height = 16;
    std::vector<uint8_t> pixels(static_cast<size_t>(width * height));
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        pixels[i] = static_cast<uint8_t>((i * 7) % 256);
    }

    EventClip clip;
    clip.triggers.push_back(ClipTrigger{0, 0, testTime(100), 0, false});
    clip.frames.push_back(smallFrame(0, testTime(100), pixels, width, height));

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(std::move(clip));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));

    QImage image(QDir(results[0].folder).filePath("0000.png"));
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), width);
    QCOMPARE(image.height(), height);
    QCOMPARE(image.format(), QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y)
    {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < width; ++x)
        {
            QCOMPARE(row[x], pixels[static_cast<size_t>(y * width + x)]);
        }
    }
}

void EventWriterTest::clipBeyondQueueLimitIsLoggedNotSaved()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    // Room for one waiting frame. The clip has two.
    EventWriter writer(&context, root.path(), 1, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });

    EventClip clip = smallClip(testTime(200));
    const FramePtr watched = clip.frames[0];
    writer.submit(std::move(clip));

    // The frames were dropped at once, before the writer thread did anything.
    QCOMPARE(watched.use_count(), 1L);

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    QVERIFY(results[0].folder.isEmpty());
    QCOMPARE(results[0].notSaved, QString("writer busy"));
    QVERIFY(!results[0].recordFailed);

    QVERIFY(clipFolders(root.path()).isEmpty());
    const QStringList lines = candidateLines(root.path());
    QCOMPARE(lines.size(), static_cast<qsizetype>(1));
    QVERIFY(lines[0].startsWith(utcText(testTime(200))));
    QVERIFY(lines[0].endsWith("not_saved=writer busy"));
}

void EventWriterTest::badClipDoesNotStopTheWriter()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });

    // A 2x2 frame that holds three bytes cannot be written.
    EventClip bad = smallClip(testTime(300));
    bad.frames[1] = smallFrame(5, testTime(300), {1, 2, 3});
    writer.submit(std::move(bad));
    writer.submit(smallClip(testTime(400)));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(2), 10000);

    QVERIFY(results[0].folder.isEmpty());
    QVERIFY(results[0].notSaved.contains("holds 3 bytes"));
    QVERIFY(!results[0].recordFailed);

    QVERIFY2(results[1].notSaved.isEmpty(), qPrintable(results[1].notSaved));
    QCOMPARE(QDir(results[1].folder).dirName(), utcStamp(testTime(400)));
    QVERIFY(readText(QDir(results[1].folder).filePath("event.txt")).trimmed().endsWith("complete: yes"));

    // The failed clip left a folder without the completion marker.
    const QString partial = readText(QDir(root.path()).filePath(utcStamp(testTime(300)) + "/event.txt"));
    QVERIFY(!partial.isEmpty());
    QVERIFY(!partial.contains("complete: yes"));

    const QStringList lines = candidateLines(root.path());
    QCOMPARE(lines.size(), static_cast<qsizetype>(2));
    QVERIFY(lines[0].contains("not_saved="));
    QVERIFY(lines[0].contains("holds 3 bytes"));
    QVERIFY(lines[1].endsWith("clip=" + utcStamp(testTime(400))));
}

void EventWriterTest::sameTriggerTimeGetsTwoFolders()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(smallClip(testTime(500)));
    writer.submit(smallClip(testTime(500)));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(2), 10000);

    QCOMPARE(QDir(results[0].folder).dirName(), utcStamp(testTime(500)));
    QCOMPARE(QDir(results[1].folder).dirName(), utcStamp(testTime(500)) + "-2");
    QCOMPARE(clipFolders(root.path()).size(), static_cast<qsizetype>(2));
    for (const ClipWriteResult& result : results)
    {
        QVERIFY(readText(QDir(result.folder).filePath("event.txt")).trimmed().endsWith("complete: yes"));
    }
}

void EventWriterTest::unwritableRootIsARecordFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // The output root lies under a file, so nothing can be created there.
    const QString blocker = QDir(dir.path()).filePath("blocker");
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, blocker + "/events", 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(smallClip(testTime(600)));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);

    QVERIFY(results[0].folder.isEmpty());
    QVERIFY(!results[0].notSaved.isEmpty());
    QVERIFY(results[0].recordFailed);
}

void EventWriterTest::submitAfterShutdownIsRefused()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.shutdown();

    EventClip clip = smallClip(testTime(700));
    const FramePtr watched = clip.frames[0];
    writer.submit(std::move(clip));
    QCOMPARE(watched.use_count(), 1L);

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    QVERIFY(results[0].folder.isEmpty());
    QVERIFY(results[0].recordFailed);
    QVERIFY(clipFolders(root.path()).isEmpty());
}

void EventWriterTest::captureToWriterEndToEnd()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 1000, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    EventCapture capture(50, [] {}, [&writer](EventClip clip) {
        writer.submit(std::move(clip));
    });

    // Two flashes far enough apart to make two clips, the second while the
    // first may still be with the writer.
    uint64_t index = 0;
    runFlash(capture, index, kPostEventFrames);
    runFlash(capture, index, kPostEventFrames);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(2), 30000);

    for (const ClipWriteResult& result : results)
    {
        QVERIFY2(result.notSaved.isEmpty(), qPrintable(result.notSaved));
        QVERIFY(readText(QDir(result.folder).filePath("event.txt")).trimmed().endsWith("complete: yes"));
    }
    QCOMPARE(QDir(results[0].folder).entryList(QStringList() << "*.png").size(),
        static_cast<qsizetype>(5 + 1 + 1 + kPostEventFrames));
    QCOMPARE(candidateLines(root.path()).size(), static_cast<qsizetype>(2));
}

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    int status = 0;
    {
        FramePoolTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        RingBufferTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        LatestFrameSlotTest tests;
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

#include "core_tests.moc"
