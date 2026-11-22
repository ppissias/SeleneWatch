#include "fake_playerone_api.h"
#include "cameras/playerone_source.h"
#include "recording/ser_reader.h"
#include "session/session.h"

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
} // namespace

// Feature-16: the Player One adapter's lifecycle, native frames and evidence, with a test camera.
class PlayerOneTest : public QObject
{
    Q_OBJECT
private slots:
    void openSetsTheRequestedSettingsAndReadsThemBack();
    void invalidSettingsAndFailuresCloseTheCamera();
    void dropsAreCountedAndADisconnectIsReported();
    void liveControlsChangeOnTheSameConnection();
    void sessionKeepsNativeEvidence();
};

void PlayerOneTest::openSetsTheRequestedSettingsAndReadsThemBack()
{
    for (const bool raw16 : {false, true})
    {
        auto api = std::make_shared<FakePlayerOneApi>();
        const auto profile = PlayerOneSource::inspect(api, 0);
        QCOMPARE(profile.camera.serial, std::string("TEST-POA-0001"));
        QCOMPARE(api->opened.load(), api->closed.load());
        auto settings = profile.current;
        settings.bin = 2;
        settings.width = 32;
        settings.height = 16;
        settings.x = 4;
        settings.y = 2;
        settings.format = raw16 ? PixelFormat::Mono16 : PixelFormat::Mono8;
        settings.controls[PlayerOneControl::Gain] = 123;
        Receiver sink;
        PlayerOneSource source(api, profile, settings);
        std::string error;
        QVERIFY2(source.open(error), error.c_str());
        QVERIFY(api->orientationSet);
        const auto actual = source.actualProfile();
        QCOMPARE(actual.current.bin, 2);
        QCOMPARE(actual.current.controls.at(PlayerOneControl::Gain), int64_t{120});
        const auto info = source.streamInfo();
        QCOMPARE(info.serial, std::string("TEST-POA-0001"));
        QCOMPARE(info.binning, std::string("2, pixels averaged"));
        QCOMPARE(info.gain, std::string("120"));
        source.start(sink);
        QTRY_VERIFY_WITH_TIMEOUT(sink.count() >= 3, 3000);
        source.stop();
        QVERIFY(sink.error.empty());
        for (size_t i = 0; i < 3; ++i)
        {
            const auto& f = *sink.frames[i];
            QCOMPARE(f.frameIndex, static_cast<uint64_t>(i));
            QCOMPARE(f.width, 32);
            QCOMPARE(f.significantBits, raw16 ? 16 : 8);
            QCOMPARE(f.bayer, BayerPattern::None);
            QVERIFY(f.testSource);
            QCOMPARE(f.sourceDroppedFrames.value_or(99), uint64_t{0});
            QCOMPARE(static_cast<int>(f.data[0]), raw16 ? 400 & 255 : 100);
        }
        QCOMPARE(api->starts.load(), api->stops.load());
        QCOMPARE(api->opened.load(), api->closed.load());
    }
}

void PlayerOneTest::invalidSettingsAndFailuresCloseTheCamera()
{
    auto api = std::make_shared<FakePlayerOneApi>();
    const auto profile = PlayerOneSource::inspect(api, 0);
    for (int kind = 0; kind < 5; ++kind)
    {
        auto s = profile.current;
        if (kind == 0) s.width = 126;                          // not a multiple of 4
        if (kind == 1) s.height = 63;                          // not a multiple of 2
        if (kind == 2) { s.bin = 2; }                          // full width does not fit the binned sensor
        if (kind == 3) s.controls[PlayerOneControl::Gain] = 501;
        if (kind == 4) s.format = PixelFormat::RGB888;
        PlayerOneSource source(api, profile, s);
        std::string error;
        QVERIFY(!source.open(error));
        QVERIFY(!error.empty());
        QCOMPARE(api->opened.load(), api->closed.load());
    }
    // Colour cameras are binned only once their Bayer layout is verified.
    api->camera.bayer = BayerPattern::RGGB;
    auto colour = PlayerOneSource::inspect(api, 0);
    auto s = colour.current;
    s.bin = 2; s.width = 64; s.height = 32;
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, PlayerOneSource::validateSettings(colour.camera, s));
    for (const auto* operation : {"initialize", "orientation", "controls", "setRoi"})
    {
        api->failOperation = operation;
        PlayerOneSource source(api, profile, profile.current);
        std::string error;
        QVERIFY(!source.open(error));
        QCOMPARE(api->opened.load(), api->closed.load());
    }
    QCOMPARE(api->starts.load(), 0);
}

