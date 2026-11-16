#include "recording/clip_overview.h"
#include "recording/event_writer.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <cmath>
#include <functional>
#include <random>

namespace {

constexpr int kWidth = 128;
constexpr int kHeight = 96;

std::chrono::system_clock::time_point timeOf(uint64_t index)
{
    // 30 frames a second, from an arbitrary night.
    return std::chrono::system_clock::time_point{} + std::chrono::seconds{1'790'000'000}
        + std::chrono::microseconds{static_cast<int64_t>(index) * 33'333};
}

// A Mono16 frame with 12 significant low bits. value gives each pixel's
// level before noise; noise is Gaussian with the given standard deviation.
FramePtr frame(uint64_t index, const std::function<double(int x, int y)>& value, double noise, std::mt19937& random,
    MemoryBudget budget = {})
{
    auto made = std::make_shared<Frame>(std::move(budget));
    made->width = kWidth; made->height = kHeight;
    made->format = PixelFormat::Mono16; made->significantBits = 12;
    made->frameIndex = index; made->timestamp = timeOf(index);
    made->testSource = true;
    made->data.resize(frameBytes(kWidth, kHeight, PixelFormat::Mono16));
    std::normal_distribution<double> scatter(0, noise);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
        {
            const auto sample = static_cast<uint16_t>(std::clamp(std::lround(value(x, y) + scatter(random)), 0L, 4095L));
            const size_t i = 2 * (static_cast<size_t>(y) * kWidth + x);
            made->data[i] = static_cast<uint8_t>(sample & 255);
            made->data[i + 1] = static_cast<uint8_t>(sample >> 8);
        }
    return made;
}

// A dim, even scene like the earthshine, with a 3 x 3 flash on one frame.
std::vector<FramePtr> flashClip(uint64_t flashFrame, int flashX, int flashY, std::mt19937& random, MemoryBudget budget = {})
{
    std::vector<FramePtr> frames;
    for (uint64_t i = 0; i < 60; ++i)
        frames.push_back(frame(i, [&](int x, int y) {
            const bool flash = i == flashFrame && x >= flashX && x < flashX + 3 && y >= flashY && y < flashY + 3;
            return 200.0 + (flash ? 80 : 0);
        }, 3, random, budget));
    return frames;
}

bool isMarkColour(QRgb pixel)
{
    return qRed(pixel) > 200 && qGreen(pixel) > 160 && qBlue(pixel) < 110;
}

// Whether a pixel of the mark's colour lies within radius of a point.
bool markNear(const QImage& image, QPoint at, int radius)
{
    for (int y = std::max(0, at.y() - radius); y <= std::min(image.height() - 1, at.y() + radius); ++y)
        for (int x = std::max(0, at.x() - radius); x <= std::min(image.width() - 1, at.x() + radius); ++x)
            if (isMarkColour(image.pixel(x, y))) return true;
    return false;
}

QString contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

// A clip as the capture hands it to the writer, around a flash on frame 20.
EventClip writerClip(MemoryBudget budget = {})
{
    std::mt19937 random(5);
    EventClip clip;
    clip.stream.source = "TEST: constructed overview clip";
    for (uint64_t i = 0; i < 34; ++i)
        clip.frames.push_back(frame(i, [i](int x, int y) {
            return 200.0 + (i == 20 && x >= 30 && x < 33 && y >= 20 && y < 23 ? 80 : 0);
        }, 3, random, budget));
    ClipTrigger trigger{20, 20, timeOf(20), 9, false};
    trigger.peakSigma = 26; trigger.peakX = 31; trigger.peakY = 21;
    trigger.boxX = 30; trigger.boxY = 20; trigger.boxWidth = 3; trigger.boxHeight = 3;
    clip.triggers.push_back(trigger);
    clip.framesBefore = 20;
    clip.framesAfter = 13;
    clip.endReason = "post-event interval complete";
    return clip;
}

} // namespace

// Feature-12: the overview picture of a clip.
class OverviewTest : public QObject
{
    Q_OBJECT
private slots:
    void aFlashIsMarkedAndShownInPlace();
    void aMovingPointLeavesItsWholePath();
    void aDarkSilhouetteShowsAsAFall();
    void aSaveWithNothingInItSaysSo();
    void aSlowDriftMarksAlmostNothing();
    void twoTriggersAreBothMarked();
    void theWriterSavesTheOverviewBesideTheEvidence();
    void aRefusedOverviewLeavesACompleteClip();
};

void OverviewTest::aFlashIsMarkedAndShownInPlace()
{
    std::mt19937 random(11);
    const auto frames = flashClip(40, 60, 40, random);
    OverviewRequest request;
    request.onsetFrameIndex = 40;
    request.peakFrameIndex = 40;
    request.referenceTime = timeOf(40);
    request.marks = {QRect(60, 40, 3, 3)};
    request.caption = {"TEST: a constructed flash"};
    const ClipOverview overview = makeClipOverview(frames, request);

    QVERIFY(!overview.image.isNull());
    QCOMPARE(overview.image.width(), 640); // A small frame is enlarged, five times here.
    QCOMPARE(overview.mainFrameIndex, uint64_t{40});
    QCOMPARE(overview.activeFrames, 1);
    QCOMPARE(overview.changedBounds, QRect(60, 40, 3, 3));
    QCOMPARE(overview.stripFrames, (std::vector<uint64_t>{39, 40, 41}));
    for (const QPoint& centre : overview.stripCentres) QCOMPARE(centre, QPoint(61, 41));
    QCOMPARE(overview.speed, 0.0);
    QVERIFY2(overview.activity.contains("1 frame,") && overview.activity.contains("rise") && overview.activity.contains("in place"),
        qPrintable(overview.activity));

    // The mark is found at full size, and in a thumbnail 320 pixels wide.
    const QPoint flash(61 * 5 + 2, 41 * 5 + 2);
    QVERIFY(markNear(overview.image, flash, 30));
    const QImage thumbnail = overview.image.scaledToWidth(320, Qt::SmoothTransformation);
    QVERIFY(markNear(thumbnail, flash / 2, 12));
    // The flash itself is drawn in the rise colour, not covered by the mark.
    const QRgb centre = overview.image.pixel(flash);
    QVERIFY2(qRed(centre) > 200 && qGreen(centre) > 90 && qGreen(centre) < 200 && qBlue(centre) < 90,
        qPrintable(QString::number(centre, 16)));
}

void OverviewTest::aMovingPointLeavesItsWholePath()
{
    std::mt19937 random(12);
    std::vector<FramePtr> frames;
    // A bright 3 x 3 point, the detector's smallest patch, crosses at 3
    // pixels a frame, 90 pixels a second.
    for (uint64_t i = 0; i < 80; ++i)
        frames.push_back(frame(i, [i](int x, int y) {
            const bool inside = i >= 35 && i <= 66 && y >= 50 && y < 53
                && x >= 10 + 3 * static_cast<int>(i - 35) && x < 13 + 3 * static_cast<int>(i - 35);
            return 200.0 + (inside ? 120 : 0);
        }, 3, random));
    OverviewRequest request;
    request.onsetFrameIndex = 36;
    request.referenceTime = timeOf(36);
    const ClipOverview overview = makeClipOverview(frames, request);

    QCOMPARE(overview.changedBounds, QRect(10, 50, 3 * 31 + 3, 3));
    QVERIFY2(std::abs(overview.speed - 90) < 9, qPrintable(QString::number(overview.speed)));
    QVERIFY(overview.movedX > 80);
    QVERIFY(overview.activity.contains("right"));
    QVERIFY(overview.stripFrames.size() >= 3);
    for (size_t c = 1; c < overview.stripCentres.size(); ++c)
        QVERIFY(overview.stripCentres[c].x() >= overview.stripCentres[c - 1].x());
    // The whole path is coloured in the main view.
    for (int x = 12; x < 100; x += 10)
    {
        const QRgb pixel = overview.image.pixel(x * 5 + 2, 51 * 5 + 2);
        QVERIFY2(qRed(pixel) > 180 && qBlue(pixel) < 120, qPrintable(QString("x=%1 %2").arg(x).arg(pixel, 0, 16)));
    }
}

void OverviewTest::aDarkSilhouetteShowsAsAFall()
{
    std::mt19937 random(13);
    std::vector<FramePtr> frames;
    // A dark disk crosses a bright scene at 4 pixels a frame, as a plane
    // crosses the sunlit Moon. It is saved later with Save now, so there is
    // no trigger and no detector peak.
    for (uint64_t i = 0; i < 80; ++i)
        frames.push_back(frame(i, [i](int x, int y) {
            const double centreX = 20 + 4.0 * (static_cast<double>(i) - 40);
            const bool inside = i >= 40 && i < 60 && std::hypot(x - centreX, y - 48.0) <= 6;
            return inside ? 1500.0 : 3000.0;
        }, 20, random));
    OverviewRequest request;
    request.onsetFrameIndex = 75;
    request.referenceTime = timeOf(75);
    const ClipOverview overview = makeClipOverview(frames, request);

    QVERIFY2(overview.activity.contains("fall"), qPrintable(overview.activity));
    QVERIFY(overview.changedPixels > 100);
    QVERIFY2(std::abs(overview.speed - 120) < 12, qPrintable(QString::number(overview.speed)));
    QVERIFY(overview.mainFrameIndex >= 40 && overview.mainFrameIndex < 60);
    const QRgb pixel = overview.image.pixel(48 * 5, 48 * 5);
    QVERIFY2(qBlue(pixel) > qRed(pixel) + 60, qPrintable(QString::number(pixel, 16)));
}

void OverviewTest::aSaveWithNothingInItSaysSo()
{
    std::mt19937 random(14);
    std::vector<FramePtr> frames;
    for (uint64_t i = 0; i < 60; ++i) frames.push_back(frame(i, [](int, int) { return 200.0; }, 3, random));
    OverviewRequest request;
    request.onsetFrameIndex = 59;
    const ClipOverview overview = makeClipOverview(frames, request);
    QVERIFY(!overview.image.isNull());
    QCOMPARE(overview.mainFrameIndex, uint64_t{59});
    QCOMPARE(overview.activeFrames, 0);
    QVERIFY(overview.stripFrames.empty());
    QVERIFY2(overview.activity.startsWith("No activity at K 6"), qPrintable(overview.activity));
}

void OverviewTest::aSlowDriftMarksAlmostNothing()
{
    std::mt19937 random(15);
    std::vector<FramePtr> frames;
    // Unguided drift: a textured scene moves 1 pixel a second.
    constexpr double pi = 3.14159265358979323846;
    for (uint64_t i = 0; i < 150; ++i)
    {
        const double shift = static_cast<double>(i) / 30;
        frames.push_back(frame(i, [shift](int x, int y) {
            return 1000 + 300 * std::sin(2 * pi * (x - shift) / 40) + 300 * std::sin(2 * pi * y / 30);
        }, 5, random));
    }
    OverviewRequest request;
    request.onsetFrameIndex = 149;
    const ClipOverview overview = makeClipOverview(frames, request);
    QVERIFY2(overview.changedPixels < kWidth * kHeight / 1000, qPrintable(QString::number(overview.changedPixels)));
}

void OverviewTest::twoTriggersAreBothMarked()
{
    std::mt19937 random(16);
    std::vector<FramePtr> frames;
    for (uint64_t i = 0; i < 60; ++i)
        frames.push_back(frame(i, [i](int x, int y) {
            const bool first = i == 40 && x >= 20 && x < 23 && y >= 20 && y < 23;
            const bool second = i == 45 && x >= 100 && x < 103 && y >= 70 && y < 73;
            return 200.0 + (first || second ? 80 : 0);
        }, 3, random));
    OverviewRequest request;
    request.onsetFrameIndex = 40;
    request.peakFrameIndex = 40;
    request.marks = {QRect(20, 20, 3, 3), QRect(100, 70, 3, 3)};
    const ClipOverview overview = makeClipOverview(frames, request);
    QCOMPARE(overview.activeFrames, 2);
    QVERIFY(markNear(overview.image, QPoint(21 * 5, 21 * 5), 30));
    QVERIFY(markNear(overview.image, QPoint(101 * 5, 71 * 5), 30));
}

void OverviewTest::theWriterSavesTheOverviewBesideTheEvidence()
{
    QTemporaryDir directory;
    QObject context;
    std::vector<ClipWriteResult> results;
    {
        EventWriter writer(&context, directory.path(), 128 * 1024 * 1024, [&](const auto& result) { results.push_back(result); });
        writer.submit(writerClip());
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), size_t{1}, 10000);
    }
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    const QDir folder(results[0].folder);
    QVERIFY(QFile::exists(folder.filePath("00_overview.png")));
    QVERIFY(QFile::exists(folder.filePath("event.ser")));
    const QString text = contents(folder.filePath("event.txt"));
    QVERIFY2(text.contains("overview: 00_overview.png main_frame_index=20 sigma=6 active_frames=1"), qPrintable(text));
    QVERIFY(text.endsWith("complete: yes\n"));
    const QImage saved(folder.filePath("00_overview.png"));
    QCOMPARE(saved.width(), 640);
    QVERIFY(markNear(saved, QPoint(31 * 5 + 2, 21 * 5 + 2), 30));
}

void OverviewTest::aRefusedOverviewLeavesACompleteClip()
{
    MemoryBudget budget(size_t{1} << 30);
    EventClip clip = writerClip(budget);
    // Room for the other previews, not for the overview's per-pixel buffers.
    QVERIFY(budget.setLimit(budget.usage().used + 30 * 1024));
    QTemporaryDir directory;
    QObject context;
    std::vector<ClipWriteResult> results;
    {
        EventWriter writer(&context, directory.path(), 128 * 1024 * 1024, [&](const auto& result) { results.push_back(result); });
        writer.submit(std::move(clip));
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), size_t{1}, 10000);
    }
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    const QDir folder(results[0].folder);
    QVERIFY(!QFile::exists(folder.filePath("00_overview.png")));
    QVERIFY(QFile::exists(folder.filePath("02_event_peak.png")));
    const QString text = contents(folder.filePath("event.txt"));
    QVERIFY2(text.contains("overview: not written: RAM limit\n"), qPrintable(text));
    QVERIFY(text.endsWith("complete: yes\n"));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    OverviewTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "overview_tests.moc"
