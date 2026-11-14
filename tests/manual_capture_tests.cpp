#include "event_capture.h"
#include "recording/event_writer.h"
#include "recording/ser_reader.h"
#include "session.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

#include <condition_variable>
#include <algorithm>
#include <deque>
#include <future>
#include <thread>
#include <stdexcept>

namespace {
FramePtr sample(uint64_t n, PixelFormat format=PixelFormat::Mono8, BitAlignment alignment=BitAlignment::Low)
{
    auto f=std::make_shared<Frame>();
    f->width=64; f->height=48; f->frameIndex=n;
    f->timestamp=std::chrono::system_clock::time_point{}+std::chrono::milliseconds{static_cast<int64_t>(n)*100};
    f->format=format; f->alignment=alignment; f->significantBits=format==PixelFormat::Mono16?12:8;
    f->bayer=format==PixelFormat::Mono16?BayerPattern::RGGB:BayerPattern::None;
    f->testSource=true;
    for (int i=0;i<64*48;++i)
    {
        const int value=100+i%20;
        if (format==PixelFormat::Mono16)
        {
            const int word=value*16 << (alignment==BitAlignment::High?4:0);
            f->data.push_back(static_cast<uint8_t>(word&255)); f->data.push_back(static_cast<uint8_t>(word>>8));
        }
        else
        {
            f->data.push_back(static_cast<uint8_t>(value));
            if (format==PixelFormat::RGB888) { f->data.push_back(value+1); f->data.push_back(value+2); }
        }
    }
    return f;
}
FramePtr flash(uint64_t n)
{
    auto f=std::make_shared<Frame>(*sample(n));
    for (int y=10;y<13;++y) for (int x=10;x<13;++x) f->data[static_cast<size_t>(y)*64+x]+=40;
    return f;
}
QString read(const QString& path)
{
    QFile f(path); return f.open(QIODevice::ReadOnly)?QString::fromUtf8(f.readAll()):QString{};
}

// A real worker, advanced explicitly by each test. This makes pending requests
// at Stop/EOF/failure/loop boundaries deterministic without timing sleeps.
class ControlledSource : public FrameSource
{
public:
    ~ControlledSource() override { stop(); }
    bool open(std::string&) override { return true; }
    StreamInfo streamInfo() const override { StreamInfo info; info.source="TEST: controlled manual capture"; return info; }
    void start(FrameSourceListener& listener) override
    {
        listener_=&listener;
        worker_=std::jthread([this](std::stop_token stop) {
            while (true)
            {
                std::packaged_task<void()> task;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock,[&]{ return stop.stop_requested() || !commands_.empty(); });
                    if (commands_.empty()) return;
                    task=std::move(commands_.front()); commands_.pop_front();
                }
                task();
            }
        });
    }
    void stop() override { if (worker_.joinable()) { worker_.request_stop(); ready_.notify_all(); worker_.join(); } }
    void frame(uint64_t n, bool bright=false)
    {
        command([this,n,bright] {
            auto f=pool_.acquire(64*48);
            f->width=64; f->height=48; f->format=PixelFormat::Mono8; f->significantBits=8;
            f->alignment=BitAlignment::Low; f->bayer=BayerPattern::None;
            f->frameIndex=n;
            f->timestamp=std::chrono::system_clock::time_point{}+std::chrono::milliseconds{static_cast<int64_t>(n)*100};
            f->steadyTimestamp={}; f->testSource=true; f->syntheticFlash=false; f->blip.reset();
            std::fill(f->data.begin(),f->data.end(),uint8_t{100});
            if (bright)
                for (int y=10;y<13;++y) for (int x=10;x<13;++x) f->data[static_cast<size_t>(y)*64+x]+=40;
            listener_->onFrame(std::move(f));
        });
    }
    void settingsChanged()
    {
        command([this] { auto info=streamInfo(); info.gain="42"; listener_->onSettingsChanged(info,std::chrono::duration<double>{0}); });
    }
    void boundary(int kind) { command([this,kind] {
        if (kind==1) listener_->onEnded();
        else if (kind==2) listener_->onFailed("controlled failure");
        else listener_->onDiscontinuity();
    }); }
private:
    void command(std::function<void()> call)
    {
        std::packaged_task<void()> task(std::move(call)); auto done=task.get_future();
        { std::lock_guard lock(mutex_); commands_.push_back(std::move(task)); }
        ready_.notify_one();
        if (done.wait_for(std::chrono::seconds{5})!=std::future_status::ready) throw std::runtime_error("controlled source timed out");
        done.get();
    }
    FrameSourceListener* listener_{nullptr};
    std::mutex mutex_; std::condition_variable ready_;
    std::deque<std::packaged_task<void()>> commands_;
    std::jthread worker_;
};
}

