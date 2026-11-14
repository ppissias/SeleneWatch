#include "event_capture.h"
#include "frame_pool.h"
#include "preview_image.h"
#include "recording/ser_writer.h"
#include "session.h"
#include "sources/synthetic_frame_source.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <thread>
#include <atomic>

namespace {

Frame flat(uint64_t index, int fps = 10)
{
    Frame frame;
    frame.width = 64; frame.height = 48;
    frame.data.assign(64 * 48, 100);
    frame.frameIndex = index;
    frame.timestamp = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{static_cast<int64_t>(index) * 1000 / fps};
    frame.testSource = true;
    return frame;
}

void flash(Frame& frame)
{
    for (int y = 20; y < 23; ++y) for (int x = 20; x < 23; ++x) frame.data[static_cast<size_t>(y) * frame.width + x] += 20;
}

QByteArray serBytes(const Frame& frame)
{
    QBuffer output; output.open(QIODevice::ReadWrite);
    SerWriter writer(output);
    QString error;
    if (!writer.start(frame, "display test", error) || !writer.append(frame, error) || !writer.finish(error)) return {};
    return output.data();
}

// Exhausts admission after a real candidate, before acquiring the next
// source frame. The deliberate pressure is released before reporting failure,
// leaving the writer able to preserve the already collected raw frames.
class MemoryTestSource : public FrameSource
{
public:
    explicit MemoryTestSource(bool exhaust = true) : exhaust_(exhaust) {}
    ~MemoryTestSource() override { stop(); }
    bool open(std::string&) override { return true; }
    StreamInfo streamInfo() const override { StreamInfo info; info.source = "TEST: forced RAM exhaustion"; return info; }
    void start(FrameSourceListener& listener) override
    {
        worker_ = std::jthread([this, &listener](std::stop_token stop) {
            try
            {
                BudgetVector<uint8_t> pressure;
                for (uint64_t i = 0; i < (exhaust_ ? 40u : 800u) && !stop.stop_requested(); ++i)
                {
                    auto frame = pool_.acquire(64 * 48);
                    const MemoryBudget budget = frame->data.get_allocator().budget;
                    Frame values = flat(i, exhaust_ ? 10 : 1000);
                    if (exhaust_ ? i == 5 : i >= 10) flash(values);
                    frame->width = values.width; frame->height = values.height;
                    frame->frameIndex = i; frame->timestamp = values.timestamp; frame->testSource = true;
                    std::copy(values.data.begin(), values.data.end(), frame->data.begin());
                    listener.onFrame(std::move(frame));
                    if (exhaust_ && i == 5)
                    {
                        pressure = BudgetVector<uint8_t>(BudgetAllocator<uint8_t>(budget));
                        const auto usage = budget.usage();
                        pressure.resize(usage.limit - usage.used);
                    }
                }
                listener.onEnded();
            }
            catch (const std::exception& ex) { listener.onFailed(ex.what()); }
        });
    }
    void stop() override { if (worker_.joinable()) { worker_.request_stop(); worker_.join(); } }
private:
    bool exhaust_;
    std::jthread worker_;
};

} // namespace

class DisplayCaptureTest : public QObject
{
    Q_OBJECT
private slots:
    void nativeLevelsRevealFaintSamplesWithoutChangingEvidence();
    void allBayerPatternsInterpolateOnlyForDisplay();
    void syntheticNativeLayoutsExposeAFaintTestField();
    void oneBudgetFollowsCopiesMapsAndSharedImages();
    void deniedOverlayLeavesTheDecisionUnchanged();
    void historyUsesSourceSecondsAndClampsByBytes();
    void captureTailHasTheSameDurationAtDifferentRates();
    void longRunsContinueAtTheTimeLimit();
    void memoryFailurePreservesTheOpenClipAndReportsItsEnd();
    void memoryCapSplitsTheRunAndLeavesRoomForItsDifference();
};

