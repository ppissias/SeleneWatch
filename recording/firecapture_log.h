#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

// What FireCapture's log beside a recording says. FireCapture writes a text
// file next to each recording ("Moon_180548.txt" beside "Moon_180548.avi")
// with key=value lines.
struct FireCaptureLog
{
    // The log's own file name, for the record.
    std::string fileName;

    // As written in the log, units included ("8.088ms").
    std::string camera;
    std::string shutter;
    std::string gain;

    // When the capture started, in UTC. Empty unless the log gives Date,
    // Start, and Start(UT), with Date as ddMMyy.
    std::optional<std::chrono::system_clock::time_point> startUtc;
};

// Reads the log beside a recording. Empty when there is none, when the text
// is not FireCapture's, or when it names another recording.
//
// Date and Start are local; Start(UT) is UTC. Near midnight the local date
// is not the UTC date, so the UTC start is the local date and time minus
// the zone offset that Start and Start(UT) differ by.
[[nodiscard]] std::optional<FireCaptureLog> readFireCaptureLog(const std::filesystem::path& recording);
