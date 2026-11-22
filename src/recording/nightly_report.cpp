#include "nightly_report.h"
#include "nightly_report_style.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMap>
#include <QtCore/QSaveFile>
#include <QtCore/QTextStream>
#include <QtCore/QUrl>
#include <algorithm>

namespace {
using Fields = QMap<QString, QString>;

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
    return "<a href=\"" + url(file) + "\">" + h(title) + "</a> ";
}

struct Clip
{
    QString name, path;
    Fields info;
    QStringList triggers;
};

// Event metadata can contain a long per-frame ledger. Keep only named fields
// and trigger rows; generating a report never opens a raw image or SER.
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
    else description = line.section(' ',1);
    return h(readableTime(line.section(' ',0,0))) + " &middot; " + h(description);
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
        else if (start.isValid() && time > start)
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

void card(QTextStream& out, const QDir& root, const Clip& clip, int index)
{
    const auto& info = clip.info;
    const QString base = clip.path + "/";
    const QString suggestion = info.value("suggested_kind").section(';', 0, 0);
    const QString kind = suggestion.section(" (", 0, 0);
    QStringList tags;
    if (!kind.isEmpty()) tags << kind;
    if (info.value("contains_test_signal") == "yes") tags << "test signal";
    if (info.value("complete") != "yes") tags << "partial";
    if (info.contains("gap")) tags << "frame gap";
    for (const auto& t : clip.triggers)
    {
        if (field(" " + t, "kind") == "manual") tags << "manual save";
        if (t.contains(" during_mount_correction=yes")) tags << "guide pulse";
    }
    tags.removeDuplicates();
    out << "<article id='clip-" << index << "' data-tags=\"" << h(tags.join('|')) << "\" data-key=\"" << h(clip.name) << "\">";
    if (QFileInfo::exists(root.filePath(base + "00_overview.png")))
        out << "<a class='picture' href='" << url(base + "00_overview.png") << "'><img loading='lazy' alt='Annotated clip overview' src='"
            << url(base + "00_overview.png") << "'></a>";
    out << "<div class='body'><div class='row'><h3>" << h(timeOf(clip).isEmpty() ? clip.name : readableTime(timeOf(clip)))
        << "</h3><button class='mark' aria-pressed='false' title='Mark for follow-up'>☆</button></div>";
    if (!suggestion.isEmpty()) out << "<p>Suggested · " << h(suggestion) << "</p>";
    for (const auto& tag : tags.mid(kind.isEmpty() ? 0 : 1)) out << "<span class='tag'>" << h(tag) << "</span> ";
    out << "<details><summary>" << clip.triggers.size() << " triggers &amp; measurements</summary><ul>";
    for (const QString& t : clip.triggers)
    {
        const QString line = " " + t;
        out << "<li>" << h(field(line,"trigger_time")) << " · " << h(field(line,"kind"));
        if (!field(line,"peak_sigma").isEmpty()) out << " · " << field(line,"peak_sigma").toDouble() << " σ · "
            << h(field(line,"bright_pixel_count")) << " px at " << h(field(line,"peak"));
        out << "</li>";
    }
    out << "</ul></details>";
    if (QFileInfo::exists(root.filePath(base + "04_event_preview.gif")))
        out << "<details class='animation'><summary>Play preview</summary><img alt='Animated event preview' data-src='"
            << url(base + "04_event_preview.gif") << "'></details>";
    out << "<footer>" << link(root, base + "event.ser", "Raw SER") << link(root, base + "event.txt", "Clip metadata")
        << link(root, base + "03_diff_annotated.png", "Difference image") << "</footer></div></article>";
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
    QStringList boundaries;
    int starts = 0, ends = 0;
    double seconds = 0;
    QFile manifest(root.filePath("session.txt"));
    if (manifest.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QTextStream in(&manifest);
        while (!in.atEnd())
        {
            const QString line = in.readLine();
            if (line.contains(" monitoring_start ")) { ++starts; boundaries << line; }
            if (line.contains(" monitoring_stop ")) { ++ends; seconds += field(line, "seconds").toDouble(); boundaries << line; }
            if (line.contains(" mount_at_end ")) boundaries << line;
        }
    }
    QDir parent = root;
    parent.cdUp();
    const QString targetName = parent.dirName();
    const QString first = j.starts.isEmpty() ? root.dirName() : j.starts.front().section(' ',0,0);
    const QString label = targetName.compare("sky", Qt::CaseInsensitive) == 0 ? "Sky" :
        targetName.compare("moon", Qt::CaseInsensitive) == 0 ? "Moon" : "Monitoring";
    QSaveFile report(root.filePath("report.html"));
    if (!report.open(QIODevice::WriteOnly)) { error = report.errorString(); return {}; }
    QTextStream out(&report);
    out << "<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        << "<title>" << label << " watch · " << h(first) << "</title>" << QString::fromUtf8(reportStyle)
        << "<main><header><div class='brand'>SELENEWATCH &nbsp; / &nbsp; OBSERVING REPORT</div><h1>" << label << " watch</h1>"
        << "<p>" << h(readableTime(first)) << " &nbsp;·&nbsp; UTC</p></header><div class='stats'>";
    const auto metric = [&](const QString& value, const QString& name) {
        out << "<div><strong>" << h(value) << "</strong>" << h(name) << "</div>";
    };
    metric(starts > 0 && starts == ends ? duration(seconds) : "Unknown", "Monitoring time");
    metric(QString::number(j.automatic), "Automatic candidates");
    metric(QString::number(clips.size()), "Clip folders");
    metric(QString::number(j.manual), "Manual saves");
    metric(QString::number(j.exceptions.size()), "Unsaved or unresolved triggers");
    out << "</div><nav><a href='#coverage'>Coverage</a><a href='#findings'>Clips</a><a href='#conditions'>Observing conditions</a></nav>"
        << "<section id='coverage'><h2>Coverage</h2>";
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
    if (!clips.isEmpty())
    {
        out << "<h3>Clip times · source UTC</h3><div class='ruler'>";
        const auto ms = [](const Clip& c) { return QDateTime::fromString(timeOf(c),Qt::ISODateWithMs).toMSecsSinceEpoch(); };
        const auto start = ms(clips.first()), end = ms(clips.last());
        for (qsizetype i = 0; i < clips.size(); ++i)
        {
            const double portion = end > start ? double(ms(clips[i]) - start) / double(end - start) : 0.5;
            out << "<a style='left:" << QString::number(100 * std::clamp(portion,0.0,1.0),'f',3)
                << "%' href='#clip-" << i << "' aria-label='Clip " << i+1 << "' title='" << h(timeOf(clips[i])) << "'></a>";
        }
        out << "</div><div class='ends'><span>" << h(timeOf(clips.first())) << "</span><span>"
            << h(timeOf(clips.last())) << "</span></div>";
    }
    out << "</section><section id='findings'><h2>Clip review</h2>";
    QMap<QString,int> suggestions;
    for (const auto& clip : clips)
    {
        const auto kind = clip.info.value("suggested_kind").section(" (",0,0).section(';',0,0);
        if (!kind.isEmpty()) ++suggestions[kind];
    }
    if (!suggestions.isEmpty())
    {
        out << "<p class='kinds'>Suggested kinds by clip · ";
        for (auto it = suggestions.cbegin(); it != suggestions.cend(); ++it)
            out << "<button class='kindFilter' value=\"" << h(it.key()) << "\">" << h(it.key()) << " <b>"
                << it.value() << "</b></button> ";
        out << "</p>";
    }
    if (clips.isEmpty()) out << "<p>No saved clips for this watch.</p>";
    else out << "<div class='tools'><label>Show <select id='filter'><option value=''>All clips</option><option value='*'>Marked for follow-up</option></select></label>"
        << "<span id='count' aria-live='polite'></span></div><div class='grid'>";
    for (qsizetype i = 0; i < clips.size(); ++i) card(out, root, clips[i], static_cast<int>(i));
    if (!clips.isEmpty()) out << "</div><details><summary>Keep follow-up marks</summary>"
        << "<p class='hint'>Stars are remembered in this browser. Save a copy beside the report to carry your marks with the watch.</p>"
        << "<button id='saveMarks'>Save marks</button> <label>Load marks <input id='loadMarks' type='file' accept='.json'></label>"
        << "<p id='markStatus' aria-live='polite'></p></details>";
    if (!j.exceptions.isEmpty())
    {
        out << "<details open><summary>Unsaved or unresolved triggers (" << j.exceptions.size() << ")</summary><ul>";
        for (const auto& s : j.exceptions) out << "<li>" << h(s) << "</li>";
        out << "</ul></details>";
    }
    out << "</section>";
    if (!recordings.isEmpty())
    {
        out << "<section><h2>Continuous recordings</h2><ul>";
        for (const auto& r : recordings)
            out << "<li>" << h(r.info.value("first_frame_utc", r.name)) << " · " << h(r.info.value("started_by"))
                << " · " << h(r.info.value("frames_written", "unknown")) << " frames · "
                << (r.info.value("complete") == "yes" ? "complete" : "partial") << " "
                << link(root,r.path+"/recording.ser","Raw SER") << link(root,r.path+"/recording.txt","Metadata") << "</li>";
        out << "</ul></section>";
    }
    out << "<section id='conditions'><details><summary>Observing conditions and source files</summary>";
    for (const auto& s : j.starts) out << "<p>" << h(s.section(' ',0,0)) << " · " << conditions(s) << "</p>";
    out << link(root,"candidates.txt","Candidate journal") << link(root,"session.txt","Session record")
        << link(root,"guiding.txt","Guiding log") << "</details></section></main>" << QString::fromUtf8(reportScript) << "</html>";
    out.flush();
    if (out.status() != QTextStream::Ok || !report.commit()) { error = "Could not save report: " + report.errorString(); return {}; }
    return report.fileName();
}
