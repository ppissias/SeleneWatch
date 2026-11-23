#include "remote/status_server.h"

#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace {

constexpr int kMaxOpen = 16;
constexpr int kRequestTimeoutMs = 5000;
// A request's first line and headers; the page sends no body.
constexpr qsizetype kMaxHead = 8192;

} // namespace

StatusServer::StatusServer(RemoteSources sources, QObject* parent) : QObject(parent), sources_(std::move(sources)) {}

StatusServer::~StatusServer() { stop(); }

bool StatusServer::start(quint16 port, QString key, QString& error, const QHostAddress& address)
{
    stop();
    key_ = std::move(key);
    server_ = new QTcpServer(this);
    if (!server_->listen(address, port))
    {
        error = server_->errorString();
        delete server_;
        server_ = nullptr;
        return false;
    }
    connect(server_, &QTcpServer::pendingConnectionAvailable, this, &StatusServer::onConnection);
    return true;
}

void StatusServer::stop()
{
    if (!server_) return;
    server_->close();
    delete server_;
    server_ = nullptr;
}

bool StatusServer::listening() const { return server_ && server_->isListening(); }

quint16 StatusServer::port() const { return server_ ? server_->serverPort() : 0; }

void StatusServer::onConnection()
{
    while (server_ && server_->hasPendingConnections())
    {
        QTcpSocket* socket = server_->nextPendingConnection();
        if (!socket) return;
        // Owned by this object, so a socket still open when the server stops
        // goes with it.
        socket->setParent(this);
        if (open_ >= kMaxOpen || !allowedPeer(socket->peerAddress()))
        {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ++open_;
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            --open_;
            socket->deleteLater();
        });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadable(socket); });
        QTimer::singleShot(kRequestTimeoutMs, socket, [socket] { socket->abort(); });
        if (socket->bytesAvailable() > 0) onReadable(socket);
    }
}

void StatusServer::onReadable(QTcpSocket* socket)
{
    if (socket->property("answered").toBool()) return;
    QByteArray head = socket->property("head").toByteArray() + socket->read(kMaxHead);
    const qsizetype end = head.indexOf("\r\n\r\n");
    if (end < 0)
    {
        if (head.size() >= kMaxHead) socket->abort();
        else socket->setProperty("head", head);
        return;
    }
    socket->setProperty("answered", true);
    socket->write(httpResponse(respond(head.left(end + 2), key_, sources_)));
    socket->disconnectFromHost();
}
