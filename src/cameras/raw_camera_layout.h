#pragma once
#include "core/frame.h"
#include <algorithm>
#include <iterator>

// Translate a sensor's Bayer cell to the actual unbinned ROI origin. Binned
// colour needs a camera-specific contract and is deliberately not inferred.
inline BayerPattern roiBayerPattern(BayerPattern pattern, int x, int y)
{
    const BayerPattern cells[]{BayerPattern::RGGB, BayerPattern::GRBG, BayerPattern::GBRG, BayerPattern::BGGR};
    const auto found=std::find(std::begin(cells),std::end(cells),pattern);
    if (found==std::end(cells)) return BayerPattern::None;
    return cells[(found-std::begin(cells)) ^ (x&1) ^ ((y&1)<<1)];
}
