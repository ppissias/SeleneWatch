#include "event_capture.h"
#include "recording/clip_overview.h"
#include "recording/event_writer.h"
#include "recording/ser_reader.h"
#include "target_defaults.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <cmath>
#include <filesystem>
#include <functional>
#include <random>

namespace {

constexpr uint64_t kGiB = uint64_t{1024} * 1024 * 1024;

std::chrono::system_clock::time_point timeOf(uint64_t index, double fps)
{
    return std::chrono::system_clock::time_point{} + std::chrono::seconds{1'790'000'000}
        + std::chrono::microseconds{std::llround(static_cast<double>(index) * 1e6 / fps)};
}

// A Mono8 frame of a dark sky with Gaussian noise. add gives each pixel's
// signal above the sky level.
FramePtr skyFrame(int width, int height, uint64_t index, double fps, std::mt19937& random,
    const std::function<double(int x, int y)>& add = {}, double level = 20, double noise = 2)
{
    auto frame = std::make_shared<Frame>();
    frame->width = width; frame->height = height; frame->frameIndex = index;
    frame->timestamp = timeOf(index, fps);
    frame->testSource = true;
    frame->data.resize(static_cast<size_t>(width) * height);
    std::normal_distribution<double> scatter(0, noise);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            frame->data[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(
                std::clamp(std::lround(level + (add ? add(x, y) : 0.0) + scatter(random)), 0L, 255L));
    return frame;
}

// A capture with the clips and candidates it produces.
struct Watch
{
    Watch(CaptureSettings capture, DetectionSettings detection)
        : capture(capture, [this] { ++candidates; }, [this](EventClip clip) { clips.push_back(std::move(clip)); }, detection) {}
    int candidates{0};
    std::vector<EventClip> clips;
    EventCapture capture;
};

QString contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

// Writes clips and waits for their results.
std::vector<ClipWriteResult> write(const QString& folder, std::vector<EventClip> clips,
    EventWriter::FreeSpace freeSpace = {})
{
    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, folder, 512 * 1024 * 1024, [&](const auto& result) { results.push_back(result); });
    if (freeSpace) writer.setFreeSpace(std::move(freeSpace));
    const size_t count = clips.size();
    for (auto& clip : clips) writer.submit(std::move(clip));
    for (int wait = 0; wait < 1000 && results.size() < count; ++wait) QTest::qWait(10);
    return results;
}

} // namespace

// Feature-11: watching the sky.
class SkyTest : public QObject
{
    Q_OBJECT
private slots:
    void aDiagonalStreakFiresOnlyInSkyMode();
    void lightningIsOneClipOfLargeBrighteningsOnlyInSkyMode();
    void aSlowRampFiresNothing();
    void aDriftingStarIsReleasedAndItsClipEnds();
    void aMovingPointKeepsFiringPastTheReleaseLimit();
    void aMaskedFlashDoesNotFireAndItsPixelsAreSaved();
    void aClipBelowTheDiskReserveIsLoggedButNotWritten();
    void linkedSkyClipsDoNotRepeatTheirHistory();
};

// Done-when 2 and 8: a 3-pixel-wide streak at 45 degrees, 20 pixels a frame.
void SkyTest::aDiagonalStreakFiresOnlyInSkyMode()
{
    for (const Target target : {Target::Moon, Target::Sky})
    {
        std::mt19937 random(21);
        Watch watch(defaultCaptureSettings(target), defaultDetectionSettings(target));
        for (uint64_t i = 0; i < 260; ++i)
        {
            const int start = i >= 30 && i < 39 ? 10 + 20 * static_cast<int>(i - 30) : -1000;
            watch.capture.onFrame(skyFrame(200, 200, i, 30, random, [start](int x, int y) {
                return x >= start && x < start + 20 && std::abs(x - y) <= 1 ? 120.0 : 0.0;
            }));
        }
        watch.capture.finishPartial();
        if (target == Target::Moon)
        {
            // Each segment is about seven times as spread out as its pixels.
            QCOMPARE(watch.candidates, 0);
            QVERIFY(watch.clips.empty());
            continue;
        }
        QCOMPARE(watch.candidates, 1);
        QCOMPARE(watch.clips.size(), size_t{1});
        const EventClip& clip = watch.clips[0];
        QCOMPARE(clip.endReason, std::string("post-event interval complete"));
        QVERIFY(clip.frames.front()->frameIndex < 30 && clip.frames.back()->frameIndex > 38);

        OverviewRequest request;
        request.onsetFrameIndex = clip.triggers.front().onsetFrameIndex;
        const ClipOverview overview = makeClipOverview(clip.frames, request);
        QVERIFY2(overview.changedBounds.left() <= 11 && overview.changedBounds.right() >= 188,
            qPrintable(QString("%1 to %2").arg(overview.changedBounds.left()).arg(overview.changedBounds.right())));
        QVERIFY(overview.activity.contains("moved"));
    }
}

// Done-when 3: three strokes over 40% of the frame, 0.3 s apart.
void SkyTest::lightningIsOneClipOfLargeBrighteningsOnlyInSkyMode()
{
    // The mean noise the detector holds for the struck area.
    const auto areaNoise = [](const EventCapture& capture) {
        const auto map = capture.detectionView().map;
        double sum = 0;
        int count = 0;
        for (int y = 0; y < map->height; ++y)
            for (int x = 0; x < 40; ++x) { sum += map->values[static_cast<size_t>(y) * map->width + x]; ++count; }
        return sum / count;
    };
    for (const Target target : {Target::Moon, Target::Sky})
    {
        std::mt19937 random(22);
        Watch watch(defaultCaptureSettings(target), defaultDetectionSettings(target));
        double before = 0, after = 0;
        int afterFirstStroke = 0;
        for (uint64_t i = 0; i < 320; ++i)
        {
            const bool stroke = i == 100 || i == 109 || i == 118;
            const bool measure = i == 99 || i == 125;
            watch.capture.onFrame(skyFrame(100, 100, i, 30, random, [stroke](int x, int) { return stroke && x < 40 ? 40.0 : 0.0; }),
                {measure ? DetectionOverlay::Noise : DetectionOverlay::None, -1, -1, measure});
            if (i == 99) before = areaNoise(watch.capture);
            if (i == 108) afterFirstStroke = watch.candidates;
            if (i == 125) after = areaNoise(watch.capture);
        }
        watch.capture.finishPartial();
        if (target == Target::Moon)
        {
            // No large brightening on the Moon, and the first stroke is too
            // large to be a patch. The Moon learns each stroke, so its noise
            // rises; by the third, the struck area is near K and breaks into
            // patches small enough to pass. That is the Moon's behaviour
            // before Feature-11, kept as it was.
            QCOMPARE(afterFirstStroke, 0);
            for (const EventClip& clip : watch.clips)
                for (const ClipTrigger& trigger : clip.triggers) QCOMPARE(trigger.change, ChangeKind::Rise);
            continue;
        }
        QCOMPARE(watch.candidates, 3);
        QCOMPARE(watch.clips.size(), size_t{1});
        QCOMPARE(watch.clips[0].triggers.size(), size_t{3});
        for (const ClipTrigger& trigger : watch.clips[0].triggers)
        {
            QCOMPARE(trigger.change, ChangeKind::LargeRise);
            QVERIFY(std::abs(trigger.share - 0.4) < 0.02);
        }
        // The strokes were not learned, so the noise did not rise after them.
        QVERIFY2(std::abs(after / before - 1) < 0.1, qPrintable(QString("%1 then %2").arg(before).arg(after)));

        QTemporaryDir directory;
        const auto results = write(directory.path(), std::move(watch.clips));
        QCOMPARE(results.size(), size_t{1});
        const QString text = contents(QDir(results[0].folder).filePath("event.txt"));
        QVERIFY(text.contains("mode: sky\n"));
        QCOMPARE(text.count(" kind=brightening share="), 3);
        QVERIFY(text.contains(" brightening_share=0.02 release_seconds=20"));
        QVERIFY(contents(QDir(directory.path()).filePath("candidates.txt")).contains(" mode=sky "));
    }
}

// Done-when 3: a twilight over ten minutes, at two frames a second.
void SkyTest::aSlowRampFiresNothing()
{
    for (const Target target : {Target::Moon, Target::Sky})
    {
        std::mt19937 random(23);
        Watch watch(defaultCaptureSettings(target), defaultDetectionSettings(target));
        for (uint64_t i = 0; i < 1200; ++i)
            watch.capture.onFrame(skyFrame(64, 64, i, 2, random, {}, 10 + 190.0 * static_cast<double>(i) / 1199));
        watch.capture.finishPartial();
        QCOMPARE(watch.candidates, 0);
    }
}

// Done-when 4: a star drifting past a fixed camera at 0.2 px a second. The
// release limit is 3 s here to keep the test short; the rule is the same at 20 s.
void SkyTest::aDriftingStarIsReleasedAndItsClipEnds()
{
    std::mt19937 random(24);
    DetectionSettings detection = defaultDetectionSettings(Target::Sky);
    detection.releaseSeconds = 3;
    Watch watch(defaultCaptureSettings(Target::Sky), detection);
    constexpr double fps = 10;
    for (uint64_t i = 0; i < 260; ++i)
    {
        const double seconds = static_cast<double>(i) / fps;
        watch.capture.onFrame(skyFrame(60, 48, i, fps, random, [i, seconds](int x, int y) {
            if (i < 20) return 0.0;
            const double dx = x - (30 + 0.2 * seconds), dy = y - 24.0;
            return 100 * std::exp(-(dx * dx + dy * dy) / (2 * 1.2 * 1.2));
        }));
    }
    QVERIFY(!watch.capture.detectionSummary().firing);
    QCOMPARE(watch.candidates, 1);
    QCOMPARE(watch.clips.size(), size_t{1});
    const EventClip& clip = watch.clips[0];
    QCOMPARE(clip.endReason, std::string("post-event interval complete"));
    QCOMPARE(clip.releases.size(), size_t{1});
    const double releasedAfter = std::chrono::duration<double>(clip.releases[0].time - timeOf(20, fps)).count();
    QVERIFY2(std::abs(releasedAfter - 3) < 0.15, qPrintable(QString::number(releasedAfter)));
    // Learned within a few seconds of the release, then one quiet tail of 5 s.
    const double endedAfter = std::chrono::duration<double>(clip.frames.back()->timestamp - clip.releases[0].time).count();
    QVERIFY2(endedAfter >= 5 && endedAfter < 12, qPrintable(QString::number(endedAfter)));
}

// Done-when 4: a point crossing at 5 px a frame stays on new pixels, so it
// keeps firing after the release limit until it leaves the frame. It is
// smeared over 6 pixels along its motion, as an exposure of a moving light
// is, so each frame's patch touches the one before.
void SkyTest::aMovingPointKeepsFiringPastTheReleaseLimit()
{
    std::mt19937 random(25);
    DetectionSettings detection = defaultDetectionSettings(Target::Sky);
    detection.releaseSeconds = 3;
    Watch watch(defaultCaptureSettings(Target::Sky), detection);
    constexpr double fps = 10;
    for (uint64_t i = 0; i < 110; ++i)
    {
        const int x0 = i >= 20 ? 5 + 5 * static_cast<int>(i - 20) : -1000;
        watch.capture.onFrame(skyFrame(400, 40, i, fps, random, [x0](int x, int y) {
            return x >= x0 && x < x0 + 6 && y >= 18 && y < 21 ? 100.0 : 0.0;
        }));
        // Fully inside the frame from frame 20 to frame 96; the limit is reached at frame 50.
        if (i >= 20 && i <= 96) QVERIFY2(watch.capture.detectionSummary().firing, qPrintable(QString("frame %1").arg(i)));
    }
    watch.capture.finishPartial();
    QCOMPARE(watch.candidates, 1);
    QCOMPARE(watch.clips.size(), size_t{1});
    QCOMPARE(watch.clips[0].releases.size(), size_t{1});
}

// Done-when 5: the ground is masked. A flash on it does not fire; one in the
// sky does, and the saved frames hold the masked pixels unchanged.
void SkyTest::aMaskedFlashDoesNotFireAndItsPixelsAreSaved()
{
    std::mt19937 random(26);
    DetectionSettings detection = defaultDetectionSettings(Target::Sky);
    detection.mask = DetectionMask({DetectionMask::rectangle(0, 40, 100, 20)});
    Watch watch(defaultCaptureSettings(Target::Sky), detection);
    std::vector<FramePtr> sent;
    for (uint64_t i = 0; i < 260; ++i)
    {
        const bool ground = i == 30, sky = i == 60;
        sent.push_back(skyFrame(100, 60, i, 30, random, [ground, sky](int x, int y) {
            const bool onGround = ground && x >= 50 && x < 53 && y >= 50 && y < 53;
            const bool inSky = sky && x >= 50 && x < 53 && y >= 10 && y < 13;
            return onGround || inSky ? 80.0 : 0.0;
        }));
        watch.capture.onFrame(sent.back());
    }
    watch.capture.finishPartial();
    QCOMPARE(watch.candidates, 1);
    QCOMPARE(watch.clips.size(), size_t{1});
    const int peakY = watch.clips[0].triggers.front().peakY;
    QVERIFY(peakY >= 10 && peakY < 13);

    QTemporaryDir directory;
    const auto results = write(directory.path(), std::move(watch.clips));
    QCOMPARE(results.size(), size_t{1});
    const QDir folder(results[0].folder);
    QVERIFY(contents(folder.filePath("event.txt")).contains(" mask=0,40_100,40_100,60_0,60"));
    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(std::filesystem::path(folder.filePath("event.ser").toStdWString()), 30, error), error.c_str());
    // The clip starts at frame 0, so position 30 is the masked flash.
    QCOMPARE(reader.frameNumber(30), uint64_t{30});
    Frame read;
    read.data.resize(reader.frameBytes());
    QVERIFY(reader.readFrame(30, read, error));
    QCOMPARE(read.data, sent[30]->data);
}

