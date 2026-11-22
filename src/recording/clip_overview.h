#pragma once

#include "detection/detection_mask.h"
#include "core/frame.h"

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QImage>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

// Feature-12: one picture that shows what happened in a clip.
//
// Activity is measured from the clip's own frames with the detector's
// per-pixel model: a running mean and variance with the detector's time
// constant, and a noise floor from a robust scale of each frame's
// difference. A pixel is active on a frame when it rises or falls by at
// least K times its noise. Unlike the detector, nothing is held from
// learning; the picture only has to show the change.
//
// The picture is a display image, never evidence. It has a main view (the
// stretched peak frame, rises orange and falls blue over it, each trigger
// marked), a strip of enlarged crops that follows the activity, and a
// caption.

// What the caller knows about the clip beyond its frames.
struct OverviewRequest
{
    // The detector's model and K.
    double sigma{6};
    double referenceTimeConstantSeconds{2};
    double minNoiseAdu{0.5};
    // A frame is active when at least this many of its pixels changed by K.
    int minPixels{8};

    // Learning ends one second into the clip, or at this frame if sooner.
    std::optional<uint64_t> onsetFrameIndex;
    // The detector's strongest frame. Without it the main view is the frame
    // with the strongest change, or the onset frame when nothing changed.
    std::optional<uint64_t> peakFrameIndex;
    // The strip's times count from here, normally the first trigger.
    std::optional<std::chrono::system_clock::time_point> referenceTime;
    // Trigger boxes in frame pixels, marked and numbered in this order.
    std::vector<QRect> marks;
    // Areas the detector leaves out (Feature-11): not measured, and hatched.
    DetectionMask mask;
    // The image scale, when the site and instrument give it (Feature-14):
    // speeds are then also given in degrees per second.
    std::optional<double> arcsecPerPixel;
    // A large brightening fired in the clip (Feature-11), for the suggested kind.
    bool largeBrightening{false};
    // Lines about the clip, printed above the line the overview adds.
    QStringList caption;
};

// The picture, and what was found to draw it.
struct ClipOverview
{
    QImage image;
    uint64_t mainFrameIndex{0};

    // Frames with activity anywhere in the clip, and the pixels that changed
    // by K on any frame.
    int activeFrames{0};
    size_t changedPixels{0};
    // The smallest box around every changed pixel, in frame pixels.
    QRect changedBounds;

    // The strip: the frames shown, and the centre of each crop in frame
    // pixels. Empty when no activity was found.
    std::vector<uint64_t> stripFrames;
    std::vector<QPoint> stripCentres;

    // How far the activity around the main frame moved, from its first to
    // its last frame, in pixels and pixels per second. Zero for a flash.
    double movedX{0}, movedY{0}, speed{0};

    // The caption line describing the activity.
    QString activity;

    // Position indices into the input `frames` vector for the detected main event
    // run, used to extract a compact animated preview around the peak event.
    size_t mainPosition{0};
    size_t firstActivePosition{0};
    size_t lastActivePosition{0};
    bool hasActiveRun{false};

    // Feature-15 Part C: what the clip probably shows, and why. One of
    // flash, meteor, satellite, aircraft, dark transit, lightning, seeing,
    // unclear. A suggestion for review, not a decision.
    QString suggestedKind;
    QString kindReason;
};

// frames must not be empty and share one layout. Throws MemoryLimitError
// when the budget refuses the per-pixel buffers or the picture.
[[nodiscard]] ClipOverview makeClipOverview(const std::vector<FramePtr>& frames,
    const OverviewRequest& request, MemoryBudget budget = {});

// Selects up to maxFrames (default 10) sorted unique indices into clip frames
// spanning [firstActive - paddingBefore .. lastActive + paddingAfter] while
// always keeping the outer context boundaries and the peak frame.
[[nodiscard]] std::vector<size_t> selectEventGifFramePositions(size_t frameCount,
    size_t firstActive, size_t peak, size_t lastActive,
    size_t paddingBefore = 2, size_t paddingAfter = 2, size_t maxFrames = 10);