class ManualCaptureTest : public QObject
{
    Q_OBJECT
private slots:
    void previewNeverWritesAndArmingReconfirmsAnOngoingPatch()
    {
        QTemporaryDir dir; QObject context;
        int candidates=0;
        std::vector<ClipWriteResult> results;
        {
            Session::Notifications notices;
            notices.candidateStarted=[&]{++candidates;};
            notices.clipWritten=[&](const auto& r){results.push_back(r);};
            Session session(&context,dir.path(),std::move(notices));
            auto source=std::make_unique<ControlledSource>(); auto* controlled=source.get();
            session.setSource(std::move(source));
            QString error; QVERIFY(session.start(error));
            QCOMPARE(session.mode(),Session::Mode::Preview);
            controlled->frame(0); controlled->frame(1,true); controlled->frame(2,true);
            QCoreApplication::processEvents();
            const auto packet=session.takePreview(); QVERIFY(packet.detection);
            QVERIFY(packet.detection->summary.triggered); QVERIFY(!packet.detection->monitoring);
            QCOMPARE(candidates,0); QVERIFY(!session.requestManualSave());
            QCOMPARE(QDir(dir.path()).entryList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot).size(),0);
            QVERIFY(session.requestMonitoring(true)); QVERIFY(session.modeChangePending());
            controlled->frame(3,true); QCoreApplication::processEvents();
            QCOMPARE(session.mode(),Session::Mode::Monitoring); QCOMPARE(candidates,1);
            QVERIFY(session.requestMonitoring(false));
            controlled->frame(4,true); QCoreApplication::processEvents();
            QCOMPARE(session.mode(),Session::Mode::Preview); QVERIFY(session.isRunning());
            QCOMPARE(session.acquiredFrames(),uint64_t{5});
            controlled->frame(5); controlled->frame(6,true);
            QCoreApplication::processEvents(); QCOMPARE(candidates,1);
            session.stop();
        }
        QCoreApplication::processEvents(); QCOMPARE(results.size(),size_t{1});
        QVERIFY(results[0].notSaved.isEmpty());
        const auto note=read(results[0].folder+"/event.txt");
        QVERIFY(note.contains("collection_end: monitoring stopped"));
        QVERIFY(note.contains("onset_frame_index=3"));
        SerReader reader; std::string error;
        QVERIFY(reader.open(std::filesystem::path((results[0].folder+"/event.ser").toStdWString()),10,error));
        QCOMPARE(reader.frameCount(),4); // Preview history 0–2 and the armed frame 3, never disarm frame 4.
    }
    void armingRequiresFreshTwoFramePersistenceWithoutLosingTheReference()
    {
        DetectionSettings settings; settings.oneFrame=false;
        std::vector<EventClip> clips;
        EventCapture capture({5,3,10},[]{},[&](EventClip c){clips.push_back(std::move(c));},settings);
        capture.setMonitoring(false);
        capture.onFrame(sample(0)); capture.onFrame(flash(1)); capture.onFrame(flash(2));
        QVERIFY(capture.detectionSummary().firing); QVERIFY(!capture.saveNow({}));
        capture.setMonitoring(true); capture.onFrame(flash(3));
        QVERIFY(capture.detectionSummary().passed); QVERIFY(!capture.detectionSummary().firing);
        capture.onFrame(flash(4)); QVERIFY(capture.detectionSummary().triggered);
        capture.setMonitoring(false);
        QCOMPARE(clips.size(),size_t{1}); QCOMPARE(clips[0].triggers[0].onsetFrameIndex,uint64_t{3});
        QCOMPARE(clips[0].triggers[0].triggerFrameIndex,uint64_t{4}); QCOMPARE(clips[0].frames.size(),size_t{5});
    }
    void liveCameraReadbacksInvalidatePreviewHistoryAndRequireAReopen()
    {
        QTemporaryDir dir; QObject context; Session session(&context,dir.path(),{});
        auto source=std::make_unique<ControlledSource>(); auto* controlled=source.get();
        session.setSource(std::move(source)); QString error; QVERIFY(session.start(error));
        controlled->frame(0); controlled->frame(1,true); QCOMPARE(session.bufferSize(),size_t{2});
        QVERIFY(session.requestMonitoring(true)); // A live edit can race with an already queued arm request.
        controlled->settingsChanged(); QCOMPARE(session.bufferSize(),size_t{0});
        QCOMPARE(session.streamInfo().gain,std::string("42"));
        QVERIFY(!session.requestMonitoring(true)); // Buffered live data cannot acquire a fixed evidence snapshot.
        controlled->frame(2); QCOMPARE(session.bufferSize(),size_t{1});
        QCOMPARE(session.mode(),Session::Mode::Preview); QVERIFY(!session.modeChangePending());
        session.stop(); QVERIFY(session.start(error,Session::Mode::Monitoring));
        controlled->frame(3); QVERIFY(session.requestManualSave()); session.stop();
    }
    void retainsHistoryAndTheSourceTimeTail()
    {
        std::vector<EventClip> clips;
        EventCapture capture({.5,.3,1},[]{},[&](EventClip c){clips.push_back(std::move(c));});
        QVERIFY(!capture.saveNow({}));
        for (uint64_t i=0;i<=10;++i) capture.onFrame(sample(i));
        const auto host=std::chrono::system_clock::now();
        QVERIFY(capture.saveNow(host));
        for (uint64_t i=11;i<=13;++i) capture.onFrame(sample(i));
        QCOMPARE(clips.size(),size_t{1});
        const auto& c=clips[0];
        QCOMPARE(c.frames.front()->frameIndex,uint64_t{5}); QCOMPARE(c.frames.back()->frameIndex,uint64_t{13});
        QCOMPARE(c.framesBefore,5); QCOMPARE(c.framesAfter,3);
        QCOMPARE(c.triggers.size(),size_t{1}); QCOMPARE(c.triggers[0].kind,TriggerKind::Manual);
        QVERIFY(c.triggers[0].requestedAt==host);
        QVERIFY(c.triggers[0].triggerTime==sample(10)->timestamp);
        QVERIFY(c.triggers[0].requestSourceTime==sample(10)->timestamp);
        QCOMPARE(c.endReason,std::string("post-event interval complete"));
    }
    void manualAndAutomaticRequestsShareAClipWithoutStoppingDetection()
    {
        std::vector<EventClip> clips; int automatic=0;
        EventCapture capture({.5,1,2},[&]{++automatic;},[&](EventClip c){clips.push_back(std::move(c));});
        for (uint64_t i=0;i<=20;++i)
        {
            capture.onFrame(i==3 || i==10 ?flash(i):sample(i));
            if (i==8) QVERIFY(capture.saveNow(std::chrono::system_clock::now()));
        }
        QCOMPARE(automatic,2); QCOMPARE(clips.size(),size_t{1});
        QCOMPARE(clips[0].triggers.size(),size_t{3});
        QCOMPARE(clips[0].triggers[1].kind,TriggerKind::Manual);
        QCOMPARE(clips[0].triggers[2].triggerFrameIndex,uint64_t{10});
        QCOMPARE(clips[0].frames.back()->frameIndex,uint64_t{20});
    }
    void aManualTailContinuesAcrossAnExistingClipCap()
    {
        std::vector<EventClip> clips;
        EventCapture capture({.5,1,2},[]{},[&](EventClip c){clips.push_back(std::move(c));});
        for (uint64_t i=0;i<=31;++i)
        {
            capture.onFrame(i>=3 && i<=20?flash(i):sample(i));
            if (i==21) QVERIFY(capture.saveNow({}));
        }
        QCOMPARE(clips.size(),size_t{2});
        QCOMPARE(clips[0].endReason,std::string("maximum post-event interval"));
        QCOMPARE(clips[1].triggers[0].kind,TriggerKind::Manual);
        QVERIFY(clips[1].triggers[0].continuesPreviousClip);
        QCOMPARE(clips[1].triggers[0].onsetFrameIndex,uint64_t{21});
        QVERIFY(clips[1].triggers[0].requestSourceTime==sample(21)->timestamp);
        QCOMPARE(clips[1].frames.back()->frameIndex,uint64_t{31});
        QCOMPARE(clips[1].endReason,std::string("post-event interval complete"));
    }
    void pendingRequestsAreHandledAtBoundaries()
    {
        for (int boundary=0;boundary<4;++boundary)
        {
            QTemporaryDir dir; QObject context; std::vector<ClipWriteResult> results; std::vector<bool> handled;
            {
                Session::Notifications notices;
                notices.manualSaveHandled=[&](bool ok){handled.push_back(ok);};
                notices.clipWritten=[&](const auto& r){results.push_back(r);};
                Session session(&context,dir.path(),std::move(notices));
                auto source=std::make_unique<ControlledSource>(); auto* control=source.get();
                session.setSource(std::move(source));
                QVERIFY(!session.requestManualSave());
                QString error; QVERIFY2(session.start(error, Session::Mode::Monitoring),qPrintable(error));
                control->frame(0); control->frame(1);
                QVERIFY(session.requestManualSave()); QVERIFY(!session.requestManualSave());
                if (boundary) control->boundary(boundary);
                session.stop();
                QVERIFY(!session.manualSavePending()); QVERIFY(!session.requestManualSave());
            }
            QCoreApplication::processEvents();
            QCOMPARE(handled.size(),size_t{1}); QVERIFY(handled[0]);
            QCOMPARE(results.size(),size_t{1}); QVERIFY2(results[0].notSaved.isEmpty(),qPrintable(results[0].notSaved));
            const auto note=read(QDir(results[0].folder).filePath("event.txt"));
            QVERIFY(note.contains("kind=manual")); QVERIFY(note.contains("complete: yes"));
            const QString ending=boundary==0?"stopped":boundary==1?"source ended":boundary==2?"source failure: controlled failure":"playback loop boundary";
            QVERIFY(note.contains("collection_end: "+ending));
        }
    }
    void anEmptySourceRejectsAPendingRequestExplicitly()
    {
        QTemporaryDir dir; QObject context; std::vector<bool> handled;
        {
            Session::Notifications notices; notices.manualSaveHandled=[&](bool ok){handled.push_back(ok);};
            Session session(&context,dir.path(),std::move(notices));
            session.setSource(std::make_unique<ControlledSource>());
            QString error; QVERIFY(session.start(error, Session::Mode::Monitoring)); QVERIFY(session.requestManualSave()); session.stop();
        }
        QCoreApplication::processEvents();
        QCOMPARE(handled.size(),size_t{1}); QVERIFY(!handled[0]);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),0);
    }
    void nativeManualEvidenceAndRefusedRequestsStayOnRecord()
    {
        for (auto format:{PixelFormat::Mono8,PixelFormat::Mono16,PixelFormat::RGB888})
            for (auto alignment:{BitAlignment::Low,BitAlignment::High})
            {
                if (format!=PixelFormat::Mono16 && alignment==BitAlignment::High) continue;
                QTemporaryDir dir; QObject context; std::vector<ClipWriteResult> results;
                EventClip clip;
                EventCapture capture({.5,.3,1},[]{},[&](EventClip c){clip=std::move(c);});
                for (uint64_t i=0;i<=5;++i) capture.onFrame(sample(i,format,alignment));
                QVERIFY(capture.saveNow(std::chrono::system_clock::now()));
                capture.finishPartial("stopped");
                const auto originals=clip.frames;
                EventWriter writer(&context,dir.path(),1024*1024,[&](const auto& r){results.push_back(r);});
                writer.submit(clip); writer.shutdown(); QCoreApplication::processEvents();
                QCOMPARE(results.size(),size_t{1}); QVERIFY(results[0].notSaved.isEmpty());
                std::string error; SerReader reader;
                QVERIFY2(reader.open(std::filesystem::path((results[0].folder+"/event.ser").toStdWString()),10,error),qPrintable(QString::fromStdString(error)));
                for (int i=0;i<reader.frameCount();++i)
                {
                    Frame actual; actual.data.resize(reader.frameBytes()); QVERIFY(reader.readFrame(i,actual,error));
                    QCOMPARE(actual.data,originals[static_cast<size_t>(i)]->data);
                    QVERIFY(reader.frameTime(i)==originals[static_cast<size_t>(i)]->timestamp);
                }
                const auto note=read(results[0].folder+"/event.txt");
                QVERIFY(note.contains("kind=manual requested_host_utc="));
                QVERIFY(note.contains("peak_preview_role: capture request frame"));
                const QString refused=dir.path()+"/refused";
                EventWriter busy(&context,refused,1,[&](const auto& r){results.push_back(r);});
                busy.submit(std::move(clip)); busy.shutdown(); QCoreApplication::processEvents();
                QCOMPARE(results.size(),size_t{2}); QVERIFY(!results[1].notSaved.isEmpty());
                const auto log=read(refused+"/candidates.txt");
                QVERIFY(log.contains("manual_pending")); QVERIFY(log.contains("kind=manual")); QVERIFY(log.contains("not_saved=writer busy"));
            }
    }
};
int main(int argc,char** argv) { QGuiApplication app(argc,argv); ManualCaptureTest test; return QTest::qExec(&test,argc,argv); }
#include "manual_capture_tests.moc"
