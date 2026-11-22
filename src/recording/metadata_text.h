#pragma once

#include "core/frame.h"
#include "core/observing_site.h"
#include "core/stream_info.h"

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QTextStream>

#include <chrono>
#include <cstdint>
#include <optional>

// Text shared by the records the writers keep: a clip's event.txt, a
// recording's recording.txt, and the candidate log. Writer threads only.

[[nodiscard]] QDateTime utcDateTime(std::chrono::system_clock::time_point time);
// 20261116-214307-512, the name of a clip's or a recording's folder.
[[nodiscard]] QString utcStamp(std::chrono::system_clock::time_point time);
// ISO 8601 with milliseconds and Z.
[[nodiscard]] QString utcText(std::chrono::system_clock::time_point time);
[[nodiscard]] QString orUnknown(const std::string& text);

// The stream and, when given, the frame layout: source, camera, settings,
// what the frame time and index mean, and the sample layout.
void writeStreamLines(QTextStream& out, const StreamInfo& stream, const Frame* layout);

// Feature-14: who observed, from where, and with what. Fields not entered
// are written as "not entered".
void writeSiteLines(QTextStream& out, const ObservingSite& site);
// The same on one line, as quoted fields for the candidate log.
[[nodiscard]] QString siteFields(const ObservingSite& site);

// The free bytes on the drive that holds a folder, or of its nearest
// existing parent before the folder is made. None when unknown.
[[nodiscard]] std::optional<uint64_t> freeBytesAt(const QString& folder);