void DisplayCaptureTest::nativeLevelsRevealFaintSamplesWithoutChangingEvidence()
{
    for (int bits : {12, 16}) for (auto alignment : {BitAlignment::Low, BitAlignment::High})
    {
        Frame frame;
        frame.width = 64; frame.height = 48; frame.format = PixelFormat::Mono16;
        frame.significantBits = bits; frame.alignment = alignment;
        const int shift = alignment == BitAlignment::High ? 16 - bits : 0;
        for (int i = 0; i < 64 * 48; ++i)
        {
            const int value = (8 + i % 48) << shift;
            frame.data.push_back(static_cast<uint8_t>(value & 255)); frame.data.push_back(static_cast<uint8_t>(value >> 8));
        }
        const auto raw = frame.data;
        const QByteArray evidence = serBytes(frame);
        QVERIFY(!evidence.isEmpty());
        const QImage linear = frameToPreviewImage(frame);
        DisplaySettings window{DisplayMode::Window, 8.0 / ((1 << bits) - 1), 56.0 / ((1 << bits) - 1)};
        const QImage stretched = frameToPreviewImage(frame, window);
        QVERIFY(qGray(stretched.pixel(24, 0)) > qGray(linear.pixel(24, 0)) + 100);
        const QImage automatic = frameToPreviewImage(frame, {DisplayMode::Auto});
        QVERIFY(qGray(automatic.pixel(24, 0)) > qGray(linear.pixel(24, 0)) + 30);
        window.invert = true;
        const QImage inverted = frameToPreviewImage(frame, window);
        QCOMPARE(qGray(inverted.pixel(24, 0)) + qGray(stretched.pixel(24, 0)), 255);
        QCOMPARE(frame.data, raw);
        QCOMPARE(serBytes(frame), evidence);
    }
}

void DisplayCaptureTest::allBayerPatternsInterpolateOnlyForDisplay()
{
    constexpr int cell[][4] = {{0,1,1,2}, {1,0,2,1}, {1,2,0,1}, {2,1,1,0}};
    for (int pattern = 0; pattern < 4; ++pattern) for (auto alignment : {BitAlignment::Low, BitAlignment::High})
    {
        Frame frame; frame.width = frame.height = 5; frame.format = PixelFormat::Mono16;
        frame.significantBits = 12; frame.alignment = alignment; frame.bayer = static_cast<BayerPattern>(pattern + 1);
        for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x)
        {
            const int channel = cell[pattern][(y & 1) * 2 + (x & 1)];
            const int value = (channel == 0 ? 4000 : channel == 1 ? 2000 : 1000) << (alignment == BitAlignment::High ? 4 : 0);
            frame.data.push_back(static_cast<uint8_t>(value & 255)); frame.data.push_back(static_cast<uint8_t>(value >> 8));
        }
        const auto raw = frame.data;
        const QImage image = frameToPreviewImage(frame);
        for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x)
            QCOMPARE(image.pixel(x, y), qRgb(250, 125, 62));
        QCOMPARE(frame.data, raw);
        DisplaySettings mosaic; mosaic.debayer = false;
        QCOMPARE(frameToPreviewImage(frame, mosaic).format(), QImage::Format_Grayscale8);
    }
}

void DisplayCaptureTest::syntheticNativeLayoutsExposeAFaintTestField()
{
    for (int bits : {12, 16}) for (auto alignment : {BitAlignment::Low, BitAlignment::High})
    {
        struct Listener : FrameSourceListener
        {
            FramePtr frame;
            std::atomic<bool> received{false};
            std::string error;
            void onFrame(std::shared_ptr<Frame> value) override { if (!frame) frame = std::move(value); received.store(true); }
            void onDiscontinuity() override {}
            void onEnded() override {}
            void onFailed(const std::string& text) override { error = text; }
        } listener;
        SyntheticFrameSource source(64, 48, 120);
        source.setPixelLayout(PixelFormat::Mono16, bits, alignment);
        std::string reason;
        QVERIFY(source.open(reason));
        source.start(listener);
        QTRY_VERIFY_WITH_TIMEOUT(listener.received.load(), 2000);
        source.stop();
        QVERIFY(listener.error.empty());
        const Frame& frame = *listener.frame;
        QCOMPARE(frame.format, PixelFormat::Mono16);
        QCOMPARE(frame.significantBits, bits);
        QCOMPARE(frame.alignment, alignment);
        QVERIFY(frame.testSource);
        const QImage linear = frameToPreviewImage(frame);
        const QImage automatic = frameToPreviewImage(frame, {DisplayMode::Auto});
        QVERIFY(qGray(automatic.pixel(40, 24)) > qGray(linear.pixel(40, 24)) + 10);
        if (bits == 12 && alignment == BitAlignment::High)
            for (size_t i = 0; i < frame.data.size(); i += 2) QCOMPARE(frame.data[i] & 15, 0);
    }
}

