#include "nightly_report.h"
#include "nightly_report_style.h"

#include "recording/clip_groups.h"
#include "recording/clip_overview.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QTextStream>
#include <QtCore/QTimeZone>
#include <QtCore/QUrl>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
using Fields = QMap<QString, QString>;
using Clock = std::chrono::system_clock;

QString field(const QString& line, const QString& key)
{
    const QString token = " " + key + "=";
    const auto pos = line.indexOf(token);
    if (pos < 0) return {};
    const auto start = pos + token.size();
    if (line.mid(start, 1) == "\"") return line.mid(start + 1).section('"', 0, 0);
    return line.mid(start).section(' ', 0, 0);
}

QString h(const QString& text) { return text.toHtmlEscaped(); }
QString url(const QString& path) { return QString::fromLatin1(QUrl::toPercentEncoding(path, "/")); }

QString link(const QDir& root, const QString& file, const QString& title)
{
    if (!QFileInfo(root.filePath(file)).isFile()) return {};
    return "<a href=\"" + url(file) + "\" target=\"_blank\" rel=\"noopener\">" + h(title) + "</a> ";
}

// Feature-46: a link the report's script turns into selenewatch:open or
// selenewatch:show, so the file opens in its own program instead of being
// downloaded. Without the script it stays a plain link.
QString openLink(const QDir& root, const QString& file, const QString& action, const QString& title)
{
    if (!QFileInfo(root.filePath(file)).isFile()) return {};
    const QString tip = action == "show" ? "Shows the file in Explorer, through SeleneWatch. The browser asks first."
                                         : "Opens the file with its program, through SeleneWatch. The browser asks first.";
    return "<a data-open='" + action + "' title='" + tip + "' href=\"" + url(file) + "\">" + h(title) + "</a> ";
}

std::optional<Clock::time_point> timePoint(const QString& iso)
{
    const QDateTime date = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!date.isValid()) return std::nullopt;
    return Clock::time_point(std::chrono::milliseconds(date.toMSecsSinceEpoch()));
}

struct Clip
{
    QString name, path;
    Fields info;
    QStringList triggers;
    // The time on the frame ledger's last line.
    QString lastFrameTime;
};

// Event metadata can contain a long per-frame ledger. Keep only named fields,
// trigger rows and the last frame's time; generating a report never opens a
// raw image or SER.
Clip inspect(const QDir& root, const QString& path, const QString& metadata)
{
    Clip clip;
    clip.name = QFileInfo(path).fileName();
    clip.path = path;
    QFile file(root.filePath(path + "/" + metadata));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return clip;
    QTextStream in(&file);
    while (!in.atEnd())
    {
        const QString line = in.readLine();
        if (!line.isEmpty() && line[0].isDigit())
        {
            clip.lastFrameTime = line.section(' ', 2, 2);
            continue;
        }
        const auto colon = line.indexOf(": ");
        if (colon < 1 || colon > 48 || !line[0].isLetter()) continue;
        const QString key = line.left(colon), value = line.mid(colon + 2);
        if (key == "trigger") clip.triggers << value;
        else clip.info.insert(key, value);
    }
    return clip;
}

QString timeOf(const Clip& clip)
{
    if (!clip.triggers.isEmpty()) return field(" " + clip.triggers.front(), "trigger_time");
    return {};
}

QString duration(double seconds)
{
    const qint64 n = qRound64(seconds);
    return QString("%1h %2m %3s").arg(n / 3600).arg(n / 60 % 60).arg(n % 60);
}

// "(291,2)" or "(290,0,7,8)" as numbers.
QList<int> numbers(const QString& text)
{
    QList<int> values;
    for (const QString& part : QString(text).remove('(').remove(')').split(',', Qt::SkipEmptyParts)) values << part.toInt();
    return values;
}

// A trigger line of event.txt, as the clip's group sees it: the fields that
// event_writer's triggerFields writes.
TriggerPoint pointOf(const QString& trigger)
{
    const QString line = " " + trigger;
    TriggerPoint point;
    point.time = timePoint(field(line, "trigger_time")).value_or(Clock::time_point{});
    point.frameIndex = field(line, "trigger_frame_index").toULongLong();
    const QString kind = field(line, "kind");
    if (kind == "manual") point.change = TriggerPoint::Change::Manual;
    else if (kind == "darkening") point.change = TriggerPoint::Change::Fall;
    else if (kind == "brightening") point.change = TriggerPoint::Change::WideRise;
    else if (kind == "large_darkening") point.change = TriggerPoint::Change::WideFall;
    if (const QList<int> peak = numbers(field(line, "peak")); peak.size() == 2) point.peak = QPoint(peak[0], peak[1]);
    if (const QList<int> box = numbers(field(line, "box")); box.size() == 4 && box[2] > 0 && box[3] > 0)
        point.box = QRect(box[0], box[1], box[2], box[3]);
    point.pixels = field(line, "bright_pixel_count").toInt();
    point.sigma = field(line, "peak_sigma").toDouble();
    point.share = field(line, "share").toDouble();
    return point;
}

// The overview line's values, without its closing remark.
QString overviewField(const Clip& clip, const QString& key)
{
    return field(" " + clip.info.value("overview").section(';', 0, 0), key);
}

std::optional<double> arcsecPerPixelOf(const Clip& clip)
{
    bool ok = false;
    const double value = clip.info.value("image_scale_arcsec_per_pixel").toDouble(&ok);
    return ok && value > 0 ? std::optional<double>(value) : std::nullopt;
}

// Clips saved before Feature-46 give the run around the peak only in the
// words of Feature-15's suggested kind, such as "a rise in place, 2 frames
// over 0.033 s" or "a steady light moving straight at 0.04 frame widths/s for
// 13.7 s". Nothing when those words say nothing about a run.
std::optional<ActivityRun> legacyRunOf(const Clip& clip)
{
    const QString reason = clip.info.value("suggested_kind").section(';', 0, 0);
    static const QRegularExpression inPlace(R"((\d+) frames? over ([\d.]+) s)");
    static const QRegularExpression still(R"(in place for ([\d.]+) s)");
    static const QRegularExpression moving(R"(([\d.]+) (frame widths|°)/s for ([\d.]+) s)");
    if (const auto m = inPlace.match(reason); m.hasMatch()) return ActivityRun{m.captured(1).toInt(), m.captured(2).toDouble(), 0};
    if (const auto m = still.match(reason); m.hasMatch()) return ActivityRun{0, m.captured(1).toDouble(), 0};
    if (const auto m = moving.match(reason); m.hasMatch())
    {
        const double rate = m.captured(1).toDouble(), seconds = m.captured(3).toDouble();
        const int width = clip.info.value("frame_size").section('x', 0, 0).toInt();
        const std::optional<double> scale = arcsecPerPixelOf(clip);
        const double pixelsPerSecond = m.captured(2) == "frame widths" ? rate * width : scale ? rate * 3600 / *scale : 0;
        // A run that moved at all moved at least the overview's 3 px.
        if (pixelsPerSecond > 0) return ActivityRun{0, seconds, std::max(3.0, pixelsPerSecond * seconds)};
    }
    return std::nullopt;
}

// The run around the peak: recorded on the overview line since Feature-46.
std::optional<ActivityRun> runOf(const Clip& clip)
{
    const QString frames = overviewField(clip, "run_frames");
    if (frames.isEmpty()) return legacyRunOf(clip);
    return ActivityRun{frames.toInt(), overviewField(clip, "run_seconds").toDouble(), overviewField(clip, "run_moved_px").toDouble()};
}

ClipSpan spanOf(const Clip& clip)
{
    ClipSpan span;
    for (const QString& trigger : clip.triggers) span.triggers.push_back(pointOf(trigger));
    const QString end = clip.info.value("collection_end");
    span.cut = end == "maximum post-event interval" || end == "clip memory allowance";
    span.end = timePoint(clip.lastFrameTime);
    return span;
}

struct Journal
{
    QStringList starts, changes, exceptions, pending;
    QMap<QString, int> outcomes;
    int automatic = 0, manual = 0;
};

QString triggerKey(const QString& line)
{
    return field(line, "record_id") + "|" + field(line, "trigger_frame_index") + "|" + field(line, "kind");
}

