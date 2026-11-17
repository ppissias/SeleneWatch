#pragma once

#include <QtWidgets/QWidget>

class QPushButton;

// For a camera family whose SDK DLL the observer installs (QHY, Player One,
// ToupTek): a button to choose that DLL and one to go back to the automatic
// locations. The choice is saved under settingsKey and survives restarts.
class RuntimeChooser final : public QWidget
{
    Q_OBJECT
public:
    // objectSuffix names the buttons "choose<suffix>Runtime" and "automatic<suffix>Runtime".
    RuntimeChooser(QString vendor, QString fileName, QString settingsKey, const QString& objectSuffix, QWidget* parent);
    // The chosen DLL, or empty for the automatic locations.
    [[nodiscard]] QString path() const { return path_; }
    void setRunning(bool running);

signals:
    // The observer chose a DLL or went back to automatic; refresh discovery.
    void changed();

private:
    void choose();
    void setPath(QString path);
    QString vendor_, fileName_, settingsKey_, path_;
    QPushButton *choose_, *automatic_;
    bool running_{false};
};
