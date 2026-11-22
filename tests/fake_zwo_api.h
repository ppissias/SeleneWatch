#pragma once

#include "cameras/zwo_api.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

// A deterministic TEST camera behind the exact boundary the real source uses.
// No vendor header/DLL, USB device, or physical camera is needed by CTest.
class FakeZwoApi final : public ZwoApi
{
public:
    FakeZwoApi()
    {
        camera={0,128,64,12,"TEST ASI120 Mono",BayerPattern::None,{1,2},{PixelFormat::Mono8,PixelFormat::Mono16},false,false,true};
        camera.pixelSizeUm=3.75; // As the ASI120 reports.
        settings.width=128; settings.height=64;
        caps={{ZwoControl::Exposure,"Exposure",64,2'000'000'000,1000,true,false},
            {ZwoControl::Gain,"Gain",0,100,10,true,false},{ZwoControl::Offset,"Offset",0,20,9,true,false},
            {ZwoControl::Bandwidth,"USB bandwidth",40,100,40,true,false},{ZwoControl::Flip,"Flip",0,3,0,true,false}};
    }
    ZwoCameraInfo camera;
    ZwoSettings settings;
    std::vector<ZwoControlInfo> caps;
    std::string failOperation;
    std::vector<int> dropCounts;
    int clampWidth{0};
    std::atomic<int> reads{0}, delivered{0}, opened{0}, closed{0}, starts{0}, stops{0};
    std::atomic<bool> connected{true}, timeouts{false};
    std::atomic<int> disconnectAt{-1}, telemetryFailureAt{-1};
    std::vector<std::string> calls;
    // Feature-34: when set, the frames' level in 8-bit codes follows the
    // exposure (microseconds) and gain the camera holds, clipped at full
    // scale; otherwise they are fixed.
    std::function<double(double exposureUs, double gain)> light;
    // With light: the frames' temporal noise, as a standard deviation in
    // 8-bit codes, from the same exposure and gain; none when unset.
    std::function<double(double exposureUs, double gain)> lightNoise;
    // Read after stop/join: verifies that live writes and reads share a worker.
    std::thread::id liveControlThread, readThread;
    std::string version() const override { return "TEST SDK 1"; }
    std::vector<ZwoCameraInfo> cameras() override { call("cameras"); return connected?std::vector{camera}:std::vector<ZwoCameraInfo>{}; }
    void open(int) override { call("open"); ++opened; }
    void initialize(int) override { call("initialize"); }
    void close(int) noexcept override { ++closed; }
    std::string serial(int) override { call("serial"); return "TEST-ASI-0001"; }
    std::vector<ZwoControlInfo> controls(int) override { call("controls"); return caps; }
    void setControl(int,ZwoControl kind,int64_t value) override
    {
        call("setControl");
        if (starts.load()>stops.load()) liveControlThread=std::this_thread::get_id();
        for (auto& c:caps) if (c.kind==kind) { c.value=kind==ZwoControl::Gain?value/2*2:value; c.automatic=false; }
    }
    ZwoSettings roi(int) override { call("roi"); return settings; }
    void setRoi(int,const ZwoSettings& s) override { call("setRoi"); settings=s; if (clampWidth) settings.width=clampWidth; }
    void disableDarkSubtract(int) override { call("disableDark"); }
    void normalMode(int) override { call("normalMode"); }
    void startVideo(int) override { call("startVideo"); ++starts; }
    void stopVideo(int) noexcept override { ++stops; }
    bool read(int,std::span<uint8_t> bytes,int waitMs) override
    {
        ++reads;
        readThread=std::this_thread::get_id();
        if (waitMs<1 || waitMs>100) throw std::runtime_error("Unbounded SDK wait");
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        if (!connected || (disconnectAt>=0 && delivered>=disconnectAt)) throw std::runtime_error("TEST camera disconnected");
        if (timeouts) return false;
        if (bytes.size()!=frameBytes(settings.width,settings.height,settings.format)) throw std::runtime_error("Wrong SDK destination buffer size");
        const double exposure=controlValue(ZwoControl::Exposure), gain=controlValue(ZwoControl::Gain);
        const double level=light?light(exposure,gain):-1;
        const double sigma=lightNoise?lightNoise(exposure,gain):0;
        std::normal_distribution<double> unit(0,1);
        for (size_t i=0;i<static_cast<size_t>(settings.width)*settings.height;++i)
        {
            const double noise=level<0||sigma<=0?0:sigma*unit(random_);
            if (settings.format==PixelFormat::Mono8)
                bytes[i]=static_cast<uint8_t>(level<0?100+i%13:std::clamp(std::lround(level+noise)+static_cast<long>(i%13),0L,255L));
            else
            {
                const long sample=level<0?static_cast<long>(400+i%13):std::clamp(std::lround((level+noise)*16)+static_cast<long>(i%13),0L,4095L);
                const int word=static_cast<int>(sample<<4); bytes[2*i]=static_cast<uint8_t>(word&255); bytes[2*i+1]=static_cast<uint8_t>(word>>8);
            }
        }
        ++delivered; return true;
    }
    int dropped(int) override
    {
        if (telemetryFailureAt>=0 && delivered>=telemetryFailureAt) throw std::runtime_error("TEST drop telemetry failed");
        if (dropCounts.empty()) return 0;
        return dropCounts[std::min(static_cast<size_t>(delivered.load()-1),dropCounts.size()-1)];
    }
private:
    double controlValue(ZwoControl kind) const
    {
        for (const auto& c:caps) if (c.kind==kind) return static_cast<double>(c.value);
        return 0;
    }
    void call(const std::string& operation)
    {
        std::lock_guard lock(mutex_); calls.push_back(operation);
        if (operation==failOperation) throw std::runtime_error("TEST "+operation+" failed");
    }
    std::mutex mutex_;
    std::mt19937 random_{11};
};
