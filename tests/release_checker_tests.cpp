#include "release_checker.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <algorithm>
#include <cstring>

namespace {
// A reply completes on the event loop, or stays pending until the checker's
// deadline aborts it. No test opens a socket or contacts GitHub.
class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest& request, QByteArray body, int status,
        NetworkError error, bool pending, int& aborts, QObject* parent)
        : QNetworkReply(parent), body_(std::move(body)), aborts_(aborts)
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (!pending) QTimer::singleShot(0, this, [this, error] {
            if (error != NoError) setError(error, "Simulated network failure");
            if (!body_.isEmpty()) emit readyRead();
            if (!isFinished())
            {
                setFinished(true);
                emit finished();
            }
        });
    }

    void abort() override
    {
        if (isFinished()) return;
        ++aborts_;
        setError(OperationCanceledError, "Aborted");
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override { return body_.size() - position_ + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 count = std::min(maxSize, body_.size() - position_);
        if (count == 0) return -1;
        std::memcpy(data, body_.constData() + position_, static_cast<size_t>(count));
        position_ += count;
        return count;
    }

private:
    QByteArray body_;
    qint64 position_{0};
    int& aborts_;
};

class FakeNetwork : public QNetworkAccessManager
{
public:
    QByteArray body;
    int status{200};
    QNetworkReply::NetworkError error{QNetworkReply::NoError};
    bool pending{false};
    int requests{0};
    int aborts{0};
    Operation operation{UnknownOperation};
    QNetworkRequest request;

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& req, QIODevice*) override
    {
        ++requests;
        operation = op;
        request = req;
        return new FakeReply(req, body, status, error, pending, aborts, this);
    }
};

QByteArray release(const QString& tag, bool draft = false, bool prerelease = false)
{
    return QJsonDocument(QJsonObject{{"tag_name", tag}, {"draft", draft}, {"prerelease", prerelease}}).toJson();
}
}

class ReleaseCheckerTest : public QObject
{
    Q_OBJECT
private slots:
    void comparesReleaseNumbers_data();
    void comparesReleaseNumbers();
    void ignoresUnpublishedAndMalformedResponses_data();
    void ignoresUnpublishedAndMalformedResponses();
    void failuresStaySilent_data();
    void failuresStaySilent();
    void checksOnlyOnceAndUsesThePublicApi();
    void invalidLocalVersionDoesNotMakeARequest();
    void deadlineDoesNotBlockTheEventLoop();
    void oversizedResponseIsAborted();
};

void ReleaseCheckerTest::comparesReleaseNumbers_data()
{
    QTest::addColumn<QString>("current");
    QTest::addColumn<QString>("tag");
    QTest::addColumn<bool>("newer");
    QTest::newRow("next-release") << "2026.11-01" << "v2026.11-02" << true;
    QTest::newRow("without-v") << "2026.11-01" << "2026.11-02" << true;
    QTest::newRow("uppercase-v") << "2026.11-01" << "V2026.11-02" << true;
    QTest::newRow("next-month") << "2026.11-09" << "v2026.12-01" << true;
    QTest::newRow("next-year") << "2026.12-99" << "v2027.01-01" << true;
    QTest::newRow("numeric-order") << "2026.11-9" << "v2026.11-10" << true;
    QTest::newRow("equal") << "2026.11-01" << "v2026.11-01" << false;
    QTest::newRow("equal-padding") << "v2026.1-1" << "2026.01-001" << false;
    QTest::newRow("older") << "2026.11-02" << "v2026.11-01" << false;
    QTest::newRow("older-month") << "2026.11-01" << "v2026.02-99" << false;
    QTest::newRow("older-year") << "2026.01-01" << "v2025.12-99" << false;
    QTest::newRow("beta-suffix") << "2026.11-01" << "v2026.11-02-beta1" << false;
    QTest::newRow("commit-suffix") << "2026.11-01" << "v2026.11-02-dirty" << false;
    QTest::newRow("invalid-month") << "2026.11-01" << "v2026.13-01" << false;
    QTest::newRow("zero-month") << "2026.11-01" << "v2027.00-01" << false;
    QTest::newRow("zero-release") << "2026.11-01" << "v2027.01-00" << false;
    QTest::newRow("unknown-format") << "2026.11-01" << "version 99" << false;
    QTest::newRow("trailing-newline") << "2026.11-01" << "v2026.11-02\n" << false;
    QTest::newRow("huge-number") << "2026.11-01" << "v2026.11-999999999999" << false;
}

void ReleaseCheckerTest::comparesReleaseNumbers()
{
    QFETCH(QString, current);
    QFETCH(QString, tag);
    QFETCH(bool, newer);
    FakeNetwork network;
    network.body = release(tag);
    ReleaseChecker checker(current, nullptr, &network);
    QSignalSpy notices(&checker, &ReleaseChecker::newerReleaseAvailable);
    QSignalSpy finished(&network, &QNetworkAccessManager::finished);
    checker.check();
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(notices.count(), newer ? 1 : 0);
    if (newer) QCOMPARE(notices.front().front().toString(), tag);
}

