#include "remote/phone_link.h"
#include "remote/status_page.h"
#include "remote/status_server.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

// Feature-47: the read-only status page for a phone.
class RemoteTest : public QObject
{
    Q_OBJECT
private slots:
    void theSnapshotBecomesJson();
    void everyRequestNeedsTheKeyAndOnlyReads();
    void onlyTheLocalNetworkIsAnswered();
    void theServerAnswersOverTheNetwork();
    void theAddressFitsInAReadableQrCode();
    void timelapseSnapshotEndpointsAndConcurrentRequests();
};

namespace {

RemoteStatus example()
{
    RemoteStatus s;
    s.software = "SeleneWatch test";
    s.updated = QDateTime::fromString("2026-11-23T21:03:15.000Z", Qt::ISODateWithMs);
    s.state = "Monitoring";
    s.detail = "MONITORING";
    s.running = true;
    s.watch = "The sky: meteors, flashes and aircraft";
    s.since = QDateTime::fromString("2026-11-23T20:38:17.613Z", Qt::ISODateWithMs);
    s.candidates = 12;
    s.clipsSaved = 5;
    s.groups = {{"brief", 2}, {"moving", 3}};
    s.lastClip = RemoteStatus::Clip{QDateTime::fromString("2026-11-23T21:01:15.721Z", Qt::ISODateWithMs), "brief",
        "1 trigger, 21 px in a 6 × 12 box at (547, 219)"};
    s.lock = "locked";
    s.locked = true;
    s.drift = QPointF(0.4, -1.25);
    s.freeBytes = 123'456'789'000;
    return s;
}

RemoteSources sources(QByteArray clip = {})
{
    RemoteSources result;
    result.status = [] { return example(); };
    result.lastClipImage = [clip] { return clip; };
    return result;
}

QByteArray get(const QByteArray& target) { return "GET " + target + " HTTP/1.1\r\nHost: laptop\r\n"; }

} // namespace

