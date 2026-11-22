#include "watch_folders.h"
#include <future>
#include "session/clock_check.h"
#include "recording/clip_overview.h"
#include "recording/frame_timing.h"
#include "recording/output_paths.h"
#include "recording/recording_writer.h"
#include "recording/ser_reader.h"
#include "recording/ser_writer.h"
#include "session/session.h"
#include "sources/recording_source.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <atomic>
#include <filesystem>
#include <random>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

using namespace std::chrono;

constexpr uint64_t kGiB = uint64_t{1024} * 1024 * 1024;

system_clock::time_point timeOf(uint64_t index)
{
    return system_clock::time_point{} + seconds{1'790'000'000} + milliseconds{static_cast<int64_t>(index) * 33};
}

// A 64 x 48 Mono8 frame with noise, and a 3 x 3 flash when asked.
FramePtr makeFrame(uint64_t index, std::mt19937& random, bool flash = false)
{
    auto frame = std::make_shared<Frame>();
    frame->width = 64; frame->height = 48; frame->frameIndex = index;
    frame->timestamp = timeOf(index);
    frame->steadyTimestamp = steady_clock::time_point{} + milliseconds{static_cast<int64_t>(index) * 33};
    frame->testSource = true;
    std::normal_distribution<double> noise(0, 2);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const double value = 100 + noise(random) + (flash && x >= 30 && x < 33 && y >= 20 && y < 23 ? 80 : 0);
            frame->data.push_back(static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L)));
        }
    return frame;
}

// A SER file of count frames, a flash on flashFrame when it is not negative.
QString makeRecording(const QString& folder, int count, int flashFrame)
{
    const QString path = QDir(folder).filePath("input.ser");
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) return {};
    SerWriter writer(file);
    QString error;
    std::mt19937 random(41);
    for (int i = 0; i < count; ++i)
    {
        const auto frame = makeFrame(static_cast<uint64_t>(i), random, i == flashFrame);
        if (i == 0 && !writer.start(*frame, "TEST: constructed session record", error)) return {};
        if (!writer.append(*frame, error)) return {};
    }
    return writer.finish(error) ? path : QString();
}

QString contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

// The one recording folder under an output root.
QDir recordingFolder(const QString& outputRoot)
{
    const QDir recordings(QDir(outputRoot).filePath("recordings"));
    const QStringList names = recordings.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    return names.size() == 1 ? QDir(recordings.filePath(names.front())) : QDir();
}

#ifdef _WIN32
// An SNTP server on this computer whose clock runs offsetMs ahead of the PC's.
class TestNtpServer
{
public:
    explicit TestNtpServer(double offsetMs) : offsetMs_(offsetMs)
    {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        int length = sizeof(address);
        getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        const DWORD wait = 200;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
        thread_ = std::jthread([this](std::stop_token stop) { serve(stop); });
    }
    ~TestNtpServer()
    {
        thread_.request_stop();
        thread_.join();
        closesocket(socket_);
        WSACleanup();
    }
    [[nodiscard]] uint16_t port() const { return port_; }

private:
    void serve(const std::stop_token& stop)
    {
        while (!stop.stop_requested())
        {
            uint8_t packet[48]{};
            sockaddr_in from{};
            int length = sizeof(from);
            const int received = recvfrom(socket_, reinterpret_cast<char*>(packet), sizeof(packet), 0,
                reinterpret_cast<sockaddr*>(&from), &length);
            if (received < 48) continue;
            uint8_t reply[48]{};
            reply[0] = 0x24; // Version 4, server.
            reply[1] = 2;    // Stratum.
            std::copy_n(packet + 40, 8, reply + 24); // Their transmit time is our origin.
            const auto now = system_clock::now() + duration_cast<system_clock::duration>(duration<double, std::milli>(offsetMs_));
            const auto since = now.time_since_epoch();
            const auto whole = duration_cast<seconds>(since);
            const uint64_t value = ((static_cast<uint64_t>(whole.count()) + 2'208'988'800ULL) << 32)
                | static_cast<uint64_t>(duration<double>(since - whole).count() * 4294967296.0);
            for (int i = 0; i < 8; ++i) reply[32 + i] = reply[40 + i] = static_cast<uint8_t>(value >> (56 - 8 * i));
            sendto(socket_, reinterpret_cast<const char*>(reply), sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), length);
        }
    }
    double offsetMs_;
    SOCKET socket_{INVALID_SOCKET};
    uint16_t port_{0};
    std::jthread thread_;
};
#endif

} // namespace

