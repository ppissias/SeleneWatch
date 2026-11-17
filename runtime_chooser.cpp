#include "runtime_chooser.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>

RuntimeChooser::RuntimeChooser(QString vendor, QString fileName, QString settingsKey, const QString& objectSuffix, QWidget* parent)
    : QWidget(parent), vendor_(std::move(vendor)), fileName_(std::move(fileName)), settingsKey_(std::move(settingsKey))
{
    path_ = QSettings().value(settingsKey_).toString();
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    choose_ = new QPushButton(QString("Choose %1 DLL…").arg(vendor_), this);
    choose_->setObjectName(QString("choose%1Runtime").arg(objectSuffix));
    automatic_ = new QPushButton("Use automatic discovery", this);
    automatic_->setObjectName(QString("automatic%1Runtime").arg(objectSuffix));
    layout->addWidget(choose_);
    layout->addWidget(automatic_);
    connect(choose_, &QPushButton::clicked, this, &RuntimeChooser::choose);
    connect(automatic_, &QPushButton::clicked, this, [this] { setPath({}); });
    setRunning(false);
}

void RuntimeChooser::choose()
{
    if (running_) return;
    const auto directory = path_.isEmpty() ? QString{} : QFileInfo(path_).absolutePath();
    const auto path = QFileDialog::getOpenFileName(this, QString("Select the installed x64 %1 runtime").arg(vendor_), directory,
        QString("%1 runtime (%2)").arg(vendor_, fileName_));
    if (!path.isEmpty()) setPath(path);
}

void RuntimeChooser::setPath(QString path)
{
    if (running_) return;
    path_ = std::move(path);
    QSettings saved;
    if (path_.isEmpty()) saved.remove(settingsKey_);
    else saved.setValue(settingsKey_, path_);
    saved.sync();
    setRunning(running_);
    emit changed();
}

void RuntimeChooser::setRunning(bool running)
{
    running_ = running;
    choose_->setEnabled(!running);
    choose_->setToolTip(path_.isEmpty() ? QString("Using automatic %1 runtime discovery").arg(vendor_) : QDir::toNativeSeparators(path_));
    automatic_->setEnabled(!running && !path_.isEmpty());
}
