#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// How far to trust the PC clock (Feature-14 Part B, PROJECT_DESCRIPTION.md
// Section 5). SeleneWatch never sets the clock; it records what it finds.

// One SNTP comparison of the PC clock with a server.
struct NtpResult
{
    std::string server;
    bool answered{false};
    // The server's time minus the PC clock, and the round trip, of the query
    // with the shortest round trip.
    double offsetMs{0};
    double roundTripMs{0};
    std::string error;
};

// Sends attempts SNTP (version 4, client mode) queries to host:port over
// UDP and keeps the one with the shortest round trip. Waits at most timeout
// for each answer. Checks that an answer is a server's reply to that query.
[[nodiscard]] NtpResult queryNtp(const std::string& host, uint16_t port = 123, int attempts = 4,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

// The Windows time service's own status (w32tm /query /status), on one
// line, or why it is not available. Waits at most a few seconds.
[[nodiscard]] std::string windowsTimeStatus();

// What one clock check found, as a clip's clock_check: line.
[[nodiscard]] std::string clockCheckText(const std::string& windowsStatus, const NtpResult* ntp,
    std::chrono::system_clock::time_point at);