void RemoteTest::theSnapshotBecomesJson()
{
    const QJsonObject o = QJsonDocument::fromJson(statusJson(example())).object();
    QCOMPARE(o["state"].toString(), QString("Monitoring"));
    QCOMPARE(o["updated"].toString(), QString("2026-11-23T21:03:15.000Z"));
    QCOMPARE(o["since"].toString(), QString("2026-11-23T20:38:17.613Z"));
    QCOMPARE(o["candidates"].toInt(), 12);
    QCOMPARE(o["groups"].toObject()["moving"].toInt(), 3);
    QCOMPARE(o["lastClip"].toObject()["group"].toString(), QString("brief"));
    QCOMPARE(o["lastClip"].toObject()["facts"].toString(), QString("1 trigger, 21 px in a 6 × 12 box at (547, 219)"));
    QCOMPARE(o["drift"].toObject()["dy"].toDouble(), -1.25);
    QCOMPARE(o["freeBytes"].toDouble(), 123'456'789'000.0);
    // What is not known is null, not zero.
    QVERIFY(o["dropped"].isNull());
    QVERIFY(o["lastCorrection"].isNull());
    QVERIFY(QJsonDocument::fromJson(statusJson(RemoteStatus{})).object()["lastClip"].isNull());
}

void RemoteTest::everyRequestNeedsTheKeyAndOnlyReads()
{
    const QString key = "abcdefghjkmn";
    QCOMPARE(respond(get("/"), key, sources()).code, 403);
    QCOMPARE(respond(get("/status.json?key=wrong"), key, sources()).code, 403);
    QCOMPARE(respond(get("/status.json?key="), QString(), sources()).code, 403);
    const RemoteResponse page = respond(get("/?key=" + key.toLatin1()), key, sources());
    QCOMPARE(page.code, 200);
    QVERIFY(page.type.startsWith("text/html"));
    QVERIFY(page.body.contains("SELENEWATCH · STATUS") && page.body.contains("fetch('status.json'+q"));
    const RemoteResponse json = respond(get("/status.json?key=" + key.toLatin1()), key, sources());
    QCOMPARE(json.code, 200);
    QCOMPARE(json.type, QByteArray("application/json"));
    QCOMPARE(QJsonDocument::fromJson(json.body).object()["clipsSaved"].toInt(), 5);
    QCOMPARE(respond(get("/last-clip.png?key=" + key.toLatin1()), key, sources()).code, 404);
    const RemoteResponse clip = respond(get("/last-clip.png?key=" + key.toLatin1() + "&t=1"), key, sources("PNGDATA"));
    QCOMPARE(clip.code, 200);
    QCOMPARE(clip.type, QByteArray("image/png"));
    QCOMPARE(clip.body, QByteArray("PNGDATA"));
    QCOMPARE(respond(get("/preview.jpg?key=" + key.toLatin1()), key, sources()).code, 404);
    QCOMPARE(respond(get("/favicon.ico"), key, sources()).code, 404);
    QCOMPARE(respond(get("/elsewhere?key=" + key.toLatin1()), key, sources()).code, 404);
    // Nothing can be sent to it.
    QCOMPARE(respond("POST /status.json?key=" + key.toLatin1() + " HTTP/1.1\r\n", key, sources()).code, 405);
    QCOMPARE(respond("not http at all\r\n", key, sources()).code, 400);

    const QByteArray bytes = httpResponse(json);
    QVERIFY(bytes.startsWith("HTTP/1.1 200 OK\r\n"));
    QVERIFY(bytes.contains("Cache-Control: no-store\r\n") && bytes.contains("Connection: close\r\n"));
    QVERIFY(bytes.contains("Content-Length: " + QByteArray::number(json.body.size()) + "\r\n"));
    QVERIFY(bytes.endsWith(json.body));

    const QString another = newAccessKey();
    QCOMPARE(another.size(), 12);
    QVERIFY(another != newAccessKey());
    for (const QChar c : another) QVERIFY(QString("abcdefghjkmnpqrstuvwxyz23456789").contains(c));
}

void RemoteTest::onlyTheLocalNetworkIsAnswered()
{
    for (const char* address : {"192.168.1.23", "10.0.0.7", "172.16.5.4", "172.31.255.1", "127.0.0.1", "::1",
             "::ffff:192.168.1.23", "169.254.10.1", "100.101.102.103", "fd7a:115c:a1e0::1", "fe80::1"})
        QVERIFY2(allowedPeer(QHostAddress(address)), address);
    for (const char* address : {"8.8.8.8", "172.32.0.1", "100.128.0.1", "2001:4860:4860::8888", "::ffff:8.8.8.8"})
        QVERIFY2(!allowedPeer(QHostAddress(address)), address);
    QCOMPARE(statusUrl("192.168.1.23", 8080, "abcdefghjkmn"), QString("http://192.168.1.23:8080/?key=abcdefghjkmn"));
    QVERIFY(localNetworkName().endsWith(".local"));
}

void RemoteTest::theServerAnswersOverTheNetwork()
{
    StatusServer server(sources());
    QString error;
    QVERIFY2(server.start(0, "abcdefghjkmn", error, QHostAddress::LocalHost), qPrintable(error));
    QVERIFY(server.listening());
    const quint16 port = server.port();
    QVERIFY(port > 0);

    const auto ask = [port](const QByteArray& request) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!socket.waitForConnected(3000)) return QByteArray("no connection");
        socket.write(request);
        QByteArray answer;
        QElapsedTimer clock;
        clock.start();
        while (socket.state() != QAbstractSocket::UnconnectedState && clock.elapsed() < 3000)
        {
            QTest::qWait(10);
            answer += socket.readAll();
        }
        return answer + socket.readAll();
    };
    // Sent in two pieces, as a slow phone might.
    QTcpSocket split;
    split.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(split.waitForConnected(3000));
    split.write("GET /status.json?key=abcdefghjkmn HTTP/1.1\r\nHo");
    QTest::qWait(50);
    split.write("st: laptop\r\n\r\n");
    QTRY_VERIFY_WITH_TIMEOUT(split.state() == QAbstractSocket::UnconnectedState || split.bytesAvailable() > 0, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(split.state(), QAbstractSocket::UnconnectedState, 3000);
    const QByteArray splitAnswer = split.readAll();
    QVERIFY2(splitAnswer.startsWith("HTTP/1.1 200 OK"), splitAnswer.left(200).constData());

    const QByteArray answer = ask(get("/status.json?key=abcdefghjkmn") + "\r\n");
    QVERIFY2(answer.startsWith("HTTP/1.1 200 OK"), answer.left(200).constData());
    const QByteArray body = answer.mid(answer.indexOf("\r\n\r\n") + 4);
    QCOMPARE(QJsonDocument::fromJson(body).object()["state"].toString(), QString("Monitoring"));
    QVERIFY(ask(get("/status.json") + "\r\n").startsWith("HTTP/1.1 403"));

    // A second server cannot take the same port, and says why.
    StatusServer other(sources());
    QVERIFY(!other.start(port, "x", error, QHostAddress::LocalHost));
    QVERIFY(!error.isEmpty());

    // After Stop nothing answers.
    server.stop();
    QVERIFY(!server.listening());
    QCOMPARE(ask(get("/status.json?key=abcdefghjkmn") + "\r\n"), QByteArray("no connection"));
}

