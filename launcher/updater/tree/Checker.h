#pragma once
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>

#include <functional>
#include <memory>

#include "Formats.h"
#include "Plan.h"
#include "State.h"
#include "QObjectPtr.h"

namespace TreeUpdater
{

/**
 * What the new updater is built with (CMake: Launcher_UPDATER_*), and where it runs.
 */
struct Config
{
    // the compiled-in hosts: base URLs of the tree, tried after the mirror list
    QStringList mirrors;
    // the pinned root keys (hex) and the compiled-in keys document, which must list exactly them
    QStringList rootKeys;
    QByteArray floorKeys;
    // the lowest tags serial accepted when nothing higher is stored
    qint64 serialFloor = 0;
    // this build's variant ID and build number
    QString variant;
    qint64 currentBuild = -1;
    // where the verified state is kept
    QString statePath;
    // the installation root, as the apply step sees it
    QString root;
    SystemRequirements::System system;
    // for tests: the time to check expiry against, and how mirror groups are shuffled
    QDateTime now;
    std::function<void(QStringList &)> shuffle = randomShuffle;
};

struct CheckResult
{
    enum Kind
    {
        // on the tier's build already
        NoUpdate,
        // a newer build
        Update,
        // an older build, after switching tiers
        Downgrade,
        // the tier is on an older build; nothing offered without a tier switch
        BehindInstalled,
        // no variant of the build runs here (messages say why)
        NotForThisMachine,
        // the build needs something the apply step can't do (messages say what)
        CannotApply,
        // no mirror gave a verified answer (messages say what went wrong where)
        Failed
    } kind = Failed;
    Tier tier = Tier::Stable;
    // as given to check()
    bool tierSwitched = false;
    bool notify = false;
    qint64 currentBuild = -1;
    // the tier's build, and the commit it was made from (when its manifest was read)
    qint64 build = 0;
    QString commit;
    // the installed build was removed from the servers
    bool currentWithdrawn = false;
    // what to install
    Variant variant;
    // the installed build's file list, when its manifest is still there
    bool haveCurrentFiles = false;
    QList<FileEntry> currentFiles;
    QStringList messages;
    // where to download from: the mirror that answered first, then the others
    QStringList mirrors;
};

/**
 * Checks the tree for an update: the keys documents from the newest known one forward, the
 * mirror list, the tags, and the manifests of the tier's build and the installed build. All
 * from one mirror; when anything fails there (unreachable, an error, a document that doesn't
 * verify or is older than what was seen already), the whole check starts again on the next
 * one. Every signature is checked over the raw bytes before anything is parsed.
 */
class Checker : public QObject
{
    Q_OBJECT
public:
    Checker(shared_qobject_ptr<QNetworkAccessManager> network, const Config &config);

    // empty when the compiled-in configuration is usable
    QString configError() const
    {
        return m_state.error();
    }
    bool isChecking() const
    {
        return m_checking;
    }

    /**
     * Start a check for the tier. `tierSwitched`: the user just switched to it, so an older
     * build is offered as a downgrade. `notify` is passed on to the result, for the UI to tell
     * the user about every outcome (a check they asked for). While a check runs, it starts
     * when that one is done (only the last one asked for).
     */
    void check(Tier tier, bool tierSwitched, bool notify);

signals:
    void finished(const TreeUpdater::CheckResult &result);

private:
    void startMirror();
    void nextKeys();
    void afterKeys();
    void afterTags();
    void afterBuild();
    void mirrorFailed(const QString &error);
    void done(CheckResult::Kind kind, const QStringList &messages = QStringList());

    QUrl url(const QString &name) const;
    void fetchSigned(const QString &name, int limit, const QStringList &trusted, std::function<void(const QByteArray &)> then);
    void fetchOptionalSigned(const QString &name, int limit, const QStringList &trusted, std::function<void(bool, const QByteArray &)> then);

    shared_qobject_ptr<QNetworkAccessManager> m_network;
    Config m_config;
    State m_state;

    bool m_checking = false;
    bool m_stateChanged = false;
    Tier m_tier = Tier::Stable;
    bool m_tierSwitched = false;
    bool m_notify = false;
    // a check asked for while one ran
    bool m_pending = false;
    Tier m_pendingTier = Tier::Stable;
    bool m_pendingTierSwitched = false;
    bool m_pendingNotify = false;
    QStringList m_mirrors;
    int m_mirrorIndex = 0;
    QStringList m_errors;

    // for the check on the current mirror
    KeysDocument m_keys;
    QByteArray m_keysData;
    TagsDocument m_tags;
    TierDecision m_decision;
    BuildManifest m_build;
    CheckResult m_result;
};

}

Q_DECLARE_METATYPE(TreeUpdater::CheckResult)
