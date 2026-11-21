#pragma once

#include "observing_site.h"

#include <QtWidgets/QDialog>

class QCheckBox;
class QComboBox;
class QPushButton;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

// Session → Site and instrument (Feature-14 Part C): who observes, from
// where, with what, and whether the PC clock is compared with an NTP server
// at each Start. It edits values; the window saves them and hands them on.
// Feature-29: the values can be saved as named presets and loaded back.
class SiteDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SiteDialog(QWidget* parent = nullptr);

    void setSite(const ObservingSite& site);
    [[nodiscard]] ObservingSite site() const;

    void setClockCheck(bool ntp, const QString& server);
    [[nodiscard]] bool ntpCheck() const;
    [[nodiscard]] QString ntpServer() const;

private:
    void showScale();
    // Fills the preset list and chooses the named preset, or none.
    void reloadPresets(const QString& select);

    QComboBox* preset_;
    QPushButton* deletePreset_;
    QLineEdit* presetName_;
    QPushButton* savePreset_;
    QLineEdit* observer_;
    QLineEdit* siteName_;
    QCheckBox* position_;
    QDoubleSpinBox* latitude_;
    QDoubleSpinBox* longitude_;
    QDoubleSpinBox* height_;
    QLineEdit* telescope_;
    QDoubleSpinBox* aperture_;
    QDoubleSpinBox* focalLength_;
    QDoubleSpinBox* pixelSize_;
    QLabel* scale_;
    QCheckBox* ntp_;
    QLineEdit* server_;
};