void DisplayCaptureTest::oneBudgetFollowsCopiesMapsAndSharedImages()
{
    MemoryBudget budget(4096);
    {
        FramePool pool; pool.setMemoryBudget(budget);
        auto frame = pool.acquire(1024);
        auto shared = frame;
        QCOMPARE(budget.usage().used, size_t{1024});
        auto copied = std::make_shared<Frame>(*frame);
        QCOMPARE(budget.usage().used, size_t{2048});
        DetectionMap map(budget); map.values.resize(256);
        QImage image = makeBudgetImage(32, 32, QImage::Format_Grayscale8, budget);
        QImage imageCopy = image;
        QCOMPARE(budget.usage().used, size_t{4096});
        QVERIFY_THROWS_EXCEPTION(MemoryLimitError, static_cast<void>(pool.acquire(1)));
        QVERIFY(!budget.setLimit(4095));
        QCOMPARE(budget.usage().peak, size_t{4096});
        pool.setMemoryBudget(budget); // original frame and its copy outlive that pool state.
        frame.reset(); shared.reset();
        QCOMPARE(budget.usage().used, size_t{3072});
        copied.reset(); image = {}; imageCopy = {};
        QCOMPARE(budget.usage().used, size_t{1024});
    }
    QCOMPARE(budget.usage().used, size_t{0});
    QCOMPARE(budget.usage().refused, size_t{1});
}

void DisplayCaptureTest::deniedOverlayLeavesTheDecisionUnchanged()
{
    MemoryBudget budget(45000);
    FlashDetector decorated({}, budget), plain;
    plain.observe(flat(0)); decorated.observe(flat(0));
    plain.observe(flat(1)); decorated.observe(flat(1));
    Frame frame = flat(2); flash(frame);
    QCOMPARE(decorated.observe(frame, false, {DetectionOverlay::Sigma, -1, -1, true}), plain.observe(frame));
    QVERIFY(decorated.summary().triggered);
    QVERIFY(decorated.summary().overlaySkipped);
    QVERIFY(!decorated.view().map);
    QCOMPARE(decorated.summary().peakSigma, plain.summary().peakSigma);
    QVERIFY(budget.usage().peak <= budget.usage().limit);
}

void DisplayCaptureTest::historyUsesSourceSecondsAndClampsByBytes()
{
    for (int fps : {10, 50})
    {
        FrameRingBuffer ring(1024 * 1024, 2);
        for (int i = 0; i <= 4 * fps; ++i) ring.push(std::make_shared<const Frame>(flat(static_cast<uint64_t>(i), fps)));
        QCOMPARE(ring.usage().seconds, 2.0);
        QCOMPARE(ring.size(), static_cast<size_t>(2 * fps + 1));
    }
    FrameRingBuffer small(3 * 64 * 48, 10);
    for (int i = 0; i < 20; ++i) small.push(std::make_shared<const Frame>(flat(static_cast<uint64_t>(i))));
    QCOMPARE(small.size(), size_t{3});
    QVERIFY(small.usage().memoryClamped);
    QCOMPARE(small.usage().bytes, size_t{3 * 64 * 48});
    small.push(std::make_shared<const Frame>(flat(200)));
    QCOMPARE(small.size(), size_t{1}); // A long source-time gap expires the old history.
}

void DisplayCaptureTest::captureTailHasTheSameDurationAtDifferentRates()
{
    for (int fps : {10, 50})
    {
        std::vector<EventClip> clips;
        EventCapture capture(CaptureSettings{0.5, 1, 3}, [] {}, [&](EventClip clip) { clips.push_back(std::move(clip)); });
        for (int i = 0; i <= 2 * fps; ++i)
        {
            Frame frame = flat(static_cast<uint64_t>(i), fps);
            if (i == fps) flash(frame);
            capture.onFrame(std::make_shared<const Frame>(std::move(frame)));
        }
        QCOMPARE(clips.size(), size_t{1});
        QCOMPARE(clips[0].framesBefore, fps / 2);
        QCOMPARE(clips[0].framesAfter, fps);
        QCOMPARE(clips[0].frames.back()->frameIndex, static_cast<uint64_t>(2 * fps));
        QCOMPARE(clips[0].endReason, std::string("post-event interval complete"));
    }
}