Journal journal(QFile& file)
{
    Journal j;
    QTextStream in(&file);
    while (!in.atEnd())
    {
        const QString line = in.readLine();
        const QString kind = line.section(' ', 1, 1);
        if (kind == "session_start") j.starts << line;
        else if (kind == "exposure_change" || kind.startsWith("recording_")) j.changes << line;
        else if (kind == "candidate_pending" || kind == "manual_pending") j.pending << line;
        else if (line.contains(" not_saved=") || line.contains(" clip="))
        {
            ++j.outcomes[triggerKey(line)];
            if (field(line, "kind") == "manual") ++j.manual; else ++j.automatic;
            if (line.contains(" not_saved="))
                j.exceptions << line.section(' ', 0, 0) + " · " + line.section(" not_saved=", 1);
        }
    }
    for (const QString& line : j.pending)
    {
        auto& matches = j.outcomes[triggerKey(line)];
        if (matches > 0) { --matches; continue; }
        if (field(line, "kind") == "manual") ++j.manual; else ++j.automatic;
        j.exceptions << line.section(' ', 0, 0) + " · outcome unknown";
    }
    return j;
}

QString readableTime(const QString& iso)
{
    const QDateTime date = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return date.isValid() ? date.toUTC().toString("dd MMM yyyy, HH:mm:ss.zzz") : iso;
}

QString shortTime(const QString& iso, const QString& format = "HH:mm:ss")
{
    const QDateTime date = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return date.isValid() ? date.toUTC().toString(format) : iso;
}

QString activity(const QString& line)
{
    const QString action = line.section(' ', 1, 1);
    QString description;
    if (action == "monitoring_start") description = "Monitoring began";
    else if (action == "monitoring_stop") description = "Monitoring ended (" + line.section(" reason=", 1) + ")";
    else if (action == "recording_start") description = "Continuous recording began (" + field(line,"started_by") + ")";
    else if (action == "recording_stop") description = "Continuous recording ended (" + field(line,"ended") + ")";
    // Feature-32: what the mount did when the session ended by itself.
    else if (action == "mount_at_end") description = "At the end, the mount: " + field(line, "result");
    else if (action == "exposure_change") description = "Sky follower adjusted exposure and gain: " + line.section(" exposure_ms=", 1).section(" reason=",0,0);
    else if (action == "detector_change") description = "Detector settings changed: K " + field(line, "sigma") + ", patch " + field(line, "min_pixels") + "–" + field(line, "max_pixels") + " px";
    else if (action == "timelapse_start") description = "Watch time-lapse started (every " + field(line, "interval_s") + " s)";
    else if (action == "timelapse_stop") description = "Watch time-lapse ended (" + field(line, "frames") + " frames, " + field(line, "reason") + ")";
    else description = line.section(' ',1);
    return h(readableTime(line.section(' ',0,0))) + " &middot; " + h(description);
}

QString watchDisplayName(const QString& watch)
{
    if (watch == "moon_flashes") return "Moon impact flashes";
    if (watch == "moon_shadows") return "Moon / Sun shadows";
    if (watch == "sky") return "Sky watch (fast)";
    if (watch == "sky_slow") return "Sky watch (slow)";
    return watch;
}

bool hasRealValue(const QString& value)
{
    return !value.isEmpty() && value != "unknown" && value != "not entered";
}

void kvRow(QTextStream& out, const QString& label, const QString& value, const char* cssClass = nullptr)
{
    if (value.isEmpty()) return;
    out << "<tr><th>" << h(label) << "</th><td";
    if (cssClass) out << " class='" << cssClass << "'";
    out << ">" << h(value) << "</td></tr>";
}

// Multi-hour safe summary of guiding.txt: running statistics over every line,
// plus up to kGuideChartBins fixed time buckets for the offline SVG drift plot.
constexpr int kGuideChartBins = 300;

struct GuideBin
{
    int valid{0};
    int lost{0};
    double minDx{0}, maxDx{0}, sumDx{0};
    double minDy{0}, maxDy{0}, sumDy{0};
};

struct GuidingSummary
{
    bool present{false};
    QString mountName;
    QString calibrated;
    QString westVector, northVector;
    double tolerancePx{0};
    double giveUpPx{0};
    double gain{0};
    int maxPulseMs{0};
    double settleS{0};
    double speedPxPerFrame{0};

    qint64 firstOffsetMs{0}, lastOffsetMs{0};
    uint64_t validSamples{0}, lostSamples{0};
    double sumDx2{0}, sumDy2{0}, sumR2{0};
    double maxAbsDx{0}, maxAbsDy{0}, maxR{0};
    uint64_t guidePulses{0}, calibrationPulses{0};
    uint64_t totalGuidePulseMs{0};
    uint64_t lockLosses{0};

    std::array<GuideBin, kGuideChartBins> bins{};
    int usedBins{0};
};

// Two-pass streaming read so even a 12-hour guiding.txt (~43,000 lines) uses
// constant memory and emits at most kGuideChartBins SVG buckets.
GuidingSummary inspectGuiding(const QString& filePath)
{
    GuidingSummary g;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return g;
    g.present = true;

    const auto parseOffset = [](const QString& line, qint64& msOut, double& dxOut, double& dyOut, bool& validOut) {
        const QDateTime dt = QDateTime::fromString(line.section(' ', 0, 0), Qt::ISODateWithMs);
        if (!dt.isValid()) return false;
        msOut = dt.toMSecsSinceEpoch();
        const QString state = field(line, "state");
        const QString validStr = field(line, "valid");
        const QString dxStr = field(line, "dx");
        const QString dyStr = field(line, "dy");
        bool okX = false, okY = false;
        dxOut = dxStr.toDouble(&okX);
        dyOut = dyStr.toDouble(&okY);
        validOut = okX && okY && validStr == "yes" && state == "Locked";
        return true;
    };

    // Pass 1: settings, pulse/loss counters, time span, and exact RMS/peak stats.
    {
        QTextStream in(&file);
        while (!in.atEnd())
        {
            const QString line = in.readLine();
            const QString kind = line.section(' ', 1, 1);
            if (kind == "guide_mount_connect")
            {
                const QString m = field(line, "name");
                if (!m.isEmpty()) g.mountName = m;
            }
            else if (kind == "guide_config" || kind == "guide_start")
            {
                if (const QString m = field(line, "mount"); !m.isEmpty() && m != "none") g.mountName = m;
                if (const QString cal = field(line, "calibrated"); !cal.isEmpty()) g.calibrated = cal;
                if (const QString w = field(line, "west_px_per_s"); !w.isEmpty()) g.westVector = w;
                if (const QString n = field(line, "north_px_per_s"); !n.isEmpty()) g.northVector = n;
                if (const QString v = field(line, "tolerance_px"); !v.isEmpty()) g.tolerancePx = v.toDouble();
                if (const QString v = field(line, "give_up_px"); !v.isEmpty()) g.giveUpPx = v.toDouble();
                if (const QString v = field(line, "gain"); !v.isEmpty()) g.gain = v.toDouble();
                if (const QString v = field(line, "max_pulse_ms"); !v.isEmpty()) g.maxPulseMs = v.toInt();
                if (const QString v = field(line, "settle_s"); !v.isEmpty()) g.settleS = v.toDouble();
                if (const QString v = field(line, "speed_px_per_frame"); !v.isEmpty()) g.speedPxPerFrame = v.toDouble();
            }
            else if (kind == "guide_calibration")
            {
                g.calibrated = "yes";
                if (const QString w = field(line, "west_px_per_s"); !w.isEmpty()) g.westVector = w;
                if (const QString n = field(line, "north_px_per_s"); !n.isEmpty()) g.northVector = n;
            }
            else if (kind == "guide_pulse")
            {
                if (field(line, "purpose") == "calibration") ++g.calibrationPulses;
                else
                {
                    ++g.guidePulses;
                    g.totalGuidePulseMs += static_cast<uint64_t>(std::max(0, field(line, "ms").toInt()));
                }
            }
            else if (kind == "guide_state")
            {
                if (field(line, "state") == "Lost") ++g.lockLosses;
            }
            else if (kind == "guide_offset")
            {
                qint64 ms = 0;
                double dx = 0, dy = 0;
                bool valid = false;
                if (!parseOffset(line, ms, dx, dy, valid)) continue;
                if (g.validSamples + g.lostSamples == 0) g.firstOffsetMs = g.lastOffsetMs = ms;
                else
                {
                    g.firstOffsetMs = std::min(g.firstOffsetMs, ms);
                    g.lastOffsetMs = std::max(g.lastOffsetMs, ms);
                }
                if (valid)
                {
                    ++g.validSamples;
                    g.sumDx2 += dx * dx;
                    g.sumDy2 += dy * dy;
                    const double r2 = dx * dx + dy * dy;
                    g.sumR2 += r2;
                    g.maxAbsDx = std::max(g.maxAbsDx, std::abs(dx));
                    g.maxAbsDy = std::max(g.maxAbsDy, std::abs(dy));
                    g.maxR = std::max(g.maxR, std::sqrt(r2));
                }
                else
                {
                    ++g.lostSamples;
                }
            }
        }
    }

    // Pass 2: stream offsets into fixed buckets for the SVG drift chart.
    const qint64 spanMs = g.lastOffsetMs - g.firstOffsetMs;
    if (g.validSamples + g.lostSamples >= 2 && spanMs > 0 && file.seek(0))
    {
        const int numBins = static_cast<int>(std::min<uint64_t>(kGuideChartBins, g.validSamples + g.lostSamples));
        g.usedBins = std::max(2, numBins);
        QTextStream in(&file);
        while (!in.atEnd())
        {
            const QString line = in.readLine();
            if (line.section(' ', 1, 1) != "guide_offset") continue;
            qint64 ms = 0;
            double dx = 0, dy = 0;
            bool valid = false;
            if (!parseOffset(line, ms, dx, dy, valid)) continue;
            const double frac = static_cast<double>(ms - g.firstOffsetMs) / static_cast<double>(spanMs);
            const int b = std::clamp(static_cast<int>(frac * g.usedBins), 0, g.usedBins - 1);
            GuideBin& bin = g.bins[static_cast<size_t>(b)];
            if (valid)
            {
                if (bin.valid == 0)
                {
                    bin.minDx = bin.maxDx = dx;
                    bin.minDy = bin.maxDy = dy;
                }
                else
                {
                    bin.minDx = std::min(bin.minDx, dx);
                    bin.maxDx = std::max(bin.maxDx, dx);
                    bin.minDy = std::min(bin.minDy, dy);
                    bin.maxDy = std::max(bin.maxDy, dy);
                }
                bin.sumDx += dx;
                bin.sumDy += dy;
                ++bin.valid;
            }
            else
            {
                ++bin.lost;
            }
        }
    }
    return g;
}

