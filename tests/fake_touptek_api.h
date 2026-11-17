#pragma once

#include "cameras/touptek_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

// A deterministic TEST ToupTek camera. Frame sequence numbers come from
// sequences when given and count on from the last one; otherwise they count
// up from 1000. All mutation
// precedes the worker's start, except the atomic controls.
class FakeToupTekApi final : public ToupTekApi
{
public:
    FakeToupTekApi()
    {
        state.camera = {{"TEST-TT-ID", "TEST ToupTek G3M178C"}, 128, 64, 12, BayerPattern::RGGB,
            {PixelFormat::Mono8, PixelFormat::Mono16}, true, true};
        state.serial = "TEST-TT-0001";
        state.sdkVersion = "TEST TOUPCAM 60";
        state.current.width = 128;
        state.current.height = 64;
        state.controls = {{ToupTekControl::Exposure, "Exposure", 100, 10'000'000, 1000},
            {ToupTekControl::Gain, "Gain (%)", 100, 5000, 100},
            {ToupTekControl::BlackLevel, "Black level", 0, 31, 10},
            {ToupTekControl::UsbBandwidth, "USB bandwidth (%)", 1, 100, 100},
            {ToupTekControl::Speed, "Speed level", 0, 2, 0}};
    }
    ToupTekProfile state;
    QString runtimeOverride;
    std::string failOperation;
    std::vector<uint32_t> sequences;
    bool noSequence{false}, upsideDownOff{false};
    std::atomic<bool> connected{true}, notReady{false};
    std::atomic<int> delivered{0}, reads{0}, opened{0}, closed{0}, starts{0}, stops{0};
    std::atomic<int> disconnectAt{-1}, frameLimit{-1};

    void setRuntimeOverride(const QString& path) override { runtimeOverride = path; }
    QString loadedRuntimePath() const override { return runtimeOverride; }
    std::string version() override { return state.sdkVersion; }
    std::vector<ToupTekDevice> devices() override { check("devices"); return connected ? std::vector{state.camera.device} : std::vector<ToupTekDevice>{}; }
    void open(const std::string&) override { check("open"); ++opened; upsideDownOff = true; }
    ToupTekProfile profile() override
    {
        check("profile");
        // The black level's range follows the raw depth, as the SDK's does.
        for (auto& c : state.controls)
            if (c.kind == ToupTekControl::BlackLevel) c.maximum = state.current.format == PixelFormat::Mono16 ? 31 << 4 : 31;
        for (const auto& c : state.controls) state.current.controls[c.kind] = c.value;
        return state;
    }
    void configureRaw(const ToupTekSettings& settings) override
    {
        check("configure");
        state.current = settings;
        for (const auto& [kind, value] : settings.controls) setControl(kind, value);
    }
    void setControl(ToupTekControl kind, int64_t value) override
    {
        check("control");
        for (auto& c : state.controls) if (c.kind == kind) c.value = value;
        state.current.controls[kind] = value;
    }
    void startVideo() override { check("start"); ++starts; }
    ToupTekFrame read(std::span<uint8_t> destination, int waitMs) override
    {
        ++reads;
        if (waitMs < 1 || waitMs > 100) throw std::runtime_error("Unbounded SDK wait");
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        if (!connected || (disconnectAt >= 0 && delivered >= disconnectAt)) throw std::runtime_error("TEST ToupTek disconnected");
        if (notReady || (frameLimit >= 0 && delivered >= frameLimit)) return {};
        const auto& s = state.current;
        if (destination.size() != frameBytes(s.width, s.height, s.format)) throw std::runtime_error("Wrong SDK destination buffer size");
        for (size_t i = 0; i < static_cast<size_t>(s.width) * s.height; ++i)
        {
            if (s.format == PixelFormat::Mono8) destination[i] = static_cast<uint8_t>(80 + i % 31);
            else { const auto word = 3000 + i % 31; destination[2 * i] = static_cast<uint8_t>(word & 255); destination[2 * i + 1] = static_cast<uint8_t>(word >> 8); }
        }
        const int n = delivered++;
        ToupTekFrame frame{true, s.width, s.height, std::nullopt};
        // Past the given numbers, the camera keeps counting from the last one.
        if (!noSequence && sequences.empty()) frame.sequence = 1000u + static_cast<uint32_t>(n);
        else if (!noSequence && static_cast<size_t>(n) < sequences.size()) frame.sequence = sequences[n];
        else if (!noSequence) frame.sequence = sequences.back() + static_cast<uint32_t>(n + 1 - sequences.size());
        return frame;
    }
    void stopVideo() noexcept override { ++stops; }
    void close() noexcept override { ++closed; }

private:
    void check(const char* operation)
    {
        if (failOperation == operation) throw std::runtime_error(std::string("TEST ToupTek ") + operation + " failed");
    }
};