// Feature-14: the full session record.
class SessionRecordTest : public QObject
{
    Q_OBJECT
private slots:
    void consecutiveRecordingsKeepTheirOwnWatch();
    void aScheduledRecordingTakesEveryFrameInItsSpan();
    void aRecordingStartedByTheObserverIsComplete();
    void aSlowDiskStopsTheRecordingWithoutLosingFrames();
    void theDiskReserveStopsTheRecording();
    void aRecordingCanBeReadBeforeItIsFinished();
    void frameIntervalsAreMeasured();
    void theClockIsComparedWithAnNtpServer();
    void theSiteIsRecordedEverywhere();
    void previewRecordingWithFrameLimitWritesExactFramesWithoutCandidates();
    void outputPathsAndValidationSeparateModesAndDetectIssues();
};

// Done-when items 1 and 2: by frame time, on a played file, with a clip in it.
void SessionRecordTest::consecutiveRecordingsKeepTheirOwnWatch()
{
    QTemporaryDir temp;
    std::vector<RecordingWriter::Status> ended;
    bool secondStillActive = false;
    // The end callback runs outside the writer's lock, so it may read the
    // writer's status; it reaches the writer through a pointer set after
    // construction (a lambda cannot name the object it initializes).
    RecordingWriter* self = nullptr;
    RecordingWriter writer([&](const RecordingWriter::Status& status) {
        ended.push_back(status);
        if (ended.size() == 1) secondStillActive = self->status().active;
    });
    self = &writer;
    std::promise<void> release;
    auto gate = release.get_future();
    writer.afterQueued([&] { gate.wait(); });
    std::mt19937 random(7);
    RecordingWriter::Start start;
    start.folder = temp.filePath("first/recordings");
    const bool first = writer.start(start, makeFrame(1, random));
    writer.stop("first watch ended");
    start.folder = temp.filePath("second/recordings");
    const bool second = writer.start(start, makeFrame(2, random));
    writer.stop("second watch ended");
    release.set_value();
    writer.waitIdle();
    QVERIFY(first && second);
    QCOMPARE(ended.size(), size_t{2});
    QVERIFY(secondStillActive);
    QCOMPARE(ended[0].parentFolder, temp.filePath("first/recordings"));
    QCOMPARE(ended[1].parentFolder, temp.filePath("second/recordings"));
    for (const auto& result : ended)
        QVERIFY(contents(QDir(result.parentFolder).filePath(result.name + "/recording.txt")).contains("complete: yes"));
}

