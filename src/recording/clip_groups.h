#pragma once

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QString>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

// Feature-46: what a clip shows, in one of four groups decided only by what
// was measured, never by what it might have been.
//
//  - Moving: three or more triggers, on three or more frames, advance
//    steadily along a straight line (fitted against time, leaving out at most
//    a fifth of them as strays). With fewer triggers, the activity the
//    overview followed around the peak moved. Usually an aircraft or a
//    satellite; on the Moon a bird, an aircraft or a satellite crossing.
//  - Brief: every trigger within 0.2 s, and no activity around the peak
//    lasting longer. A meteor, a glint, a cosmic-ray hit, one blink of a
//    strobe, a flash.
//  - Wide: a large share of the frame brightened or darkened at once.
//    Lightning, headlights or a torch, a cloud, a fireball's flare.
//  - Other: the rest, described by what was measured.
//
// A clip's group is written into event.txt and onto its overview when the
// clip is saved. The nightly report groups each clip again from event.txt
// with this same code, so clips saved by earlier versions are grouped the same
// way, and it never opens a SER to do so. The report then puts consecutive
// clips of one event together (clipsIntoEvents): a long aircraft crossing is
// saved as several clips of at most 20 s each.
//
// Times are the frames' source times. Positions are frame pixels.

enum class ClipGroup { Brief, Moving, Wide, Other };

// One trigger, as event.txt records it.
struct TriggerPoint
{
    enum class Change { Rise, Fall, WideRise, WideFall, Manual };
    Change change{Change::Rise};
    std::chrono::system_clock::time_point time;
    uint64_t frameIndex{0};
    // The patch's strongest pixel and its box. A wide change and a manual
    // save have neither.
    QPoint peak;
    QRect box;
    int pixels{0};
    double sigma{0};
    // A wide change: the share of the unmasked frame that changed.
    double share{0};
};

// The run of activity the overview followed around the clip's strongest
// frame (Feature-12): its frames, from first to last.
struct ActivityRun
{
    // Zero when not known (clips saved before Feature-46).
    int frames{0};
    double seconds{0};
    // How far it moved from its first frame to its last; zero unless it moved
    // over three frames or more.
    double movedPixels{0};
};

struct ClipDescription
{
    ClipGroup group{ClipGroup::Other};
    // What was measured, as a phrase without the group's name.
    QString facts;
};

// "brief", "moving", "wide" or "other".
[[nodiscard]] QString groupName(ClipGroup group);
// "Brief", ...: the name as a heading.
[[nodiscard]] QString groupTitle(ClipGroup group);

// The group of one clip from its triggers and, when the overview measured
// it, the run around its peak. arcsecPerPixel adds speeds in degrees.
[[nodiscard]] ClipDescription describeClip(const std::vector<TriggerPoint>& triggers,
    const std::optional<ActivityRun>& run = {}, std::optional<double> arcsecPerPixel = {});

// A clip as the report sees it when putting clips together.
struct ClipSpan
{
    std::vector<TriggerPoint> triggers;
    // The clip stopped at its length or memory limit while collecting.
    bool cut{false};
    // The time of its last frame.
    std::optional<std::chrono::system_clock::time_point> end;
};

// Puts consecutive clips, given in order of their first trigger, together
// when they show one event: a clip that starts within 2 s of the end of a
// clip that was cut continues it; and a clip that starts within 60 s of the
// event's last trigger joins it when all their triggers advance along one
// straight line and none of the new clip's leave it. Each event is the
// indices of its clips, in order.
[[nodiscard]] std::vector<std::vector<size_t>> clipsIntoEvents(const std::vector<ClipSpan>& clips);

// The group of an event of several clips, from all their triggers. members
// are the clips' own descriptions, in order; one member is returned as it is.
[[nodiscard]] ClipDescription describeEvent(const std::vector<ClipSpan>& clips,
    const std::vector<ClipDescription>& members, std::optional<double> arcsecPerPixel = {});
