#include "DownloadTask.h"

#include <QDebug>
#include <QFileInfo>
#include <QPointer>

#include "Fetch.h"

namespace TreeUpdater
{

DownloadTask::DownloadTask(shared_qobject_ptr<QNetworkAccessManager> network, const CheckResult &update, const QString &root,
                           const QString &target, QObject *parent)
    : Task(parent), m_network(network), m_update(update), m_root(root), m_updateFilesDir(target)
{
    m_updateFilesDir.setAutoRemove(false);
}

QString DownloadTask::updateFilesDir()
{
    return m_updateFilesDir.path();
}

GoUpdate::OperationList DownloadTask::operations()
{
    return m_operations;
}

void DownloadTask::executeTask()
{
    if(!m_updateFilesDir.isValid())
    {
        emitFailed(tr("Couldn't create a folder for the update's files."));
        return;
    }
    setStatus(tr("Comparing the installed files with build %1...").arg(m_update.build));
    QString error;
    if(!planFiles(m_update.variant, m_update.haveCurrentFiles ? &m_update.currentFiles : nullptr, m_root,
                  m_updateFilesDir.path(), m_downloads, m_operations, error))
    {
        emitFailed(error);
        return;
    }
    for(auto &download: m_downloads)
    {
        m_totalBytes += download.file.size;
    }
    qDebug() << "Updater: downloading" << m_downloads.size() << "files," << m_totalBytes << "bytes, to" << m_updateFilesDir.path();
    downloadNext();
}

void DownloadTask::downloadNext()
{
    if(m_next >= m_downloads.size())
    {
        emitSucceeded();
        return;
    }
    if(m_mirrorIndex >= m_update.mirrors.size())
    {
        m_operations.clear();
        emitFailed(tr("Failed to download the update:\n%1").arg(m_errors.join("\n")));
        return;
    }
    if(m_downloads.size() == 1)
    {
        setStatus(tr("Downloading one update file."));
    }
    else
    {
        setStatus(tr("Downloading update file %1 of %2.").arg(m_next + 1).arg(m_downloads.size()));
    }
    auto download = m_downloads[m_next];
    auto url = QUrl(m_update.mirrors[m_mirrorIndex]).resolved(QUrl(contentPath(download.file.sha256)));
    QPointer<DownloadTask> self(this);
    auto progress = [self, this](qint64 received)
    {
        if(self)
        {
            setProgress(m_doneBytes + received, m_totalBytes);
        }
    };
    Fetch::get(m_network, url, download.file.size, [self, this, download](const FetchResult &result)
    {
        if(!self)
        {
            return;
        }
        if(result.status != FetchResult::Ok)
        {
            mirrorFailed(QString("%1: %2").arg(download.file.path, result.error));
            return;
        }
        QFileInfo written(download.target);
        if(written.size() != download.file.size || result.sha256 != download.file.sha256)
        {
            mirrorFailed(QString("%1: the downloaded file isn't the one the manifest lists").arg(download.file.path));
            return;
        }
        m_doneBytes += download.file.size;
        m_next++;
        downloadNext();
    }, download.target, progress);
}

void DownloadTask::mirrorFailed(const QString &error)
{
    qWarning() << "Updater:" << m_update.mirrors[m_mirrorIndex] << error;
    m_errors.append(QString("%1: %2").arg(m_update.mirrors[m_mirrorIndex], error));
    // the files that are done stay: they match their hashes, wherever they came from
    m_mirrorIndex++;
    downloadNext();
}

}