void DisplayCaptureTest::longRunsContinueAtTheTimeLimit()
{
    std::vector<EventClip> clips;
    EventCapture capture(CaptureSettings{0.5, 0.25, 1}, [] {}, [&](EventClip clip) { clips.push_back(std::move(clip)); });
    for (int i = 0; i <= 24; ++i)
    {
        Frame frame = flat(static_cast<uint64_t>(i));
        if (i >= 10 && i <= 20) flash(frame);
        capture.onFrame(std::make_shared<const Frame>(std::move(frame)));
    }
    QCOMPARE(clips.size(), size_t{2});
    QCOMPARE(clips[0].endReason, std::string("maximum post-event interval"));
    QVERIFY(clips[1].triggers[0].continuesPreviousClip);
    QCOMPARE(clips[1].triggers[0].triggerFrameIndex, uint64_t{20});
}

void DisplayCaptureTest::memoryFailurePreservesTheOpenClipAndReportsItsEnd()
{
    QTemporaryDir output;
    QObject context;
    QString failure;
    std::vector<ClipWriteResult> results;
    Session::Notifications notices;
    notices.failed = [&](const QString& text) { failure = text; };
    notices.clipWritten = [&](const auto& result) { results.push_back(result); };
    Session session(&context, output.path(), std::move(notices));
    CaptureSettings settings; settings.memoryBytes = 8 * 1024 * 1024;
    session.setCaptureSettings(settings);
    session.setSource(std::make_unique<MemoryTestSource>());
    QString error;
    QVERIFY2(session.start(error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
    QVERIFY(failure.contains("RAM limit"));
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), size_t{1}, 5000);
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    QFile note(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(note.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(note.readAll());
    QVERIFY(text.contains("collection_end: source failure: RAM limit reached"));
    QVERIFY(text.contains("complete: yes"));
    const auto usage = session.memoryUsage();
    QCOMPARE(usage.peak, settings.memoryBytes);
    QVERIFY(usage.refused > 0);
}

void DisplayCaptureTest::memoryCapSplitsTheRunAndLeavesRoomForItsDifference()
{
    QTemporaryDir output;
    QObject context;
    bool ended = false;
    QString failure;
    std::vector<ClipWriteResult> results;
    Session::Notifications notices;
    notices.failed = [&](const QString& text) { failure = text; };
    notices.ended = [&] { ended = true; };
    notices.clipWritten = [&](const auto& result) { results.push_back(result); };
    Session session(&context, output.path(), std::move(notices));
    session.setCaptureSettings({0.005, 0.01, 100, 8 * 1024 * 1024});
    session.setSource(std::make_unique<MemoryTestSource>(false));
    QString error;
    QVERIFY2(session.start(error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(ended || !failure.isEmpty(), 10000);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), size_t{2}, 5000);
    QVERIFY(results[0].memoryLimited);
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    for (const auto& result : results)
    {
        if (!result.notSaved.isEmpty())
        {
            // Fast playback can fill the active+queued allowance before
            // the first clip's previews finish. The continuation must be logged.
            QCOMPARE(result.notSaved, QString("writer busy"));
            QFile log(QDir(output.path()).filePath("candidates.txt"));
            QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
            const auto text = log.readAll();
            QVERIFY(text.contains("continues_previous_clip"));
            QVERIFY(text.contains("not_saved=writer busy"));
            continue;
        }
        QFile note(QDir(result.folder).filePath("event.txt"));
        QVERIFY(note.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(note.readAll());
        QVERIFY(text.contains("complete: yes"));
        QVERIFY(text.contains("difference_memory_limited: no"));
    }
    QCOMPARE(session.memoryUsage().refused, size_t{0});
    QCOMPARE(session.acquiredFrames(), uint64_t{800});
    QVERIFY(session.memoryUsage().peak <= session.memoryUsage().limit);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    DisplayCaptureTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "display_capture_tests.moc"
