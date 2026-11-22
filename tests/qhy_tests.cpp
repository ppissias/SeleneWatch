#include "fake_qhy_api.h"
#include "cameras/qhy_source.h"
#include "session/session.h"
#include "recording/ser_reader.h"
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>
#include <mutex>

namespace {
// Collects frames and failures from the QHY worker. The test polls through locked accessors.
class Receiver : public FrameSourceListener
{
public:
    void onFrame(std::shared_ptr<Frame> f) override { std::lock_guard lock(mutex_); frames.push_back(std::move(f)); }
    void onDiscontinuity() override {}
    void onEnded() override {}
    void onFailed(const std::string& reason) override { std::lock_guard lock(mutex_); error=reason; }
    size_t count() { std::lock_guard lock(mutex_); return frames.size(); }
    bool failed() { std::lock_guard lock(mutex_); return !error.empty(); }
    std::vector<FramePtr> frames; std::string error;
private:
    std::mutex mutex_;
};
QString readText(const QString& path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly)?QString::fromUtf8(file.readAll()):QString{};
}
}
// Exercises QHY lifecycle, native frame validation and evidence using a fake SDK.
class QhyTest : public QObject
{
    Q_OBJECT
private slots:
    void failurePathsCloseAndInvalidSettingsDoNotStartCapture()
    {
        auto api=std::make_shared<FakeQhyApi>(); const auto p=QhySource::inspect(api,"TEST-QHY-ID");
        api->connected=false; QVERIFY_THROWS_EXCEPTION(std::runtime_error,QhySource::inspect(api,"TEST-QHY-ID"));
        api->connected=true;
        for (const auto operation:{"initialize","profile","configure","buffer"})
        {
            api->failOperation=operation; QhySource source(api,p,p.current); std::string error;
            QVERIFY(!source.open(error)); QVERIFY(!error.empty()); QCOMPARE(api->opened.load(),api->closed.load());
        }
        api->failOperation.clear();
        for (int kind=0;kind<4;++kind)
        {
            auto s=p.current;
            if (kind==0) s.bin=2;
            if (kind==1) s.width=129;
            if (kind==2) s.format=PixelFormat::RGB888;
            if (kind==3) s.controls[QhyControl::Gain]=101;
            QhySource source(api,p,s); std::string error; QVERIFY(!source.open(error));
            QCOMPARE(api->opened.load(),api->closed.load());
        }
        api->shortBuffer=true; QhySource shortBuffer(api,p,p.current); std::string error;
        QVERIFY(!shortBuffer.open(error)); QCOMPARE(api->starts.load(),0);
    }
    void nativeWordsReadbacksBayerAndChargedSdkTail()
    {
        for (bool raw16:{false,true})
        {
            auto api=std::make_shared<FakeQhyApi>(); auto p=QhySource::inspect(api,"TEST-QHY-ID");
            auto settings=p.current; settings.format=raw16?PixelFormat::Mono16:PixelFormat::Mono8;
            settings.width=64; settings.height=32; settings.x=settings.y=1; settings.controls[QhyControl::Gain]=13.37;
            api->frameLimit=5; MemoryBudget budget(1024*1024);
            Receiver sink; QhySource source(api,p,settings); source.setMemoryBudget(budget); std::string error;
            QVERIFY2(source.open(error),error.c_str()); QCOMPARE(source.actualProfile().current.controls.at(QhyControl::Gain),13.25);
            QVERIFY(source.streamInfo().serial.empty()); QCOMPARE(source.streamInfo().deviceId,std::string("TEST-QHY-ID"));
            source.start(sink); QTRY_VERIFY_WITH_TIMEOUT(sink.count()==5,3000); source.stop(); QVERIFY(sink.error.empty());
            QCOMPARE(source.streamInfo().gain,std::string("13.25")); QVERIFY(source.streamInfo().frameIndex.find("unknown")!=std::string::npos);
            for (size_t index=0;index<sink.frames.size();++index)
            {
                const auto& f=*sink.frames[index]; QCOMPARE(f.frameIndex,static_cast<uint64_t>(index));
                QVERIFY(!f.sourceDroppedFrames); QVERIFY(f.testSource); QCOMPARE(f.bayer,BayerPattern::BGGR);
                QCOMPARE(f.significantBits,raw16?16:8); QCOMPARE(f.alignment,BitAlignment::Low);
                QCOMPARE(f.data.size(),frameBytes(64,32,f.format)); QVERIFY(f.data.capacity()>f.data.size());
                QCOMPARE(static_cast<int>(f.data[0]),raw16?64:80);
            }
            QVERIFY(budget.usage().used>=5*api->bufferBytes()); QCOMPARE(budget.usage().refused,size_t{0});
            QCOMPARE(api->opened.load(),api->closed.load()); QCOMPARE(api->starts.load(),api->stops.load());
        }
    }
    void notReadyDisconnectAndUnexpectedChannelsAreExplicit()
    {
        for (int mode=0;mode<4;++mode)
        {
            auto api=std::make_shared<FakeQhyApi>(); auto p=QhySource::inspect(api,"TEST-QHY-ID");
            if (mode==0) api->notReady=true;
            if (mode==1) api->disconnectAt=3;
            if (mode==2) api->badLayout=true;
            if (mode==3) api->failOperation="start";
            Receiver sink; QhySource source(api,p,p.current); std::string error; QVERIFY(source.open(error)); source.start(sink);
            if (mode==0) QTRY_VERIFY_WITH_TIMEOUT(api->reads>=3,3000);
            else QTRY_VERIFY_WITH_TIMEOUT(sink.failed(),3000);
            const auto before=std::chrono::steady_clock::now(); source.stop();
            QVERIFY(std::chrono::steady_clock::now()-before<std::chrono::milliseconds{250});
            QCOMPARE(sink.count(),mode==1?size_t{3}:size_t{0}); QCOMPARE(api->opened.load(),api->closed.load());
        }
    }
    void sessionKeepsNativeManualEvidenceAndUnknownLossSemantics()
    {
        QTemporaryDir dir; QObject context; std::vector<ClipWriteResult> outputs;
        auto api=std::make_shared<FakeQhyApi>(); auto p=QhySource::inspect(api,"TEST-QHY-ID"); auto settings=p.current;
        settings.format=PixelFormat::Mono16; api->frameLimit=30;
        {
            Session::Notifications notifications; notifications.clipWritten=[&](const auto& result){outputs.push_back(result);};
            Session session(&context,dir.path(),std::move(notifications));
            session.setSource(std::make_unique<QhySource>(api,p,settings));
            QString error;
            QVERIFY2(session.start(error,Session::Mode::Monitoring),qPrintable(error));
            QTRY_VERIFY_WITH_TIMEOUT(session.acquiredFrames()>=3,3000); QVERIFY(session.requestManualSave());
            QTRY_VERIFY_WITH_TIMEOUT(!session.manualSavePending(),3000); QVERIFY(!session.droppedFrames());
            session.stop("camera settings changed");
        }
        QCoreApplication::processEvents(); QCOMPARE(outputs.size(),size_t{1}); QVERIFY(outputs[0].notSaved.isEmpty());
        const auto note=readText(outputs[0].folder+"/event.txt");
        QVERIFY(note.contains("camera_device_id: TEST-QHY-ID")); QVERIFY(note.contains("camera_serial: unknown"));
        QVERIFY(note.contains("sdk_dropped_frames=unknown")); QVERIFY(note.contains("packing unverified"));
        QVERIFY(note.contains("kind=manual")); QVERIFY(note.contains("complete: yes"));
        SerReader reader; std::string error;
        QVERIFY(reader.open(std::filesystem::path((outputs[0].folder+"/event.ser").toStdWString()),30,error));
        Frame f; f.data.resize(reader.frameBytes()); QVERIFY(reader.readFrame(0,f,error));
        QCOMPARE(f.significantBits,16); QCOMPARE(f.bayer,BayerPattern::RGGB);
        for (size_t i=0;i<static_cast<size_t>(f.width)*f.height;++i)
            QCOMPARE(static_cast<size_t>(f.data[2*i]|(f.data[2*i+1]<<8)),8000+i%31);
    }
    void longExposureChangeRestartsVideoOnTheSameConnection()
    {
        auto api=std::make_shared<FakeQhyApi>();
        const auto p=QhySource::inspect(api,"TEST-QHY-ID");
        Receiver sink;
        QhySource source(api,p,p.current);
        std::string error;
        QVERIFY2(source.open(error),error.c_str());
        const int opens=api->opened.load();
        source.start(sink);
        QTRY_VERIFY_WITH_TIMEOUT(sink.count()>=2,3000);
        api->notReady=true;
        source.requestLiveControls({{QhyControl::Exposure,1'000'000}});
        QTRY_COMPARE_WITH_TIMEOUT(source.actualProfile().current.controls.at(QhyControl::Exposure),1'000'000.0,3000);
        QCOMPARE(api->opened.load(),opens);
        QCOMPARE(api->starts.load(),2);
        QCOMPARE(api->stops.load(),1);
        source.requestLiveControls({{QhyControl::Exposure,2000}});
        QTRY_COMPARE_WITH_TIMEOUT(source.actualProfile().current.controls.at(QhyControl::Exposure),2000.0,3000);
        QCOMPARE(api->opened.load(),opens);
        QCOMPARE(api->starts.load(),3);
        api->notReady=false;
        const auto count=sink.count();
        QTRY_VERIFY_WITH_TIMEOUT(sink.count()>count,3000);
        source.stop();
        QVERIFY(!sink.failed());
        QCOMPARE(api->starts.load(),api->stops.load());
        QCOMPARE(api->opened.load(),api->closed.load());
    }
};
int main(int argc,char** argv) { QGuiApplication app(argc,argv); QhyTest test; return QTest::qExec(&test,argc,argv); }
#include "qhy_tests.moc"
