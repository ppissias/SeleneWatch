#include "tests/fake_touptek_api.h"
#include "cameras/touptek_source.h"
#include "recording/ser_reader.h"
#include "session.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <mutex>

namespace {
// Collects frames and failures from the worker; the test polls through locked accessors.
class Receiver : public FrameSourceListener
{
public:
    void onFrame(std::shared_ptr<Frame> f) override { std::lock_guard lock(mutex_); frames.push_back(std::move(f)); }
    void onDiscontinuity() override {}
    void onEnded() override {}
    void onFailed(const std::string& reason) override { std::lock_guard lock(mutex_); error = reason; }
    size_t count() { std::lock_guard lock(mutex_); return frames.size(); }
    bool failed() { std::lock_guard lock(mutex_); return !error.empty(); }
    std::vector<FramePtr> frames;
    std::string error;
private:
    std::mutex mutex_;
};
QString readText(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}
// Streams frames until count arrived or the source failed, then stops.
std::vector<FramePtr> stream(const std::shared_ptr<FakeToupTekApi>& api, size_t count, std::string& failure)
{
    const auto profile = ToupTekSource::inspect(api, "TEST-TT-ID");
    Receiver sink;
    ToupTekSource source(api, profile, profile.current);
    std::string error;
    if (!source.open(error)) { failure = error; return {}; }
    source.start(sink);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (sink.count() < count && !sink.failed() && std::chrono::steady_clock::now() < until)
        QTest::qWait(5);
    source.stop();
    failure = sink.error;
    return sink.frames;
}
} // namespace

// Feature-16: the ToupTek adapter's lifecycle, native frames, sequence-based
// loss accounting and evidence, with a test camera.
class ToupTekTest : public QObject
{
    Q_OBJECT
private slots:
    void openConfiguresRawAndReadsBack();
    void sequenceNumbersCountLosses();
    void invalidSettingsAndFailuresCloseTheCamera();
    void notReadyAndDisconnectAreExplicit();
    void liveControlsChangeOnTheSameConnection();
    void sessionKeepsNativeEvidence();
};

void ToupTekTest::openConfiguresRawAndReadsBack()
{
    auto api = std::make_shared<FakeToupTekApi>();
    const auto profile = ToupTekSource::inspect(api, "TEST-TT-ID");
    QVERIFY(api->upsideDownOff);
    QCOMPARE(profile.serial, std::string("TEST-TT-0001"));
    auto settings = profile.current;
    settings.format = PixelFormat::Mono16;
    settings.width = 64; settings.height = 32; settings.x = 2; settings.y = 4;
    // 200 is beyond the 8-bit black level range (31) and inside the 16-bit one (496).
    settings.controls[ToupTekControl::BlackLevel] = 200;
    settings.controls[ToupTekControl::Gain] = 250;
    Receiver sink;
    ToupTekSource source(api, profile, settings);
    std::string error;
    QVERIFY2(source.open(error), error.c_str());
    QCOMPARE(source.actualProfile().current.controls.at(ToupTekControl::BlackLevel), int64_t{200});
    const auto info = source.streamInfo();
    QCOMPARE(info.serial, std::string("TEST-TT-0001"));
    QCOMPARE(info.deviceId, std::string("TEST-TT-ID"));
    QVERIFY(info.gain.starts_with("250 "));
    QVERIFY(info.conversion.find("upside-down") != std::string::npos);
    source.start(sink);
    QTRY_VERIFY_WITH_TIMEOUT(sink.count() >= 3, 3000);
    source.stop();
    QVERIFY(sink.error.empty());
    for (size_t i = 0; i < 3; ++i)
    {
        const auto& f = *sink.frames[i];
        QCOMPARE(f.frameIndex, static_cast<uint64_t>(i));
        QCOMPARE(f.sourceDroppedFrames.value_or(99), uint64_t{0});
        QCOMPARE(f.bayer, BayerPattern::RGGB); // an even origin keeps the sensor's cell
        QCOMPARE(f.significantBits, 16);
        QCOMPARE(static_cast<size_t>(f.data[0] | (f.data[1] << 8)), size_t{3000});
    }
    QCOMPARE(api->starts.load(), api->stops.load());
    QCOMPARE(api->opened.load(), api->closed.load());

    // The same black level at 8 bits is refused.
    settings.format = PixelFormat::Mono8;
    ToupTekSource eightBit(api, profile, settings);
    QVERIFY(!eightBit.open(error));
    QCOMPARE(api->opened.load(), api->closed.load());
}