void writeGuidingChart(QTextStream& out, const GuidingSummary& g)
{
    if (g.usedBins < 2 || g.validSamples < 2) return;
    const double rmsTotal = std::sqrt(g.sumR2 / static_cast<double>(g.validSamples));
    const double peakAxis = std::max(g.maxAbsDx, g.maxAbsDy);
    const double capLimit = std::max({25.0, g.tolerancePx * 3.0, rmsTotal * 6.0});
    double yLimit = std::max({2.0, g.tolerancePx * 1.35, std::min(peakAxis * 1.15, capLimit)});
    // Round yLimit up to a clean tick value.
    if (yLimit <= 5.0) yLimit = std::ceil(yLimit);
    else if (yLimit <= 20.0) yLimit = std::ceil(yLimit / 2.0) * 2.0;
    else yLimit = std::ceil(yLimit / 5.0) * 5.0;
    const bool clipped = peakAxis > yLimit;

    constexpr double kW = 760.0, kH = 212.0;
    constexpr double kLeft = 52.0, kRight = 746.0, kTop = 14.0, kBot = 178.0;
    const double plotW = kRight - kLeft;
    const double plotH = kBot - kTop;
    const double midY = kTop + plotH * 0.5;
    const auto yOf = [&](double val) {
        const double clamped = std::clamp(val, -yLimit, yLimit);
        return midY - (clamped / yLimit) * (plotH * 0.5);
    };
    const double binW = plotW / static_cast<double>(g.usedBins);
    const auto xCenter = [&](int i) { return kLeft + (static_cast<double>(i) + 0.5) * binW; };

    out << "<div class='guideChart'><svg viewBox='0 0 " << kW << " " << kH
        << "' role='img' aria-label='Guiding offset over time'>";
    // Grid and zero baseline.
    out << "<rect x='" << kLeft << "' y='" << kTop << "' width='" << plotW << "' height='" << plotH
        << "' fill='#091018' stroke='#263844' stroke-width='1'/>"
        << "<line x1='" << kLeft << "' y1='" << midY << "' x2='" << kRight << "' y2='" << midY
        << "' stroke='#344954' stroke-width='1'/>";
    // Lock-loss shading.
    for (int i = 0; i < g.usedBins; ++i)
    {
        if (g.bins[static_cast<size_t>(i)].lost > 0)
        {
            out << "<rect x='" << QString::number(kLeft + i * binW, 'f', 1) << "' y='" << kTop
                << "' width='" << QString::number(std::max(1.5, binW), 'f', 1) << "' height='" << plotH
                << "' fill='#f78c6b' fill-opacity='0.18'/>";
        }
    }
    // Tolerance dashed reference lines.
    if (g.tolerancePx > 0 && g.tolerancePx <= yLimit)
    {
        const double yt = yOf(g.tolerancePx), yb = yOf(-g.tolerancePx);
        out << "<line x1='" << kLeft << "' y1='" << QString::number(yt, 'f', 1) << "' x2='" << kRight
            << "' y2='" << QString::number(yt, 'f', 1) << "' stroke='#8fb3c4' stroke-dasharray='4 4' stroke-opacity='0.65'/>"
            << "<line x1='" << kLeft << "' y1='" << QString::number(yb, 'f', 1) << "' x2='" << kRight
            << "' y2='" << QString::number(yb, 'f', 1) << "' stroke='#8fb3c4' stroke-dasharray='4 4' stroke-opacity='0.65'/>";
    }
    // Min-max vertical bands per bin so spikes in a multi-hour session remain visible.
    const auto drawBandAndLine = [&](bool isDx, const char* strokeColor) {
        QString rangePath, linePath;
        int lastPopulated = -10;
        for (int i = 0; i < g.usedBins; ++i)
        {
            const GuideBin& bin = g.bins[static_cast<size_t>(i)];
            if (bin.valid == 0) continue;
            const double x = xCenter(i);
            const double lo = isDx ? bin.minDx : bin.minDy;
            const double hi = isDx ? bin.maxDx : bin.maxDy;
            const double avg = (isDx ? bin.sumDx : bin.sumDy) / static_cast<double>(bin.valid);
            if (bin.valid > 1 && std::abs(hi - lo) > 0.05)
                rangePath += QString("M%1,%2V%3").arg(x, 0, 'f', 1).arg(yOf(hi), 0, 'f', 1).arg(yOf(lo), 0, 'f', 1);
            const char cmd = (i - lastPopulated <= 2) ? 'L' : 'M';
            linePath += QString("%1%2,%3").arg(cmd).arg(x, 0, 'f', 1).arg(yOf(avg), 0, 'f', 1);
            lastPopulated = i;
        }
        if (!rangePath.isEmpty())
            out << "<path d='" << rangePath << "' stroke='" << strokeColor << "' stroke-width='"
                << QString::number(std::max(1.5, binW), 'f', 1) << "' stroke-opacity='0.28' fill='none'/>";
        if (!linePath.isEmpty())
            out << "<path d='" << linePath << "' stroke='" << strokeColor << "' stroke-width='1.5' fill='none'/>";
    };
    drawBandAndLine(true, "#86e0c4");
    drawBandAndLine(false, "#ffd166");

    // Y-axis labels.
    out << "<g fill='#afc3ca' font-size='11' font-family='Segoe UI,system-ui,sans-serif' text-anchor='end'>"
        << "<text x='" << kLeft - 6 << "' y='" << kTop + 9 << "'>+" << QString::number(yLimit, 'g', 4) << " px</text>"
        << "<text x='" << kLeft - 6 << "' y='" << midY + 4 << "'>0</text>"
        << "<text x='" << kLeft - 6 << "' y='" << kBot << "'>-" << QString::number(yLimit, 'g', 4) << " px</text></g>";
    // X-axis UTC timestamps (start, midpoint, end).
    const auto fmtUtc = [](qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc()).toString("HH:mm:ss"); };
    const qint64 midMs = g.firstOffsetMs + (g.lastOffsetMs - g.firstOffsetMs) / 2;
    out << "<g fill='#afc3ca' font-size='11' font-family='Segoe UI,system-ui,sans-serif'>"
        << "<text x='" << kLeft << "' y='" << kH - 12 << "' text-anchor='start'>" << fmtUtc(g.firstOffsetMs) << " UTC</text>"
        << "<text x='" << (kLeft + kRight) * 0.5 << "' y='" << kH - 12 << "' text-anchor='middle'>"
        << fmtUtc(midMs) << " UTC (" << duration(static_cast<double>(g.lastOffsetMs - g.firstOffsetMs) / 1000.0) << ")</text>"
        << "<text x='" << kRight << "' y='" << kH - 12 << "' text-anchor='end'>" << fmtUtc(g.lastOffsetMs) << " UTC</text></g>"
        << "</svg><div class='legend'>"
        << "<span><i class='swatch' style='background:#86e0c4'></i>dx (mean &amp; min–max)</span>"
        << "<span><i class='swatch' style='background:#ffd166'></i>dy (mean &amp; min–max)</span>";
    if (g.tolerancePx > 0)
        out << "<span><i class='swatch' style='background:#8fb3c4'></i>±" << QString::number(g.tolerancePx, 'g', 4) << " px tolerance</span>";
    if (g.lostSamples > 0)
        out << "<span><i class='swatch box' style='background:rgba(247,140,107,.45)'></i>lock lost</span>";
    if (clipped)
        out << "<span class='hint'>Chart clamped to ±" << QString::number(yLimit, 'g', 4)
            << " px (peak " << QString::number(peakAxis, 'f', 1) << " px)</span>";
    out << "</div></div>";
}

