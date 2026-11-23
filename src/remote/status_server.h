#pragma once

#include "remote/status_page.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <QtNetwork/QHostAddress>

class QTcpServer;
class QTcpSocket;

// Feature-47: serves the status page (status_page.h) to computers and phones
// on the local network.
//
// Runs on the thread that owns it, the window's, with Qt's event loop: a
// request is read, answered from RemoteSources and closed. It never waits,
// and it never touches the frame path; the sources read the window's own
// snapshots. A connection from outside the local network is closed at once,
// one that sends no complete request within 5 s is dropped, and at most 16
// are open at a time.
class StatusServer : public QObject
{
public:
    StatusServer(RemoteSources sources, QObject* parent = nullptr);
    ~StatusServer() override;

    // Listens at this port on every interface, or on the one given (tests
    // use the loopback, which no firewall asks about); port 0 takes a free one.
    // False, with why, when the port cannot be used.
    bool start(quint16 port, QString key, QString& error, const QHostAddress& address = QHostAddress::Any);
    void stop();
    [[nodiscard]] bool listening() const;
    [[nodiscard]] quint16 port() const;
    void setKey(QString key) { key_ = std::move(key); }

private:
    void onConnection();
    void onReadable(QTcpSocket* socket);

    RemoteSources sources_;
    QString key_;
    QTcpServer* server_{nullptr};
    int open_{0};
};
