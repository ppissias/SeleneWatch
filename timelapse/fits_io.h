#pragma once

#include <QtCore/QString>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct FitsMetadata
{
    std::chrono::system_clock::time_point dateObs{};
    std::chrono::system_clock::time_point dateAvg{};
    std::chrono::system_clock::time_point dateEnd{};
    double exptimeSeconds{0};
    double gain{0};
    bool hasGain{false};
    std::string camera;
    std::string telescope;
    std::string observer;
    std::string siteName;
    std::string targetPreset;
    uint64_t framesCaptured{0};
    uint64_t framesKept{0};
    double qualityScore{0};
    double shiftX{0};
    double shiftY{0};
    double rotationDeg{0};
    std::string sharpeningProfile{"none"};
    std::string softwareVersion;
    double rawMaxAdu{255.0};
};

// Planar 32-bit float image: channels == 1 (NAXIS=2) or 3 (NAXIS=3, R then G then B),
// normalized to [0.0, 1.0].
struct FitsImage
{
    int width{0};
    int height{0};
    int channels{1};
    std::vector<float> pixels;
    FitsMetadata meta;

    [[nodiscard]] bool valid() const
    {
        return width > 0 && height > 0 && (channels == 1 || channels == 3)
            && pixels.size() == static_cast<size_t>(width) * height * channels;
    }
};

bool writeFitsFloat(const QString& path, const FitsImage& image, QString& error);
bool readFitsFloat(const QString& path, FitsImage& image, QString& error);
