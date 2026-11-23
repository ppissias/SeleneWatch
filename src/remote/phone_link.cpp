#include "remote/phone_link.h"

#include "remote/status_page.h"

#include "qrcodegen.hpp"

#include <QtGui/QPainter>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkInterface>

#include <algorithm>

namespace {

// Lower is likelier to be the network the phone is on.
int rankOf(const QNetworkInterface& adapter, const QHostAddress& address)
{
    const QString name = adapter.humanReadableName() + " " + adapter.name();
    for (const char* virtualName : {"vEthernet", "Hyper-V", "WSL", "VirtualBox", "VMware", "Docker", "Npcap"})
        if (name.contains(virtualName, Qt::CaseInsensitive)) return 4;
    if (address.isInSubnet(QHostAddress("100.64.0.0"), 10)) return 3;
    if (adapter.type() == QNetworkInterface::Wifi) return 0;
    if (adapter.type() == QNetworkInterface::Ethernet) return 1;
    return 2;
}

} // namespace

std::vector<StatusAddress> statusAddresses()
{
    std::vector<std::pair<int, StatusAddress>> found;
    for (const QNetworkInterface& adapter : QNetworkInterface::allInterfaces())
    {
        const auto flags = adapter.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry& entry : adapter.addressEntries())
        {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLinkLocal() || !allowedPeer(address)) continue;
            found.push_back({rankOf(adapter, address), {address.toString(), adapter.humanReadableName()}});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<StatusAddress> addresses;
    for (auto& [rank, address] : found) addresses.push_back(std::move(address));
    return addresses;
}

QString statusUrl(const QString& host, quint16 port, const QString& key)
{
    return QString("http://%1:%2/?key=%3").arg(host).arg(port).arg(key);
}

QString localNetworkName()
{
    return QHostInfo::localHostName().toLower() + ".local";
}

QImage qrImage(const QString& text, int moduleSize)
{
    try
    {
        const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        constexpr int kMargin = 4;
        const int side = (code.getSize() + 2 * kMargin) * moduleSize;
        QImage image(side, side, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        for (int y = 0; y < code.getSize(); ++y)
            for (int x = 0; x < code.getSize(); ++x)
                if (code.getModule(x, y))
                    painter.fillRect((x + kMargin) * moduleSize, (y + kMargin) * moduleSize, moduleSize, moduleSize, Qt::black);
        return image;
    }
    catch (const qrcodegen::data_too_long&)
    {
        return {};
    }
}