QString conditions(const QString& line)
{
    QStringList values;
    for (const QString key : {"camera", "exposure", "gain", "offset", "binning", "roi", "observer", "site", "clock_check", "playback"})
    {
        const QString value = field(line, key);
        if (!value.isEmpty()) values << h(key) + " <b>" + h(value) + "</b>";
    }
    return values.join(" · ");
}

// Plot only paired start/stop boundaries. A legacy opening time alone does
// not establish an interval. Host-clock changes can also disallow a UTC plot.
void coverageStrip(QTextStream& out, const QStringList& lines)
{
    QList<QPair<qint64,qint64>> intervals;
    QDateTime start;
    for (const auto& line : lines)
    {
        const auto time = QDateTime::fromString(line.section(' ',0,0), Qt::ISODateWithMs);
        if (line.contains(" monitoring_start ")) start = time;
        else if (line.contains(" monitoring_stop ") && start.isValid() && time > start)
        {
            intervals.append({start.toMSecsSinceEpoch(), time.toMSecsSinceEpoch()});
            start = {};
        }
    }
    if (intervals.isEmpty()) return;
    const qint64 origin = intervals.first().first;
    const double span = static_cast<double>(intervals.last().second - origin);
    if (span <= 0) return;
    out << "<div class='coverage' aria-label='Recorded monitoring intervals'>";
    for (const auto& [a,b] : intervals)
    {
        if (a < origin || b > intervals.last().second) continue;
        out << "<span style='left:" << 100 * (a-origin)/span << "%;width:" << 100 * (b-a)/span
            << "%' title='" << duration((b-a)/1000.0) << " monitored'></span>";
    }
    out << "</div><small>Green · Monitoring active &nbsp; / &nbsp; Grey · Between intervals</small>";
}

// Feature-46: one or more consecutive clips of one event, with its group.
struct Event
{
    std::vector<size_t> clips;
    ClipDescription description;
    // The clip whose trigger was strongest, for the picture.
    size_t shown{0};
};

const std::vector<ClipGroup> kGroupOrder = {ClipGroup::Brief, ClipGroup::Wide, ClipGroup::Other, ClipGroup::Moving};

QString groupMeaning(ClipGroup group)
{
    switch (group)
    {
    case ClipGroup::Brief:
        return "Everything happened in one place within 0.2 s: a meteor, a glint, a cosmic-ray hit, a flash, or one blink of a strobe.";
    case ClipGroup::Moving:
        return "The triggers advanced steadily along a straight line: usually an aircraft or a satellite; on the Moon or Sun, anything crossing it.";
    case ClipGroup::Wide:
        return "A large share of the frame brightened or darkened at once: lightning, headlights or a torch, a cloud, a fireball's flare.";
    case ClipGroup::Other:
        break;
    }
    return "Anything else, as measured: triggers in one place for longer, or spread out and not along a line, such as seeing or cloud.";
}

QString badge(ClipGroup group)
{
    return "<span class='badge " + groupName(group) + "'>" + groupTitle(group) + "</span>";
}

// The overview's row of crops (before, peak, after, change) as the event's
// picture, cut from 00_overview.png by the page; the whole overview when the
// frame size or the activity is not known. A click opens the full overview.
QString picture(const QDir& root, const Clip& clip)
{
    const QString image = clip.path + "/00_overview.png";
    if (!QFileInfo::exists(root.filePath(image))) return {};
    const QStringList size = clip.info.value("frame_size").split('x');
    const bool active = overviewField(clip, "active_frames").toInt() > 0;
    if (size.size() == 2 && size[0].toInt() > 0 && size[1].toInt() > 0 && active)
    {
        const OverviewLayout layout = overviewLayout(size[0].toInt(), size[1].toInt());
        if (layout.crops.height() > 0)
            return QString("<a class='strip' href='%1' target='_blank' rel='noopener' title='Open the whole overview' style='aspect-ratio:%2/%3'>"
                           "<img loading='lazy' alt='Crops around the peak: before, during, after, and the change' src='%1' style='margin-top:-%4%'></a>")
                .arg(url(image)).arg(layout.crops.width()).arg(layout.crops.height())
                .arg(100.0 * layout.crops.top() / layout.crops.width(), 0, 'f', 3);
    }
    return "<a class='picture' href='" + url(image) + "' target='_blank' rel='noopener'><img loading='lazy' alt='Annotated clip overview' src='" + url(image) + "'></a>";
}

QStringList tagsOf(const Clip& clip)
{
    QStringList tags;
    if (clip.info.value("contains_test_signal") == "yes") tags << "test signal";
    if (clip.info.value("complete") != "yes") tags << "partial";
    if (clip.info.contains("gap")) tags << "frame gap";
    for (const auto& t : clip.triggers)
    {
        if (field(" " + t, "kind") == "manual") tags << "manual save";
        if (t.contains(" during_mount_correction=yes")) tags << "guide pulse";
    }
    return tags;
}

// One clip's triggers, preview and files.
void clipDetails(QTextStream& out, const QDir& root, const Clip& clip, bool includeInlineGif = true)
{
    const QString base = clip.path + "/";
    out << "<details><summary>" << clip.triggers.size() << (clip.triggers.size() == 1 ? " trigger" : " triggers")
        << " &amp; measurements</summary><ul>";
    for (const QString& t : clip.triggers)
    {
        const QString line = " " + t;
        out << "<li>" << h(field(line,"trigger_time")) << " · " << h(field(line,"kind"));
        if (!field(line,"peak_sigma").isEmpty()) out << " · " << field(line,"peak_sigma").toDouble() << " σ · "
            << h(field(line,"bright_pixel_count")) << " px at " << h(field(line,"peak"));
        out << "</li>";
    }
    out << "</ul></details>";
    if (includeInlineGif && QFileInfo::exists(root.filePath(base + "04_event_preview.gif")))
        out << "<details class='animation'><summary>Play preview</summary><img alt='Animated event preview' data-src='"
            << url(base + "04_event_preview.gif") << "'></details>";
    const QString ser = QFileInfo(root.filePath(base + "event.ser")).isFile() ? base + "event.ser" : base + "event.txt";
    out << "<footer>" << link(root, base + "00_overview.png", "Overview") << openLink(root, base + "event.ser", "open", "Open SER")
        << openLink(root, ser, "show", "Show in folder") << link(root, base + "event.txt", "Clip metadata")
        << link(root, base + "03_diff_annotated.png", "Difference image") << "</footer>";
}

