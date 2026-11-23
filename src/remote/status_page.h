#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QMap>
#include <QtCore/QPointF>
#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <optional>

class QHostAddress;

// Feature-47: a read-only status page for a phone on the local network.
//
// The window fills a RemoteStatus from what it already shows (the status bar,
// the mode badge, the guiding panel, the clips written) whenever the page
// asks. The page refreshes itself every few seconds. It can only read: no
// request changes anything in SeleneWatch.
//
// Everything here is plain: the snapshot, its JSON, the page, and the
// answer to one request. StatusServer carries the answers over the network.

// What SeleneWatch is doing, as the window shows it. Times are UTC.
struct RemoteStatus
{
    QString software;
    QDateTime updated;

    // The status bar's state ("Monitoring", "Preview", "Time-lapse",
    // "Stopped", "Source failed"...) and the mode badge's text.
    QString state;
    QString detail;
    bool running{false};
    // The window shows a failure: the source or the record.
    bool problem{false};
    QString watch;
    QString source;

    // The session the observer started (Monitoring or a time-lapse), and
    // the Ends line.
    std::optional<QDateTime> since;
    QString ends;

    double fps{0};
    uint64_t frames{0};
    std::optional<uint64_t> dropped;

    // Since the stream started: candidates, clips saved by their group
    // (Feature-46), clips not saved, and the last clip.
    int candidates{0};
    int clipsSaved{0};
    int clipsNotSaved{0};
    QMap<QString, int> groups;
    struct Clip
    {
        QDateTime time;
        QString group;
        QString facts;
    };
    std::optional<Clip> lastClip;

    // The feature lock and guiding.
    QString lock;
    bool locked{false};
    std::optional<QPointF> drift;
    QString guiding;
    QString mount;
    std::optional<QDateTime> lastCorrection;
    QString guideMessage;

    // The time-lapse's progress, when one runs, and its stacks so far.
    QString timelapse;
    int stacks{0};

    std::optional<int64_t> freeBytes;

    // The status bar's last message.
    QString message;
    std::optional<QDateTime> messageTime;

    // Whether the page can show the last clip's crops, the time-lapse's
    // latest stack and the live view.
    bool hasLastClipImage{false};
    bool hasLastStack{false};
    bool hasPreview{false};
};

[[nodiscard]] QByteArray statusJson(const RemoteStatus& status);

// The page: one HTML file with its style and script, for a phone's browser.
[[nodiscard]] QByteArray statusPageHtml();

// One answer.
struct RemoteResponse
{
    int code{200};
    QByteArray type;
    QByteArray body;
};

// What the answers are made from, asked for only when needed.
struct RemoteSources
{
    std::function<RemoteStatus()> status;
    // The last clip's crops, as PNG; empty when there is none.
    std::function<QByteArray()> lastClipImage;
    // The time-lapse's latest stack, as PNG; empty when there is none.
    std::function<QByteArray()> lastStack;
    // The live view, as JPEG; empty when there is none.
    std::function<QByteArray()> preview;
};

// The answer to one request, from its first line and headers. Every path
// but /favicon.ico needs ?key= with the access key.
[[nodiscard]] RemoteResponse respond(const QByteArray& requestHead, const QString& key, const RemoteSources& sources);

// The answer as HTTP/1.1 bytes: never cached, and the connection closes.
[[nodiscard]] QByteArray httpResponse(const RemoteResponse& response);

// A computer on the local network: a private, loopback or link-local
// address, or one in 100.64.0.0/10 (Tailscale and other private overlays).
[[nodiscard]] bool allowedPeer(const QHostAddress& address);

// A new access key: 12 random letters and digits that are not confused.
[[nodiscard]] QString newAccessKey();
