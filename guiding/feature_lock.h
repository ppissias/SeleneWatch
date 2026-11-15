#pragma once

#include "frame.h"

#include <optional>
#include <string>
#include <vector>

// A rectangle in frame pixels, as the observer chose it.
struct LockRegion
{
    int x{0}, y{0}, width{0}, height{0};
};

// What the lockability check measured on the template, in its binned pixels.
struct LockQuality
{
    // Template standard deviation over the estimated pixel noise.
    double contrast{0};
    // Weaker over stronger gradient direction, after removing the noise's share.
    // Near 0 for a straight edge, near 1 for structure in every direction.
    double structureRatio{0};
    // Share of the region's pixels at the stream's highest code.
    double saturatedFraction{0};
    double noiseAdu{0};
};

// One search for the locked feature in a frame.
struct LockMatch
{
    // Where the feature is, relative to where it was locked, in frame pixels.
    // Positive is right and down.
    double dx{0}, dy{0};
    // Zero-mean normalised cross-correlation at the peak, from -1 to 1.
    double score{0};
    bool valid{false};
    // Why the match is not valid. Empty when it is.
    std::string reason;
};

// A lunar feature taken as a template, and the search for it in later frames.
//
// The template is the chosen region, binned by a whole factor so it is at
// most kMaxTemplateSide pixels on its longer side. A Bayer mosaic is always
// binned by an even factor of at least 2, so each binned pixel holds one full
// colour cell and the colour pattern does not enter the match. RGB frames use
// the mean of the three channels. Values are native ADU, so a Mono16 sample
// is read with its declared alignment.
//
// The match is a zero-mean normalised cross-correlation in plain loops, so it
// does not depend on gain, exposure, or bit depth. A parabola through the peak
// and its neighbours gives the sub-pixel position. Plain data: no thread, no
// clock, and the frames are only read.
class FeatureLock
{
public:
    static constexpr int kMaxTemplateSide = 96;
    static constexpr int kMinTemplateSide = 8;
    static constexpr double kMinLockScore = 0.6;
    // Contrast c is the template's standard deviation over the pixel noise.
    // With noise in the template and in the frame, a perfect match scores
    // about 1 - 1/c^2: 0.89 at c = 3, safely above kMinLockScore, while a
    // flat region measures c near 1.
    static constexpr double kMinContrast = 3.0;
    static constexpr double kMinStructureRatio = 0.1;
    static constexpr double kMaxSaturatedFraction = 0.5;

    // Takes the template from frame. Empty, with the reason in refusal, when
    // the region is outside the frame, too small, saturated, too flat, or has
    // structure in one direction only.
    [[nodiscard]] static std::optional<FeatureLock> take(const Frame& frame, LockRegion region, std::string& refusal);

    // Searches within radiusPixels of the expected offset. The match is valid
    // when the score reaches kMinLockScore and the peak is not on the edge of
    // the search window (the window edge includes the frame edge).
    [[nodiscard]] LockMatch find(const Frame& frame, double expectedDx, double expectedDy, int radiusPixels) const;

    // The region the template covers: the chosen one, trimmed to whole bins.
    [[nodiscard]] LockRegion region() const { return region_; }
    [[nodiscard]] int binning() const { return binning_; }
    [[nodiscard]] const LockQuality& quality() const { return quality_; }

    // True when frame has the layout the template was taken from.
    [[nodiscard]] bool sameLayout(const Frame& frame) const;

private:
    FeatureLock() = default;

    LockRegion region_;
    int binning_{1};
    int templateWidth_{0}, templateHeight_{0};
    int frameWidth_{0}, frameHeight_{0};
    PixelFormat format_{PixelFormat::Mono8};
    BayerPattern bayer_{BayerPattern::None};
    // Template minus its mean, row by row, and the sum of its squares.
    std::vector<float> template_;
    double energy_{0};
    LockQuality quality_;
};
