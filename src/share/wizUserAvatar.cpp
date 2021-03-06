#include "wizUserAvatar.h"

#include <QImage>
#include <QTimer>
#include <QDebug>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>

#include "wizDatabase.h"
#include "../sync/apientry.h"


/* --------------------- CWizUserAvatarDownloaderHost --------------------- */

CWizUserAvatarDownloaderHost::CWizUserAvatarDownloaderHost(const QString& strPath,
                                                           QObject* parent)
    : QObject(parent)
{
    m_downloader = new CWizUserAvatarDownloader(strPath);
    connect(m_downloader, SIGNAL(downloaded(QString, bool)),
            SLOT(on_downloaded(QString, bool)));

    m_thread = new QThread(this);
    connect(m_thread, SIGNAL(started()), SLOT(on_thread_started()));

    m_downloader->moveToThread(m_thread);
}

void CWizUserAvatarDownloaderHost::download(const QString& strUserGUID)
{
    if (!m_listUser.contains(strUserGUID) && strUserGUID != m_strUserCurrent) {
        m_listUser.append(strUserGUID);
        m_thread->start();
    }
}

void CWizUserAvatarDownloaderHost::setDefault(const QString& strPath)
{
    m_strDefaultAvatarPath = strPath;
}

void CWizUserAvatarDownloaderHost::download_impl()
{
    if (m_listUser.isEmpty()) {
        qDebug() << "[Avatar Downloader]download pool is clean, thread: "
                 << QThread::currentThreadId();

        m_thread->quit();
        return;
    }

    m_strUserCurrent = m_listUser.takeFirst();

    if (!QMetaObject::invokeMethod(m_downloader, "download",
                                   Q_ARG(QString, m_strUserCurrent),
                                   Q_ARG(QString, m_strDefaultAvatarPath))) {
        qDebug() << "[Avatar Downloader]failed: unable to invoke download!";
    }
}

void CWizUserAvatarDownloaderHost::on_thread_started()
{
    download_impl();
}

void CWizUserAvatarDownloaderHost::on_downloaded(QString strUserGUID, bool bSucceed)
{
    if (bSucceed) {
        m_strUserCurrent.clear(); // Clear current otherwise download twice will be failed
        Q_EMIT downloaded(strUserGUID);
    }

    download_impl();
}


/* ----------------------- CWizUserAvatarDownloader ----------------------- */

CWizUserAvatarDownloader::CWizUserAvatarDownloader(const QString& strPath,
                                                   QObject* parent)
    : QObject(parent)
    , m_strAvatarPath(strPath)
    , m_net(new QNetworkAccessManager(this))
{
}

void CWizUserAvatarDownloader::download(const QString& strUserGUID, const QString& strDefaultAvatarPath)
{
    m_strCurrentUser = strUserGUID;
    m_strDefaultAvatarPath = strDefaultAvatarPath;

    if (m_strAvatarRequestUrl.isEmpty()) {
        acquireApiEntry();
        return;
    }

    fetchUserAvatar(m_strCurrentUser);
}

void CWizUserAvatarDownloader::acquireApiEntry()
{
    qDebug() << "[Avatar Downloader]user avatar entry is empty, acquire entry...";

    on_acquireUserAvatarEntry_finished(WizService::ApiEntry::avatarDownloadUrl());

    //CWizApiEntry* entry = new CWizApiEntry(this);
    //connect(entry, SIGNAL(entryAcquired(const QString&)),
    //        SLOT(on_acquireUserAvatarEntry_finished(const QString&)));
    //entry->getAvatarUrl();
}

void CWizUserAvatarDownloader::on_acquireUserAvatarEntry_finished(const QString& strReply)
{
    //sender()->deleteLater();

    if (strReply.isEmpty()) {
        qDebug() << "[Avatar Downloader]failed to acquire avatar entry!";
        fetchUserAvatarEnd(false);
        return;
    }

    m_strAvatarRequestUrl = strReply;

    qDebug() << "[Avatar Downloader]acquire entry finished, url: " << strReply;

    // chain back
    fetchUserAvatar(m_strCurrentUser);
}

void CWizUserAvatarDownloader::fetchUserAvatar(const QString& strUserGUID)
{
    qDebug() << "[Avatar Downloader]fetching start, guid: " << strUserGUID;

    // remote return: http://as.wiz.cn/wizas/a/users/avatar/{userGuid}
    // do substitution and add optional default arg for downloading default avatar
    QString requestUrl = m_strAvatarRequestUrl + "?default=" + (m_strDefaultAvatarPath.isEmpty() ? "true" : "false");
    requestUrl.replace(QRegExp("\\{.*\\}"), strUserGUID);

    QNetworkReply* reply = m_net->get(QNetworkRequest(requestUrl));
    connect(reply, SIGNAL(finished()), SLOT(on_queryUserAvatar_finished()));
}

void CWizUserAvatarDownloader::on_queryUserAvatar_finished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();

    if (reply->error()) {
        qDebug() << "[Avatar Downloader]Error occured: " << reply->errorString();

        if (m_strDefaultAvatarPath.isEmpty()) {
            fetchUserAvatarEnd(false);
            return;
        }

        // fallback, avoid download frequently, the default avatar
        // the server provided is not suit for our theme style
        if (saveDefaultUserAvatar()) {
            fetchUserAvatarEnd(true);
        } else {
            fetchUserAvatarEnd(false);
        }

        return;
    }

    // cause we use "default", redirection may occur
    QVariant possibleRedirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    m_urlRedirectedTo = redirectUrl(possibleRedirectUrl.toUrl(), m_urlRedirectedTo);

    if(!m_urlRedirectedTo.isEmpty()) {
        qDebug() << "[Avatar Downloader]fetching redirected, url: "
                 << m_urlRedirectedTo.toString();

        QNetworkReply* replyNext = m_net->get(QNetworkRequest(m_urlRedirectedTo));
        connect(replyNext, SIGNAL(finished()), SLOT(on_queryUserAvatar_finished()));
    } else {
        // finally arrive destination...

        // read and save avatar
        QByteArray bReply = reply->readAll();

        if (!saveUserAvatar(m_strCurrentUser, bReply)) {
            qDebug() << "[Avatar Downloader]failed: unable to save user avatar, guid: "
                     << m_strCurrentUser;

            fetchUserAvatarEnd(false);
            return;
        }

        qDebug() << "[Avatar Downloader]fetching finished, guid: " << m_strCurrentUser;
        fetchUserAvatarEnd(true);
    }
}

QUrl CWizUserAvatarDownloader::redirectUrl(const QUrl& possibleRedirectUrl,
                                         const QUrl& oldRedirectUrl) const
{
    QUrl redirectUrl;

    if(!possibleRedirectUrl.isEmpty() && possibleRedirectUrl != oldRedirectUrl)
        redirectUrl = possibleRedirectUrl;
    return redirectUrl;
}

void CWizUserAvatarDownloader::fetchUserAvatarEnd(bool bSucceed)
{
    Q_EMIT downloaded(m_strCurrentUser, bSucceed);
}

bool CWizUserAvatarDownloader::saveDefaultUserAvatar()
{
    Q_ASSERT(!m_strDefaultAvatarPath.isEmpty());

    QString strFileName = m_strAvatarPath + m_strCurrentUser + ".png";
    return QFile::copy(m_strDefaultAvatarPath, strFileName);
}

bool CWizUserAvatarDownloader::saveUserAvatar(const QString& strUserGUID,
                                            const QByteArray& bytes)
{
    QString strFileName = m_strAvatarPath + strUserGUID + ".png";
    QImage img = QImage::fromData(bytes);

    return img.save(strFileName);
}
