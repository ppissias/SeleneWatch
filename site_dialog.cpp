#include "site_dialog.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>

namespace {

QDoubleSpinBox* number(QWidget* parent, const QString& name, double low, double high, int decimals, const QString& suffix,
    const QString& unset = {})
{
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(name);
    box->setRange(low, high);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    if (!unset.isEmpty()) box->setSpecialValueText(unset);
    return box;
}

} // namespace

SiteDialog::SiteDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Site and instrument");
    setObjectName("siteDialog");
    auto* layout = new QVBoxLayout(this);

    auto* who = new QGroupBox("Observer and site", this);
    auto* whoForm = new QFormLayout(who);
    observer_ = new QLineEdit(who); observer_->setObjectName("siteObserver");
    siteName_ = new QLineEdit(who); siteName_->setObjectName("siteName");
    position_ = new QCheckBox("Position known", who); position_->setObjectName("sitePositionKnown");
    latitude_ = number(who, "siteLatitude", -90, 90, 5, "°");
    latitude_->setToolTip("North positive.");
    longitude_ = number(who, "siteLongitude", -180, 180, 5, "°");
    longitude_->setToolTip("East positive.");
    height_ = number(who, "siteHeight", -500, 9000, 0, " m");
    whoForm->addRow("Observer", observer_);
    whoForm->addRow("Site", siteName_);
    whoForm->addRow(position_);
    whoForm->addRow("Latitude", latitude_);
    whoForm->addRow("Longitude", longitude_);
    whoForm->addRow("Height", height_);
    layout->addWidget(who);

    auto* with = new QGroupBox("Instrument", this);
    auto* withForm = new QFormLayout(with);
    telescope_ = new QLineEdit(with); telescope_->setObjectName("siteTelescope");
    aperture_ = number(with, "siteAperture", 0, 5000, 0, " mm", "not entered");
    focalLength_ = number(with, "siteFocalLength", 0, 100000, 0, " mm", "not entered");
    pixelSize_ = number(with, "sitePixelSize", 0, 100, 2, " µm", "not entered");
    pixelSize_->setToolTip("The pixel as imaged: with binning, the binned pixel.");
    scale_ = new QLabel(with); scale_->setObjectName("siteScale");
    withForm->addRow("Telescope", telescope_);
    withForm->addRow("Aperture", aperture_);
    withForm->addRow("Focal length", focalLength_);
    withForm->addRow("Pixel size", pixelSize_);
    withForm->addRow("Image scale", scale_);
    layout->addWidget(with);

    auto* time = new QGroupBox("Time", this);
    auto* timeForm = new QFormLayout(time);
    ntp_ = new QCheckBox("Compare the PC clock with an NTP server at each Start", time);
    ntp_->setObjectName("ntpCheck");
    ntp_->setToolTip("Sends a few small time queries over the internet. Off unless you turn it on.");
    server_ = new QLineEdit(time); server_->setObjectName("ntpServer");
    timeForm->addRow(ntp_);
    timeForm->addRow("Server", server_);
    timeForm->addRow(new QLabel("The Windows time service's status is always recorded when it runs.", time));
    layout->addWidget(time);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(position_, &QCheckBox::toggled, this, [this](bool known) {
        for (auto* box : {latitude_, longitude_, height_}) box->setEnabled(known);
    });
    for (auto* box : {focalLength_, pixelSize_})
        connect(box, &QDoubleSpinBox::valueChanged, this, &SiteDialog::showScale);
    connect(ntp_, &QCheckBox::toggled, server_, &QLineEdit::setEnabled);
    setSite({});
    setClockCheck(false, "pool.ntp.org");
}

void SiteDialog::setSite(const ObservingSite& site)
{
    observer_->setText(QString::fromStdString(site.observer));
    siteName_->setText(QString::fromStdString(site.site));
    position_->setChecked(site.hasPosition());
    latitude_->setValue(site.latitudeDeg.value_or(0));
    longitude_->setValue(site.longitudeDeg.value_or(0));
    height_->setValue(site.heightM.value_or(0));
    for (auto* box : {latitude_, longitude_, height_}) box->setEnabled(site.hasPosition());
    telescope_->setText(QString::fromStdString(site.telescope));
    aperture_->setValue(site.apertureMm);
    focalLength_->setValue(site.focalLengthMm);
    pixelSize_->setValue(site.pixelSizeUm);
    showScale();
}

ObservingSite SiteDialog::site() const
{
    ObservingSite site;
    site.observer = observer_->text().trimmed().toStdString();
    site.site = siteName_->text().trimmed().toStdString();
    if (position_->isChecked())
    {
        site.latitudeDeg = latitude_->value();
        site.longitudeDeg = longitude_->value();
        site.heightM = height_->value();
    }
    site.telescope = telescope_->text().trimmed().toStdString();
    site.apertureMm = aperture_->value();
    site.focalLengthMm = focalLength_->value();
    site.pixelSizeUm = pixelSize_->value();
    return site;
}

void SiteDialog::setClockCheck(bool ntp, const QString& server)
{
    ntp_->setChecked(ntp);
    server_->setText(server);
    server_->setEnabled(ntp);
}

bool SiteDialog::ntpCheck() const { return ntp_->isChecked(); }
QString SiteDialog::ntpServer() const { return server_->text().trimmed(); }

void SiteDialog::showScale()
{
    const auto scale = site().arcsecPerPixel();
    scale_->setText(scale ? QString("%1″ per pixel").arg(*scale, 0, 'f', 3) : QString("needs the focal length and pixel size"));
}
