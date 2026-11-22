#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUrl>

class QNetworkAccessManager;

// One asynchronous GitHub release check per application launch. Only a newer
// stable YYYY.MM-NN release produces a notice; all failures stay silent.
// The checker owns its network manager unless a test supplies one that outlives it.
// It has no dependency on the observing session or the frame path.
class ReleaseChecker : public QObject
{
    Q_OBJECT

public:
    explicit ReleaseChecker(QString currentRelease, QObject* parent = nullptr,
        QNetworkAccessManager* network = nullptr);
    void check();
    [[nodiscard]] static QUrl releasesUrl();

signals:
    void newerReleaseAvailable(const QString& tag);

private:
    QString currentRelease_;
    QNetworkAccessManager* network_;
    bool checked_{false};
};