void SessionRecordTest::aScheduledRecordingTakesEveryFrameInItsSpan()
{
    QTemporaryDir directory;
    const QString input = makeRecording(directory.path(), 120, 60);
    QVERIFY(!input.isEmpty());
    const QString output = QDir(directory.path()).filePath("events");
    QObject context;
    bool ended = false;
    Session::Notifications notices;
    notices.ended = [&] { ended = true; };
    Session session(&context, output, notices);
    auto source = std::make_unique<RecordingSource>(std::filesystem::path(input.toStdWString()));
    source->setSpeed(RecordingSource::Speed::Fast);
    session.setSource(std::move(source));
    CaptureSettings capture;
    capture.historySeconds = 0.5; capture.postSeconds = 0.3; capture.maxPostSeconds = 1;
    session.setCaptureSettings(capture);
    // From frame 40's time for 1.5 s: frames 40 to 85 (85 x 33 ms is the
    // last before 40 x 33 + 1500).
    session.scheduleRecording({timeOf(40), timeOf(40) + milliseconds{1500}});
    QString error;
    QVERIFY2(session.start(error, Session::Mode::Monitoring), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(ended, 10000);
    session.stop();
    session.recordingWriter().waitIdle();
    QVERIFY(!session.schedule());

    const QDir folder = recordingFolder(latestWatch(eventsFolder(output, Target::Moon)));
    QVERIFY(folder.exists());
    const QString note = contents(folder.filePath("recording.txt"));
    QVERIFY2(note.contains("started_by: a schedule from ") && note.contains("first_frame_index: 40\n")
        && note.contains("last_frame_index: 85\n") && note.contains("frames_written: 46\n")
        && note.contains("ended: the scheduled end\n") && note.endsWith("complete: yes\n"), qPrintable(note));
    SerReader reader;
    std::string readError;
    QVERIFY(reader.open(std::filesystem::path(folder.filePath("recording.ser").toStdWString()), 30, readError));
    QCOMPARE(reader.frameCount(), 46);
    SerReader original;
    QVERIFY(original.open(std::filesystem::path(input.toStdWString()), 30, readError));
    for (int i = 0; i < 46; ++i)
    {
        Frame recorded, played;
        recorded.data.resize(reader.frameBytes());
        played.data.resize(original.frameBytes());
        QVERIFY(reader.readFrame(i, recorded, readError));
        QVERIFY(original.readFrame(40 + i, played, readError));
        QCOMPARE(recorded.data, played.data);
        QVERIFY(reader.frameTime(i) == timeOf(static_cast<uint64_t>(40 + i)));
    }
    const QString report = latestWatch(eventsFolder(output, Target::Moon)) + "/report.html";
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(report), 10000);
    QVERIFY(contents(report).contains("recordings/" + folder.dirName() + "/recording.ser"));
    QVERIFY(contents(report).contains("46 frames"));
    const QStringList frames = contents(folder.filePath("frames.txt")).split('\n', Qt::SkipEmptyParts);
    QCOMPARE(frames.size(), qsizetype{46});
    QVERIFY(frames.front().startsWith("0 40 ") && frames.back().startsWith("45 85 "));

    const QString moonEvents = latestWatch(eventsFolder(output, Target::Moon));
    QTRY_VERIFY2_WITH_TIMEOUT(([&] {
        const QString log = contents(QDir(moonEvents).filePath("candidates.txt"));
        return log.contains(" recording_start name=" + folder.dirName() + " first_frame_index=40")
            && log.contains(" recording_stop name=" + folder.dirName() + " frames=46");
    })(), qPrintable(contents(QDir(moonEvents).filePath("candidates.txt"))), 10000);
    // The flash on frame 60 made a clip that names the recording.
    const QStringList clips = QDir(moonEvents + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    bool named = false;
    for (const QString& clip : clips)
        named = named || contents(QDir(QDir(moonEvents + "/clips").filePath(clip)).filePath("event.txt")).contains("during_recording: " + folder.dirName() + "\n");
    QVERIFY(named);
}

// Done-when item 1, by the observer's request, while a file plays in real time.
void SessionRecordTest::aRecordingStartedByTheObserverIsComplete()
{
    QTemporaryDir directory;
    // Ten seconds of frames, so a slow start still leaves room to record.
    const QString input = makeRecording(directory.path(), 300, -1);
    const QString output = QDir(directory.path()).filePath("events");
    QObject context;
    int changes = 0;
    Session::Notifications notices;
    notices.recordingChanged = [&] { ++changes; };
    Session session(&context, output, notices);
    session.setSource(std::make_unique<RecordingSource>(std::filesystem::path(input.toStdWString())));
    QString error;
    QVERIFY(session.start(error, Session::Mode::Monitoring));
    QVERIFY(session.requestRecording(true));
    QTRY_VERIFY2_WITH_TIMEOUT(session.recordingStatus().frames >= 20, qPrintable(QString("frames=%1 active=%2 acquired=%3 end=%4")
        .arg(session.recordingStatus().frames).arg(session.recordingStatus().active).arg(session.acquiredFrames())
        .arg(QString::fromStdString(session.recordingStatus().endReason))), 5000);
    QVERIFY(session.requestRecording(false));
    QTRY_VERIFY_WITH_TIMEOUT(!session.recordingStatus().active, 5000);
    session.stop();
    session.recordingWriter().waitIdle();
    QTRY_VERIFY_WITH_TIMEOUT(changes >= 2, 3000);
    const QString note = contents(recordingFolder(latestWatch(eventsFolder(output, Target::Moon))).filePath("recording.txt"));
    QVERIFY2(note.contains("started_by: the observer\n") && note.contains("ended: stopped by the observer\n")
        && note.contains("dropped_frame_gaps: 0 missing_frames=0\n") && note.endsWith("complete: yes\n"), qPrintable(note));
    // Recording needs Monitoring.
    QVERIFY(!session.requestRecording(true));
}

// Done-when item 3: the queue fills, the recording stops and says why, and
// what it holds is contiguous and complete.
void SessionRecordTest::aSlowDiskStopsTheRecordingWithoutLosingFrames()
{
    QTemporaryDir directory;
    std::vector<RecordingWriter::Status> ended;
    RecordingWriter writer([&](const RecordingWriter::Status& status) { ended.push_back(status); });
    writer.setWriteDelay(milliseconds{20});
    std::mt19937 random(42);
    RecordingWriter::Start start;
    start.folder = QDir(directory.path()).filePath("recordings");
    start.startedBy = "a test";
    start.queueBytes = 10 * 64 * 48;
    QVERIFY(writer.start(start, makeFrame(0, random)));
    uint64_t accepted = 1;
    for (uint64_t i = 1; i < 200; ++i)
    {
        if (!writer.push(makeFrame(i, random))) break;
        ++accepted;
    }
    QVERIFY(accepted < 200);
    QVERIFY(!writer.push(makeFrame(500, random)));
    writer.waitIdle();
    QCOMPARE(ended.size(), size_t{1});
    QVERIFY2(ended[0].endReason.starts_with("disk too slow"), ended[0].endReason.c_str());
    QCOMPARE(ended[0].frames, accepted);
    const QDir folder = recordingFolder(directory.path());
    QVERIFY(contents(folder.filePath("recording.txt")).endsWith("complete: yes\n"));
    SerReader reader;
    std::string error;
    QVERIFY(reader.open(std::filesystem::path(folder.filePath("recording.ser").toStdWString()), 30, error));
    QCOMPARE(static_cast<uint64_t>(reader.frameCount()), accepted);
    QVERIFY(reader.frameTime(static_cast<int>(accepted - 1)) == timeOf(accepted - 1));
}

// Done-when item 4.
void SessionRecordTest::theDiskReserveStopsTheRecording()
{
    QTemporaryDir directory;
    RecordingWriter writer;
    writer.setFreeSpace([](const QString&) { return std::optional<uint64_t>{kGiB}; });
    std::mt19937 random(43);
    RecordingWriter::Start start;
    start.folder = QDir(directory.path()).filePath("recordings");
    start.diskReserveBytes = 10 * kGiB;
    start.queueBytes = 64 * 1024 * 1024;
    QVERIFY(writer.start(start, makeFrame(0, random)));
    // The free space is checked once a second.
    bool stopped = false;
    for (uint64_t i = 1; i < 300 && !stopped; ++i)
    {
        stopped = !writer.push(makeFrame(i, random));
        std::this_thread::sleep_for(milliseconds{10});
    }
    QVERIFY(stopped);
    writer.waitIdle();
    QVERIFY2(writer.status().endReason.starts_with("disk reserve: 1.0 GiB free"), writer.status().endReason.c_str());
}

// Done-when item 5: after a second the header holds the frames so far.
void SessionRecordTest::aRecordingCanBeReadBeforeItIsFinished()
{
    QTemporaryDir directory;
    RecordingWriter writer;
    std::mt19937 random(44);
    RecordingWriter::Start start;
    start.folder = QDir(directory.path()).filePath("recordings");
    start.queueBytes = 64 * 1024 * 1024;
    QVERIFY(writer.start(start, makeFrame(0, random)));
    for (uint64_t i = 1; i < 130; ++i)
    {
        QVERIFY(writer.push(makeFrame(i, random)));
        std::this_thread::sleep_for(milliseconds{10});
    }
    QTest::qWait(300);
    // Not finished: no trailer, no completion marker, but readable.
    const QDir folder = recordingFolder(directory.path());
    QVERIFY(!contents(folder.filePath("recording.txt")).contains("complete: yes"));
    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(std::filesystem::path(folder.filePath("recording.ser").toStdWString()), 30, error), error.c_str());
    QVERIFY(reader.frameCount() >= 50);
    Frame frame;
    frame.data.resize(reader.frameBytes());
    QVERIFY(reader.readFrame(reader.frameCount() - 1, frame, error));
    writer.stop("the test ended");
    writer.waitIdle();
}

