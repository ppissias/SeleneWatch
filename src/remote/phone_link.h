#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>

#include <vector>

// Feature-47: how a phone finds the status page. This computer's addresses
// on the local network, the page's address with its access key, and that
// address as a QR code for the phone's camera.

struct StatusAddress
{
    QString host;
    // The network adapter, as Windows names it ("Wi-Fi", "Ethernet").
    QString adapter;
};

// The IPv4 addresses a phone on the local network can reach, the likeliest
// first: Wi-Fi, then wired, then overlays such as Tailscale, then virtual
// adapters (Hyper-V, WSL, VirtualBox, VMware), which a phone cannot reach.
[[nodiscard]] std::vector<StatusAddress> statusAddresses();

// "http://host:port/?key=key".
[[nodiscard]] QString statusUrl(const QString& host, quint16 port, const QString& key);

// This computer's name on the local network: "<name>.local".
[[nodiscard]] QString localNetworkName();

// The text as a QR code (medium error correction), black on white with the
// standard four-module margin, each module this many pixels. Null when the
// text is too long for a QR code.
[[nodiscard]] QImage qrImage(const QString& text, int moduleSize = 6);
