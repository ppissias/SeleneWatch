#pragma once
#include "cameras/qhy_api.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

// Deterministic native Bayer TEST device; SDK-sized buffers include a tail
// that must never become image data. All mutation precedes worker start,
// except the explicit atomic disconnect/not-ready controls.
class FakeQhyApi final : public QhyApi
{
public:
    FakeQhyApi()
    {
        state.camera={{"TEST-QHY-ID","TEST QHY5L-II-C"},128,64,12,BayerPattern::RGGB,
            {PixelFormat::Mono8,PixelFormat::Mono16},true};
        state.sdkVersion="TEST QHY SDK"; state.current.width=128; state.current.height=64;
        state.controls={{QhyControl::Exposure,"Exposure",64,1'000'000,1,1000},
            {QhyControl::Gain,"Gain",0,100,.25,12.5},{QhyControl::Offset,"Offset",0,255,1,16},
            {QhyControl::UsbTraffic,"USB traffic",0,255,1,10},{QhyControl::TransferSpeed,"Transfer speed",0,2,1,0}};
        for (const auto& c:state.controls) state.current.controls[c.kind]=c.value;
    }
    QhyProfile state;
    QString runtimeOverride;
    std::string failOperation;
    int clampWidth{0};
    size_t padding{4096};
    bool shortBuffer{false}, badLayout{false};
    std::atomic<bool> connected{true}, notReady{false};
    std::atomic<int> delivered{0}, reads{0}, opened{0}, closed{0}, starts{0}, stops{0};
    std::atomic<int> disconnectAt{-1}, frameLimit{-1};
    void setRuntimeOverride(const QString& path) override { runtimeOverride=path; }
    QString loadedRuntimePath() const override { return runtimeOverride; }
    std::string version() override { return state.sdkVersion; }
    std::vector<QhyDevice> devices() override { check("devices"); return connected?std::vector{state.camera.device}:std::vector<QhyDevice>{}; }
    void open(const std::string&) override { check("open"); ++opened; }
    void initializeLive() override { check("initialize"); }
    QhyProfile profile() override
    { check("profile");for (const auto& c:state.controls) state.current.controls[c.kind]=c.value;return state; }
    void configureRaw(const QhySettings& settings) override
    {
        check("configure"); state.current=settings;
        if (clampWidth) state.current.width=clampWidth;
        for (const auto& [kind,value]:settings.controls) setControl(kind,value);
    }
    void setControl(QhyControl kind, double value) override
    {
        check("control");
        auto control=std::find_if(state.controls.begin(),state.controls.end(),
            [kind](const auto& c) { return c.kind==kind; });
        if (control==state.controls.end()) throw std::invalid_argument("Unknown TEST QHY control");
        auto& c=*control;
        c.value=c.minimum+std::floor((value-c.minimum)/c.step)*c.step;
        state.current.controls[kind]=c.value;
    }
    size_t bufferBytes() override
    {
        check("buffer");
        return shortBuffer?1:static_cast<size_t>(state.current.width)*state.current.height*6+padding;
    }
    void startVideo() override { check("start"); ++starts; }
    QhyRead read(std::span<uint8_t> destination) override
    {
        ++reads; std::this_thread::sleep_for(std::chrono::milliseconds{2});
        if (!connected || (disconnectAt>=0 && delivered>=disconnectAt)) throw std::runtime_error("TEST QHY disconnected");
        if (notReady || (frameLimit>=0 && delivered>=frameLimit)) return {};
        if (destination.size()<bufferBytes()) throw std::runtime_error("QHY destination omitted SDK padding");
        std::fill(destination.begin(),destination.end(),uint8_t{0xA5});
        const auto& s=state.current;
        for (size_t i=0;i<static_cast<size_t>(s.width)*s.height;++i)
        {
            if (s.format==PixelFormat::Mono8) destination[i]=static_cast<uint8_t>(80+i%31);
            else { const auto word=8000+i%31; destination[2*i]=static_cast<uint8_t>(word&255); destination[2*i+1]=static_cast<uint8_t>(word>>8); }
        }
        ++delivered;
        return {true,s.width,s.height,s.format==PixelFormat::Mono16?16:8,badLayout?3:1};
    }
    void stopVideo() noexcept override { ++stops; }
    void close() noexcept override { ++closed; }
private:
    void check(const char* operation)
    {
        if (failOperation==operation) throw std::runtime_error(std::string("TEST QHY ")+operation+" failed");
    }
};
