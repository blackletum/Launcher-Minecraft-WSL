#pragma once
#include <QString>
#include <QStringList>

#include <functional>

#include "Formats.h"
#include "updater/GoUpdate.h"

/**
 * The new updater's decisions, apart from the network: which build a tier gives, which variant
 * of a build this machine installs, the order of the mirrors, and what to download and change
 * on disk (as the existing apply step's operations).
 */
namespace TreeUpdater
{

enum class Tier
{
    Latest,
    Testing,
    Stable
};

// `latest`, `testing`, `stable`: the names in the tags document and in the settings
QString tierName(Tier tier);
bool parseTier(const QString &name, Tier &tier);

struct TierDecision
{
    enum Kind
    {
        // the tier's build is the installed one
        NoUpdate,
        // the tier's build is newer
        Update,
        // the tier's build is older, and the user switched to this tier: offered with a warning
        Downgrade,
        // the tier's build is older, and the user didn't ask for it: nothing is offered
        BehindInstalled
    } kind = NoUpdate;
    qint64 build = 0;
};

/**
 * What the tags offer an installed build on a tier. A Launcher never goes to an older build
 * unless `tierSwitched` (the user just switched to this tier).
 * Testing resolves to Stable when there is no newer Testing build.
 */
TierDecision decideTier(const TagsDocument &tags, Tier tier, qint64 currentBuild, bool tierSwitched);

struct VariantChoice
{
    bool found = false;
    Variant variant;
    // when nothing was found: why, for the user
    QStringList unmet;
};

/**
 * Which variant of a build this machine installs: following the chain of successors from the
 * Launcher's own variant, the furthest one whose requirements the machine meets; otherwise its
 * own variant, if the build has it and the machine meets it.
 */
VariantChoice chooseVariant(const BuildManifest &build, const QString &ownVariant, const SystemRequirements::System &system);

/**
 * The order to try mirrors in: the groups of the mirror list in order, each one shuffled
 * (`shuffle` reorders a group in place), then the compiled-in hosts that aren't in the list,
 * in their order.
 */
QStringList mirrorOrder(const QList<QStringList> &groups, const QStringList &compiledIn, std::function<void(QStringList &)> shuffle);
// shuffles with a random seed
void randomShuffle(QStringList &list);

/**
 * The symbolic links of a variant that aren't on disk under `root` as listed. The apply step
 * can't make links, so an update is only offered when they are all there already.
 */
QStringList missingLinks(const QList<LinkEntry> &links, const QString &root);

/**
 * A content file to download: where it goes, what it must be.
 */
struct Download
{
    FileEntry file;
    QString target;
};

/**
 * What it takes to go from the installed files to `next` under `root`: files whose content
 * differs are downloaded into `tempPath` and replace the installed ones; files of `current`
 * (the installed build's file list, when known) that `next` doesn't have are deleted.
 * Return false when an installed file can't be read.
 */
bool planFiles(const Variant &next, const QList<FileEntry> *current, const QString &root, const QString &tempPath,
               QList<Download> &downloads, GoUpdate::OperationList &operations, QString &error);

}