void PlayerOneTest::dropsAreCountedAndADisconnectIsReported()
{
    auto api = std::make_shared<FakePlayerOneApi>();
    api->dropCounts = {0, 0, 3, 3};
    const auto profile = PlayerOneSource::inspect(api, 0);
    {
        Receiver sink;
        PlayerOneSource source(api, profile, profile.current);
        std::string error;
        QVERIFY(source.open(error));
        source.start(sink);
        QTRY_VERIFY_WITH_TIMEOUT(sink.count() >= 4, 3000);
        source.stop();
        QCOMPARE(sink.frames[1]->frameIndex, uint64_t{1});
        QCOMPARE(sink.frames[2]->frameIndex, uint64_t{5});
        QCOMPARE(sink.frames[3]->frameIndex, uint64_t{6});
        QCOMPARE(sink.frames[3]->sourceDroppedFrames.value_or(0), uint64_t{3});
    }
    api->dropCounts.clear();
    api->delivered = 0;
    api->disconnectAt = 2;
    Receiver sink;
    PlayerOneSource source(api, profile, profile.current);
    std::string error;
    QVERIFY(source.open(error));
    source.start(sink);
    QTRY_VERIFY_WITH_TIMEOUT(sink.failed(), 3000);
    source.stop();
    QCOMPARE(sink.count(), size_t{2});
    QVERIFY(sink.error.find("disconnected") != std::string::npos);
    QCOMPARE(api->opened.load(), api->closed.load());
}

void PlayerOneTest::liveControlsChangeOnTheSameConnection()
{
    auto api = std::make_shared<FakePlayerOneApi>();
    const auto profile = PlayerOneSource::inspect(api, 0);
    Receiver sink;
    PlayerOneSource source(api, profile, profile.current);
    std::string error;
    QVERIFY(source.open(error));
    const int opens = api->opened.load();
    source.start(sink);
    QTRY_VERIFY_WITH_TIMEOUT(sink.count() >= 2, 3000);
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, source.requestLiveControls({{PlayerOneControl::Gain, 501}}));
    source.requestLiveControls({{PlayerOneControl::Gain, 250}, {PlayerOneControl::Exposure, 2000}});
    QTRY_COMPARE_WITH_TIMEOUT(source.actualProfile().current.controls.at(PlayerOneControl::Exposure), int64_t{2000}, 3000);
    QCOMPARE(source.actualProfile().current.controls.at(PlayerOneControl::Gain), int64_t{250});
    QCOMPARE(api->opened.load(), opens);
    QCOMPARE(api->starts.load(), 1);
    source.stop();
    QVERIFY(!sink.failed());
}

void PlayerOneTest::sessionKeepsNativeEvidence()
{
    QTemporaryDir dir;
    QObject context;
    std::vector<ClipWriteResult> outputs;
    auto api = std::make_shared<FakePlayerOneApi>();
    const auto profile = PlayerOneSource::inspect(api, 0);
    auto settings = profile.current;
    settings.format = PixelFormat::Mono16;
    {
        Session::Notifications notifications;
        notifications.clipWritten = [&](const auto& result) { outputs.push_back(result); };
        Session session(&context, dir.path(), std::move(notifications));
        session.setSource(std::make_unique<PlayerOneSource>(api, profile, settings));
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
    QVERIFY(note.contains("camera_serial: TEST-POA-0001"));
    QVERIFY(note.contains("camera_sdk_version: TEST POA SDK 3"));
    QVERIFY(note.contains("hardware qualification pending"));
    QVERIFY(note.contains("complete: yes"));
    SerReader reader;
    std::string error;
    QVERIFY(reader.open(std::filesystem::path((outputs[0].folder + "/event.ser").toStdWString()), 30, error));
    Frame f;
    f.data.resize(reader.frameBytes());
    QVERIFY(reader.readFrame(0, f, error));
    QCOMPARE(f.significantBits, 16);
    for (size_t i = 0; i < static_cast<size_t>(f.width) * f.height; ++i)
        QCOMPARE(static_cast<size_t>(f.data[2 * i] | (f.data[2 * i + 1] << 8)), 400 + i % 13);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    PlayerOneTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "playerone_tests.moc"