// Done-when item 6.
void SessionRecordTest::frameIntervalsAreMeasured()
{
    std::vector<FramePtr> frames;
    const int steady[] = {0, 10, 21, 30, 39, 50};
    for (int i = 0; i < 6; ++i)
    {
        auto frame = std::make_shared<Frame>();
        frame->steadyTimestamp = steady_clock::time_point{} + milliseconds{steady[i]};
        // The PC clock is put forward by 500 ms before the fourth frame.
        frame->timestamp = system_clock::time_point{} + seconds{1'790'000'000} + milliseconds{steady[i] + (i >= 3 ? 500 : 0)};
        frames.push_back(frame);
    }
    const FrameTiming camera = measureFrameTiming(frames, true);
    QCOMPARE(camera.intervals, size_t{5});
    QVERIFY(std::abs(camera.meanMs - 10) < 1e-9);
    QVERIFY(std::abs(camera.minMs - 9) < 1e-9 && std::abs(camera.maxMs - 11) < 1e-9);
    QVERIFY(std::abs(camera.sdMs - std::sqrt(4.0 / 4)) < 1e-9);
    QVERIFY(camera.largestClockStepMs && std::abs(*camera.largestClockStepMs - 500) < 1e-9);
    const FrameTiming file = measureFrameTiming(frames, false);
    QVERIFY(std::abs(file.maxMs - 509) < 1e-9);
    QVERIFY(!file.largestClockStepMs);
    QVERIFY(timingText(camera).startsWith("clock=\"host monotonic clock\" intervals=5 mean_ms=10.000"));
}

// Done-when item 7.
void SessionRecordTest::theClockIsComparedWithAnNtpServer()
{
#ifdef _WIN32
    {
        TestNtpServer server(2500);
        const NtpResult result = queryNtp("127.0.0.1", server.port(), 4, milliseconds{500});
        QVERIFY2(result.answered, result.error.c_str());
        QVERIFY2(std::abs(result.offsetMs - 2500) < 5, qPrintable(QString::number(result.offsetMs)));
        QVERIFY(result.roundTripMs < 50);
        const std::string text = clockCheckText("status: test", &result, system_clock::now());
        QVERIFY(text.find("ntp_server=127.0.0.1 server_minus_pc_ms=25") != std::string::npos);
    }
    // Nothing listens on this port any more.
    TestNtpServer gone(0);
    const uint16_t port = gone.port();
    const NtpResult silent = queryNtp("127.0.0.1", static_cast<uint16_t>(port == 65535 ? 65534 : port + 1), 2, milliseconds{200});
    QVERIFY(!silent.answered);
    QVERIFY(!silent.error.empty());
    QVERIFY(clockCheckText("status: test", nullptr, system_clock::now()).ends_with(" ntp=off"));
#else
    QSKIP("SNTP is built for Windows only");
#endif
}

// Done-when item 8: the site on session_start, in event.txt, in
// recording.txt, in the SER header, and speeds in degrees per second.
void SessionRecordTest::theSiteIsRecordedEverywhere()
{
    ObservingSite site;
    site.observer = "A. Observer";
    site.site = "Test field";
    site.latitudeDeg = 37.98;
    site.longitudeDeg = 23.73;
    site.heightM = 100;
    site.telescope = "C9.25";
    site.apertureMm = 235;
    site.focalLengthMm = 2350;
    site.pixelSizeUm = 3.75;
    QVERIFY(std::abs(*site.arcsecPerPixel() - 0.32914) < 1e-4);

    QTemporaryDir directory;
    const QString input = makeRecording(directory.path(), 90, 50);
    const QString output = QDir(directory.path()).filePath("events");
    QObject context;
    bool ended = false;
    Session::Notifications notices;
    notices.ended = [&] { ended = true; };
    Session session(&context, output, notices);
    session.setSite(site);
    auto source = std::make_unique<RecordingSource>(std::filesystem::path(input.toStdWString()));
    source->setSpeed(RecordingSource::Speed::Fast);
    session.setSource(std::move(source));
    CaptureSettings capture;
    capture.historySeconds = 0.5; capture.postSeconds = 0.3; capture.maxPostSeconds = 1;
    session.setCaptureSettings(capture);
    session.scheduleRecording({timeOf(10), timeOf(80)});
    QString error;
    QVERIFY(session.start(error, Session::Mode::Monitoring));
    QTRY_VERIFY_WITH_TIMEOUT(ended, 10000);
    session.stop();
    session.recordingWriter().waitIdle();

    const QString moonEvents = latestWatch(eventsFolder(output, Target::Moon));
    QTRY_VERIFY2_WITH_TIMEOUT(([&] {
        return contents(QDir(moonEvents).filePath("candidates.txt"))
            .contains(" observer=\"A. Observer\" site=\"Test field\" latitude_deg=37.98000 longitude_deg=23.73000 height_m=100 telescope=\"C9.25\"");
    })(), qPrintable(contents(QDir(moonEvents).filePath("candidates.txt"))), 10000);
    const QDir folder = recordingFolder(latestWatch(eventsFolder(output, Target::Moon)));
    const QString note = contents(folder.filePath("recording.txt"));
    QVERIFY(note.contains("observer: A. Observer\n") && note.contains("image_scale_arcsec_per_pixel: 0.329\n"));
    QFile ser(folder.filePath("recording.ser"));
    QVERIFY(ser.open(QIODevice::ReadOnly));
    QCOMPARE(QByteArray(ser.read(178).constData() + 122), QByteArray("C9.25"));
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        for (const QString& clip : QDir(moonEvents + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (contents(QDir(QDir(moonEvents + "/clips").filePath(clip)).filePath("event.txt")).contains("telescope: C9.25\n"))
                return true;
        return false;
    })(), 10000);

    // The overview gives the speed in degrees per second.
    std::mt19937 random(45);
    std::vector<FramePtr> frames;
    for (uint64_t i = 0; i < 70; ++i)
    {
        auto frame = std::const_pointer_cast<Frame>(makeFrame(i, random));
        if (i >= 35 && i < 60)
            for (int y = 20; y < 23; ++y)
                for (int x = 2 * static_cast<int>(i - 35); x < 2 * static_cast<int>(i - 35) + 3; ++x)
                    frame->data[static_cast<size_t>(y) * 64 + x] = 220;
        frames.push_back(frame);
    }
    OverviewRequest request;
    request.onsetFrameIndex = 36;
    request.arcsecPerPixel = site.arcsecPerPixel();
    const ClipOverview overview = makeClipOverview(frames, request);
    QVERIFY2(overview.activity.contains("°/s)"), qPrintable(overview.activity));
}