// Done-when 6: below the disk reserve the clip is not written, and its
// candidate is still on record.
void SkyTest::aClipBelowTheDiskReserveIsLoggedButNotWritten()
{
    const auto clipWithReserve = [](uint64_t reserve) {
        std::mt19937 random(27);
        EventClip clip;
        clip.stream.source = "TEST: constructed disk reserve clip";
        for (uint64_t i = 0; i < 3; ++i) clip.frames.push_back(skyFrame(32, 24, i, 30, random));
        clip.triggers.push_back({1, 1, clip.frames[1]->timestamp, 9, false});
        clip.captureSettings.diskReserveBytes = reserve;
        return clip;
    };
    QTemporaryDir directory;
    std::vector<EventClip> clips;
    clips.push_back(clipWithReserve(10 * kGiB));
    auto results = write(directory.path(), std::move(clips), [](const QString&) { return std::optional<uint64_t>{kGiB}; });
    QCOMPARE(results.size(), size_t{1});
    QCOMPARE(results[0].notSaved, QString("disk reserve"));
    QVERIFY(results[0].diskReserve);
    QCOMPARE(results[0].freeBytes, kGiB);
    QVERIFY(results[0].folder.isEmpty());
    QVERIFY(QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    const QString log = contents(QDir(directory.path()).filePath("candidates.txt"));
    QVERIFY2(log.contains(" mode=moon ") && log.trimmed().endsWith("not_saved=disk reserve"), qPrintable(log));

    // With room above the reserve the same clip is written.
    clips.clear();
    clips.push_back(clipWithReserve(10 * kGiB));
    results = write(directory.path(), std::move(clips), [](const QString&) { return std::optional<uint64_t>{20 * kGiB}; });
    QCOMPARE(results.size(), size_t{1});
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    QVERIFY(contents(QDir(results[0].folder).filePath("event.txt")).contains("disk_reserve_bytes: 10737418240\n"));
}

// Done-when 7: an event firing for 5 s with a 2 s maximum. In Sky mode each
// linked clip starts after the one before; on the Moon it repeats the history.
void SkyTest::linkedSkyClipsDoNotRepeatTheirHistory()
{
    for (const Target target : {Target::Moon, Target::Sky})
    {
        std::mt19937 random(28);
        CaptureSettings capture = defaultCaptureSettings(target);
        capture.historySeconds = 1; capture.postSeconds = 1; capture.maxPostSeconds = 2;
        DetectionSettings detection = defaultDetectionSettings(target);
        detection.releaseSeconds = 0;
        Watch watch(capture, detection);
        for (uint64_t i = 0; i < 300; ++i)
        {
            const bool lit = i >= 30 && i < 180;
            watch.capture.onFrame(skyFrame(60, 40, i, 30, random, [lit](int x, int y) {
                return lit && x >= 30 && x < 33 && y >= 20 && y < 23 ? 80.0 : 0.0;
            }));
        }
        watch.capture.finishPartial();
        QVERIFY(watch.clips.size() >= 3);
        for (size_t c = 1; c < watch.clips.size(); ++c)
        {
            const EventClip& previous = watch.clips[c - 1];
            const EventClip& clip = watch.clips[c];
            QVERIFY(clip.continuesClipTriggeredAt.has_value());
            QVERIFY(*clip.continuesClipTriggeredAt == previous.triggers.front().triggerTime);
            if (target == Target::Sky)
                QCOMPARE(clip.frames.front()->frameIndex, previous.frames.back()->frameIndex + 1);
            else
                QVERIFY(clip.frames.front()->frameIndex <= previous.frames.back()->frameIndex);
        }
    }
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    SkyTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "sky_tests.moc"
