#include "ui/release_checker.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <array>
#include <optional>
#include <utility>

namespace {
constexpr int kTimeoutMs = 4000;
constexpr qint64 kMaxResponseBytes = 1024 * 1024;

// Compare the release identity, not its spelling or a source commit suffix.
// Unknown tag formats and prerelease suffixes are deliberately not guessed.
std::optional<std::array<int, 3>> releaseNumbers(const QString& tag)
{
    static const QRegularExpression pattern("\\A[vV]?([0-9]{4})\\.([0-9]{1,2})-([0-9]{1,9})\\z");
    const auto match = pattern.match(tag);
    if (!match.hasMatch()) return std::nullopt;
    bool valid = false;
    const int number = match.captured(3).toInt(&valid);
    const int month = match.captured(2).toInt();
    if (!valid || number < 1 || month < 1 || month > 12) return std::nullopt;
    return std::array<int, 3>{match.captured(1).toInt(), month, number};
}
}

ReleaseChecker::ReleaseChecker(QString currentRelease, QObject* parent, QNetworkAccessManager* network)
    : QObject(parent)
    , currentRelease_(std::move(currentRelease))
    , network_(network ? network : new QNetworkAccessManager(this))
{
}

QUrl ReleaseChecker::releasesUrl()
{
    return QUrl("https://github.com/ppissias/SeleneWatch/releases");
}

void ReleaseChecker::check()
{
    if (checked_) return;
    checked_ = true;
    if (!releaseNumbers(currentRelease_)) return;

    QNetworkRequest request(QUrl("https://api.github.com/repos/ppissias/SeleneWatch/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "SeleneWatch/" + currentRelease_.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);

    auto* reply = network_->get(request);
    reply->setReadBufferSize(kMaxResponseBytes + 1);
    auto* deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QIODevice::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > kMaxResponseBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, deadline] {
        deadline->stop();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
            || reply->bytesAvailable() > kMaxResponseBytes)
            return;

        const auto document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject()) return;
        const auto release = document.object();
        // Missing or malformed flags are not a stable release either.
        if (release.value("draft").toBool(true) || release.value("prerelease").toBool(true)) return;
        const QString tag = release.value("tag_name").toString();
        const auto candidate = releaseNumbers(tag);
        const auto current = releaseNumbers(currentRelease_);
        if (candidate && current && *candidate > *current) emit newerReleaseAvailable(tag);
    });
    // One overall deadline also bounds DNS, redirects, and a slow response.
    deadline->start(kTimeoutMs);
}
