#include "tests/fake_zwo_api.h"
#include "cameras/zwo_source.h"
#include "session.h"
#include "recording/output_paths.h"
#include "recording/ser_reader.h"
#include "recording/ser_writer.h"
#include "tests/detector_observation.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>
#include <condition_variable>

namespace {
// Collects ASI frames, failures and live-setting notices from the source worker.
class Receiver : public FrameSourceListener
{
public:
    void onFrame(std::shared_ptr<Frame> f) override { std::lock_guard lock(mutex); frames.push_back(std::move(f)); }
    void onDiscontinuity() override {}
    void onSettingsChanged(const StreamInfo& info,std::chrono::duration<double> duration) override
    {
        std::lock_guard lock(mutex); settings=info; settling=duration.count(); ++settingsChanges;
    }
    void onEnded() override {}
    void onFailed(const std::string& e) override { std::lock_guard lock(mutex); failure=e; }
    size_t count() { std::lock_guard lock(mutex); return frames.size(); }
    bool failed() { std::lock_guard lock(mutex); return !failure.empty(); }
    int changes() { std::lock_guard lock(mutex); return settingsChanges; }
    std::vector<FramePtr> frames; std::string failure; std::mutex mutex;
    StreamInfo settings; int settingsChanges{0}; double settling{0};
};
QString text(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly|QIODevice::Text)?QString::fromUtf8(f.readAll()):QString{}; }
}

