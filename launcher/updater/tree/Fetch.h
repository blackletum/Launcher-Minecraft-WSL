#pragma once
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <functional>

#include "QObjectPtr.h"

namespace TreeUpdater
{

struct FetchResult
{
    enum Status
    {
        Ok,
        // the server says there is no such file
        NotFound,
        Failed
    } status = Failed;
    // the content, when not written to a file
    QByteArray data;
    // when written to a file: the SHA-256 of what was written, hex
    QString sha256;
    QString error;
};

/**
 * Gets one file of the tree, bypassing caches, and refuses more than `limit` bytes before
 * anything looks at them. With `filePath`, the content goes into that file instead of memory.
 * A transfer that stalls for 30 seconds fails. Calls `done` once, then deletes itself.
 */
class Fetch : public QObject
{
    Q_OBJECT
public:
    static void get(shared_qobject_ptr<QNetworkAccessManager> network, const QUrl &url, qint64 limit,
                    std::function<void(const FetchResult &)> done, const QString &filePath = QString(),
                    std::function<void(qint64)> progress = nullptr);

private:
    Fetch(qint64 limit, std::function<void(const FetchResult &)> done, std::function<void(qint64)> progress);
    void start(shared_qobject_ptr<QNetworkAccessManager> network, const QUrl &url, const QString &filePath);
    void readSome();
    void finish();
    void stop(const QString &error);

    qint64 m_limit;
    qint64 m_received = 0;
    std::function<void(const FetchResult &)> m_done;
    std::function<void(qint64)> m_progress;
    QNetworkReply *m_reply = nullptr;
    QFile m_file;
    QCryptographicHash m_hash;
    QTimer m_stall;
    FetchResult m_result;
    bool m_finished = false;
};

}
