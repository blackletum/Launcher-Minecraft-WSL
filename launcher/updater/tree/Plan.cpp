#include "Plan.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>

#include <algorithm>
#include <random>

#include "FileSystem.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace TreeUpdater
{

QString tierName(Tier tier)
{
    switch(tier)
    {
        case Tier::Latest:
            return "latest";
        case Tier::Testing:
            return "testing";
        case Tier::Stable:
        default:
            return "stable";
    }
}

bool parseTier(const QString &name, Tier &tier)
{
    for(auto t: {Tier::Latest, Tier::Testing, Tier::Stable})
    {
        if(name == tierName(t))
        {
            tier = t;
            return true;
        }
    }
    return false;
}

TierDecision decideTier(const TagsDocument &tags, Tier tier, qint64 currentBuild, bool tierSwitched)
{
    TierDecision out;
    switch(tier)
    {
        case Tier::Latest:
            out.build = tags.latest;
            break;
        case Tier::Testing:
            // the publisher writes it resolved already; the documents can't say otherwise, this is for clarity
            out.build = std::max(tags.testing, tags.stable);
            break;
        case Tier::Stable:
            out.build = tags.stable;
            break;
    }
    if(out.build > currentBuild)
    {
        out.kind = TierDecision::Update;
    }
    else if(out.build == currentBuild)
    {
        out.kind = TierDecision::NoUpdate;
    }
    else
    {
        out.kind = tierSwitched ? TierDecision::Downgrade : TierDecision::BehindInstalled;
    }
    return out;
}

VariantChoice chooseVariant(const BuildManifest &build, const QString &ownVariant, const SystemRequirements::System &system)
{
    VariantChoice out;
    QMap<QString, QString> successor;
    for(auto &v: build.variants)
    {
        for(auto &other: v.supersedes)
        {
            successor.insert(other, v.id);
        }
    }
    // the manifest has no loops in it (checked when it was parsed)
    QStringList chain;
    auto current = ownVariant;
    while(successor.contains(current))
    {
        current = successor.value(current);
        chain.append(current);
    }

    QStringList firstUnmet;
    for(int i = chain.size() - 1; i >= 0; i--)
    {
        auto candidate = build.variants.value(chain[i]);
        auto unmet = SystemRequirements::unmetRequirements(candidate.requirements, system);
        if(unmet.isEmpty())
        {
            out.found = true;
            out.variant = candidate;
            return out;
        }
        if(i == 0)
        {
            firstUnmet = unmet;
        }
    }
    if(build.variants.contains(ownVariant))
    {
        auto own = build.variants.value(ownVariant);
        auto unmet = SystemRequirements::unmetRequirements(own.requirements, system);
        if(unmet.isEmpty())
        {
            out.found = true;
            out.variant = own;
            return out;
        }
        out.unmet = unmet;
        return out;
    }
    if(!firstUnmet.isEmpty())
    {
        out.unmet = firstUnmet;
        return out;
    }
    out.unmet.append(QObject::tr("Build %1 has no version for this installation (%2).").arg(build.build).arg(ownVariant));
    return out;
}

QStringList mirrorOrder(const QList<QStringList> &groups, const QStringList &compiledIn, std::function<void(QStringList &)> shuffle)
{
    QStringList out;
    for(auto group: groups)
    {
        shuffle(group);
        out.append(group);
    }
    for(auto &host: compiledIn)
    {
        if(!out.contains(host))
        {
            out.append(host);
        }
    }
    return out;
}

void randomShuffle(QStringList &list)
{
    // MinGW's std::random_device can give the same numbers every time, so the time goes in too
    std::random_device device;
    std::mt19937 generator(device() ^ uint(QDateTime::currentMSecsSinceEpoch()));
    std::shuffle(list.begin(), list.end(), generator);
}

QStringList missingLinks(const QList<LinkEntry> &links, const QString &root)
{
    QStringList out;
    for(auto &link: links)
    {
        auto path = FS::PathCombine(root, link.path);
#ifdef Q_OS_WIN
        // Windows variants have no links
        out.append(link.path);
        Q_UNUSED(path);
#else
        auto local = QFile::encodeName(path);
        char buffer[4096];
        auto size = readlink(local.constData(), buffer, sizeof(buffer));
        if(size < 0 || size >= ssize_t(sizeof(buffer)) || QFile::decodeName(QByteArray(buffer, int(size))) != link.target)
        {
            out.append(link.path);
        }
#endif
    }
    return out;
}

bool planFiles(const Variant &next, const QList<FileEntry> *current, const QString &root, const QString &tempPath,
               QList<Download> &downloads, GoUpdate::OperationList &operations, QString &error)
{
    downloads.clear();
    operations.clear();

    QSet<QString> nextPaths;
    for(auto &file: next.files)
    {
        nextPaths.insert(file.path);
    }
    for(auto &link: next.links)
    {
        nextPaths.insert(link.path);
    }
    if(current)
    {
        for(auto &file: *current)
        {
            if(!nextPaths.contains(file.path) && QFileInfo(FS::PathCombine(root, file.path)).exists())
            {
                operations.append(GoUpdate::Operation::DeleteOp(file.path));
            }
        }
    }

    int number = 0;
    for(auto &file: next.files)
    {
        auto path = FS::PathCombine(root, file.path);
        QFileInfo info(path);
        if(info.exists())
        {
            QFile installed(path);
            if(!info.isReadable() || !info.isWritable() || !installed.open(QFile::ReadOnly))
            {
                error = QObject::tr("The installed file %1 can't be read or written.").arg(path);
                downloads.clear();
                operations.clear();
                return false;
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(&installed);
            if(info.size() == file.size && QString::fromLatin1(hash.result().toHex()) == file.sha256)
            {
                continue;
            }
        }
        Download download;
        download.file = file;
        // the apply step moves each one into place, so every operation has its own file
        download.target = FS::PathCombine(tempPath, QString("%1-%2").arg(number++).arg(file.path.section('/', -1)));
        downloads.append(download);
        operations.append(GoUpdate::Operation::CopyOp(download.target, file.path, file.executable ? 0755 : 0644));
    }
    return true;
}

}