void ToupTekTest::sequenceNumbersCountLosses()
{
    std::string failure;
    {
        auto api = std::make_shared<FakeToupTekApi>();
        api->sequences = {500, 501, 504, 505};
        const auto frames = stream(api, 4, failure);
        QVERIFY2(failure.empty(), failure.c_str());
        QCOMPARE(frames[2]->frameIndex, uint64_t{4});
        QCOMPARE(frames[2]->sourceDroppedFrames.value_or(0), uint64_t{2});
        QCOMPARE(frames[3]->frameIndex, uint64_t{5});
        QCOMPARE(frames[3]->sourceDroppedFrames.value_or(0), uint64_t{2});
    }
    {
        // The camera's 32-bit counter wraps.
        auto api = std::make_shared<FakeToupTekApi>();
        api->sequences = {0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u};
        const auto frames = stream(api, 4, failure);
        QVERIFY2(failure.empty(), failure.c_str());
        QCOMPARE(frames[3]->frameIndex, uint64_t{3});
        QCOMPARE(frames[3]->sourceDroppedFrames.value_or(99), uint64_t{0});
    }
    {
        auto api = std::make_shared<FakeToupTekApi>();
        api->sequences = {10, 11, 9};
        const auto frames = stream(api, 3, failure);
        QVERIFY(failure.find("backwards") != std::string::npos);
        QCOMPARE(frames.size(), size_t{2});
    }
    {
        // Without sequence numbers, losses are unknown.
        auto api = std::make_shared<FakeToupTekApi>();
        api->noSequence = true;
        const auto frames = stream(api, 3, failure);
        QVERIFY(failure.empty());
        QCOMPARE(frames[2]->frameIndex, uint64_t{2});
        QVERIFY(!frames[2]->sourceDroppedFrames);
    }
}

void ToupTekTest::invalidSettingsAndFailuresCloseTheCamera()
{
    auto api = std::make_shared<FakeToupTekApi>();
    const auto profile = ToupTekSource::inspect(api, "TEST-TT-ID");
    for (int kind = 0; kind < 5; ++kind)
    {
        auto s = profile.current;
        if (kind == 0) s.width = 63;      // odd
        if (kind == 1) s.x = 1;           // odd origin
        if (kind == 2) s.height = 6;      // below the minimum of 8
        if (kind == 3) s.width = 130;     // outside the sensor
        if (kind == 4) s.controls[ToupTekControl::Gain] = 99;
        ToupTekSource source(api, profile, s);
        std::string error;
        QVERIFY(!source.open(error));
        QVERIFY(!error.empty());
        QCOMPARE(api->opened.load(), api->closed.load());
    }
    for (const auto* operation : {"profile", "configure", "control"})
    {
        auto s = profile.current;
        s.controls[ToupTekControl::Gain] = 200;
        api->failOperation = operation;
        ToupTekSource source(api, profile, s);
        std::string error;
        QVERIFY(!source.open(error));
        QCOMPARE(api->opened.load(), api->closed.load());
    }
    api->failOperation.clear();
    api->connected = false;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, ToupTekSource::inspect(api, "TEST-TT-ID"));
    QCOMPARE(api->starts.load(), 0);
}

