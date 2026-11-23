#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtTest/QtTest>

#include <map>
#include <set>

// Design-2: the folders under src/ and the folders each may include from.
// Dependencies point one way, from the window down to the frame; a folder
// that is not listed, or an include that is not allowed, fails the test. Change
// this table only with DEVELOPER.md's Code organization section.
class DependencyRulesTest : public QObject
{
    Q_OBJECT

private slots:
    void eachFolderIncludesOnlyWhatItMay();
    void onlyTheWindowUsesQtWidgets();

private:
    struct Include
    {
        QString file;
        QString header;
    };
    // Every quoted #include under src/, by the folder of the file that has it.
    static std::map<QString, std::vector<Include>> includesByFolder();
};

namespace {

const QString kSource = QStringLiteral(SELENEWATCH_SOURCE_DIR) + "/src";

const std::map<QString, std::set<QString>>& allowedFolders()
{
    static const std::map<QString, std::set<QString>> allowed{
        {"core", {}},
        {"display", {"core"}},
        {"detection", {"core"}},
        {"capture", {"core", "detection"}},
        {"recording", {"core", "capture", "detection", "display"}},
        {"sources", {"core", "recording"}},
        {"cameras", {"core", "sources"}},
        {"sky", {"core", "detection"}},
        {"autotune", {"core", "detection", "sky"}},
        {"mounts", {"core"}},
        {"guiding", {"core", "mounts", "recording"}},
        {"timelapse", {"core", "display", "guiding", "recording"}},
        {"settings", {"core", "detection"}},
        {"session", {"core", "display", "detection", "capture", "recording", "sources", "cameras", "sky", "autotune",
            "mounts", "guiding", "timelapse", "settings"}},
        {"remote", {}},
        {"ui/widgets", {"core"}},
        {"ui/cameras", {"core", "sources", "cameras", "ui/widgets"}},
        {"ui", {"core", "display", "detection", "capture", "recording", "sources", "cameras", "sky", "autotune", "mounts",
            "guiding", "timelapse", "settings", "session", "remote", "ui/widgets", "ui/cameras"}},
        {"app", {"core", "recording", "session", "ui"}},
    };
    return allowed;
}

} // namespace

std::map<QString, std::vector<DependencyRulesTest::Include>> DependencyRulesTest::includesByFolder()
{
    static const QRegularExpression include(R"re(^\s*#\s*include\s+"([^"]+)")re",
        QRegularExpression::MultilineOption);
    std::map<QString, std::vector<Include>> found;
    const QDir root(kSource);
    QDirIterator it(kSource, {"*.h", "*.cpp"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        const QString relative = root.relativeFilePath(path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QString text = QString::fromUtf8(file.readAll());
        auto& list = found[QFileInfo(relative).path()];
        for (auto match = include.globalMatch(text); match.hasNext();)
            list.push_back({relative, match.next().captured(1)});
    }
    return found;
}

void DependencyRulesTest::eachFolderIncludesOnlyWhatItMay()
{
    QVERIFY2(QDir(kSource).exists(), qPrintable(kSource));
    const auto found = includesByFolder();
    QStringList problems;
    for (const auto& [folder, includes] : found)
    {
        const auto allowed = allowedFolders().find(folder);
        if (allowed == allowedFolders().end())
        {
            problems << QString("src/%1 is not in the table of folders").arg(folder);
            continue;
        }
        for (const Include& include : includes)
        {
            // A header beside the file, or one the build generates (ui_mainwindow.h, selenewatch_version.h).
            if (!include.header.contains('/')) continue;
            const QString target = QFileInfo(include.header).path();
            if (target != folder && !allowed->second.count(target))
                problems << QString("%1 includes %2: %3 may not use %4").arg(include.file, include.header, folder, target);
        }
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
}

void DependencyRulesTest::onlyTheWindowUsesQtWidgets()
{
    QStringList problems;
    const QDir root(kSource);
    QDirIterator it(kSource, {"*.h", "*.cpp"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString relative = root.relativeFilePath(it.next());
        if (relative.startsWith("ui/") || relative.startsWith("app/")) continue;
        QFile file(root.filePath(relative));
        if (!file.open(QIODevice::ReadOnly)) continue;
        if (QString::fromUtf8(file.readAll()).contains("<QtWidgets/"))
            problems << relative;
    }
    QVERIFY2(problems.isEmpty(), qPrintable("Qt Widgets outside ui/ and app/: " + problems.join(", ")));
}

QTEST_GUILESS_MAIN(DependencyRulesTest)
#include "dependency_rules_tests.moc"