// Feature-17: manual recording in Preview works with an optional frame limit,
// writes complete SER evidence, and never touches candidates.txt.
void SessionRecordTest::previewRecordingWithFrameLimitWritesExactFramesWithoutCandidates()
{
    QTemporaryDir directory;
    const QString input = makeRecording(directory.path(), 40, -1);
    const QString root = QDir(directory.path()).filePath("root");
    QDir().mkpath(root);
    QObject context;
    Session session(&context, root, {});
    session.setSource(std::make_unique<RecordingSource>(std::filesystem::path(input.toStdWString())));
    QString error;
    QVERIFY2(session.start(error, Session::Mode::Preview), qPrintable(error));
    QVERIFY(session.requestRecording(true, 10));
    QTRY_VERIFY_WITH_TIMEOUT(session.recordingStatus().frames == 10 && !session.recordingStatus().active, 5000);
    session.recordingWriter().waitIdle();
    session.stop();

    const QDir folder = recordingFolder(root);
    QVERIFY(folder.exists());
    const QString note = contents(folder.filePath("recording.txt"));
    QVERIFY2(note.contains("session_mode: preview\n") && note.contains("frame_limit: 10\n")
        && note.contains("frames_written: 10\n") && note.contains("ended: reached frame limit\n")
        && note.endsWith("complete: yes\n"), qPrintable(note));
    SerReader reader;
    std::string readError;
    QVERIFY2(reader.open(std::filesystem::path(folder.filePath("recording.ser").toStdWString()), 30, readError), readError.c_str());
    QCOMPARE(reader.frameCount(), 10);
    QVERIFY(!QFile::exists(QDir(eventsFolder(root, Target::Moon)).filePath("candidates.txt")));
}