void ReleaseCheckerTest::ignoresUnpublishedAndMalformedResponses_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("draft") << release("v2026.11-02", true);
    QTest::newRow("prerelease") << release("v2026.11-02", false, true);
    QTest::newRow("missing-flags") << QByteArray(R"({"tag_name":"v2026.11-02"})");
    QTest::newRow("invalid-flags") << QByteArray(R"({"tag_name":"v2026.11-02","draft":"false","prerelease":false})");
    QTest::newRow("missing-tag") << QByteArray(R"({"draft":false,"prerelease":false})");
    QTest::newRow("invalid-tag-type") << QByteArray(R"({"tag_name":2027,"draft":false,"prerelease":false})");
    QTest::newRow("invalid-json") << QByteArray("not JSON");
    QTest::newRow("array") << QByteArray("[]");
    QTest::newRow("empty") << QByteArray();
}

void ReleaseCheckerTest::ignoresUnpublishedAndMalformedResponses()
{
    QFETCH(QByteArray, body);
    FakeNetwork network;
    network.body = body;
    ReleaseChecker checker("2026.11-01", nullptr, &network);
    QSignalSpy notices(&checker, &ReleaseChecker::newerReleaseAvailable);
    QSignalSpy finished(&network, &QNetworkAccessManager::finished);
    checker.check();
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(notices.count(), 0);
}

void ReleaseCheckerTest::failuresStaySilent_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<int>("error");
    QTest::newRow("no-releases") << 404 << int(QNetworkReply::ContentNotFoundError);
    QTest::newRow("forbidden") << 403 << int(QNetworkReply::ContentAccessDenied);
    QTest::newRow("rate-limit") << 429 << int(QNetworkReply::UnknownContentError);
    QTest::newRow("server-error") << 500 << int(QNetworkReply::InternalServerError);
    QTest::newRow("dns-error") << 0 << int(QNetworkReply::HostNotFoundError);
    QTest::newRow("tls-error") << 0 << int(QNetworkReply::SslHandshakeFailedError);
}

void ReleaseCheckerTest::failuresStaySilent()
{
    QFETCH(int, status);
    QFETCH(int, error);
    FakeNetwork network;
    network.body = release("v2026.11-02");
    network.status = status;
    network.error = static_cast<QNetworkReply::NetworkError>(error);
    ReleaseChecker checker("2026.11-01", nullptr, &network);
    QSignalSpy notices(&checker, &ReleaseChecker::newerReleaseAvailable);
    QSignalSpy finished(&network, &QNetworkAccessManager::finished);
    checker.check();
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(notices.count(), 0);
}

void ReleaseCheckerTest::checksOnlyOnceAndUsesThePublicApi()
{
    FakeNetwork network;
    network.body = release("v2026.11-02");
    ReleaseChecker checker("2026.11-01", nullptr, &network);
    QSignalSpy notices(&checker, &ReleaseChecker::newerReleaseAvailable);
    checker.check();
    checker.check();
    QTRY_COMPARE(notices.count(), 1);
    checker.check();
    QCOMPARE(network.requests, 1);
    QCOMPARE(network.operation, QNetworkAccessManager::GetOperation);
    QCOMPARE(network.request.url(), QUrl("https://api.github.com/repos/ppissias/SeleneWatch/releases/latest"));
    QCOMPARE(network.request.rawHeader("Accept"), QByteArray("application/vnd.github+json"));
    QCOMPARE(network.request.rawHeader("User-Agent"), QByteArray("SeleneWatch/2026.11-01"));
    QVERIFY(!network.request.hasRawHeader("Authorization"));
    QCOMPARE(ReleaseChecker::releasesUrl(), QUrl("https://github.com/ppissias/SeleneWatch/releases"));
}

void ReleaseCheckerTest::invalidLocalVersionDoesNotMakeARequest()
{
    FakeNetwork network;
    ReleaseChecker checker("unknown", nullptr, &network);
    checker.check();
    QCOMPARE(network.requests, 0);
}

void ReleaseCheckerTest::deadlineDoesNotBlockTheEventLoop()
{
    FakeNetwork network;
    network.pending = true;
    ReleaseChecker checker("2026.11-01", nullptr, &network);
    QSignalSpy notices(&checker, &ReleaseChecker::newerReleaseAvailable);
    QSignalSpy finished(&network, &QNetworkAccessManager::finished);
    bool responsive = false;
    QTimer::singleShot(0, &checker, [&responsive] { responsive = true; });
    checker.check();
    QTRY_VERIFY(responsive);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 6000);
    QCOMPARE(network.aborts, 1);
    QCOMPARE(notices.count(), 0);
}

void ReleaseCheckerTest::oversizedResponseIsAborted()
{
    FakeNetwork network;
    network.body = QByteArray(1024 * 1024 + 1, ' ');
    ReleaseChecker checker("2026.11-01", nullptr, &network);
    QSignalSpy notices(&checker, &ReleaseChecker::newerReleaseAvailable);
    QSignalSpy finished(&network, &QNetworkAccessManager::finished);
    checker.check();
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(network.aborts, 1);
    QCOMPARE(notices.count(), 0);
}

QTEST_GUILESS_MAIN(ReleaseCheckerTest)
#include "release_checker_tests.moc"
