#include "Checker.h"

#include <QDebug>
#include <QPointer>

#include "Fetch.h"

namespace TreeUpdater
{

Checker::Checker(shared_qobject_ptr<QNetworkAccessManager> network, const Config &config)
    : m_network(network), m_config(config), m_state(config.floorKeys, config.rootKeys, config.serialFloor)
{
    if(!m_state.error().isEmpty())
    {
        qCritical() << "Updater:" << m_state.error();
        return;
    }
    if(!m_config.statePath.isEmpty())
    {
        m_state.load(m_config.statePath);
    }
    qDebug() << "Updater: keys version" << m_state.keys().version << "tags serial" << m_state.serial() << "variant" << m_config.variant;
}

void Checker::check(Tier tier, bool tierSwitched, bool notify)
{
    if(m_checking)
    {
        qDebug() << "Updater: a check is running already, this one starts after it";
        m_pending = true;
        m_pendingTier = tier;
        m_pendingTierSwitched = tierSwitched;
        m_pendingNotify = notify;
        return;
    }
    m_checking = true;
    m_stateChanged = false;
    m_tier = tier;
    m_tierSwitched = tierSwitched;
    m_notify = notify;
    m_errors.clear();
    m_result = CheckResult();
    if(!configError().isEmpty())
    {
        done(CheckResult::Failed, {configError()});
        return;
    }
    QList<QStringList> groups;
    if(m_state.hasMirrors())
    {
        groups = m_state.mirrors().groups;
    }
    m_mirrors = mirrorOrder(groups, m_config.mirrors, m_config.shuffle);
    m_mirrorIndex = 0;
    qDebug() << "Updater: checking" << tierName(tier) << "on" << m_mirrors;
    startMirror();
}

QUrl Checker::url(const QString &name) const
{
    return QUrl(m_mirrors[m_mirrorIndex]).resolved(QUrl(name));
}

void Checker::startMirror()
{
    if(m_mirrorIndex >= m_mirrors.size())
    {
        if(m_mirrors.isEmpty())
        {
            m_errors.append(tr("No servers to update from are known."));
        }
        // nothing from a mirror that failed part way counts
        m_result = CheckResult();
        done(CheckResult::Failed);
        return;
    }
    m_result = CheckResult();
    m_keys = m_state.keys();
    m_keysData.clear();
    nextKeys();
}

void Checker::mirrorFailed(const QString &error)
{
    qWarning() << "Updater:" << m_mirrors[m_mirrorIndex] << error;
    m_errors.append(QString("%1: %2").arg(m_mirrors[m_mirrorIndex], error));
    m_mirrorIndex++;
    startMirror();
}

void Checker::fetchOptionalSigned(const QString &name, int limit, const QStringList &trusted, std::function<void(bool, const QByteArray &)> then)
{
    QPointer<Checker> self(this);
    Fetch::get(m_network, url(name), limit, [self, this, name, trusted, then](const FetchResult &document)
    {
        if(!self)
        {
            return;
        }
        if(document.status == FetchResult::NotFound)
        {
            then(false, QByteArray());
            return;
        }
        if(document.status != FetchResult::Ok)
        {
            mirrorFailed(QString("%1: %2").arg(name, document.error));
            return;
        }
        auto data = document.data;
        Fetch::get(m_network, url(name + ".sig"), MaxSignatureFileSize, [self, this, name, trusted, then, data](const FetchResult &signature)
        {
            if(!self)
            {
                return;
            }
            if(signature.status != FetchResult::Ok)
            {
                mirrorFailed(QString("%1.sig: %2").arg(name, signature.error));
                return;
            }
            // the signature over the raw bytes first; nothing is parsed before it verifies
            QString error;
            if(!checkSignatureFile(data, signature.data, trusted, error))
            {
                mirrorFailed(QString("%1: %2").arg(name, error));
                return;
            }
            then(true, data);
        });
    });
}

void Checker::fetchSigned(const QString &name, int limit, const QStringList &trusted, std::function<void(const QByteArray &)> then)
{
    fetchOptionalSigned(name, limit, trusted, [this, name, then](bool found, const QByteArray &data)
    {
        if(!found)
        {
            mirrorFailed(QString("%1: not found").arg(name));
            return;
        }
        then(data);
    });
}

void Checker::nextKeys()
{
    auto version = m_keys.version + 1;
    auto name = QString("keys/%1.json").arg(version);
    // keys/N+1.json is checked against the root keys of keys/N.json
    fetchOptionalSigned(name, MaxDocumentSize, m_keys.rootKeys, [this, name, version](bool found, const QByteArray &data)
    {
        if(!found)
        {
            afterKeys();
            return;
        }
        KeysDocument keys;
        QString error;
        if(!parseKeys(data, version, keys, error))
        {
            mirrorFailed(QString("%1: %2").arg(name, error));
            return;
        }
        m_keys = keys;
        m_keysData = data;
        nextKeys();
    });
}

void Checker::afterKeys()
{
    // the documents on the way may have expired, the newest one must not have
    auto now = m_config.now.isValid() ? m_config.now : QDateTime::currentDateTimeUtc();
    if(m_keys.expires <= now)
    {
        mirrorFailed(tr("The newest keys document (version %1) expired at %2, so updates can't be trusted.")
                         .arg(m_keys.version).arg(m_keys.expires.toString(Qt::ISODate)));
        return;
    }
    if(!m_keysData.isEmpty() && m_state.acceptKeys(m_keys, m_keysData))
    {
        qDebug() << "Updater: now on keys version" << m_keys.version;
        m_stateChanged = true;
    }

    fetchSigned("mirrors.json", MaxDocumentSize, m_keys.onlineKeys, [this](const QByteArray &data)
    {
        MirrorsDocument mirrors;
        QString error;
        if(!parseMirrors(data, mirrors, error))
        {
            mirrorFailed(QString("mirrors.json: %1").arg(error));
            return;
        }
        // an older list is ignored, not refused: mirrors may lag behind
        if(m_state.acceptMirrors(mirrors, data))
        {
            m_stateChanged = true;
        }

        fetchSigned("tags.json", MaxDocumentSize, m_keys.onlineKeys, [this](const QByteArray &data)
        {
            QString error;
            if(!parseTags(data, m_tags, error))
            {
                mirrorFailed(QString("tags.json: %1").arg(error));
                return;
            }
            if(m_tags.serial < m_state.serial())
            {
                mirrorFailed(QString("tags.json: serial %1 is lower than %2, seen already").arg(m_tags.serial).arg(m_state.serial()));
                return;
            }
            if(m_state.acceptSerial(m_tags.serial))
            {
                m_stateChanged = true;
            }
            afterTags();
        });
    });
}

void Checker::afterTags()
{
    m_result.tier = m_tier;
    m_result.currentBuild = m_config.currentBuild;
    m_result.currentWithdrawn = m_tags.withdrawn.contains(m_config.currentBuild);
    if(m_result.currentWithdrawn)
    {
        m_result.messages.append(tr("This build (%1) was withdrawn from the servers.").arg(m_config.currentBuild));
    }
    m_decision = decideTier(m_tags, m_tier, m_config.currentBuild, m_tierSwitched);
    m_result.build = m_decision.build;
    if(m_decision.kind == TierDecision::NoUpdate)
    {
        done(CheckResult::NoUpdate);
        return;
    }
    if(m_decision.kind == TierDecision::BehindInstalled)
    {
        done(CheckResult::BehindInstalled);
        return;
    }

    auto name = QString("builds/%1.json").arg(m_decision.build);
    fetchSigned(name, MaxManifestSize, m_keys.onlineKeys, [this, name](const QByteArray &data)
    {
        QString error;
        if(!parseBuild(data, m_decision.build, m_build, error))
        {
            mirrorFailed(QString("%1: %2").arg(name, error));
            return;
        }
        auto choice = chooseVariant(m_build, m_config.variant, m_config.system);
        if(!choice.found)
        {
            done(CheckResult::NotForThisMachine, choice.unmet);
            return;
        }
        m_result.variant = choice.variant;
        m_result.commit = m_build.commit;

        // the installed build's files, to know what to delete; it may be gone (withdrawn or purged)
        auto currentName = QString("builds/%1.json").arg(m_config.currentBuild);
        fetchOptionalSigned(currentName, MaxManifestSize, m_keys.onlineKeys, [this, currentName](bool found, const QByteArray &data)
        {
            if(found)
            {
                BuildManifest current;
                QString error;
                if(!parseBuild(data, m_config.currentBuild, current, error))
                {
                    mirrorFailed(QString("%1: %2").arg(currentName, error));
                    return;
                }
                if(current.variants.contains(m_config.variant))
                {
                    m_result.haveCurrentFiles = true;
                    m_result.currentFiles = current.variants.value(m_config.variant).files;
                }
            }
            afterBuild();
        });
    });
}

void Checker::afterBuild()
{
    auto missing = missingLinks(m_result.variant.links, m_config.root);
    if(!missing.isEmpty())
    {
        done(CheckResult::CannotApply, {tr("Build %1 needs symbolic links that this version can't create: %2")
                                            .arg(m_decision.build).arg(missing.join(", "))});
        return;
    }
    done(m_decision.kind == TierDecision::Downgrade ? CheckResult::Downgrade : CheckResult::Update);
}

void Checker::done(CheckResult::Kind kind, const QStringList &messages)
{
    m_result.kind = kind;
    m_result.tier = m_tier;
    m_result.tierSwitched = m_tierSwitched;
    m_result.notify = m_notify;
    m_result.currentBuild = m_config.currentBuild;
    m_result.messages.append(messages);
    if(kind == CheckResult::Failed)
    {
        m_result.messages.append(m_errors);
    }
    m_result.mirrors.clear();
    for(int i = 0; i < m_mirrors.size(); i++)
    {
        m_result.mirrors.append(m_mirrors[(m_mirrorIndex + i) % m_mirrors.size()]);
    }
    if(m_stateChanged && !m_config.statePath.isEmpty() && !m_state.save(m_config.statePath))
    {
        qWarning() << "Updater: couldn't save the state to" << m_config.statePath;
    }
    m_checking = false;
    qDebug() << "Updater: check finished:" << kind << "build" << m_result.build << m_result.messages;
    emit finished(m_result);
    if(m_pending && !m_checking)
    {
        m_pending = false;
        check(m_pendingTier, m_pendingTierSwitched, m_pendingNotify);
    }
}

}