void card(QTextStream& out, const QDir& root, const QList<Clip>& clips, const std::vector<ClipDescription>& described,
    const Event& event, int index)
{
    const Clip& first = clips[static_cast<qsizetype>(event.clips.front())];
    const Clip& shown = clips[static_cast<qsizetype>(event.shown)];
    const QString gifPath = shown.path + "/04_event_preview.gif";
    const bool hasGif = QFileInfo::exists(root.filePath(gifPath));
    QStringList tags{groupName(event.description.group)};
    for (size_t c : event.clips) tags << tagsOf(clips[static_cast<qsizetype>(c)]);
    tags.removeDuplicates();
    out << "<article id='event-" << index << "' data-tags=\"" << h(tags.join('|')) << "\" data-key=\"" << h(first.name) << "\">"
        << picture(root, shown);
    if (hasGif)
        out << "<div class='cardGif' hidden><img alt='Animated event preview' data-src='" << url(gifPath) << "'></div>";
    out << "<div class='body'><div class='row'><h3>" << h(timeOf(first).isEmpty() ? first.name : readableTime(timeOf(first)))
        << "</h3><div class='actions'>";
    if (hasGif)
        out << "<button class='gifToggle' type='button' aria-pressed='false' title='Play or hide animated event preview'>▶ Play GIF</button>";
    out << "<button class='mark' aria-pressed='false' title='Mark for follow-up'>☆</button></div></div>"
        << "<p class='facts'>" << badge(event.description.group) << h(event.description.facts) << "</p>";
    for (const auto& tag : tags.mid(1)) out << "<span class='tag'>" << h(tag) << "</span> ";
    if (event.clips.size() == 1)
    {
        clipDetails(out, root, first, false);
        out << "</div></article>";
        return;
    }
    const Clip& last = clips[static_cast<qsizetype>(event.clips.back())];
    const QString end = last.triggers.isEmpty() ? QString() : field(" " + last.triggers.back(), "trigger_time");
    out << "<details><summary>" << event.clips.size() << " clips, " << h(shortTime(timeOf(first))) << " – "
        << h(shortTime(end)) << " UTC</summary><ol class='clips'>";
    for (size_t c : event.clips)
    {
        const Clip& clip = clips[static_cast<qsizetype>(c)];
        out << "<li><b>" << h(shortTime(timeOf(clip), "HH:mm:ss.zzz")) << "</b> " << badge(described[c].group)
            << h(described[c].facts);
        clipDetails(out, root, clip);
        out << "</li>";
    }
    out << "</ol></details></div></article>";
}
} // namespace

