#include "ui/remote_status_dialog.h"

#include "remote/phone_link.h"
#include "remote/status_page.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPixmap>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

RemoteStatusDialog::RemoteStatusDialog(Settings settings, std::function<QString(const Settings&)> apply, QWidget* parent)
    : QDialog(parent), settings_(std::move(settings)), apply_(std::move(apply))
{
    setWindowTitle("Status on your phone");
    setObjectName("remoteStatusDialog");
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel("See what SeleneWatch is doing from your phone's browser: the mode, the lock and guiding, "
                             "this session's events and a small live view. The page only shows; nothing on it changes "
                             "SeleneWatch.", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    enabled_ = new QCheckBox("Show the status page on this network", this);
    enabled_->setObjectName("remoteEnabled");
    enabled_->setChecked(settings_.enabled);
    enabled_->setToolTip("Off by default. While on, SeleneWatch answers on the port below, only to computers and "
                         "phones on the local network that have the access key in the address.");
    layout->addWidget(enabled_);

    auto* form = new QFormLayout;
    port_ = new QSpinBox(this);
    port_->setObjectName("remotePort");
    port_->setRange(1024, 65535);
    port_->setValue(settings_.port);
    port_->setToolTip("Change it only if another program already uses 8080.");
    form->addRow("Port", port_);
    addresses_ = new QComboBox(this);
    addresses_->setObjectName("remoteAddress");
    addresses_->setToolTip("This computer's addresses on its networks, the likeliest first. Choose the one of the "
                           "network your phone is on (normally Wi-Fi).");
    for (const StatusAddress& address : statusAddresses())
        addresses_->addItem(QString("%1  (%2)").arg(address.host, address.adapter), address.host);
    form->addRow("Address", addresses_);
    layout->addLayout(form);

    code_ = new QLabel(this);
    code_->setObjectName("remoteQrCode");
    code_->setAlignment(Qt::AlignCenter);
    code_->setToolTip("Point your phone's camera at it and open the link it shows.");
    layout->addWidget(code_);
    url_ = new QLabel(this);
    url_->setObjectName("remoteUrl");
    url_->setAlignment(Qt::AlignCenter);
    url_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(url_);
    name_ = new QLabel(this);
    name_->setObjectName("remoteName");
    name_->setAlignment(Qt::AlignCenter);
    name_->setWordWrap(true);
    name_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(name_);

    auto* buttons = new QHBoxLayout;
    copy_ = new QPushButton("Copy address", this);
    copy_->setToolTip("Copy the address with its key, to send it to your phone another way.");
    newKey_ = new QPushButton("New key…", this);
    newKey_->setToolTip("Make a new access key. Addresses saved with the old key stop working.");
    buttons->addWidget(copy_);
    buttons->addWidget(newKey_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    state_ = new QLabel(this);
    state_->setObjectName("remoteState");
    state_->setWordWrap(true);
    layout->addWidget(state_);

    auto* help = new QLabel("Your phone must be on the same network as this computer. The first time, Windows may ask "
                            "whether SeleneWatch may use the network: allow it on private networks. If the router gives "
                            "this computer another address, scan the code again, or reserve its address in the router. "
                            "Away from home, a private network app such as Tailscale on both reaches it the same way.", this);
    help->setWordWrap(true);
    help->setStyleSheet("color: palette(placeholder-text);");
    layout->addWidget(help);

    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);

    connect(enabled_, &QCheckBox::toggled, this, [this] { applySettings(); });
    connect(port_, &QSpinBox::editingFinished, this, [this] {
        if (port_->value() != settings_.port) applySettings();
    });
    connect(addresses_, &QComboBox::currentIndexChanged, this, [this] { showAddress(); });
    connect(copy_, &QPushButton::clicked, this, [this] {
        if (addresses_->count())
            QGuiApplication::clipboard()->setText(statusUrl(addresses_->currentData().toString(), settings_.port, settings_.key));
    });
    connect(newKey_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "New key", "Make a new access key? The address saved on your phone stops "
                                  "working; scan the new code.") != QMessageBox::Yes)
            return;
        settings_.key = newAccessKey();
        applySettings();
    });
    showAddress();
}

void RemoteStatusDialog::applySettings()
{
    settings_.enabled = enabled_->isChecked();
    settings_.port = static_cast<quint16>(port_->value());
    error_ = apply_(settings_);
    showAddress();
}

void RemoteStatusDialog::showAddress()
{
    const bool on = settings_.enabled && error_.isEmpty();
    const bool known = addresses_->count() > 0;
    const QString host = known ? addresses_->currentData().toString() : QString();
    const QString url = known ? statusUrl(host, settings_.port, settings_.key) : QString();
    const QImage code = on && known ? qrImage(url, 5) : QImage();
    code_->setPixmap(code.isNull() ? QPixmap() : QPixmap::fromImage(code));
    code_->setVisible(on && known);
    url_->setText(on && known ? url : QString());
    url_->setVisible(on && known);
    name_->setText(on ? QString("On many phones this works too, and survives a new address:\n%1")
                            .arg(statusUrl(localNetworkName(), settings_.port, settings_.key))
                      : QString());
    name_->setVisible(on);
    addresses_->setEnabled(on && known);
    copy_->setEnabled(on && known);
    newKey_->setEnabled(on);
    if (!error_.isEmpty())
        state_->setText(QString("<b style='color:#c0392b'>Not running:</b> %1").arg(error_.toHtmlEscaped()));
    else if (!settings_.enabled)
        state_->setText("Off. Tick the box to show the page.");
    else if (!known)
        state_->setText("On, but this computer has no address on a local network. Connect it to your Wi-Fi.");
    else
        state_->setText(QString("<b>On:</b> answering on port %1.").arg(settings_.port));
    adjustSize();
}
