#include "tools/detection_probe.h"
#include "blip_injector.h"
#include "event_capture.h"
#include "recording/recording_reader.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>

#include <algorithm>
#include <filesystem>
#include <map>
#include <numeric>

namespace {
double sample(const Frame& f,int x,int y)
{
    const size_t i=static_cast<size_t>(y)*f.width+x;
    if (f.format==PixelFormat::Mono8) return f.data[i];
    if (f.format==PixelFormat::RGB888) return (f.data[3*i]+f.data[3*i+1]+f.data[3*i+2])/3.0;
    return (f.data[2*i] | (f.data[2*i+1]<<8)) >> (f.alignment==BitAlignment::High?16-f.significantBits:0);
}
bool aliases(const QString& a,const QString& b)
{
    std::error_code error;
    return std::filesystem::equivalent(std::filesystem::path(a.toStdWString()),std::filesystem::path(b.toStdWString()),error);
}
struct Trial
{
    double sigma;
    bool inject;
    std::unique_ptr<EventCapture> capture;
    int candidates{0}, passing{0}, firing{0}, run{0}, longestRun{0}, updatedFrames{0};
    size_t maxProtected{0};
    double longestPatch{0};
    std::vector<double> timings;
    QJsonArray pulses;
};
}

// Streams the original once; all trials see exactly the same frame/time. Only
// scheduled TEST frames are cloned. No archive or evidence file is written.
int runRearmingProbe(const QStringList& args)
{
    if (args.size()!=5) return 2;
    const QString input=args[2],output=args[3];
    if (aliases(input,output) || aliases(input,output+".csv")) return 1;
    QFile settingsFile(args[4]); if (!settingsFile.open(QIODevice::ReadOnly)) return 1;
    const auto document=QJsonDocument::fromJson(settingsFile.readAll()); if (!document.isObject()) return 1;
    const auto config=document.object();
    std::string error;
    auto reader=openRecordingReader(std::filesystem::path(input.toStdWString()),30,error);
    if (!reader) { QTextStream(stderr)<<QString::fromStdString(error)<<'\n'; return 1; }
    const int count=std::min(reader->frameCount(),config.value("frameLimit").toInt(500));
    if (count<2) return 1;
    std::map<int,BlipSettings> injections;
    for (const auto item:config.value("injections").toArray())
    {
        const auto p=item.toObject();
        const int at=p.value("atFrame").toInt(-1);
        const BlipSettings blip{p.value("x").toInt(-1),p.value("y").toInt(-1),
            p.value("fwhm").toDouble(5),p.value("amplitude").toDouble(),1};
        BlipInjector check;
        if (at<1 || at>=count || !check.request(blip) || !injections.emplace(at,blip).second) return 1;
    }
    if (injections.empty()) return 1;
    std::vector<Trial> trials;
    for (const auto value:config.value("sigmaLevels").toArray(QJsonArray{6,18}))
    {
        const double k=value.toDouble(); if (!DetectionSettings{k,8,400,true}.valid()) return 1;
        for (bool inject:{false,true})
            trials.push_back({k,inject,std::make_unique<EventCapture>(CaptureSettings{},[]{},[](EventClip){},DetectionSettings{k,8,400,true})});
    }
    if (trials.empty() || !QDir().mkpath(QFileInfo(output).absolutePath())) return 1;
    QFile trace(output+".csv"); if (!trace.open(QIODevice::WriteOnly|QIODevice::Text)) return 1;
    QTextStream csv(&trace);
    csv<<"position,source_index,sigma,injected_trial,passed,firing,new_candidates,active_patches,protected_pixels,learning_frames,longest_patch_seconds,processing_ms,candidate_peaks\n";
    std::chrono::system_clock::time_point firstTime{},lastTime{};
    int width=0,height=0; QString format;
    for (int i=0;i<count;++i)
    {
        auto original=std::make_shared<Frame>(); original->data.resize(reader->frameBytes());
        if (!reader->readFrame(i,*original,error)) return 1;
        original->frameIndex=reader->frameNumber(i); original->timestamp=reader->frameTime(i);
        if (i==0)
        {
            firstTime=original->timestamp; width=original->width; height=original->height; format=pixelFormatName(original->format);
            for (const auto& [at,p]:injections) { (void)at; if (p.x>=width || p.y>=height) return 1; }
        }
        lastTime=original->timestamp;
        const auto injection=injections.find(i);
        for (auto& trial:trials)
        {
            FramePtr current=original;
            const bool injecting=trial.inject && injection!=injections.end();
            const bool prior=trial.capture->detectionSummary().firing;
            if (injecting)
            {
                auto changed=std::make_shared<Frame>(*original);
                BlipInjector blip; blip.request(injection->second); blip.apply(*changed); current=std::move(changed);
            }
            trial.capture->onFrame(current,{DetectionOverlay::None,injecting?injection->second.x:-1,injecting?injection->second.y:-1,false});
            const auto& s=trial.capture->detectionSummary();
            trial.candidates+=static_cast<int>(s.newCandidates); trial.passing+=s.passed; trial.firing+=s.firing;
            trial.run=s.firing?trial.run+1:0; trial.longestRun=std::max(trial.longestRun,trial.run);
            trial.maxProtected=std::max(trial.maxProtected,s.protectedPixels);
            trial.longestPatch=std::max(trial.longestPatch,s.longestActiveSeconds);
            trial.updatedFrames+=s.ready && s.protectedPixels<static_cast<size_t>(width)*height;
            trial.timings.push_back(s.processingMs);
            bool matched=false; QString peaks;
            for (const auto& c:trial.capture->detectionCandidates())
            {
                peaks+=QString("%1:%2|").arg(c.peakX).arg(c.peakY);
                matched=matched || (injecting && sample(*current,c.peakX,c.peakY)>sample(*original,c.peakX,c.peakY));
            }
            if (injecting)
            {
                const auto& p=injection->second;
                trial.pulses.append(QJsonObject{{"position",i},{"x",p.x},{"y",p.y},{"requestedAdu",p.amplitude},
                    {"actualCentreAdditionAdu",sample(*current,p.x,p.y)-sample(*original,p.x,p.y)},
                    {"probeNoiseAdu",s.probeNoise},{"priorRunActive",prior},{"matchedNewCandidate",matched}});
            }
            csv<<i<<','<<current->frameIndex<<','<<trial.sigma<<','<<trial.inject<<','<<s.passed<<','<<s.firing<<','
                <<s.newCandidates<<','<<s.activePatches<<','<<s.protectedPixels<<','<<s.learningFrames<<','
                <<s.longestActiveSeconds<<','<<s.processingMs<<','<<peaks<<'\n';
        }
    }
    csv.flush(); if (csv.status()!=QTextStream::Ok || !trace.flush()) return 1; trace.close();
    QJsonArray results;
    for (auto& t:trials)
    {
        t.capture->finishPartial();
        const double mean=std::accumulate(t.timings.begin(),t.timings.end(),0.0)/t.timings.size();
        std::sort(t.timings.begin(),t.timings.end());
        results.append(QJsonObject{{"sigma",t.sigma},{"injectedTrial",t.inject},{"framesAnalyzed",count},
            {"candidates",t.candidates},{"passingFrames",t.passing},{"firingFrames",t.firing},{"longestFiringRunFrames",t.longestRun},
            {"framesUpdatingBackground",t.updatedFrames},{"maxProtectedPixels",static_cast<double>(t.maxProtected)},
            {"longestPatchSeconds",t.longestPatch},{"meanDetectorMs",mean},{"p95DetectorMs",t.timings[(t.timings.size()-1)*95/100]},
            {"injections",t.pulses}});
    }
    QFile report(output); if (!report.open(QIODevice::WriteOnly)) return 1;
    const QJsonDocument result(QJsonObject{{"input",input},{"frames",count},{"width",width},{"height",height},{"format",format},
        {"streamSeconds",std::chrono::duration<double>(lastTime-firstTime).count()},
        {"frameTimeMeaning",QString::fromStdString(reader->describe().frameTime)}, {"config",config},{"trials",results},
        {"scope","unregistered native frames; no evidence output; timing excludes reading/copy/injection/clip assembly; first frame initializes; separate compact patches can trigger during other activity"}});
    const auto bytes=result.toJson();
    if (report.write(bytes)!=bytes.size() || !report.flush()) return 1;
    QTextStream(stdout)<<"Analyzed "<<count<<" original frames in "<<trials.size()<<" trials; "<<output<<'\n';
    return 0;
}