// Exercises ASI capture, live readbacks and loss handling using a scripted SDK.
class ZwoTest : public QObject
{
    Q_OBJECT
private slots:
    void monitoringCanStopDuringALongExposureWithoutRestartingAcquisition()
    {
        QTemporaryDir dir; QObject context;
        auto api=std::make_shared<FakeZwoApi>(); const auto p=ZwoSource::inspect(api,0);
        Session session(&context,dir.path(),{}); session.setSource(std::make_unique<ZwoSource>(api,p,p.current));
        QString error; QVERIFY(session.start(error,Session::Mode::Monitoring));
        QTRY_VERIFY_WITH_TIMEOUT(session.acquiredFrames()>=2,3000);
        api->timeouts=true;
        QVERIFY(session.requestMonitoring(false));
        QTRY_COMPARE_WITH_TIMEOUT(session.mode(),Session::Mode::Preview,1000);
        QVERIFY(!session.modeChangePending()); QVERIFY(session.isRunning());
        QCOMPARE(api->starts.load(),1); QCOMPARE(api->stops.load(),0);
        session.stop(); QCOMPARE(api->closed.load(),api->opened.load());
    }
    void everySupportedNativeControlChangesOnTheWorkerWithoutRestarting()
    {
        auto api=std::make_shared<FakeZwoApi>();
        api->caps.push_back({ZwoControl::HighSpeed,"High speed",0,1,0,true,false});
        const auto p=ZwoSource::inspect(api,0); ZwoSource source(api,p,p.current);
        std::string error; QVERIFY(source.open(error)); Receiver sink;
        source.start(sink); QTRY_VERIFY_WITH_TIMEOUT(sink.count()>=2,3000);
        auto controls=p.current.controls;
        controls[ZwoControl::Exposure]=2000; controls[ZwoControl::Gain]=13;
        controls[ZwoControl::Offset]=12; controls[ZwoControl::Bandwidth]=60; controls[ZwoControl::HighSpeed]=1;
        source.requestLiveControls(controls);
        QTRY_COMPARE_WITH_TIMEOUT(sink.changes(),1,3000);
        const auto actual=source.actualProfile();
        QCOMPARE(actual.current.controls.at(ZwoControl::Exposure),int64_t{2000});
        QCOMPARE(actual.current.controls.at(ZwoControl::Gain),int64_t{12});
        QCOMPARE(actual.current.controls.at(ZwoControl::Offset),int64_t{12});
        QCOMPARE(actual.current.controls.at(ZwoControl::Bandwidth),int64_t{60});
        QCOMPARE(actual.current.controls.at(ZwoControl::HighSpeed),int64_t{1});
        QCOMPARE(source.streamInfo().gain,std::string("12"));
        auto bad=controls; bad[ZwoControl::Flip]=1;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,source.requestLiveControls(bad));
        bad=controls; bad[ZwoControl::Gain]=1000;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,source.requestLiveControls(bad));
        QCOMPARE(api->starts.load(),1); QCOMPARE(api->stops.load(),0);
        QCOMPARE(api->opened.load(),2); // One inspection and one acquisition open.
        source.stop(); QVERIFY(sink.failure.empty());
        QCOMPARE(api->liveControlThread,api->readThread);
        QVERIFY(api->liveControlThread!=std::this_thread::get_id());
        QCOMPARE(sink.settings.exposure,std::string("2000 us")); QVERIFY(sink.settling>=1);
        QCOMPARE(api->closed.load(),api->opened.load());
    }
    void aCameraChangeOutsidePreviewStopsBeforeMislabelledEvidenceCanArrive()
    {
        QTemporaryDir dir; QObject context; std::vector<ClipWriteResult> results;
        auto api=std::make_shared<FakeZwoApi>(); const auto p=ZwoSource::inspect(api,0);
        {
            Session::Notifications notices; notices.clipWritten=[&](const auto& r){results.push_back(r);};
            Session session(&context,dir.path(),std::move(notices));
            auto source=std::make_unique<ZwoSource>(api,p,p.current); auto* camera=source.get();
            session.setSource(std::move(source)); QString error;
            QVERIFY(session.start(error,Session::Mode::Monitoring));
            QTRY_VERIFY_WITH_TIMEOUT(session.acquiredFrames()>=2,3000);
            QVERIFY(session.requestManualSave()); QTRY_VERIFY_WITH_TIMEOUT(!session.manualSavePending(),3000);
            auto changed=p.current.controls; changed[ZwoControl::Gain]=14;
            camera->requestLiveControls(changed);
            QTRY_VERIFY_WITH_TIMEOUT(session.sourceFinished(),3000);
            QCOMPARE(session.streamInfo().gain,std::string("10")); session.stop();
        }
        QCoreApplication::processEvents(); QCOMPARE(results.size(),size_t{1}); QVERIFY(results[0].notSaved.isEmpty());
        const auto note=text(results[0].folder+"/event.txt");
        QVERIFY(note.contains("gain: 10\n")); QVERIFY(!note.contains("gain: 14\n"));
        QVERIFY(note.contains("source failure: camera settings changed outside Preview"));
        QCOMPARE(api->closed.load(),api->opened.load());
    }
    void replayKeepsTheDeclaredWordInterpretationAndDecisions()
    {
        QTemporaryDir dir; QFile output(dir.path()+"/word-range.ser"); QVERIFY(output.open(QIODevice::ReadWrite));
        Frame baseline; baseline.width=64; baseline.height=32; baseline.format=PixelFormat::Mono16;
        baseline.significantBits=16; baseline.testSource=true; baseline.data.resize(64*32*2);
        for (size_t i=0;i<baseline.data.size();i+=2) { baseline.data[i]=0; baseline.data[i+1]=25; }
        Frame flash=baseline; flash.frameIndex=1; flash.timestamp+=std::chrono::milliseconds{33};
        for (int y=10;y<13;++y) for (int x=10;x<13;++x) flash.data[(y*64+x)*2]=16;
        SerWriter writer(output); QString error;
        QVERIFY(writer.start(baseline,"TEST camera words",error)); QVERIFY(writer.append(baseline,error));
        QVERIFY(writer.append(flash,error)); QVERIFY(writer.finish(error)); output.close();
        FlashDetector live,replay;
        QVERIFY(!observe(live,baseline)); QVERIFY(observe(live,flash));
        SerReader reader; std::string reason; QVERIFY(reader.open(std::filesystem::path(output.fileName().toStdWString()),30,reason));
        for (int i=0;i<2;++i)
        {
            Frame f; f.data.resize(reader.frameBytes()); QVERIFY(reader.readFrame(i,f,reason));
            QCOMPARE(f.significantBits,16); QCOMPARE(f.alignment,BitAlignment::Low);
            QCOMPARE(observe(replay,f),i==1);
        }
        QCOMPARE(replay.summary().peakSigma,live.summary().peakSigma);
        QVERIFY(reader.describe().sampleLayout.find("explicit")!=std::string::npos);
    }
    void absenceAndPartialOpenFailuresAreUsable()
    {
        auto api=std::make_shared<FakeZwoApi>(); api->connected=false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,static_cast<void>(ZwoSource::inspect(api,0)));
        api->connected=true; auto p=ZwoSource::inspect(api,0);
        for (const auto operation:{"initialize","disableDark","setControl","setRoi","roi","controls","serial"})
        {
            auto failing=std::make_shared<FakeZwoApi>(); failing->failOperation=operation;
            ZwoSource source(failing,p,p.current); std::string error;
            QVERIFY(!source.open(error)); QVERIFY(!error.empty()); QCOMPARE(failing->closed.load(),failing->opened.load());
        }
    }
    void invalidLayoutsNeverReachVideoCapture()
    {
        auto api=std::make_shared<FakeZwoApi>(); const auto p=ZwoSource::inspect(api,0);
        for (int kind=0;kind<6;++kind)
        {
            auto s=p.current;
            if (kind==0) s.width=127;
            if (kind==1) s.height=63;
            if (kind==2) s.bin=0;
            if (kind==3) s.x=1;
            if (kind==4) s.format=PixelFormat::RGB888;
            if (kind==5) s.width=120; // 120*64 is not a multiple of 1024.
            ZwoSource source(api,p,s); std::string error; QVERIFY(!source.open(error)); QVERIFY(!error.empty());
        }
        QCOMPARE(api->starts.load(),0);
    }
    void realReadbacksAndNativeWordsDetermineEvidence()
    {
        auto api=std::make_shared<FakeZwoApi>(); auto p=ZwoSource::inspect(api,0); auto requested=p.current;
        api->clampWidth=64; requested.format=PixelFormat::Mono16; requested.controls[ZwoControl::Gain]=13;
        ZwoSource source(api,p,requested); MemoryBudget budget(1024*1024); source.setMemoryBudget(budget);
        std::string error; QVERIFY2(source.open(error),error.c_str());
        QCOMPARE(source.actualProfile().current.width,64);
        QCOMPARE(source.streamInfo().gain,std::string("12")); QCOMPARE(source.streamInfo().sensorBits,std::string("12"));
        QCOMPARE(source.streamInfo().sdkVersion,std::string("TEST SDK 1"));
        Receiver sink; source.start(sink); QTRY_VERIFY_WITH_TIMEOUT(sink.count()>=3,3000); source.stop();
        QVERIFY(sink.failure.empty());
        const auto& f=*sink.frames.front(); QCOMPARE(f.width,64); QCOMPARE(f.significantBits,16);
        QCOMPARE(f.data[0],uint8_t{0}); QCOMPARE(f.data[1],uint8_t{25});
        QCOMPARE(f.alignment,BitAlignment::Low); QVERIFY(f.testSource);
        QVERIFY(f.timestamp.time_since_epoch().count()>0); QVERIFY(f.steadyTimestamp.time_since_epoch().count()>0);
        QVERIFY(f.sourceDroppedFrames==uint64_t{0}); QVERIFY(budget.usage().used>0);
        QCOMPARE(api->starts.load(),1); QCOMPARE(api->stops.load(),1); QCOMPARE(api->opened.load(),api->closed.load());
    }
    void bayerOriginIsTrackedAndColourBinningIsRefused()
    {
        auto api=std::make_shared<FakeZwoApi>(); api->camera.bayer=BayerPattern::RGGB;
        const auto p=ZwoSource::inspect(api,0); auto s=p.current;
        s.width=64; s.height=32; s.x=s.y=1;
        ZwoSource source(api,p,s); std::string error; QVERIFY(source.open(error));
        Receiver sink; source.start(sink); QTRY_VERIFY_WITH_TIMEOUT(sink.count()>=1,3000); source.stop();
        QCOMPARE(sink.frames[0]->bayer,BayerPattern::BGGR);
        s.bin=2; s.x=s.y=0; ZwoSource binned(api,p,s); QVERIFY(!binned.open(error));
    }
    void timeoutsStopPromptlyAndDisconnectsCloseTheDevice()
    {
        auto api=std::make_shared<FakeZwoApi>(); auto p=ZwoSource::inspect(api,0); api->timeouts=true;
        ZwoSource source(api,p,p.current); std::string error; QVERIFY(source.open(error)); Receiver sink;
        source.start(sink); QTRY_VERIFY_WITH_TIMEOUT(api->reads>=3,3000);
        const auto start=std::chrono::steady_clock::now(); source.stop();
        QVERIFY(std::chrono::steady_clock::now()-start<std::chrono::milliseconds{250}); QVERIFY(sink.failure.empty());
        api->timeouts=false; api->delivered=0; api->disconnectAt=2;
        QVERIFY(source.open(error)); source.start(sink); QTRY_VERIFY_WITH_TIMEOUT(sink.failed(),3000); source.stop();
        QCOMPARE(sink.count(),size_t{2}); QVERIFY(sink.failure.find("disconnected")!=std::string::npos);
        QCOMPARE(api->closed.load(),api->opened.load());
    }
    void dropTelemetryCreatesGapsAndFailureStillPreservesTheReceivedFrame()
    {
        for (int mode:{0,1,2})
        {
            auto api=std::make_shared<FakeZwoApi>(); api->dropCounts={0,0,3,3};
            if (mode==2) api->dropCounts={0,3,0}; // An unexpected SDK counter reset.
            auto p=ZwoSource::inspect(api,0); ZwoSource source(api,p,p.current); std::string error; QVERIFY(source.open(error));
            if (mode==1) api->telemetryFailureAt=3; else api->disconnectAt=4;
            Receiver sink; source.start(sink); QTRY_VERIFY_WITH_TIMEOUT(sink.failed(),3000); source.stop();
            QCOMPARE(sink.count(),mode?size_t{3}:size_t{4});
            if (mode) QVERIFY(!sink.frames[2]->sourceDroppedFrames);
            else { QCOMPARE(sink.frames[2]->frameIndex,uint64_t{5}); QVERIFY(sink.frames[2]->sourceDroppedFrames==uint64_t{3}); }
            if (mode==2) QVERIFY(sink.failure.find("backwards")!=std::string::npos);
        }
    }
    void sessionSavesManualNativeEvidenceAndSdkMetadata()
    {
        QTemporaryDir dir; QObject context; std::vector<ClipWriteResult> results;
        auto api=std::make_shared<FakeZwoApi>(); auto p=ZwoSource::inspect(api,0); auto settings=p.current; settings.format=PixelFormat::Mono16;
        {
            Session::Notifications notices; notices.clipWritten=[&](const auto& r){results.push_back(r);};
            Session session(&context,dir.path(),std::move(notices));
            session.setSource(std::make_unique<ZwoSource>(api,p,settings));
            QString error; QVERIFY2(session.start(error, Session::Mode::Monitoring),qPrintable(error));
            QTRY_VERIFY_WITH_TIMEOUT(session.acquiredFrames()>=2,3000); QVERIFY(session.requestManualSave());
            QTRY_VERIFY_WITH_TIMEOUT(!session.manualSavePending(),3000);
            QVERIFY(session.droppedFrames()==uint64_t{0}); session.stop("camera settings changed");
        }
        QCoreApplication::processEvents(); QCOMPARE(results.size(),size_t{1}); QVERIFY(results[0].notSaved.isEmpty());
        const auto note=text(results[0].folder+"/event.txt");
        QVERIFY(note.contains("camera_sdk_version: TEST SDK 1")); QVERIFY(note.contains("camera_serial: TEST-ASI-0001"));
        QVERIFY(note.contains("sensor_adc_bits: 12")); QVERIFY(note.contains("packing unverified"));
        QVERIFY(note.contains("kind=manual")); QVERIFY(note.contains("sdk_dropped_frames=0"));
        QVERIFY(note.contains("collection_end: camera settings changed")); QVERIFY(note.contains("contains_test_signal: yes"));
        SerReader reader; std::string error; QVERIFY(reader.open(std::filesystem::path((results[0].folder+"/event.ser").toStdWString()),30,error));
        Frame f; f.data.resize(reader.frameBytes()); QVERIFY(reader.readFrame(0,f,error));
        QCOMPARE(f.significantBits,16); QCOMPARE(f.alignment,BitAlignment::Low);
        for (size_t i=0;i<static_cast<size_t>(f.width)*f.height;++i)
            QCOMPARE(static_cast<int>(f.data[2*i]|(f.data[2*i+1]<<8)),static_cast<int>((400+i%13)<<4));
        QVERIFY(text(eventsFolder(dir.path(), Target::Moon)+"/candidates.txt").contains("sdk=\"TEST SDK 1\""));
    }
};
int main(int argc,char** argv) { QGuiApplication app(argc,argv); ZwoTest tests; return QTest::qExec(&tests,argc,argv); }
#include "zwo_tests.moc"
