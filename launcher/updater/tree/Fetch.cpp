#include "Fetch.h"

#include <QNetworkRequest>

#include "BuildConfig.h"

namespace TreeUpdater
{

namespace {
const int StallTimeout = 30 * 1000;
}

void Fetch::get(shared_qobject_ptr<QNetworkAccessManager> network, const QUrl &url, qint64 limit,
                std::function<void(const FetchResult &)> done, const QString &filePath, std::function<void(qint64)> progress)
{
    auto fetch = new Fetch(limit, done, progress);
    fetch->start(network, url, filePath);
}

Fetch::Fetch(qint64 limit, std::function<void(const FetchResult &)> done, std::function<void(qint64)> progress)
    : m_limit(limit), m_done(done), m_progress(progress), m_hash(QCryptographicHash::Sha256)
{
    m_stall.setSingleShot(true);
    m_stall.setInterval(StallTimeout);
    connect(&m_stall, &QTimer::timeout, this, [this]() { stop("the transfer stalled"); });
}

void Fetch::start(shared_qobject_ptr<QNetworkAccessManager> network, const QUrl &url, const QString &filePath)
{
    if(!filePath.isEmpty())
    {
        m_file.setFileName(filePath);
        if(!m_file.open(QFile::WriteOnly | QFile::Truncate))
        {
            // after returning, so `done` is never called before get() returns
            QTimer::singleShot(0, this, [this, filePath]() { stop(QString("can't write %1").arg(filePath)); });
            return;
        }
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, BuildConfig.USER_AGENT_UNCACHED);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    // never from https to http
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    m_reply = network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &Fetch::readSome);
    connect(m_reply, &QNetworkReply::finished, this, &Fetch::finish);
    m_stall.start();
}

void Fetch::readSome()
{
    if(m_finished)
    {
        return;
    }
    m_stall.start();
    auto data = m_reply->readAll();
    m_received += data.size();
    if(m_received > m_limit)
    {
        stop(QString("more than %1 bytes").arg(m_limit));
        return;
    }
    if(m_file.isOpen())
    {
        m_hash.addData(data);
        if(m_file.write(data) != data.size())
        {
            stop(QString("can't write %1").arg(m_file.fileName()));
            return;
        }
    }
    else
    {
        m_result.data.append(data);
    }
    if(m_progress)
    {
        m_progress(m_received);
    }
}

void Fetch::finish()
{
    if(m_finished)
    {
        return;
    }
    readSome();
    if(m_finished)
    {
        return;
    }
    m_finished = true;
    m_stall.stop();
    auto status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if(m_reply->error() == QNetworkReply::ContentNotFoundError)
    {
        m_result.status = FetchResult::NotFound;
        m_result.error = "not found";
    }
    else if(m_reply->error() != QNetworkReply::NoError)
    {
        m_result.status = FetchResult::Failed;
        m_result.error = m_reply->errorString();
    }
    else if(status.isValid() && status.toInt() != 200)
    {
        m_result.status = FetchResult::Failed;
        m_result.error = QString("HTTP status %1").arg(status.toInt());
    }
    else
    {
        m_result.status = FetchResult::Ok;
    }
    if(m_file.isOpen())
    {
        m_file.close();
        m_result.sha256 = QString::fromLatin1(m_hash.result().toHex());
        if(m_result.status != FetchResult::Ok)
        {
            m_file.remove();
        }
    }
    m_reply->deleteLater();
    m_done(m_result);
    deleteLater();
}

void Fetch::stop(const QString &error)
{
    if(m_finished)
    {
        return;
    }
    m_finished = true;
    m_stall.stop();
    if(m_reply)
    {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
    if(m_file.isOpen())
    {
        m_file.close();
        m_file.remove();
    }
    m_result.status = FetchResult::Failed;
    m_result.error = error;
    m_result.data.clear();
    m_done(m_result);
    deleteLater();
}

}
