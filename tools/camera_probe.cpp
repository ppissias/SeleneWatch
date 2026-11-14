#include "cameras/zwo_source.h"
#include "session.h"
#include "recording/ser_reader.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <algorithm>
#include <set>
#include <thread>
#include <cmath>
#include <limits>
#include <numeric>

namespace {
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
int main(int argc,char** argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen"); QGuiApplication app(argc,argv);
    const auto args=app.arguments();
    try
    {
        auto api=makeZwoApi(); if (!api) throw std::runtime_error("This build has no ZWO SDK; configure SELENEWATCH_ZWO_SDK_DIR");
        if (args.size()==3 && args[1]=="inspect")
        {
            QJsonArray list; for (const auto& c:api->cameras()) list.append(profileJson(ZwoSource::inspect(api,c.id)));
            return write(args[2],{{"sdk",QString::fromStdString(api->version())},{"cameras",list}})?0:1;
        }
        if (args.size()!=4 || args[1]!="bench")
        { QTextStream(stderr)<<"Usage: SeleneWatchCameraProbe inspect OUTPUT.json | bench NEW_OUTPUT_DIRECTORY CONFIG.json\n"; return 2; }
        const QDir root(args[2]);
        if (root.exists() && !root.entryList(QDir::AllEntries|QDir::NoDotAndDotDot).empty())
            throw std::runtime_error("Choose an unused camera bench output directory");
        QFile configFile(args[3]); if (!configFile.open(QIODevice::ReadOnly)) return 1;
        const auto document=QJsonDocument::fromJson(configFile.readAll()); if (!document.isObject()) return 1;
        const auto config=document.object();
        const int readDelayMs=config.value("readDelayMs").toInt();
        if (readDelayMs<0 || readDelayMs>1000) throw std::invalid_argument("QA read delay must be 0 to 1000 ms");
        if (readDelayMs) api=std::make_shared<DelayedCamera>(api,readDelayMs);
        if (config.value("frames").toInt(100)<=0 || !std::isfinite(config.value("seconds").toDouble(15))
            || config.value("seconds").toDouble(15)<=0 || config.value("seconds").toDouble(15)>3600)
            throw std::invalid_argument("Bench frame count/duration must be positive; duration at most one hour");
        auto profile=ZwoSource::inspect(api,config.value("id").toInt(0)); auto settings=profile.current;
        settings.width=config.value("width").toInt(profile.camera.width); settings.height=config.value("height").toInt(profile.camera.height);
        settings.bin=config.value("bin").toInt(1); settings.x=config.value("x").toInt(0); settings.y=config.value("y").toInt(0);
        settings.format=config.value("raw16").toBool()?PixelFormat::Mono16:PixelFormat::Mono8;
        const auto control=[&](const char* key,ZwoControl kind) {
            if (!config.contains(key)) return;
            const double value=config.value(key).toDouble();
            if (!std::isfinite(value) || value<std::numeric_limits<long>::min() || value>std::numeric_limits<long>::max()) throw std::invalid_argument("Bench control exceeds the SDK integer range");
            settings.controls[kind]=static_cast<int64_t>(value);
        };
        control("exposureUs",ZwoControl::Exposure); control("gain",ZwoControl::Gain); control("offset",ZwoControl::Offset); control("bandwidth",ZwoControl::Bandwidth);
        QObject context; std::vector<ClipWriteResult> written;
        struct Sample { std::chrono::system_clock::time_point time; QByteArray hash; bool matched{false}; };
        std::vector<Sample> samples;
        std::vector<double> detectorTimes;
        QJsonObject report; QString failure; uint64_t acquired=0; MemoryUsage memory; WriterUsage usage;
        double elapsed=0; int candidates=0; bool injectionRequested=false;
        {
            Session* active=nullptr;
            const auto started=std::chrono::steady_clock::now();
            Session::Notifications notices;
            notices.previewReady=[&] {
                if (!active) return;
                auto preview=active->takePreview();
                if (preview.frame && samples.size()<20) samples.push_back({preview.frame->timestamp,hash(*preview.frame)});
                if (preview.detection && preview.detection->summary.ready) detectorTimes.push_back(preview.detection->summary.processingMs);
            };
            notices.clipWritten=[&](const auto& result){written.push_back(result);};
            notices.candidateStarted=[&]{++candidates;};
            notices.failed=[&](const QString& error){failure=error;app.quit();};
            Session session(&context,root.filePath("events"),std::move(notices)); active=&session;
            session.setDetectionSettings({config.value("sigma").toDouble(1000),8,400,true});
            auto source=std::make_unique<ZwoSource>(api,profile,settings); auto* camera=source.get();
            session.setSource(std::move(source)); QString error;
            if (!session.start(error)) throw std::runtime_error(error.toStdString());
            report=profileJson(camera->actualProfile());
            settings=camera->actualProfile().current;
            QTimer tick; tick.setInterval(10);
            uint64_t lastManual=0;
            QObject::connect(&tick,&QTimer::timeout,&context,[&] {
                const auto n=session.acquiredFrames();
                if (n>=3 && (lastManual==0 || n-lastManual>=10) && session.requestManualSave()) lastManual=n;
                if (config.value("inject").toBool() && !injectionRequested && n>=10)
                    injectionRequested=session.requestBlip({settings.width/2,settings.height/2,4,config.value("amplitude").toDouble(8192),1});
                elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
                if (n>=static_cast<uint64_t>(config.value("frames").toInt(100)) || elapsed>=config.value("seconds").toDouble(15)) app.quit();
            });
            tick.start(); app.exec(); tick.stop(); session.stop("camera bench ended");
            acquired=session.acquiredFrames(); memory=session.memoryUsage(); usage=session.writerUsage();
            report["sdkDrops"]=session.droppedFrames()?QJsonValue(static_cast<double>(*session.droppedFrames())):QJsonValue();
            const auto drainStarted=std::chrono::steady_clock::now();
            while (session.writerUsage().outstandingClips>0)
            {
                QCoreApplication::processEvents(); std::this_thread::sleep_for(std::chrono::milliseconds{10});
                if (std::chrono::steady_clock::now()-drainStarted>std::chrono::seconds{60}) break;
            }
            usage=session.writerUsage(); active=nullptr;
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
                if (f.significantBits!=(settings.format==PixelFormat::Mono16?16:8) || f.alignment!=BitAlignment::Low)
                    throw std::runtime_error("Saved camera sample interpretation changed on replay");
                times.insert(reader.frameTime(i));
                if (injected.contains(i)) for (const auto& c:triggers)
                {
                    if (c.index!=indices[i] || c.x>=f.width || c.y>=f.height) continue;
                    const double dx=(c.x-settings.width/2)/4.0,dy=(c.y-settings.height/2)/4.0;
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
        report["purpose"]="Connected-camera bench validation; no astronomical event claim"; report["config"]=config;
        report["acquiredFrames"]=static_cast<double>(acquired); report["elapsedSeconds"]=elapsed;
        report["savedSampleBits"]=settings.format==PixelFormat::Mono16?16:8;
        report["savedAlignment"]="low"; report["recordedGapLinesIncludingOverlap"]=static_cast<double>(recordedGaps);
        report["deliveredFps"]=elapsed>0?acquired/elapsed:0; report["savedFramesIncludingOverlap"]=static_cast<double>(savedFrames);
        const double sourceSpan=times.size()>1?std::chrono::duration<double>(*times.rbegin()-*times.begin()).count():0;
        report["uniqueSavedTimestamps"]=static_cast<double>(times.size()); report["sourceSpanSeconds"]=sourceSpan;
        report["arrivalFpsFromSavedTimes"]=sourceSpan>0?(times.size()-1)/sourceSpan:0;
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