void SessionRecordTest::outputPathsAndValidationSeparateModesAndDetectIssues()
{
    QTemporaryDir dir;
    const QString root = QDir(dir.path()).filePath("astro");
    QCOMPARE(eventsFolder(root, Target::Moon), QDir(root).filePath("events/moon"));
    QCOMPARE(eventsFolder(root, Target::Sky), QDir(root).filePath("events/sky"));
    QCOMPARE(recordingsFolder(root), QDir(root).filePath("recordings"));
    QCOMPARE(timelapsesFolder(root), QDir(root).filePath("timelapses"));

    QVERIFY(!isCloudSyncedPath(defaultOutputFolder()));
    QVERIFY(isCloudSyncedPath("C:/Users/Test/OneDrive/Pictures/SeleneWatch"));

    const OutputFolderCheck missing = checkOutputFolder(root, 0, false);
    QVERIFY(!missing.ok && missing.folderMissing && !missing.mountMissing);

    const OutputFolderCheck created = checkOutputFolder(root, 0, true);
    QVERIFY(created.ok && !created.belowReserve);

    const OutputFolderCheck lowSpace = checkOutputFolder(root, 10 * kGiB, false,
        [](const QString&) { return std::optional<uint64_t>{kGiB}; });
    QVERIFY(lowSpace.ok && lowSpace.belowReserve);

    QFile blocker(QDir(dir.path()).filePath("blocker"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    const OutputFolderCheck blocked = checkOutputFolder(QDir(dir.path()).filePath("blocker/out"), 0, true);
    QVERIFY(!blocked.ok && blocked.notWritable);

    const OutputFolderCheck missingDrive = checkOutputFolder("Z:/DefinitelyMissingDriveXYZ/SeleneWatch", 0, true);
    QVERIFY(!missingDrive.ok && missingDrive.mountMissing);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    SessionRecordTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "session_record_tests.moc"
