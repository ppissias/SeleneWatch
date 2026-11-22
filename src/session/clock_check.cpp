#include "session/clock_check.h"

#include <QtCore/QDateTime>
#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <QtCore/QTimeZone>

#include <array>
#include <cstring>
#include <format>
#include <limits>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

// Seconds from the NTP epoch, 1900-01-01, to the Unix epoch.
constexpr uint64_t kNtpToUnix = 2'208'988'800ULL;

// UTC as a 64-bit NTP timestamp: seconds and a binary fraction.
uint64_t toNtp(std::chrono::system_clock::time_point time)
{
    const auto since = time.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since);
    const auto fraction = std::chrono::duration<double>(since - seconds).count();
    return ((static_cast<uint64_t>(seconds.count()) + kNtpToUnix) << 32)
        | static_cast<uint64_t>(fraction * 4294967296.0);
}

double ntpSeconds(uint64_t value)
{
    return static_cast<double>(value >> 32) - static_cast<double>(kNtpToUnix)
        + static_cast<double>(value & 0xFFFFFFFFULL) / 4294967296.0;
}

double secondsOf(std::chrono::system_clock::time_point time)
{
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

void putBigEndian(uint8_t* bytes, uint64_t value)
{
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (56 - 8 * i));
}

uint64_t getBigEndian(const uint8_t* bytes)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}

} // namespace

NtpResult queryNtp(const std::string& host, uint16_t port, int attempts, std::chrono::milliseconds timeout)
{
    NtpResult result;
    result.server = host;
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        result.error = "Winsock did not start";
        return result;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* address = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &address) != 0 || !address)
    {
        result.error = "the server's name could not be resolved";
        WSACleanup();
        return result;
    }
    const SOCKET socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket == INVALID_SOCKET)
    {
        result.error = "no UDP socket";
        freeaddrinfo(address);
        WSACleanup();
        return result;
    }
    const DWORD wait = static_cast<DWORD>(timeout.count());
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));

    double bestRoundTrip = std::numeric_limits<double>::infinity();
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        std::array<uint8_t, 48> packet{};
        packet[0] = 0x23; // No leap warning, version 4, client.
        const auto sent = std::chrono::system_clock::now();
        const uint64_t origin = toNtp(sent);
        putBigEndian(packet.data() + 40, origin);
        if (sendto(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                address->ai_addr, static_cast<int>(address->ai_addrlen)) == SOCKET_ERROR)
        {
            result.error = "the query could not be sent";
            continue;
        }
        std::array<uint8_t, 68> answer{};
        const int received = recv(socket, reinterpret_cast<char*>(answer.data()), static_cast<int>(answer.size()), 0);
        const auto arrived = std::chrono::system_clock::now();
        if (received < 48)
        {
            if (!result.answered) result.error = "no answer";
            continue;
        }
        // A server's reply to this query: mode 4, a stratum, and our time echoed.
        const int mode = answer[0] & 0x7;
        if (mode != 4 || answer[1] == 0 || getBigEndian(answer.data() + 24) != origin)
        {
            if (!result.answered) result.error = "an answer that was not a server's reply to this query";
            continue;
        }
        const double t1 = secondsOf(sent), t4 = secondsOf(arrived);
        const double t2 = ntpSeconds(getBigEndian(answer.data() + 32));
        const double t3 = ntpSeconds(getBigEndian(answer.data() + 40));
        const double roundTrip = (t4 - t1) - (t3 - t2);
        if (roundTrip < bestRoundTrip)
        {
            bestRoundTrip = roundTrip;
            result.answered = true;
            result.error.clear();
            result.offsetMs = ((t2 - t1) + (t3 - t4)) / 2 * 1000;
            result.roundTripMs = roundTrip * 1000;
        }
    }
    closesocket(socket);
    freeaddrinfo(address);
    WSACleanup();
#else
    (void)port; (void)attempts; (void)timeout;
    result.error = "not built for this system";
#endif
    return result;
}

std::string windowsTimeStatus()
{
    QProcess process;
    process.start("w32tm", {"/query", "/status"});
    if (!process.waitForStarted(2000)) return "w32tm could not be run";
    if (!process.waitForFinished(3000))
    {
        process.kill();
        process.waitForFinished(1000);
        return "w32tm did not answer";
    }
    // The text is in the system's language; it is kept as it is, on one line.
    QStringList lines;
    for (const QString& line : QString::fromLocal8Bit(process.readAllStandardOutput() + process.readAllStandardError())
             .split('\n', Qt::SkipEmptyParts))
        if (!line.trimmed().isEmpty()) lines << line.trimmed();
    const QString text = lines.join("; ");
    const QString kind = process.exitCode() == 0 ? "status" : "unavailable";
    return std::format("{}: {}", kind.toStdString(), text.left(400).toStdString());
}

std::string clockCheckText(const std::string& windowsStatus, const NtpResult* ntp, std::chrono::system_clock::time_point at)
{
    const QString when = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count(), QTimeZone::UTC)
        .toString(Qt::ISODateWithMs);
    std::string text = std::format("at={} windows_time=\"{}\"", when.toStdString(), windowsStatus);
    if (!ntp) return text + " ntp=off";
    if (!ntp->answered) return text + std::format(" ntp_server={} ntp=\"{}\"", ntp->server, ntp->error);
    return text + std::format(" ntp_server={} server_minus_pc_ms={:.1f} round_trip_ms={:.1f} "
        "(the true offset is within half the round trip)", ntp->server, ntp->offsetMs, ntp->roundTripMs);
}