void ToupTekTest::notReadyAndDisconnectAreExplicit()
{
    for (int mode = 0; mode < 3; ++mode)
    {
        auto api = std::make_shared<FakeToupTekApi>();
        const auto profile = ToupTekSource::inspect(api, "TEST-TT-ID");
        if (mode == 0) api->notReady = true;
        if (mode == 1) api->disconnectAt = 3;
        if (mode == 2) api->failOperation = "start";
        Receiver sink;
        ToupTekSource source(api, profile, profile.current);
        std::string error;
        QVERIFY(source.open(error));
        source.start(sink);
        if (mode == 0) QTRY_VERIFY_WITH_TIMEOUT(api->reads >= 3, 3000);
        else QTRY_VERIFY_WITH_TIMEOUT(sink.failed(), 3000);
        const auto before = std::chrono::steady_clock::now();
        source.stop();
        QVERIFY(std::chrono::steady_clock::now() - before < std::chrono::milliseconds{250});
        QCOMPARE(sink.count(), mode == 1 ? size_t{3} : size_t{0});
        QCOMPARE(api->opened.load(), api->closed.load());
    }
}

void ToupTekTest::liveControlsChangeOnTheSameConnection()
{
    auto api = std::make_shared<FakeToupTekApi>();
    const auto profile = ToupTekSource::inspect(api, "TEST-TT-ID");
    Receiver sink;
    ToupTekSource source(api, profile, profile.current);
    std::string error;
    QVERIFY(source.open(error));
    const int opens = api->opened.load();
    source.start(sink);
    QTRY_VERIFY_WITH_TIMEOUT(sink.count() >= 2, 3000);
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, source.requestLiveControls({{ToupTekControl::Gain, 99}}));
    source.requestLiveControls({{ToupTekControl::Exposure, 5000}, {ToupTekControl::Gain, 300}});
    QTRY_COMPARE_WITH_TIMEOUT(source.actualProfile().current.controls.at(ToupTekControl::Exposure), int64_t{5000}, 3000);
    QCOMPARE(source.actualProfile().current.controls.at(ToupTekControl::Gain), int64_t{300});
    QCOMPARE(api->opened.load(), opens);
    QCOMPARE(api->starts.load(), 1);
    source.stop();
    QVERIFY(!sink.failed());
}

void ToupTekTest::sessionKeepsNativeEvidence()
{
    QTemporaryDir dir;
    QObject context;
    std::vector<ClipWriteResult> outputs;
    auto api = std::make_shared<FakeToupTekApi>();
    api->frameLimit = 40;
    const auto profile = ToupTekSource::inspect(api, "TEST-TT-ID");
    {
        Session::Notifications notifications;
        notifications.clipWritten = [&](const auto& result) { outputs.push_back(result); };
        Session session(&context, dir.path(), std::move(notifications));
        session.setSource(std::make_unique<ToupTekSource>(api, profile, profile.current));
        QString error;
        QVERIFY2(session.start(error, Session::Mode::Monitoring), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(session.acquiredFrames() >= 3, 3000);
        QVERIFY(session.requestManualSave());
        QTRY_VERIFY_WITH_TIMEOUT(!session.manualSavePending(), 3000);
        session.stop("test finished");
    }
    QCoreApplication::processEvents();
    QCOMPARE(outputs.size(), size_t{1});
    const auto note = readText(outputs[0].folder + "/event.txt");
    QVERIFY(note.contains("camera_serial: TEST-TT-0001"));
    QVERIFY(note.contains("camera_device_id: TEST-TT-ID"));
    QVERIFY(note.contains("camera frame sequence number"));
    QVERIFY(note.contains("complete: yes"));
    SerReader reader;
    std::string error;
    QVERIFY(reader.open(std::filesystem::path((outputs[0].folder + "/event.ser").toStdWString()), 30, error));
    Frame f;
    f.data.resize(reader.frameBytes());
    QVERIFY(reader.readFrame(0, f, error));
    QCOMPARE(f.bayer, BayerPattern::RGGB);
    for (size_t i = 0; i < static_cast<size_t>(f.width) * f.height; ++i)
        QCOMPARE(static_cast<size_t>(f.data[i]), 80 + i % 31);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    ToupTekTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "touptek_tests.moc"