void RemoteTest::theAddressFitsInAReadableQrCode()
{
    const QImage code = qrImage(statusUrl("192.168.178.123", 8080, "abcdefghjkmn"), 4);
    QVERIFY(!code.isNull());
    QCOMPARE(code.width(), code.height());
    const int modules = code.width() / 4 - 8;
    QVERIFY2(modules >= 21 && (modules - 17) % 4 == 0, qPrintable(QString::number(modules)));
    const auto dark = [&code](int x, int y) { return qGray(code.pixel((x + 4) * 4 + 2, (y + 4) * 4 + 2)) < 128; };
    // The margin is light; each finder pattern has a dark ring, a light ring and a dark centre.
    QVERIFY(qGray(code.pixel(1, 1)) > 128);
    for (const QPoint corner : {QPoint(0, 0), QPoint(modules - 7, 0), QPoint(0, modules - 7)})
    {
        QVERIFY(dark(corner.x(), corner.y()));
        QVERIFY(!dark(corner.x() + 1, corner.y() + 1));
        QVERIFY(dark(corner.x() + 3, corner.y() + 3));
    }
    QVERIFY(qrImage(QString(5000, 'a')).isNull());
}

void RemoteTest::timelapseSnapshotEndpointsAndConcurrentRequests()
{
    const QString key = "abcdefghjkmn";
    RemoteStatus state = example();
    state.state = "Time-lapse";
    state.detail = "TIME-LAPSE";
    state.timelapse = "Stacks: 12 · Next burst in 18 s";
    state.stacks = 12;
    state.hasLastStack = true;
    state.hasPreview = true;
    state.lock = "Feature Lock";
    state.locked = true;

    const QByteArray fakePng("PNGSTACKBYTES12345");
    const QByteArray fakeJpg("JPGPREVIEWBYTES67890");
    RemoteSources src;
    src.status = [&] { return state; };
    src.lastStack = [&] { return fakePng; };
    src.preview = [&] { return fakeJpg; };

    // Verify Time-lapse JSON fields and image endpoints directly.
    const QJsonObject obj = QJsonDocument::fromJson(statusJson(state)).object();
    QCOMPARE(obj["state"].toString(), QString("Time-lapse"));
    QCOMPARE(obj["timelapse"].toString(), QString("Stacks: 12 · Next burst in 18 s"));
    QCOMPARE(obj["stacks"].toInt(), 12);
    QCOMPARE(obj["hasLastStack"].toBool(), true);
    QCOMPARE(obj["hasPreview"].toBool(), true);
    QCOMPARE(obj["lock"].toString(), QString("Feature Lock"));

    const RemoteResponse stackResp = respond(get("/last-stack.png?key=" + key.toLatin1() + "&t=12"), key, src);
    QCOMPARE(stackResp.code, 200);
    QCOMPARE(stackResp.type, QByteArray("image/png"));
    QCOMPARE(stackResp.body, fakePng);
    const RemoteResponse prevResp = respond(get("/preview.jpg?key=" + key.toLatin1()), key, src);
    QCOMPARE(prevResp.code, 200);
    QCOMPARE(prevResp.type, QByteArray("image/jpeg"));
    QCOMPARE(prevResp.body, fakeJpg);
    QCOMPARE(respond(get("/last-stack.png?key=" + key.toLatin1()), key, sources()).code, 404);

    // Burst of 12 simultaneous TCP clients hitting /status.json, /last-stack.png,
    // /preview.jpg, and an unauthorized key while the snapshot updates.
    StatusServer server(src);
    QString err;
    QVERIFY2(server.start(0, key, err, QHostAddress::LocalHost), qPrintable(err));
    const quint16 port = server.port();

    struct Client
    {
        std::unique_ptr<QTcpSocket> sock;
        QByteArray request;
        QByteArray reply;
        int expectedCode{200};
        QByteArray expectedBody;
    };
    std::vector<Client> clients;
    const QByteArray k = key.toLatin1();
    for (int i = 0; i < 12; ++i)
    {
        Client c;
        c.sock = std::make_unique<QTcpSocket>();
        if (i % 4 == 0)
        {
            c.request = get("/status.json?key=" + k) + "\r\n";
            c.expectedCode = 200;
        }
        else if (i % 4 == 1)
        {
            c.request = get("/last-stack.png?key=" + k) + "\r\n";
            c.expectedCode = 200;
            c.expectedBody = fakePng;
        }
        else if (i % 4 == 2)
        {
            c.request = get("/preview.jpg?key=" + k) + "\r\n";
            c.expectedCode = 200;
            c.expectedBody = fakeJpg;
        }
        else
        {
            c.request = get("/status.json?key=invalidkey00") + "\r\n";
            c.expectedCode = 403;
        }
        c.sock->connectToHost(QHostAddress::LocalHost, port);
        clients.push_back(std::move(c));
    }

    for (auto& c : clients)
    {
        QVERIFY(c.sock->waitForConnected(3000));
        c.sock->write(c.request);
    }

    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 5000)
    {
        state.stacks += 1;
        QTest::qWait(10);
        bool allDone = true;
        for (auto& c : clients)
        {
            c.reply += c.sock->readAll();
            if (c.sock->state() != QAbstractSocket::UnconnectedState) allDone = false;
        }
        if (allDone) break;
    }

    for (auto& c : clients)
    {
        c.reply += c.sock->readAll();
        QCOMPARE(c.sock->state(), QAbstractSocket::UnconnectedState);
        const QByteArray expectedHead = "HTTP/1.1 " + QByteArray::number(c.expectedCode) + " ";
        QVERIFY2(c.reply.startsWith(expectedHead), c.reply.left(120).constData());
        const int sep = c.reply.indexOf("\r\n\r\n");
        QVERIFY(sep > 0);
        const QByteArray body = c.reply.mid(sep + 4);
        if (!c.expectedBody.isEmpty())
            QCOMPARE(body, c.expectedBody);
        else if (c.expectedCode == 200)
            QCOMPARE(QJsonDocument::fromJson(body).object()["state"].toString(), QString("Time-lapse"));
    }
    server.stop();
}

QTEST_MAIN(RemoteTest)
#include "remote_tests.moc"
