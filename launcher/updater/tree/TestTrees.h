#pragma once
// Shared by the new updater's unit tests: the test trees from make-test-trees.py

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUrl>

#include "updater/SystemRequirements.h"

namespace TestTrees
{

inline QString path(const QString &tree)
{
    return QFINDTESTDATA("data/trees/" + tree);
}

inline QByteArray read(const QString &tree, const QString &name)
{
    QFile file(QDir(path(tree)).absoluteFilePath(name));
    file.open(QFile::ReadOnly);
    return file.readAll();
}

// the tree as a base URL, the way mirrors are given
inline QString url(const QString &tree)
{
    return QUrl::fromLocalFile(QDir(path(tree)).absolutePath() + "/").toString();
}

// keys/1.json of `main`: the floor of every test
inline QByteArray floor()
{
    return read("main", "keys/1.json");
}

inline QStringList rootKeys()
{
    QStringList out;
    for(auto key: QJsonDocument::fromJson(floor()).object().value("root_keys").toArray())
    {
        out.append(key.toString());
    }
    return out;
}

// a machine that runs the trees' linux-x86_64-qt5 variant of every build
inline SystemRequirements::System linuxSystem()
{
    SystemRequirements::System system;
    system.os = "linux";
    system.osVersion = "6.1.0";
    system.machineArch = "x86_64";
    system.launcherArch = "x86_64";
    system.glibc = "2.27";
    system.glibcxx = "3.4.25";
    system.qt = "5.9.5";
    return system;
}

}
