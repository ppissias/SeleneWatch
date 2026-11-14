#pragma once

#include "frame.h"

#include <vector>

struct FrameTranslation
{
    int dx{0}, dy{0};
    double correlation{0}, peakGap{0};
    bool valid{false};
};

// A native-sample copy of the common interior, explicitly marked TEST.
// Positive shifts read farther right/down in the current source frame.
[[nodiscard]] FramePtr registrationInterior(const FramePtr& source, int margin, int dx, int dy);

// Manual registration experiment only, not on the application's frame path.
// Estimates integer translation relative to one fixed frame using normalized
// cross correlation: 8x8 box-averaged coarse search, then sparse native-pixel
// refinement. A common interior excludes all potentially exposed borders.
class TranslationEstimator
{
public:
    explicit TranslationEstimator(const Frame& reference, int maxShift = 48);
    [[nodiscard]] FrameTranslation estimate(const Frame& frame);
    [[nodiscard]] int margin() const { return margin_; }
private:
    struct Point { int x, y; float reference; };
    struct Grid { std::vector<Point> points; double energy{0}; };
    static Grid grid(const std::vector<float>& image, int width, int height, int margin, int step);
    static double score(const Grid& reference, const std::vector<float>& image, int width, int dx, int dy);
    void read(const Frame& frame);
    int width_, height_, bits_, maxShift_, margin_;
    PixelFormat format_;
    BitAlignment alignment_;
    Grid coarse_, fine_;
    std::vector<float> gray_, binned_;
};
