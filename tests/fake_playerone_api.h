#pragma once

#include "cameras/playerone_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

// A deterministic TEST Player One camera behind the boundary the real
// adapter uses. No vendor header, DLL or camera is needed by CTest.
class FakePlayerOneApi final : public PlayerOneApi
{
public:
    FakePlayerOneApi()
    {
        camera = {0, 128, 64, 12, "TEST Neptune-M", "TEST-POA-0001", "IMX000", BayerPattern::None, {1, 2},
            {PixelFormat::Mono8, PixelFormat::Mono16}, true, true};
        settings.width = 128;
        settings.height = 64;
        caps = {{PlayerOneControl::Exposure, "Exposure", 10, 2'000'000'000, 1000, true, false},
            {PlayerOneControl::Gain, "Gain", 0, 500, 100, true, false},
            {PlayerOneControl::Offset, "Offset", 0, 255, 10, true, false},
            {PlayerOneControl::UsbBandwidth, "USB bandwidth", 35, 100, 80, true, false}};
    }
    PlayerOneCameraInfo camera;
    PlayerOneSettings settings;
    std::vector<PlayerOneControlInfo> caps;
    std::string failOperation;
    std::vector<int> dropCounts;
    QString runtimeOverride;
    bool orientationSet{false};
    std::atomic<int> reads{0}, delivered{0}, opened{0}, closed{0}, starts{0}, stops{0};
    std::atomic<bool> connected{true}, timeouts{false};
    std::atomic<int> disconnectAt{-1};

    void setRuntimeOverride(const QString& path) override { runtimeOverride = path; }
    QString loadedRuntimePath() const override { return runtimeOverride; }
    std::string version() override { return "TEST POA SDK 3"; }
    std::vector<PlayerOneCameraInfo> cameras() override { call("cameras"); return connected ? std::vector{camera} : std::vector<PlayerOneCameraInfo>{}; }
    void open(int) override { call("open"); ++opened; }
    void initialize(int) override { call("initialize"); }
    void close(int) noexcept override { ++closed; }
    std::vector<PlayerOneControlInfo> controls(int) override { call("controls"); return caps; }
    void setControl(int, PlayerOneControl kind, int64_t value) override
    {
        call("setControl");
        // The test camera keeps gain in steps of 5, as hardware may round.
        for (auto& c : caps) if (c.kind == kind) c.value = kind == PlayerOneControl::Gain ? value / 5 * 5 : value;
    }
    PlayerOneSettings roi(int) override { call("roi"); return settings; }
    void setRoi(int, const PlayerOneSettings& s) override { call("setRoi"); settings = s; }
    void nativeOrientation(int) override { call("orientation"); orientationSet = true; }
    void startVideo(int) override { call("startVideo"); ++starts; }
    void stopVideo(int) noexcept override { ++stops; }
    bool read(int, std::span<uint8_t> bytes, int waitMs) override
    {
        ++reads;
        if (waitMs < 1 || waitMs > 100) throw std::runtime_error("Unbounded SDK wait");
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        if (!connected || (disconnectAt >= 0 && delivered >= disconnectAt)) throw std::runtime_error("TEST Player One disconnected");
        if (timeouts) return false;
        if (bytes.size() != frameBytes(settings.width, settings.height, settings.format)) throw std::runtime_error("Wrong SDK destination buffer size");
        for (size_t i = 0; i < static_cast<size_t>(settings.width) * settings.height; ++i)
        {
            if (settings.format == PixelFormat::Mono8) bytes[i] = static_cast<uint8_t>(100 + i % 13);
            else { const int word = static_cast<int>(400 + i % 13); bytes[2 * i] = static_cast<uint8_t>(word & 255); bytes[2 * i + 1] = static_cast<uint8_t>(word >> 8); }
        }
        ++delivered;
        return true;
    }
    int dropped(int) override
    {
        if (dropCounts.empty()) return 0;
        return dropCounts[std::min(static_cast<size_t>(delivered.load() - 1), dropCounts.size() - 1)];
    }

private:
    void call(const std::string& operation)
    {
        std::lock_guard lock(mutex_);
        if (operation == failOperation) throw std::runtime_error("TEST " + operation + " failed");
    }
    std::mutex mutex_;
};
