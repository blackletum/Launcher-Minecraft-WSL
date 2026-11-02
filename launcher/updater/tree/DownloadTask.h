#pragma once
#include <QTemporaryDir>

#include "tasks/Task.h"
#include "updater/GoUpdate.h"
#include "Checker.h"
#include "Plan.h"
#include "QObjectPtr.h"

namespace TreeUpdater
{

/**
 * Downloads what an update needs into a new directory and makes the existing apply step's
 * operations for it. Content files come from the mirror that answered the check; when one
 * fails (unreachable, an error, the wrong size or SHA-256), it goes on with the next mirror.
 */
class DownloadTask : public Task
{
    Q_OBJECT
public:
    /**
     * `target` is a template: XXXXXX at the end is replaced by something unique.
     */
    DownloadTask(shared_qobject_ptr<QNetworkAccessManager> network, const CheckResult &update, const QString &root,
                 const QString &target, QObject *parent = nullptr);

    QString updateFilesDir();
    GoUpdate::OperationList operations();

protected:
    void executeTask() override;

private:
    void downloadNext();
    void mirrorFailed(const QString &error);

    shared_qobject_ptr<QNetworkAccessManager> m_network;
    CheckResult m_update;
    QString m_root;
    QTemporaryDir m_updateFilesDir;
    QList<Download> m_downloads;
    GoUpdate::OperationList m_operations;
    int m_next = 0;
    int m_mirrorIndex = 0;
    qint64 m_doneBytes = 0;
    qint64 m_totalBytes = 0;
    QStringList m_errors;
};

}