QString writeNightlyReport(const QString& folder, QString& error)
{
    const QDir root(QFileInfo(folder).absoluteFilePath());
    QFile log(root.filePath("candidates.txt"));
    if (!log.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        error = "Cannot read monitoring journal: " + log.fileName();
        return {};
    }
    Journal j = journal(log);
    QList<Clip> clips, recordings;
    for (const QString& name : QDir(root.filePath("clips")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        clips << inspect(root, "clips/" + name, "event.txt");
    for (const QString& name : QDir(root.filePath("recordings")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        recordings << inspect(root, "recordings/" + name, "recording.txt");

    // Feature-46: each clip's group from its triggers, then consecutive clips
    // of one event together.
    std::vector<ClipSpan> spans;
    std::vector<ClipDescription> described;
    for (const Clip& clip : clips)
    {
        spans.push_back(spanOf(clip));
        described.push_back(describeClip(spans.back().triggers, runOf(clip), arcsecPerPixelOf(clip)));
    }
    std::vector<Event> events;
    for (const std::vector<size_t>& members : clipsIntoEvents(spans))
    {
        Event event;
        event.clips = members;
        std::vector<ClipSpan> memberSpans;
        std::vector<ClipDescription> memberDescriptions;
        double strongest = -1;
        for (size_t c : members)
        {
            memberSpans.push_back(spans[c]);
            memberDescriptions.push_back(described[c]);
            for (const TriggerPoint& t : spans[c].triggers)
                if (t.sigma > strongest) { strongest = t.sigma; event.shown = c; }
        }
        if (strongest < 0) event.shown = members.front();
        event.description = describeEvent(memberSpans, memberDescriptions, arcsecPerPixelOf(clips[static_cast<qsizetype>(members.front())]));
        events.push_back(std::move(event));
    }
    QMap<ClipGroup, int> counts;
    for (const Event& event : events) ++counts[event.description.group];

    QStringList boundaries, detectorChanges;
    QString watchKind, detectorConfigLine, timelapseStartLine, timelapseStopLine;
    int starts = 0, ends = 0;
    double seconds = 0;
    uint64_t totalFrames = 0, totalFrameGaps = 0, totalSourceDrops = 0;
    uint64_t totalStabActive = 0, totalStabFallback = 0;
    bool hasFrames = false, hasSourceDrops = false;
    bool hasShadowsWatch = false, hasFlashWatch = false;
    QFile manifest(root.filePath("session.txt"));
    if (manifest.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QTextStream in(&manifest);
        while (!in.atEnd())
        {
            const QString line = in.readLine();
            if (line.contains(" monitoring_start "))
            {
                ++starts;
                boundaries << line;
                const QString watch = field(line, "watch");
                if (!watch.isEmpty() && watchKind.isEmpty()) watchKind = watch;
                if (watch == "moon_shadows") hasShadowsWatch = true;
                else if (watch == "moon_flashes") hasFlashWatch = true;
            }
            else if (line.contains(" monitoring_stop "))
            {
                ++ends;
                seconds += field(line, "seconds").toDouble();
                boundaries << line;
                if (const QString f = field(line, "frames"); !f.isEmpty())
                {
                    hasFrames = true;
                    totalFrames += f.toULongLong();
                    totalFrameGaps += field(line, "frame_gaps").toULongLong();
                }
                if (const QString sd = field(line, "source_drops"); !sd.isEmpty())
                {
                    hasSourceDrops = true;
                    totalSourceDrops += sd.toULongLong();
                }
                totalStabActive += field(line, "stab_active").toULongLong();
                totalStabFallback += field(line, "stab_fallback").toULongLong();
            }
            else if (line.contains(" mount_at_end ")) boundaries << line;
            else if (line.contains(" detector_config "))
            {
                if (detectorConfigLine.isEmpty()) detectorConfigLine = line;
            }
            else if (line.contains(" detector_change "))
            {
                detectorChanges << line;
                boundaries << line;
            }
            else if (line.contains(" timelapse_start "))
            {
                if (timelapseStartLine.isEmpty()) timelapseStartLine = line;
                boundaries << line;
            }
            else if (line.contains(" timelapse_stop "))
            {
                timelapseStopLine = line;
                boundaries << line;
            }
        }
    }
    const GuidingSummary guiding = inspectGuiding(root.filePath("guiding.txt"));
    const QString firstStart = j.starts.isEmpty() ? QString() : j.starts.front();
    std::optional<double> sessionArcsecPerPx;
    if (!firstStart.isEmpty())
    {
        bool ok = false;
        const double sc = field(firstStart, "image_scale_arcsec_per_pixel").toDouble(&ok);
        if (ok && sc > 0) sessionArcsecPerPx = sc;
    }
    if (!sessionArcsecPerPx && !clips.isEmpty()) sessionArcsecPerPx = arcsecPerPixelOf(clips.front());
    const bool hasTimelapse = !timelapseStartLine.isEmpty() || !timelapseStopLine.isEmpty()
        || QFileInfo(root.filePath("timelapse/timelapse.mp4")).isFile()
        || QFileInfo(root.filePath("timelapse/timelapse.txt")).isFile();
    bool allDarkeningClips = !spans.empty();
    for (const ClipSpan& span : spans)
    {
        bool clipHasDarkening = false;
        for (const TriggerPoint& t : span.triggers)
        {
            if (t.change == TriggerPoint::Change::Fall || t.change == TriggerPoint::Change::WideFall) clipHasDarkening = true;
            else if (t.change == TriggerPoint::Change::Rise || t.change == TriggerPoint::Change::WideRise) allDarkeningClips = false;
        }
        if (!clipHasDarkening) allDarkeningClips = false;
    }
    const bool shadowsOnly = hasShadowsWatch ? !hasFlashWatch : (!hasFlashWatch && allDarkeningClips);
    QDir parent = root;
    parent.cdUp();
    const QString targetName = parent.dirName();
    const QString first = j.starts.isEmpty() ? root.dirName() : j.starts.front().section(' ',0,0);
    const QString label = targetName.compare("sky", Qt::CaseInsensitive) == 0 ? "Sky" :
        targetName.compare("moon", Qt::CaseInsensitive) == 0
            ? (shadowsOnly ? "Moon (or Sun) shadows" : "Moon")
            : "Monitoring";
    QSaveFile report(root.filePath("report.html"));
    if (!report.open(QIODevice::WriteOnly)) { error = report.errorString(); return {}; }
    QTextStream out(&report);
    out << "<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        << "<title>" << label << " watch · " << h(first) << "</title>" << QString::fromUtf8(reportStyle)
        << "<main><header><div class='brand'>SELENEWATCH &nbsp; / &nbsp; OBSERVING REPORT</div><h1>" << label << " watch</h1>"
        << "<p>" << h(readableTime(first)) << " &nbsp;·&nbsp; UTC</p>";
    if (!clips.isEmpty())
    {
        QStringList parts;
        for (ClipGroup group : kGroupOrder)
            if (counts.value(group))
                parts << "<a href='#group-" + groupName(group) + "'>" + QString::number(counts.value(group)) + " " + groupName(group) + "</a>";
        out << "<p class='summary'>" << events.size() << (events.size() == 1 ? " event" : " events") << " in " << clips.size()
            << (clips.size() == 1 ? " clip" : " clips") << ": " << parts.join(" · ") << "</p>";
    }
    out << "</header><div class='stats'>";
    const auto metric = [&](const QString& value, const QString& name) {
        out << "<div><strong>" << h(value) << "</strong>" << h(name) << "</div>";
    };
    metric(starts > 0 && starts == ends ? duration(seconds) : "Unknown", "Monitoring time");
    metric(QString::number(events.size()), "Events");
    metric(QString::number(clips.size()), "Clips");
    metric(QString::number(j.automatic), "Automatic triggers");
    if (j.manual) metric(QString::number(j.manual), "Manual saves");
    if (!j.exceptions.isEmpty()) metric(QString::number(j.exceptions.size()), "Unsaved or unresolved triggers");
    if (hasFrames)
    {
        const double avgFps = seconds > 0.0 ? static_cast<double>(totalFrames) / seconds : 0.0;
        metric(QString("%1 (%2 fps)").arg(totalFrames).arg(avgFps, 0, 'f', 1), "Frames processed");
        if (totalFrameGaps > 0) metric(QString::number(totalFrameGaps), "Frame gaps");
    }
    if (guiding.validSamples > 0)
    {
        const double rmsTotal = std::sqrt(guiding.sumR2 / static_cast<double>(guiding.validSamples));
        QString rmsText = QString::number(rmsTotal, 'f', 2) + " px";
        if (sessionArcsecPerPx) rmsText += QString(" (%1″)").arg(rmsTotal * *sessionArcsecPerPx, 0, 'f', 2);
        metric(rmsText, "Guiding RMS");
    }
    out << "</div><nav><a href='#setup'>Setup</a><a href='#coverage'>Coverage</a><a href='#findings'>Events</a>";
    if (guiding.present) out << "<a href='#guiding'>Guiding</a>";
    if (hasTimelapse) out << "<a href='#timelapse'>Time-lapse</a>";
    if (!recordings.isEmpty()) out << "<a href='#recordings'>Recordings</a>";
    out << "<a href='#conditions'>Source files</a></nav>";

    // Setup & Configuration: Camera/Optics, Detector/Capture, and Timing/Stream Health.
    out << "<section id='setup'><h2>Setup &amp; configuration</h2><div class='configGrid'>";
    {
        // Card 1: Camera & Optics
        out << "<div class='configCard'><h3>Camera &amp; optics</h3><table class='kv'>";
        const auto sf = [&](const QString& key) { return field(firstStart, key); };
        if (hasRealValue(sf("camera"))) kvRow(out, "Camera", sf("camera"));
        QStringList expGain;
        if (hasRealValue(sf("exposure"))) expGain << sf("exposure");
        if (hasRealValue(sf("gain"))) expGain << "gain " + sf("gain");
        if (hasRealValue(sf("offset"))) expGain << "offset " + sf("offset");
        if (!expGain.isEmpty()) kvRow(out, "Exposure / gain", expGain.join(" · "));
        QStringList geom;
        if (hasRealValue(sf("binning"))) geom << "bin " + sf("binning");
        if (hasRealValue(sf("roi"))) geom << "ROI " + sf("roi");
        if (!geom.isEmpty()) kvRow(out, "Binning / ROI", geom.join(" · "));
        QStringList format;
        if (hasRealValue(sf("sensor_adc_bits"))) format << sf("sensor_adc_bits") + "-bit ADC";
        if (hasRealValue(sf("sample_layout"))) format << sf("sample_layout");
        if (!format.isEmpty()) kvRow(out, "Sample layout", format.join(" · "));
        QStringList usb;
        if (hasRealValue(sf("usb_bandwidth"))) usb << "USB " + sf("usb_bandwidth");
        if (hasRealValue(sf("high_speed"))) usb << "high-speed " + sf("high_speed");
        if (!usb.isEmpty()) kvRow(out, "Transfer", usb.join(" · "));
        QStringList optics;
        if (hasRealValue(sf("telescope"))) optics << sf("telescope");
        if (hasRealValue(sf("aperture_mm"))) optics << sf("aperture_mm") + " mm";
        if (hasRealValue(sf("focal_length_mm"))) optics << "f=" + sf("focal_length_mm") + " mm";
        if (!optics.isEmpty()) kvRow(out, "Telescope", optics.join(" · "));
        QStringList scale;
        if (hasRealValue(sf("pixel_size_um"))) scale << sf("pixel_size_um") + " µm";
        if (sessionArcsecPerPx) scale << QString::number(*sessionArcsecPerPx, 'f', 3) + "″/px";
        if (!scale.isEmpty()) kvRow(out, "Image scale", scale.join(" · "));
        QStringList who;
        if (hasRealValue(sf("observer"))) who << sf("observer");
        if (hasRealValue(sf("site"))) who << sf("site");
        if (!who.isEmpty()) kvRow(out, "Observer / site", who.join(" · "));
        if (hasRealValue(sf("playback"))) kvRow(out, "Playback", sf("playback"));
        const QString sw = hasRealValue(sf("software")) ? sf("software")
            : (!clips.isEmpty() ? clips.front().info.value("software") : QString());
        if (hasRealValue(sw)) kvRow(out, "Software", sw);
        out << "</table></div>";

        // Card 2: Detector & Capture (from session.txt detector_config, falling back to first clip for older watches).
        out << "<div class='configCard'><h3>Detector &amp; capture</h3><table class='kv'>";
        const QString detSrc = !detectorConfigLine.isEmpty() ? detectorConfigLine
            : (!clips.isEmpty() && !clips.front().triggers.isEmpty() ? (" " + clips.front().triggers.front()) : QString());
        const auto df = [&](const QString& key, const QString& fallback = {}) {
            const QString v = field(detSrc, key);
            return !v.isEmpty() ? v : (fallback.isEmpty() ? QString() : field(detSrc, fallback));
        };
        if (!watchKind.isEmpty()) kvRow(out, "Watch", watchDisplayName(watchKind));
        const QString kVal = df("sigma", "detection_sigma");
        if (!kVal.isEmpty()) kvRow(out, "Threshold (K)", kVal + " σ");
        const QString minPx = df("min_pixels", "detection_min_pixels");
        const QString maxPx = df("max_pixels", "detection_max_pixels");
        if (!minPx.isEmpty() && !maxPx.isEmpty()) kvRow(out, "Patch size", minPx + " – " + maxPx + " px");
        const QString oneFrame = df("one_frame", "detection_one_frame");
        if (!oneFrame.isEmpty()) kvRow(out, "Confirmation", oneFrame == "yes" ? "1 frame" : "2 consecutive frames");
        const QString refS = df("ref_s", "reference_time_constant_seconds");
        if (!refS.isEmpty()) kvRow(out, "Reference time", refS + " s");
        const QString minNoise = df("min_noise_adu");
        if (!minNoise.isEmpty()) kvRow(out, "Min noise floor", minNoise + " ADU");
        const QString boxRatio = df("max_box_ratio", "max_patch_box_ratio");
        if (!boxRatio.isEmpty()) kvRow(out, "Max box ratio", boxRatio);
        const QString dark = df("darkening");
        if (dark == "yes")
        {
            const QString maxDark = df("max_dark_pixels");
            kvRow(out, "Darkening", maxDark.isEmpty() ? "On" : ("On (max " + maxDark + " px)"));
        }
        else if (dark == "no") kvRow(out, "Darkening", "Off");
        QStringList wide;
        if (const QString bs = df("brightening_share"); !bs.isEmpty())
            wide << QString("bright ≥%1%").arg(bs.toDouble() * 100.0, 0, 'f', 1);
        if (const QString ds = df("darkening_share"); !ds.isEmpty())
            wide << QString("dark ≥%1%").arg(ds.toDouble() * 100.0, 0, 'f', 1);
        if (!wide.isEmpty()) kvRow(out, "Wide change", wide.join(" · "));
        if (const QString ig = df("ignore_above_share"); !ig.isEmpty())
            kvRow(out, "Ignore above", QString("%1% of full scale").arg(ig.toDouble() * 100.0, 0, 'f', 0));
        if (const QString ms = df("mask_shapes"); !ms.isEmpty())
            kvRow(out, "Mask", ms + (ms == "1" ? " shape" : " shapes"));
        else if (!df("mask").isEmpty())
            kvRow(out, "Mask", "Active");
        else if (!detSrc.isEmpty())
            kvRow(out, "Mask", "None");
        const QString stab = df("stabilize", "stabilize_detection");
        if (!detSrc.isEmpty()) kvRow(out, "Stabilization", stab == "yes" ? "On" : "Off");
        const QString histS = df("history_s"), postS = df("post_s"), maxPostS = df("max_post_s");
        if (!histS.isEmpty() && !postS.isEmpty())
            kvRow(out, "Clip buffers", QString("%1 s pre · %2–%3 s post").arg(histS, postS, maxPostS.isEmpty() ? postS : maxPostS));
        if (detSrc.isEmpty()) kvRow(out, "Status", "Not recorded for this watch");
        out << "</table>";
        if (!detectorChanges.isEmpty())
        {
            out << "<details><summary>" << detectorChanges.size()
                << (detectorChanges.size() == 1 ? " mid-watch change" : " mid-watch changes") << "</summary><ul>";
            for (const QString& ch : detectorChanges) out << "<li>" << activity(ch) << "</li>";
            out << "</ul></details>";
        }
        out << "</div>";

        // Card 3: Timing & Stream Health
        out << "<div class='configCard'><h3>Timing &amp; stream health</h3><table class='kv'>";
        const QString clk = sf("clock_check");
        kvRow(out, "Clock check", clk.isEmpty() ? "Not recorded" : clk);
        if (hasRealValue(sf("frame_time"))) kvRow(out, "Frame timestamps", sf("frame_time"));
        if (hasRealValue(sf("frame_index"))) kvRow(out, "Frame index", sf("frame_index"));
        if (sf("testing_tools") == "on") kvRow(out, "Testing tools", "On (test signals enabled)", "warn");
        if (hasFrames)
        {
            const double avgFps = seconds > 0.0 ? static_cast<double>(totalFrames) / seconds : 0.0;
            kvRow(out, "Frames processed", QString("%1 (avg %2 fps)").arg(totalFrames).arg(avgFps, 0, 'f', 2));
            kvRow(out, "Frame gaps", QString::number(totalFrameGaps), totalFrameGaps > 0 ? "warn" : nullptr);
            if (hasSourceDrops)
                kvRow(out, "Camera drops", QString::number(totalSourceDrops), totalSourceDrops > 0 ? "warn" : nullptr);
            if (totalStabActive + totalStabFallback > 0)
            {
                const double pct = 100.0 * static_cast<double>(totalStabActive) / static_cast<double>(totalStabActive + totalStabFallback);
                kvRow(out, "Stabilizer lock", QString("%1% (%2 active, %3 fallback)")
                    .arg(pct, 0, 'f', 1).arg(totalStabActive).arg(totalStabFallback),
                    totalStabFallback > 0 ? "warn" : nullptr);
            }
        }
        if (!j.exceptions.isEmpty())
            kvRow(out, "Unsaved triggers", QString::number(j.exceptions.size()), "warn");
        out << "</table></div>";
    }
    out << "</div></section>";

    out << "<section id='coverage'><h2>Coverage</h2>";
    if (starts && starts == ends)
        out << "<p>" << starts << (starts == 1 ? " monitoring interval" : " monitoring intervals")
            << " · " << h(duration(seconds)) << " monitored. Times below are host UTC.</p>";
    if (!starts || starts != ends)
        out << "<p>Monitoring end times are incomplete. Total monitored time and the duration of gaps are unknown.</p>";
    coverageStrip(out, boundaries);
    if (boundaries.isEmpty()) for (const auto& s : j.starts)
        boundaries << s.section(' ',0,0) + " monitoring_start";
    QStringList timeline = boundaries + j.changes;
    timeline.sort();
    out << "<details open><summary>Monitoring and recording timeline · UTC</summary><ul>";
    for (const auto& line : timeline) out << "<li>" << activity(line) << "</li>";
    out << "</ul></details>";
    if (!events.empty())
    {
        out << "<h3>Event times · source UTC</h3><div class='ruler'>";
        const auto ms = [](const Clip& c) { return QDateTime::fromString(timeOf(c),Qt::ISODateWithMs).toMSecsSinceEpoch(); };
        const auto start = ms(clips.first()), end = ms(clips.last());
        for (size_t e = 0; e < events.size(); ++e)
        {
            const Clip& clip = clips[static_cast<qsizetype>(events[e].clips.front())];
            const double portion = end > start ? double(ms(clip) - start) / double(end - start) : 0.5;
            out << "<a class='" << groupName(events[e].description.group) << "' style='left:"
                << QString::number(100 * std::clamp(portion,0.0,1.0),'f',3) << "%' href='#event-" << e
                << "' aria-label='Event " << e+1 << "' title='" << h(shortTime(timeOf(clip))) << " UTC · "
                << groupName(events[e].description.group) << "'></a>";
        }
        out << "</div><div class='ends'><span>" << h(shortTime(timeOf(clips.first()), "dd MMM HH:mm:ss")) << " UTC</span><span>"
            << h(shortTime(timeOf(clips.last()), "dd MMM HH:mm:ss")) << " UTC</span></div>";
    }
    out << "</section><section id='findings'><h2>Events</h2>";
    if (clips.isEmpty()) out << "<p>No saved clips for this watch.</p>";
    else
    {
        out << "<p class='hint'>Each clip is grouped by what was measured, not by what the object was. Consecutive clips "
               "of one crossing, or of activity that went on past a clip's length, are shown as one event.</p><p class='kinds'>";
        for (ClipGroup group : kGroupOrder)
            if (counts.value(group))
                out << "<button class='kindFilter' value='" << groupName(group) << "'>" << groupTitle(group) << " <b>"
                    << counts.value(group) << "</b></button> ";
        const bool anyGif = std::any_of(events.begin(), events.end(), [&](const Event& ev) {
            return QFileInfo::exists(root.filePath(clips[static_cast<qsizetype>(ev.shown)].path + "/04_event_preview.gif"));
        });
        out << "</p><div class='tools'><label>Show <select id='filter'><option value=''>All events</option>"
            << "<option value='*'>Marked for follow-up</option></select></label>";
        if (anyGif) out << "<button id='toggleAllGifs' type='button' aria-pressed='false'>Play all GIFs</button>";
        out << "<span id='count' aria-live='polite'></span></div>";
        for (ClipGroup group : kGroupOrder)
        {
            if (!counts.value(group)) continue;
            out << "<div class='groupBlock' id='group-" << groupName(group) << "'><h3 class='groupTitle'>" << badge(group) << " "
                << counts.value(group) << (counts.value(group) == 1 ? " event" : " events") << "</h3><p class='hint'>"
                << h(groupMeaning(group)) << "</p><div class='grid'>";
            for (size_t e = 0; e < events.size(); ++e)
                if (events[e].description.group == group) card(out, root, clips, described, events[e], static_cast<int>(e));
            out << "</div></div>";
        }
        out << "<details><summary>Keep follow-up marks</summary>"
            << "<p class='hint'>Stars are remembered in this browser. Save a copy beside the report to carry your marks with the watch.</p>"
            << "<button id='saveMarks'>Save marks</button> <label>Load marks <input id='loadMarks' type='file' accept='.json'></label>"
            << "<p id='markStatus' aria-live='polite'></p></details>"
            << "<p class='hint'><b>Open SER</b> and <b>Show in folder</b> go through SeleneWatch, so the file opens in its own "
               "program instead of being downloaded. The browser asks first. They work on the computer where SeleneWatch last ran.</p>";
    }
    if (!j.exceptions.isEmpty())
    {
        out << "<details open><summary>Unsaved or unresolved triggers (" << j.exceptions.size() << ")</summary><ul>";
        for (const auto& s : j.exceptions) out << "<li>" << h(s) << "</li>";
        out << "</ul></details>";
    }
    out << "</section>";

    if (guiding.present)
    {
        out << "<section id='guiding'><h2>Guiding</h2><div class='configGrid'>";
        out << "<div class='configCard'><h3>Guiding configuration</h3><table class='kv'>";
        if (!guiding.mountName.isEmpty()) kvRow(out, "Mount", guiding.mountName);
        if (!guiding.calibrated.isEmpty()) kvRow(out, "Calibrated", guiding.calibrated == "yes" ? "Yes" : "No");
        if (!guiding.westVector.isEmpty() || !guiding.northVector.isEmpty())
            kvRow(out, "Calibration (px/s)", QString("W (%1) · N (%2)").arg(guiding.westVector, guiding.northVector));
        if (guiding.tolerancePx > 0)
        {
            QString tol = QString::number(guiding.tolerancePx, 'f', 1) + " px";
            if (guiding.giveUpPx > 0) tol += QString(" (give up %1 px)").arg(guiding.giveUpPx, 0, 'f', 1);
            kvRow(out, "Tolerance", tol);
        }
        if (guiding.gain > 0 || guiding.maxPulseMs > 0)
            kvRow(out, "Correction tuning", QString("gain %1 · max pulse %2 ms · settle %3 s")
                .arg(guiding.gain, 0, 'g', 4).arg(guiding.maxPulseMs).arg(guiding.settleS, 0, 'g', 4));
        out << "</table></div>";

        out << "<div class='configCard'><h3>Tracking performance</h3><table class='kv'>";
        const uint64_t totalSamples = guiding.validSamples + guiding.lostSamples;
        if (totalSamples > 0)
        {
            const double lockedPct = 100.0 * static_cast<double>(guiding.validSamples) / static_cast<double>(totalSamples);
            kvRow(out, "Lock coverage", QString("%1% (%2 locked, %3 lost)")
                .arg(lockedPct, 0, 'f', 1).arg(guiding.validSamples).arg(guiding.lostSamples),
                guiding.lostSamples > 0 ? "warn" : nullptr);
        }
        if (guiding.validSamples > 0)
        {
            const double n = static_cast<double>(guiding.validSamples);
            const double rmsDx = std::sqrt(guiding.sumDx2 / n);
            const double rmsDy = std::sqrt(guiding.sumDy2 / n);
            const double rmsR = std::sqrt(guiding.sumR2 / n);
            const auto fmtPx = [&](double px) {
                if (sessionArcsecPerPx)
                    return QString("%1 px (%2″)").arg(px, 0, 'f', 2).arg(px * *sessionArcsecPerPx, 0, 'f', 2);
                return QString("%1 px").arg(px, 0, 'f', 2);
            };
            kvRow(out, "RMS error (total)", fmtPx(rmsR));
            kvRow(out, "RMS dx / dy", QString("%1 / %2").arg(fmtPx(rmsDx), fmtPx(rmsDy)));
            kvRow(out, "Peak excursion", QString("%1 (max |dx| %2, |dy| %3)")
                .arg(fmtPx(guiding.maxR)).arg(guiding.maxAbsDx, 0, 'f', 2).arg(guiding.maxAbsDy, 0, 'f', 2));
        }
        kvRow(out, "Guide pulses", guiding.guidePulses > 0
            ? QString("%1 (%2 ms total)").arg(guiding.guidePulses).arg(guiding.totalGuidePulseMs)
            : QString("0"));
        if (guiding.lockLosses > 0) kvRow(out, "Lock losses", QString::number(guiding.lockLosses), "warn");
        out << "</table></div></div>";
        writeGuidingChart(out, guiding);
        out << "<p class='hint'>" << link(root, "guiding.txt", "Open full guiding log") << "</p></section>";
    }

    if (hasTimelapse)
    {
        out << "<section id='timelapse'><h2>Watch time-lapse</h2><div class='configCard'><table class='kv'>";
        if (const QString iv = field(timelapseStartLine, "interval_s"); !iv.isEmpty())
            kvRow(out, "Cadence", "1 frame every " + iv + " s");
        if (const QString fr = field(timelapseStopLine, "frames"); !fr.isEmpty())
        {
            const QString sk = field(timelapseStopLine, "skipped");
            kvRow(out, "Frames saved", sk.isEmpty() || sk == "0" ? fr : (fr + " (" + sk + " skipped)"));
        }
        if (const QString rsn = field(timelapseStopLine, "reason"); !rsn.isEmpty())
            kvRow(out, "Ended", rsn);
        if (const QString prob = field(timelapseStopLine, "problem"); !prob.isEmpty())
            kvRow(out, "Problem", prob, "warn");
        out << "</table><footer>"
            << openLink(root, "timelapse/timelapse.mp4", "open", "Open video")
            << openLink(root, "timelapse/timelapse.mp4", "show", "Show in folder")
            << link(root, "timelapse/timelapse.txt", "Time-lapse metadata")
            << "</footer></div></section>";
    }

    if (!recordings.isEmpty())
    {
        out << "<section id='recordings'><h2>Continuous recordings</h2><ul>";
        for (const auto& r : recordings)
            out << "<li>" << h(r.info.value("first_frame_utc", r.name)) << " · " << h(r.info.value("started_by"))
                << " · " << h(r.info.value("frames_written", "unknown")) << " frames · "
                << (r.info.value("complete") == "yes" ? "complete" : "partial") << " "
                << openLink(root, r.path + "/recording.ser", "open", "Open SER") << openLink(root, r.path + "/recording.ser", "show", "Show in folder")
                << link(root, r.path + "/recording.txt", "Metadata") << "</li>";
        out << "</ul></section>";
    }
    out << "<section id='conditions'><h2>Source files</h2><footer>"
        << link(root, "candidates.txt", "Candidate journal")
        << link(root, "session.txt", "Session record")
        << link(root, "guiding.txt", "Guiding log")
        << link(root, "autotune.txt", "Auto-tune log") << "</footer>";
    if (j.starts.size() > 1)
    {
        out << "<details><summary>Stream starts (" << j.starts.size() << ")</summary>";
        for (const auto& s : j.starts) out << "<p>" << h(s.section(' ', 0, 0)) << " · " << conditions(s) << "</p>";
        out << "</details>";
    }
    out << "</section></main>" << QString::fromUtf8(reportScript) << "</html>";
    out.flush();
    if (out.status() != QTextStream::Ok || !report.commit()) { error = "Could not save report: " + report.errorString(); return {}; }
    return report.fileName();
}
