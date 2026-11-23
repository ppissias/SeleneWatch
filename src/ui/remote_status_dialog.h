#pragma once

#include <QtCore/QString>
#include <QtWidgets/QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

// Feature-47: Session → Status on your phone. Turns the read-only status page
// on and off, and shows its address with the access key, as text and as a
// QR code for the phone's camera. The window owns the server and the saved
// settings; the dialog hands it each change through apply, which answers
// with an error, or nothing when the page now runs as asked.
class RemoteStatusDialog : public QDialog
{
public:
    struct Settings
    {
        bool enabled{false};
        quint16 port{8080};
        QString key;
    };

    RemoteStatusDialog(Settings settings, std::function<QString(const Settings&)> apply, QWidget* parent = nullptr);

private:
    void applySettings();
    void showAddress();

    Settings settings_;
    std::function<QString(const Settings&)> apply_;
    QString error_;
    QCheckBox* enabled_{nullptr};
    QSpinBox* port_{nullptr};
    QComboBox* addresses_{nullptr};
    QLabel* code_{nullptr};
    QLabel* url_{nullptr};
    QLabel* name_{nullptr};
    QLabel* state_{nullptr};
    QPushButton* copy_{nullptr};
    QPushButton* newKey_{nullptr};
};
