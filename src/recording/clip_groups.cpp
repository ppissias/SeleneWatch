#include "recording/clip_groups.h"

#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace {

using Clock = std::chrono::system_clock;

// Everything within this long is brief: a few frames at video rates.
constexpr double kBriefSeconds = 0.2;
// A track is at least this long, and its triggers stay on average within the
// larger of kTrackJitter and kTrackTolerance of its length from the line.
constexpr double kMinTrackPixels = 15;
constexpr double kTrackJitter = 3;
constexpr double kTrackTolerance = 0.05;
// At most this share of the triggers may be strays off a track.
constexpr double kStrayShare = 0.2;
// Positions closer than this are one place.
constexpr double kOnePlacePixels = 10;
// A clip that was cut is continued by one that starts this soon after it.
constexpr double kContinuationSeconds = 1;
// A clip joins an event's track when it starts this soon after its last trigger.
constexpr double kMaxTrackGapSeconds = 60;

double secondsOf(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

bool isPatch(const TriggerPoint& trigger)
{
    return trigger.change == TriggerPoint::Change::Rise || trigger.change == TriggerPoint::Change::Fall;
}

bool isWide(const TriggerPoint& trigger)
{
    return trigger.change == TriggerPoint::Change::WideRise || trigger.change == TriggerPoint::Change::WideFall;
}

// The middle of the patch's box, steadier than its single brightest pixel:
// the middle pixel of the box's first and last.
QPointF centreOf(const TriggerPoint& trigger)
{
    if (!trigger.box.isValid()) return QPointF(trigger.peak);
    return {trigger.box.x() + (trigger.box.width() - 1) / 2.0, trigger.box.y() + (trigger.box.height() - 1) / 2.0};
}

double distance(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

size_t framesOf(const std::vector<const TriggerPoint*>& triggers)
{
    std::set<Clock::rep> times;
    for (const TriggerPoint* trigger : triggers) times.insert(trigger->time.time_since_epoch().count());
    return times.size();
}

double spanOf(const std::vector<const TriggerPoint*>& triggers)
{
    if (triggers.empty()) return 0;
    const auto [first, last] = std::minmax_element(triggers.begin(), triggers.end(),
        [](const TriggerPoint* a, const TriggerPoint* b) { return a->time < b->time; });
    return secondsOf((*last)->time - (*first)->time);
}

QString timeText(double seconds)
{
    if (seconds >= 60)
    {
        const qint64 whole = std::llround(seconds);
        return QString("%1 min %2 s").arg(whole / 60).arg(whole % 60);
    }
    return QString("%1 s").arg(seconds, 0, 'f', seconds < 1 ? 3 : 1);
}

QString pointText(QPointF point)
{
    return QString("(%1, %2)").arg(std::lround(point.x())).arg(std::lround(point.y()));
}

QString speedText(double pixelsPerSecond, std::optional<double> arcsecPerPixel)
{
    QString text = QString("%1 px/s").arg(pixelsPerSecond, 0, 'f', pixelsPerSecond < 10 ? 1 : 0);
    if (arcsecPerPixel) text += QString(" (%1°/s)").arg(pixelsPerSecond * *arcsecPerPixel / 3600, 0, 'f', 2);
    return text;
}

// A straight track: the triggers lie along one line and advance along it as
// time goes on. The speed may change: an aircraft heading for the horizon
// slows down in the frame.
struct Track
{
    // A point on the line, and the unit direction the triggers went.
    QPointF centre, direction;
    double tolerance{0};
    double duration{0};
    // The first and the last trigger in time.
    QPointF from, to;
    // Which of the triggers given lie on it; strays and wide changes do not.
    std::vector<bool> kept;
    size_t used{0}, of{0};

    double along(QPointF p) const { const QPointF v = p - centre; return v.x() * direction.x() + v.y() * direction.y(); }
    double offset(QPointF p) const { const QPointF v = p - centre; return std::abs(v.x() * direction.y() - v.y() * direction.x()); }
    // Along the line, from the first trigger to the last.
    double speed() const { return duration > 0 ? (along(to) - along(from)) / duration : 0; }
};

// Fits a line through the patch triggers, leaving out the worst one at a
// time, up to kStrayShare of them, until the rest lie along it: on average
// within the tolerance, and never going back by more than twice it. Needs
// three frames.
std::optional<Track> straightTrack(const std::vector<TriggerPoint>& triggers)
{
    std::vector<size_t> points;
    for (size_t i = 0; i < triggers.size(); ++i)
        if (isPatch(triggers[i])) points.push_back(i);
    const size_t of = points.size();
    size_t strays = static_cast<size_t>(std::floor(kStrayShare * static_cast<double>(of)));
    // In time order, so going back along the line can be seen.
    std::stable_sort(points.begin(), points.end(), [&](size_t a, size_t b) { return triggers[a].time < triggers[b].time; });
    const auto framesIn = [&](const std::vector<size_t>& chosen) {
        std::set<Clock::rep> times;
        for (size_t i : chosen) times.insert(triggers[i].time.time_since_epoch().count());
        return times.size();
    };
    while (framesIn(points) >= 3)
    {
        Track track;
        const double n = static_cast<double>(points.size());
        double xm = 0, ym = 0, tm = 0;
        const auto origin = triggers[points.front()].time;
        for (size_t i : points)
        {
            const QPointF c = centreOf(triggers[i]);
            xm += c.x();
            ym += c.y();
            tm += secondsOf(triggers[i].time - origin);
        }
        track.centre = QPointF(xm / n, ym / n);
        tm /= n;
        double sxx = 0, syy = 0, sxy = 0;
        for (size_t i : points)
        {
            const QPointF v = centreOf(triggers[i]) - track.centre;
            sxx += v.x() * v.x();
            syy += v.y() * v.y();
            sxy += v.x() * v.y();
        }
        const double angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
        track.direction = QPointF(std::cos(angle), std::sin(angle));
        // Point the line the way the triggers went.
        double progress = 0;
        for (size_t i : points) progress += track.along(centreOf(triggers[i])) * (secondsOf(triggers[i].time - origin) - tm);
        if (progress < 0) track.direction = -track.direction;

        double low = std::numeric_limits<double>::max(), high = -low, squares = 0;
        for (size_t i : points)
        {
            const QPointF c = centreOf(triggers[i]);
            low = std::min(low, track.along(c));
            high = std::max(high, track.along(c));
            squares += track.offset(c) * track.offset(c);
        }
        const double length = high - low;
        track.tolerance = std::max(kTrackJitter, kTrackTolerance * length);
        // How far each trigger is off the line, or back from the furthest
        // point reached before it.
        double furthest = -std::numeric_limits<double>::max(), worstScore = -1, back = 0;
        size_t worst = 0;
        for (size_t k = 0; k < points.size(); ++k)
        {
            const QPointF c = centreOf(triggers[points[k]]);
            const double behind = std::max(0.0, furthest - track.along(c));
            back = std::max(back, behind);
            const double score = std::max(track.offset(c), behind);
            if (score > worstScore) { worstScore = score; worst = k; }
            furthest = std::max(furthest, track.along(c));
        }
        if (length >= kMinTrackPixels && std::sqrt(squares / n) <= track.tolerance && back <= 2 * track.tolerance)
        {
            const QPointF first = centreOf(triggers[points.front()]), last = centreOf(triggers[points.back()]);
            track.from = first;
            track.to = last;
            track.duration = secondsOf(triggers[points.back()].time - origin);
            track.kept.assign(triggers.size(), false);
            for (size_t i : points) track.kept[i] = true;
            track.used = points.size();
            track.of = of;
            return track;
        }
        if (strays == 0) return std::nullopt;
        points.erase(points.begin() + static_cast<std::ptrdiff_t>(worst));
        --strays;
    }
    return std::nullopt;
}

// "53 triggers on 53 frames over 19.6 s", or "1 trigger".
QString countText(const std::vector<const TriggerPoint*>& triggers, bool darkening)
{
    const QString noun = darkening ? "darkening trigger" : "trigger";
    if (triggers.size() == 1) return "1 " + noun;
    const size_t frames = framesOf(triggers);
    QString text = QString("%1 %2s on %3 %4").arg(triggers.size()).arg(noun).arg(frames).arg(frames == 1 ? "frame" : "frames");
    if (frames > 1) text += " over " + timeText(spanOf(triggers));
    return text;
}

QString patchText(const TriggerPoint& trigger)
{
    QString text = QString(", %1 px").arg(trigger.pixels);
    if (trigger.box.isValid()) text += QString(" in a %1 × %2 box").arg(trigger.box.width()).arg(trigger.box.height());
    return text + " at " + pointText(trigger.peak);
}

QString runText(const ActivityRun& run)
{
    if (run.frames <= 0) return QString("active for %1").arg(timeText(run.seconds));
    if (run.frames == 1) return "active on 1 frame";
    return QString("active on %1 frames over %2").arg(run.frames).arg(timeText(run.seconds));
}

// The run alone: a manual save, or a picture made without triggers.
ClipDescription describeRun(const std::optional<ActivityRun>& run, bool manual, std::optional<double> arcsecPerPixel)
{
    const QString prefix = manual ? "saved by hand; " : QString();
    if (!run) return {ClipGroup::Other, manual ? "saved by hand, with no automatic trigger" : "no trigger was recorded"};
    if (run->movedPixels > 0)
        return {ClipGroup::Moving, prefix + QString("the activity around the peak moved %1 px over %2 at %3")
            .arg(std::lround(run->movedPixels)).arg(timeText(run->seconds))
            .arg(speedText(run->seconds > 0 ? run->movedPixels / run->seconds : 0, arcsecPerPixel))};
    if (run->seconds <= kBriefSeconds) return {ClipGroup::Brief, prefix + runText(*run) + " in one place"};
    return {ClipGroup::Other, prefix + QString("activity in one place for %1").arg(timeText(run->seconds))};
}

} // namespace

QString groupName(ClipGroup group)
{
    switch (group)
    {
    case ClipGroup::Brief: return "brief";
    case ClipGroup::Moving: return "moving";
    case ClipGroup::Wide: return "wide";
    case ClipGroup::Other: break;
    }
    return "other";
}

QString groupTitle(ClipGroup group)
{
    QString name = groupName(group);
    name[0] = name[0].toUpper();
    return name;
}

ClipDescription describeClip(const std::vector<TriggerPoint>& triggers, const std::optional<ActivityRun>& run,
    std::optional<double> arcsecPerPixel)
{
    std::vector<const TriggerPoint*> automatic, patches;
    bool manual = false, wideRise = false, wideFall = false, allFalls = true;
    double share = 0, strongest = 0;
    for (const TriggerPoint& trigger : triggers)
    {
        if (trigger.change == TriggerPoint::Change::Manual) { manual = true; continue; }
        automatic.push_back(&trigger);
        if (isWide(trigger))
        {
            wideRise = wideRise || trigger.change == TriggerPoint::Change::WideRise;
            wideFall = wideFall || trigger.change == TriggerPoint::Change::WideFall;
            share = std::max(share, trigger.share);
            continue;
        }
        patches.push_back(&trigger);
        allFalls = allFalls && trigger.change == TriggerPoint::Change::Fall;
        strongest = std::max(strongest, trigger.sigma);
    }
    const QString strongestText = strongest > 0 ? QString(", strongest %1 σ").arg(strongest, 0, 'f', 1) : QString();
    const bool darkening = !patches.empty() && allFalls;

    if (wideRise || wideFall)
    {
        const QString what = wideRise && wideFall ? "a large brightening and darkening"
            : wideRise ? "a large brightening" : "a large darkening";
        const QString percent = QString::number(100 * share, 'f', share < 0.1 ? 1 : 0);
        return {ClipGroup::Wide, QString("%1 over %2% of the frame; %3%4").arg(what, percent, countText(automatic, false), strongestText)};
    }
    if (patches.empty()) return describeRun(run, manual, arcsecPerPixel);

    const QString count = countText(patches, darkening);
    if (const auto track = straightTrack(triggers))
    {
        QString text = QString("%1, along a straight line from %2 to %3 at %4")
            .arg(count, pointText(track->from), pointText(track->to), speedText(track->speed(), arcsecPerPixel));
        if (track->used < track->of) text += QString(" (%1 of the %2 triggers)").arg(track->used).arg(track->of);
        return {ClipGroup::Moving, text + strongestText};
    }
    if (patches.size() < 3 && run && run->movedPixels > 0)
        return {ClipGroup::Moving, count + "; " + describeRun(run, false, arcsecPerPixel).facts + strongestText};

    const double span = spanOf(patches);
    if (span <= kBriefSeconds && (!run || (run->seconds <= kBriefSeconds && run->movedPixels <= 0)))
    {
        QString text = count;
        if (patches.size() == 1) text += patchText(*patches.front());
        else
        {
            double apart = 0;
            for (const TriggerPoint* a : patches)
                for (const TriggerPoint* b : patches) apart = std::max(apart, distance(centreOf(*a), centreOf(*b)));
            text += apart < kOnePlacePixels ? QString(" in one place") : QString(", up to %1 px apart").arg(std::lround(apart));
        }
        if (run) text += ", " + runText(*run);
        return {ClipGroup::Brief, text + strongestText};
    }

    QString text = count;
    if (patches.size() == 1)
    {
        text += patchText(*patches.front());
        if (run) text += QString(", active in one place for %1").arg(timeText(run->seconds));
    }
    else
    {
        double left = std::numeric_limits<double>::max(), top = left, right = -left, bottom = -left;
        for (const TriggerPoint* p : patches)
        {
            const QPointF c = centreOf(*p);
            left = std::min(left, c.x()); right = std::max(right, c.x());
            top = std::min(top, c.y()); bottom = std::max(bottom, c.y());
        }
        if (right - left < kOnePlacePixels && bottom - top < kOnePlacePixels) text += " in one place";
        else
        {
            text += QString(", spread over %1 × %2 px").arg(std::lround(right - left)).arg(std::lround(bottom - top));
            if (framesOf(patches) >= 3) text += ", not along a straight line";
        }
    }
    return {ClipGroup::Other, text + strongestText};
}

std::vector<std::vector<size_t>> clipsIntoEvents(const std::vector<ClipSpan>& clips)
{
    std::vector<std::vector<size_t>> events;
    std::vector<TriggerPoint> current;
    const auto earliest = [](const std::vector<TriggerPoint>& triggers) {
        return std::min_element(triggers.begin(), triggers.end(),
            [](const TriggerPoint& a, const TriggerPoint& b) { return a.time < b.time; })->time;
    };
    const auto latest = [](const std::vector<TriggerPoint>& triggers) {
        return std::max_element(triggers.begin(), triggers.end(),
            [](const TriggerPoint& a, const TriggerPoint& b) { return a.time < b.time; })->time;
    };
    for (size_t i = 0; i < clips.size(); ++i)
    {
        const ClipSpan& clip = clips[i];
        bool join = false;
        if (!events.empty() && !clip.triggers.empty() && !current.empty())
        {
            const auto start = earliest(clip.triggers);
            const ClipSpan& previous = clips[events.back().back()];
            const double sinceLast = secondsOf(start - latest(current));
            if (previous.cut && previous.end && std::abs(secondsOf(start - *previous.end)) <= kContinuationSeconds)
                join = true;
            else if (sinceLast >= 0 && sinceLast <= kMaxTrackGapSeconds
                && std::any_of(clip.triggers.begin(), clip.triggers.end(), isPatch))
            {
                // One line through both, which leaves out no trigger that
                // the event or the clip keeps on a line of its own.
                std::vector<TriggerPoint> both = current;
                both.insert(both.end(), clip.triggers.begin(), clip.triggers.end());
                if (const auto track = straightTrack(both))
                {
                    const auto strays = [](const std::vector<TriggerPoint>& triggers) {
                        const auto own = straightTrack(triggers);
                        return own ? own->of - own->used : size_t{0};
                    };
                    size_t eventLeft = 0, clipLeft = 0;
                    for (size_t t = 0; t < both.size(); ++t)
                        if (isPatch(both[t]) && !track->kept[t]) ++(t < current.size() ? eventLeft : clipLeft);
                    join = eventLeft <= strays(current) && clipLeft <= strays(clip.triggers);
                }
            }
        }
        if (join)
        {
            events.back().push_back(i);
            current.insert(current.end(), clip.triggers.begin(), clip.triggers.end());
        }
        else
        {
            events.push_back({i});
            current = clip.triggers;
        }
    }
    return events;
}

ClipDescription describeEvent(const std::vector<ClipSpan>& clips, const std::vector<ClipDescription>& members,
    std::optional<double> arcsecPerPixel)
{
    if (members.size() == 1) return members.front();
    std::vector<TriggerPoint> all;
    for (const ClipSpan& clip : clips) all.insert(all.end(), clip.triggers.begin(), clip.triggers.end());
    ClipDescription event = describeClip(all, {}, arcsecPerPixel);
    const auto any = [&members](ClipGroup group) {
        return std::any_of(members.begin(), members.end(), [group](const ClipDescription& m) { return m.group == group; });
    };
    // Each clip moved, but not along one line together: still a moving light.
    if ((event.group == ClipGroup::Other || event.group == ClipGroup::Brief) && any(ClipGroup::Moving))
        event.group = ClipGroup::Moving;
    return event;
}
