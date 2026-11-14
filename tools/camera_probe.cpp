#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "tools/camera_probe.h"
#include "cameras/zwo_source.h"
#include "cameras/raw_camera_layout.h"
#include "session.h"
#include "recording/ser_reader.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDateTime>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QStorageInfo>
#include <QtGui/QGuiApplication>
#include <algorithm>
#include <set>
#include <thread>
#include <cmath>
#include <limits>
#include <numeric>
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

namespace {
QJsonObject processUsage()
{
    QJsonObject result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
    {
        result["workingSetBytes"]=static_cast<double>(memory.WorkingSetSize);
        result["privateBytes"]=static_cast<double>(memory.PrivateUsage);
    }
#endif
    return result; // Unknown on other platforms; never substitute charged payload.
}
uint64_t directoryBytes(const QDir& directory)
{
    uint64_t total=0;
    QDirIterator files(directory.absolutePath(), QDir::Files|QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext())
    {
        files.next();
        if (!files.fileInfo().isSymLink()) total+=static_cast<uint64_t>(files.fileInfo().size());
    }
    return total;
}
double number(const QJsonObject& config, const char* key, double fallback, double minimum, double maximum)
{
    const double value=config.value(key).toDouble(fallback);
    if ((config.contains(key) && !config.value(key).isDouble()) || !std::isfinite(value) || value<minimum || value>maximum)
        throw std::invalid_argument(std::string("Invalid camera probe setting: ")+key);
    return value;
}
// Manual QA only: make the consumer fall behind without altering camera data.
// The normal source and SDK wrapper have no artificial throttling path.
class DelayedCamera final : public ZwoApi
{
public:
    DelayedCamera(std::shared_ptr<ZwoApi> api,int delayMs):api_(std::move(api)),delayMs_(delayMs) {}
    std::string version() const override { return api_->version(); }
    std::vector<ZwoCameraInfo> cameras() override
    { auto list=api_->cameras(); for (auto& c:list) c.testSource=true; return list; }
    void open(int id) override { api_->open(id); }
    void initialize(int id) override { api_->initialize(id); }
    void close(int id) noexcept override { api_->close(id); }
    std::string serial(int id) override { return api_->serial(id); }
    std::vector<ZwoControlInfo> controls(int id) override { return api_->controls(id); }
    void setControl(int id,ZwoControl c,int64_t value) override { api_->setControl(id,c,value); }
    ZwoSettings roi(int id) override { return api_->roi(id); }
    void setRoi(int id,const ZwoSettings& s) override { api_->setRoi(id,s); }
    void disableDarkSubtract(int id) override { api_->disableDarkSubtract(id); }
    void normalMode(int id) override { api_->normalMode(id); }
    void startVideo(int id) override { api_->startVideo(id); }
    void stopVideo(int id) noexcept override { api_->stopVideo(id); }
    bool read(int id,std::span<uint8_t> data,int waitMs) override
    { std::this_thread::sleep_for(std::chrono::milliseconds{delayMs_}); return api_->read(id,data,waitMs); }
    int dropped(int id) override { return api_->dropped(id); }
private:
    std::shared_ptr<ZwoApi> api_;
    int delayMs_;
};

bool write(const QString& path,const QJsonObject& value)
{
    QFile file(path); const auto bytes=QJsonDocument(value).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.flush();
}
QByteArray hash(const Frame& f)
{
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(f.data.data()),static_cast<qsizetype>(f.data.size())),QCryptographicHash::Sha256);
}
QJsonObject profileJson(const ZwoProfile& p)
{
    QJsonArray controls;
    for (const auto& c:p.controls) controls.append(QJsonObject{{"name",QString::fromStdString(c.name)},
        {"minimum",static_cast<double>(c.minimum)},{"maximum",static_cast<double>(c.maximum)},
        {"value",static_cast<double>(c.value)},{"automatic",c.automatic},{"writable",c.writable}});
    return {{"camera",QString::fromStdString(p.camera.name)},{"serial",QString::fromStdString(p.serial)},
        {"sdk",QString::fromStdString(p.sdkVersion)},{"sensorBits",p.camera.sensorBits},{"width",p.current.width},
        {"height",p.current.height},{"bin",p.current.bin},{"x",p.current.x},{"y",p.current.y},
        {"rawBits",p.current.format==PixelFormat::Mono16?16:8},{"controls",controls}};
}
}
int runCameraProbeCommand(QCoreApplication& app,const QStringList& args,
    const std::function<CameraProbeInput(const QJsonObject&)>& makeInput)
{
    try
    {
        if (args.size()!=4 || (args[1]!="bench" && args[1]!="soak"))
        { QTextStream(stderr)<<"Usage: "<<args.value(0)<<" bench/soak NEW_OUTPUT_DIRECTORY CONFIG.json\n"; return 2; }
        const bool soak=args[1]=="soak";
        const QDir root(args[2]);
        if (root.exists() && !root.entryList(QDir::AllEntries|QDir::NoDotAndDotDot).empty())
            throw std::runtime_error("Choose an unused camera bench output directory");
        QFile configFile(args[3]); if (!configFile.open(QIODevice::ReadOnly)) return 1;
        const auto document=QJsonDocument::fromJson(configFile.readAll()); if (!document.isObject()) return 1;
        const auto config=document.object();
        for (const auto* key:{"raw16","inject"})
            if (config.contains(key) && !config.value(key).isBool()) throw std::invalid_argument(std::string("Expected a boolean setting: ")+key);
        for (const auto* key:{"width","height","x","y","bin","frames","readDelayMs"})
        {
            if (!config.contains(key)) continue;
            const auto value=number(config,key,0,0,1'000'000'000);
            if (value!=std::floor(value)) throw std::invalid_argument(std::string("Expected an integer setting: ")+key);
        }
        const double sigma=number(config,"sigma",1000,.001,1'000'000);
        const double duration=number(config,"seconds",soak?3600:15,.05,3600);
        const auto frameLimit=static_cast<uint64_t>(number(config,"frames",soak?1'000'000'000:100,1,1'000'000'000));
        const double manualInterval=number(config,"manualIntervalSeconds",soak?120:0,0,3600);
        const double sampleInterval=number(config,"sampleIntervalSeconds",10,.01,3600);
        const auto outputLimit=static_cast<uint64_t>(number(config,"maximumOutputBytes",8.0*1024*1024*1024,1,1e12));
        const auto minimumFree=static_cast<qint64>(number(config,"minimumFreeBytes",4.0*1024*1024*1024,0,1e12));
        CaptureSettings capture;
        capture.historySeconds=number(config,"historySeconds",capture.historySeconds,0,600);
        capture.postSeconds=number(config,"postSeconds",capture.postSeconds,.001,3600);
        capture.maxPostSeconds=number(config,"maxPostSeconds",capture.maxPostSeconds,capture.postSeconds,3600);
        const int readDelayMs=config.value("readDelayMs").toInt();
        if (readDelayMs<0 || readDelayMs>1000) throw std::invalid_argument("QA read delay must be 0 to 1000 ms");
        if (soak && manualInterval<=0) throw std::invalid_argument("Soak manual interval must be positive");
        if (!QDir().mkpath(root.absolutePath())) throw std::runtime_error("Could not create camera probe output directory");
        QFile progress(root.filePath("progress.jsonl"));
        if (soak && !progress.open(QIODevice::WriteOnly|QIODevice::NewOnly)) throw std::runtime_error("Could not create soak progress log");
        auto input=makeInput(config);
        CameraProbeReadback layout;
        QObject context; std::vector<ClipWriteResult> written;
        struct Sample { std::chrono::system_clock::time_point time; QByteArray hash; bool matched{false}; };
        std::vector<Sample> samples;
        std::vector<double> detectorTimes;
        QJsonObject report; QString failure; uint64_t acquired=0; MemoryUsage memory; WriterUsage usage;
        double elapsed=0; int candidates=0; bool injectionRequested=false;
        QString stopReason="duration";
        double sampleUntil=0;
        uint64_t peakWorkingSet=0, peakPrivate=0;
        const auto started=std::chrono::steady_clock::now();
        const auto seconds=[&] { return std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count(); };
        Session* active=nullptr; // Outlives queued preview callbacks drained after Session destruction.
        {
            Session::Notifications notices;
            notices.previewReady=[&] {
                if (!active) return;
                auto preview=active->takePreview();
                const bool sample=soak?(seconds()<=sampleUntil && samples.size()<4096):samples.size()<20;
                if (preview.frame && sample) samples.push_back({preview.frame->timestamp,hash(*preview.frame)});
                if (preview.detection && preview.detection->summary.ready) detectorTimes.push_back(preview.detection->summary.processingMs);
            };
            notices.clipWritten=[&](const auto& result){
                written.push_back(result);
                if (!result.notSaved.isEmpty() || result.recordFailed)
                { failure="Evidence writer failed or refused a clip"; stopReason="writer-failed"; app.quit(); }
            };
            notices.candidateStarted=[&]{++candidates;};
            notices.manualSaveHandled=[&](bool saved) {
                if (soak && saved) sampleUntil=seconds()+std::min(1.0,capture.postSeconds/2);
            };
            notices.failed=[&](const QString& error){failure=error;stopReason="source-failed";app.quit();};
            Session session(&context,root.filePath("events"),std::move(notices)); active=&session;
            session.setCaptureSettings(capture);
            session.setDetectionSettings({sigma,8,400,true});
            session.setSource(std::move(input.source)); QString error;
            if (!session.start(error)) throw std::runtime_error(error.toStdString());
            layout=input.readback();report=layout.metadata;
            if (layout.width<=0 || layout.height<=0 || (layout.format!=PixelFormat::Mono8 && layout.format!=PixelFormat::Mono16))
                throw std::runtime_error("Camera probe received an invalid raw readback layout");
            QTimer tick; tick.setInterval(10);
            uint64_t lastManual=0;
            double lastManualTime=-manualInterval, lastSample=-sampleInterval, lastResource=-1;
            uint64_t outputBytes=0;
            const auto snapshot=[&](const char* phase) {
                const auto payload=session.memoryUsage(); const auto writer=session.writerUsage();
                auto row=processUsage();
                peakWorkingSet=std::max(peakWorkingSet,static_cast<uint64_t>(row.value("workingSetBytes").toDouble()));
                peakPrivate=std::max(peakPrivate,static_cast<uint64_t>(row.value("privateBytes").toDouble()));
                row["phase"]=phase; row["hostUtc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                row["elapsedSeconds"]=seconds(); row["acquiredFrames"]=static_cast<double>(session.acquiredFrames());
                const auto drops=session.droppedFrames(); row["sdkDrops"]=drops?QJsonValue(static_cast<double>(*drops)):QJsonValue();
                row["chargedBytes"]=static_cast<double>(payload.used); row["chargedPeakBytes"]=static_cast<double>(payload.peak);
                row["memoryRefusals"]=static_cast<double>(payload.refused);
                row["writerRetainedBytes"]=static_cast<double>(writer.retainedBytes);
                row["writerOutstandingClips"]=static_cast<double>(writer.outstandingClips);
                row["bufferedSerBytes"]=static_cast<double>(writer.writtenBytes);
                row["outputBytes"]=static_cast<double>(outputBytes); row["newCandidates"]=candidates;
                const auto bytes=QJsonDocument(row).toJson(QJsonDocument::Compact)+'\n';
                if (progress.write(bytes)!=bytes.size() || !progress.flush())
                { failure="Could not flush soak progress log"; stopReason="progress-failed"; app.quit(); }
            };
            QObject::connect(&tick,&QTimer::timeout,&context,[&] {
                const auto n=session.acquiredFrames();
                elapsed=seconds();
                if (soak && elapsed-lastResource>=1)
                {
                    lastResource=elapsed; outputBytes=directoryBytes(root);
                    if (QFileInfo::exists(root.filePath("stop.request")))
                    { stopReason="requested-stop"; app.quit(); return; }
                    QStorageInfo storage(root.absolutePath()); storage.refresh();
                    if (outputBytes>=outputLimit)
                    { failure="Soak output budget reached"; stopReason="output-limit"; app.quit(); return; }
                    if (!storage.isValid() || storage.bytesAvailable()<minimumFree)
                    { failure="Soak free-space reserve reached or unavailable"; stopReason="disk-space"; app.quit(); return; }
                }
                if (n>=3 && (manualInterval>0 ? elapsed-lastManualTime>=manualInterval : lastManual==0 || n-lastManual>=10)
                    && session.requestManualSave())
                {
                    lastManual=n; lastManualTime=elapsed;
                }
                if (config.value("inject").toBool() && !injectionRequested && n>=10)
                    injectionRequested=session.requestBlip({layout.width/2,layout.height/2,4,config.value("amplitude").toDouble(8192),1});
                if (soak && elapsed-lastSample>=sampleInterval) { lastSample=elapsed; snapshot("streaming"); }
                if (n>=frameLimit || elapsed>=duration)
                { stopReason=n>=frameLimit?"frame-limit":"duration"; app.quit(); }
            });
            if (soak) snapshot("starting");
            tick.start(); if (failure.isEmpty()) app.exec(); tick.stop();
            session.stop((soak?"camera soak ended: ":"camera bench ended: ")+stopReason.toStdString());
            acquired=session.acquiredFrames(); memory=session.memoryUsage(); usage=session.writerUsage();
            report["sdkDrops"]=session.droppedFrames()?QJsonValue(static_cast<double>(*session.droppedFrames())):QJsonValue();
            const auto drainStarted=std::chrono::steady_clock::now();
            while (session.writerUsage().outstandingClips>0)
            {
                QCoreApplication::processEvents(); std::this_thread::sleep_for(std::chrono::milliseconds{10});
                if (std::chrono::steady_clock::now()-drainStarted>std::chrono::seconds{60}) break;
            }
            usage=session.writerUsage();
            if (usage.outstandingClips>0) { failure="Evidence writer did not drain within 60 seconds"; stopReason="writer-drain-timeout"; }
            if (soak) { outputBytes=directoryBytes(root); snapshot("stopped"); }
            active=nullptr;
        }
        QCoreApplication::processEvents();
        uint32_t wordOr=0; uint32_t minimum=65535,maximum=0; uint64_t savedFrames=0; int injectionFrames=0;
        bool matchedInjection=false;
        std::set<std::chrono::system_clock::time_point> times;
        uint64_t recordedGaps=0;
        QJsonArray outputs;
        for (const auto& result:written)
        {
            outputs.append(QJsonObject{{"folder",result.folder},{"notSaved",result.notSaved},{"recordFailed",result.recordFailed}});
            if (result.folder.isEmpty()) continue;
            SerReader reader; std::string error;
            if (!reader.open(std::filesystem::path((result.folder+"/event.ser").toStdWString()),30,error)) throw std::runtime_error(error);
            Frame f; f.data.resize(reader.frameBytes());
            QFile note(result.folder+"/event.txt"); std::set<int> injected;
            std::map<int,uint64_t> indices;
            struct Candidate { uint64_t index; int x,y; };
            std::vector<Candidate> triggers;
            if (note.open(QIODevice::ReadOnly)) for (const auto& line:QString::fromUtf8(note.readAll()).split('\n'))
            {
                if (line.contains(" test_blip="))
                { const int position=line.section(' ',0,0).toInt(); injected.insert(position); indices[position]=line.section(' ',1,1).toULongLong(); ++injectionFrames; }
                const auto match=QRegularExpression("^trigger: .*trigger_frame_index=([0-9]+).*peak=\\(([0-9]+),([0-9]+)\\)").match(line);
                if (match.hasMatch()) triggers.push_back({match.captured(1).toULongLong(),match.captured(2).toInt(),match.captured(3).toInt()});
                if (line.startsWith("gap:")) ++recordedGaps;
            }
            for (int i=0;i<reader.frameCount();++i)
            {
                if (!reader.readFrame(i,f,error)) throw std::runtime_error(error);
                if (f.width!=layout.width || f.height!=layout.height || f.format!=layout.format || f.bayer!=layout.bayer)
                    throw std::runtime_error("Saved camera geometry, format or Bayer pattern changed on replay");
                if (f.significantBits!=(layout.format==PixelFormat::Mono16?16:8) || f.alignment!=BitAlignment::Low)
                    throw std::runtime_error("Saved camera sample interpretation changed on replay");
                times.insert(reader.frameTime(i));
                if (injected.contains(i)) for (const auto& c:triggers)
                {
                    if (c.index!=indices[i] || c.x>=f.width || c.y>=f.height) continue;
                    const double dx=(c.x-layout.width/2)/4.0,dy=(c.y-layout.height/2)/4.0;
                    const double addition=config.value("amplitude").toDouble(8192)*std::exp(-4*std::log(2.0)*(dx*dx+dy*dy));
                    const size_t p=static_cast<size_t>(c.y)*f.width+c.x;
                    const int value=f.format==PixelFormat::Mono16?(f.data[2*p]|(f.data[2*p+1]<<8)):f.data[p];
                    // A positive rounded addition and a nonsaturated stored
                    // sample prove this selected peak was actually modified.
                    matchedInjection=matchedInjection || (addition>=.5 && value<(f.format==PixelFormat::Mono16?65535:255));
                }
                for (auto& s:samples) if (s.time==reader.frameTime(i)) { if (hash(f)!=s.hash) throw std::runtime_error("Native camera bytes changed before SER output"); s.matched=true; }
                if (f.format==PixelFormat::Mono16 && !injected.contains(i)) for (size_t p=0;p<f.data.size();p+=2)
                { const uint32_t value=f.data[p]|(f.data[p+1]<<8); wordOr|=value; minimum=std::min(minimum,value); maximum=std::max(maximum,value); }
                ++savedFrames;
            }
        }
        const int matched=static_cast<int>(std::count_if(samples.begin(),samples.end(),[](const auto& s){return s.matched;}));
        if ((samples.empty() || matched!=static_cast<int>(samples.size())) && failure.isEmpty())
        { failure="Missing native-byte verification samples in saved evidence"; stopReason="verification-failed"; }
        report["purpose"]="Camera pipeline validation; source and TEST marks identify provenance; no astronomical event claim"; report["config"]=config;
        report["mode"]=soak?"soak":"bench"; report["stopReason"]=stopReason;
        if (soak)
        {
            report["sampledPeakWorkingSetBytes"]=peakWorkingSet?QJsonValue(static_cast<double>(peakWorkingSet)):QJsonValue();
            report["sampledPeakPrivateBytes"]=peakPrivate?QJsonValue(static_cast<double>(peakPrivate)):QJsonValue();
            report["finalOutputBytes"]=static_cast<double>(directoryBytes(root));
            report["maximumOutputBytes"]=static_cast<double>(outputLimit);
            report["outputLimitPolicy"]="checked once per second; pending and final partial clips finish, so the final size can exceed this stop threshold";
            report["manualIntervalSeconds"]=manualInterval;
        }
        report["acquiredFrames"]=static_cast<double>(acquired); report["elapsedSeconds"]=elapsed;
        report["savedSampleBits"]=layout.format==PixelFormat::Mono16?16:8;
        report["savedAlignment"]="low"; report["recordedGapLinesIncludingOverlap"]=static_cast<double>(recordedGaps);
        report["deliveredFps"]=elapsed>0?acquired/elapsed:0; report["savedFramesIncludingOverlap"]=static_cast<double>(savedFrames);
        const double sourceSpan=times.size()>1?std::chrono::duration<double>(*times.rbegin()-*times.begin()).count():0;
        report["uniqueSavedTimestamps"]=static_cast<double>(times.size()); report["sourceSpanSeconds"]=sourceSpan;
        report["arrivalFpsFromSavedTimes"]=soak?QJsonValue():QJsonValue(sourceSpan>0?(times.size()-1)/sourceSpan:0);
        if (soak) report["frameRatePolicy"]="acquiredFrames / elapsedSeconds includes startup; saved clips are periodic and do not measure full-run fps";
        report["previewHashes"]=static_cast<int>(samples.size()); report["matchedPreviewHashes"]=matched;
        report["nativeWordOr"]=static_cast<double>(wordOr); report["nativeWordMin"]=static_cast<double>(minimum); report["nativeWordMax"]=static_cast<double>(maximum);
        report["memoryPeakBytes"]=static_cast<double>(memory.peak); report["memoryRefusals"]=static_cast<double>(memory.refused);
        report["bufferedSerBytesAtStop"]=static_cast<double>(usage.writtenBytes); report["bufferedSerSecondsAtStop"]=usage.writeSeconds;
        report["newCandidates"]=candidates; report["injectionRequested"]=injectionRequested; report["injectedFramesInSavedClips"]=injectionFrames;
        report["injectionMatchedNewCandidate"]=matchedInjection;
        report["failure"]=failure; report["outputs"]=outputs;
        if (!detectorTimes.empty())
        {
            report["previewSampledDetectorMeanMs"]=std::accumulate(detectorTimes.begin(),detectorTimes.end(),0.0)/detectorTimes.size();
            std::sort(detectorTimes.begin(),detectorTimes.end());
            report["previewSampledDetectorP95Ms"]=detectorTimes[(detectorTimes.size()-1)*95/100];
            report["detectorTimingSamples"]=static_cast<int>(detectorTimes.size());
        }
        if (!write(root.filePath("bench.json"),report)) return 1;
        QTextStream(stdout)<<QJsonDocument(report).toJson(QJsonDocument::Compact)<<'\n';
        return failure.isEmpty() && acquired>0 && matched==static_cast<int>(samples.size()) && !written.empty()
            && std::all_of(written.begin(),written.end(),[](const auto& r){return r.notSaved.isEmpty() && !r.recordFailed;})?0:1;
    }
    catch (const std::exception& ex) { QTextStream(stderr)<<ex.what()<<'\n'; return 1; }
}

int runCameraProbe(QCoreApplication& app,const QStringList& args,std::shared_ptr<ZwoApi> api)
{
    try
    {
        if (!api) throw std::runtime_error("This build has no ZWO SDK; configure SELENEWATCH_ZWO_SDK_DIR");
        if (args.size()==3 && args[1]=="inspect")
        {
            QJsonArray list;for (const auto& c:api->cameras()) list.append(profileJson(ZwoSource::inspect(api,c.id)));
            return write(args[2],{{"sdk",QString::fromStdString(api->version())},{"cameras",list}})?0:1;
        }
        return runCameraProbeCommand(app,args,[api](const QJsonObject& config) {
            std::shared_ptr<ZwoApi> cameraApi=api;
            const int delay=config.value("readDelayMs").toInt();
            if (delay) cameraApi=std::make_shared<DelayedCamera>(cameraApi,delay);
            auto profile=ZwoSource::inspect(cameraApi,config.value("id").toInt(0));auto settings=profile.current;
            settings.width=config.value("width").toInt(profile.camera.width);settings.height=config.value("height").toInt(profile.camera.height);
            settings.bin=config.value("bin").toInt(1);settings.x=config.value("x").toInt(0);settings.y=config.value("y").toInt(0);
            settings.format=config.value("raw16").toBool()?PixelFormat::Mono16:PixelFormat::Mono8;
            const auto control=[&](const char* key,ZwoControl kind) {
                if (!config.contains(key)) return;
                const double value=number(config,key,0,std::numeric_limits<long>::min(),std::numeric_limits<long>::max());
                settings.controls[kind]=static_cast<int64_t>(value);
            };
            control("exposureUs",ZwoControl::Exposure);control("gain",ZwoControl::Gain);control("offset",ZwoControl::Offset);control("bandwidth",ZwoControl::Bandwidth);
            auto source=std::make_unique<ZwoSource>(cameraApi,profile,settings);auto* camera=source.get();
            return CameraProbeInput{std::move(source),[camera] {
                const auto& p=camera->actualProfile();return CameraProbeReadback{profileJson(p),p.current.width,p.current.height,p.current.format,
                    roiBayerPattern(p.camera.bayer,p.current.x,p.current.y)};
            }};
        });
    }
    catch (const std::exception& ex) { QTextStream(stderr)<<ex.what()<<'\n';return 1; }
}
