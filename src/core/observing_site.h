#pragma once

#include <optional>
#include <string>

// Who observed, from where, and with what (Feature-14 Part C). The observer
// enters it once; every clip, recording and session_start line records it
// (Evidence rule 8). Empty text and missing numbers mean not entered.
struct ObservingSite
{
    std::string observer;
    std::string site;
    // Degrees, north and east positive; metres above sea level.
    std::optional<double> latitudeDeg;
    std::optional<double> longitudeDeg;
    std::optional<double> heightM;

    std::string telescope;
    // Zero when not entered.
    double apertureMm{0};
    double focalLengthMm{0};
    // The camera's pixel as imaged: with binning, the binned pixel.
    double pixelSizeUm{0};

    [[nodiscard]] bool hasPosition() const { return latitudeDeg.has_value() && longitudeDeg.has_value(); }

    // Arcseconds per pixel, from the focal length and the pixel size.
    [[nodiscard]] std::optional<double> arcsecPerPixel() const
    {
        if (focalLengthMm <= 0 || pixelSizeUm <= 0) return std::nullopt;
        return 206.264806 * pixelSizeUm / focalLengthMm;
    }

    bool operator==(const ObservingSite&) const = default;
};
