/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
 * Copyright (C) by Krzesimir Nowak <krzesimir@endocode.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QMutex>
#include <QDebug>
#include <QNetworkReply>
#include <QSettings>
#include <QInputDialog>

#include <qtkeychain/keychain.h>

#include "mirall/account.h"
#include "mirall/mirallaccessmanager.h"
#include "mirall/utility.h"
#include "mirall/theme.h"
#include "creds/credentialscommon.h"
#include "creds/httpcredentials.h"

using namespace QKeychain;

namespace Mirall
{

namespace
{

int getauth(const char *prompt,
            char *buf,
            size_t len,
            int echo,
            int verify,
            void *userdata)
{
    int re = 0;
    QMutex mutex;
    // ### safe?
    HttpCredentials* http_credentials = qobject_cast<HttpCredentials*>(AccountManager::instance()->account()->credentials());

    if (!http_credentials) {
      qDebug() << "Not a HTTP creds instance!";
      return -1;
    }

    QString qPrompt = QString::fromLatin1( prompt ).trimmed();
    QString user = http_credentials->user();
    QString pwd  = http_credentials->password();

    if( qPrompt == QLatin1String("Enter your username:") ) {
        // qDebug() << "OOO Username requested!";
        QMutexLocker locker( &mutex );
        qstrncpy( buf, user.toUtf8().constData(), len );
    } else if( qPrompt == QLatin1String("Enter your password:") ) {
        QMutexLocker locker( &mutex );
        // qDebug() << "OOO Password requested!";
        qstrncpy( buf, pwd.toUtf8().constData(), len );
    } else {
        re = handleNeonSSLProblems(prompt, buf, len, echo, verify, userdata);
    }
    return re;
}

const char userC[] = "user";

} // ns

class HttpCredentialsAccessManager : public MirallAccessManager {
public:
    HttpCredentialsAccessManager(const HttpCredentials *cred, QObject* parent = 0)
        : MirallAccessManager(parent), _cred(cred) {}
protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) {
        QByteArray credHash = QByteArray(_cred->user().toUtf8()+":"+_cred->password().toUtf8()).toBase64();
        QNetworkRequest req(request);
        req.setRawHeader(QByteArray("Authorization"), QByteArray("Basic ") + credHash);
        //qDebug() << "Request for " << req.url() << "with authorization" << QByteArray::fromBase64(credHash);
        return MirallAccessManager::createRequest(op, req, outgoingData);
    }
private:
    const HttpCredentials *_cred;
};

HttpCredentials::HttpCredentials()
    : _user(),
      _password(),
      _ready(false)
{}

HttpCredentials::HttpCredentials(const QString& user, const QString& password)
    : _user(user),
      _password(password),
      _ready(true),
      _fetchJobInProgress(false)
{
}

void HttpCredentials::syncContextPreInit (CSYNC* ctx)
{
    csync_set_auth_callback (ctx, getauth);
}

void HttpCredentials::syncContextPreStart (CSYNC* ctx)
{
    // TODO: This should not be a part of this method, but we don't have
    // any way to get "session_key" module property from csync. Had we
    // have it, then we could remove this code and keep it in
    // csyncthread code (or folder code, git remembers).
    QList<QNetworkCookie> cookies(AccountManager::instance()->account()->lastAuthCookies());
    QString cookiesAsString;

    // Stuff cookies inside csync, then we can avoid the intermediate HTTP 401 reply
    // when https://github.com/owncloud/core/pull/4042 is merged.
    foreach(QNetworkCookie c, cookies) {
        cookiesAsString += c.name();
        cookiesAsString += '=';
        cookiesAsString += c.value();
        cookiesAsString += "; ";
    }

    csync_set_module_property(ctx, "session_key", cookiesAsString.toLatin1().data());
}

bool HttpCredentials::changed(AbstractCredentials* credentials) const
{
    HttpCredentials* other(dynamic_cast< HttpCredentials* >(credentials));

    if (!other || (other->user() != this->user())) {
        return true;
    }

    return false;
}

QString HttpCredentials::authType() const
{
    return QString::fromLatin1("http");
}

QString HttpCredentials::user() const
{
    return _user;
}

QString HttpCredentials::password() const
{
    return _password;
}

QNetworkAccessManager* HttpCredentials::getQNAM() const
{
    MirallAccessManager* qnam = new HttpCredentialsAccessManager(this);

    connect( qnam, SIGNAL(authenticationRequired(QNetworkReply*, QAuthenticator*)),
             this, SLOT(slotAuthentication(QNetworkReply*,QAuthenticator*)));

    return qnam;
}

bool HttpCredentials::ready() const
{
    return _ready;
}

QString HttpCredentials::fetchUser(Account* account)
{
    _user = account->credentialSetting(QLatin1String(userC)).toString();
    return _user;
}

void HttpCredentials::fetch(Account *account)
{
    if( !account ) {
        return;
    }

    if (_fetchJobInProgress) {
        return;
    }

    fetchUser(account);

    QSettings *settings = account->settingsWithGroup(Theme::instance()->appName());
    QString kck = keychainKey(account->url().toString(), _user );

    QString key = QString::fromLatin1( "%1/data" ).arg( kck );
    if( settings && settings->contains(key) ) {
        // Clean the password from the config file if it is in there.
        // we do not want a security problem.
        settings->remove(key);
        key = QString::fromLatin1( "%1/type" ).arg( kck );
        settings->remove(kck);
        settings->sync();
    }

    if (_ready) {
        Q_EMIT fetched();
    } else {
        ReadPasswordJob *job = new ReadPasswordJob(Theme::instance()->appName());
        job->setInsecureFallback(false);
        job->setKey(keychainKey(account->url().toString(), _user));
        connect(job, SIGNAL(finished(QKeychain::Job*)), SLOT(slotReadJobDone(QKeychain::Job*)));
        job->setProperty("account", QVariant::fromValue(account));
        job->start();
        _fetchJobInProgress = true;
    }
}
bool HttpCredentials::stillValid(QNetworkReply *reply)
{
    return ((reply->error() != QNetworkReply::AuthenticationRequiredError)
            // returned if user or password is incorrect
            && (reply->error() != QNetworkReply::OperationCanceledError));
}

void HttpCredentials::slotReadJobDone(QKeychain::Job *job)
{
    ReadPasswordJob *readJob = static_cast<ReadPasswordJob*>(job);
    delete readJob->settings();
    _password = readJob->textData();
    Account *account = qvariant_cast<Account*>(readJob->property("account"));

    if( _user.isEmpty()) {
        qDebug() << "Strange: User is empty!";
    }

    QKeychain::Error error = job->error();

    if( !_password.isEmpty() && error == NoError ) {
        // All cool, the keychain did not come back with error.
        // Still, the password can be empty which indicates a problem and
        // the password dialog has to be opened.
        _ready = true;
        _fetchJobInProgress = false;
        emit fetched();
    } else {
        if( error != NoError ) {
            qDebug() << "Error while reading password" << job->errorString();
        }
        bool ok;
        QString pwd = queryPassword(&ok);
        _fetchJobInProgress = false;
        if (ok) {
            _password = pwd;
            _ready = true;
            persist(account);
        }
        emit fetched();
    }
}

QString HttpCredentials::queryPassword(bool *ok)
{
    qDebug() << AccountManager::instance()->account()->state();
    if (ok) {
        QString str = QInputDialog::getText(0, tr("Enter Password"),
                                     tr("Please enter %1 password for user '%2':")
                                     .arg(Theme::instance()->appNameGUI(), _user),
                                     QLineEdit::Password, QString(), ok);
        qDebug() << AccountManager::instance()->account()->state();
        return str;
    } else {
        return QString();
    }
}

void HttpCredentials::invalidateToken(Account *account)
{
    _password = QString();
    DeletePasswordJob *job = new DeletePasswordJob(Theme::instance()->appName());
    job->setSettings(account->settingsWithGroup(Theme::instance()->appName()));
    job->setInsecureFallback(true);
    connect(job, SIGNAL(destroyed(QObject*)), job->settings(), SLOT(deleteLater()));
    job->setKey(keychainKey(account->url().toString(), _user));
    job->start();
    _ready = false;
}

void HttpCredentials::persist(Account *account)
{
    account->setCredentialSetting(QLatin1String(userC), _user);
    WritePasswordJob *job = new WritePasswordJob(Theme::instance()->appName());
    job->setInsecureFallback(false);
    connect(job, SIGNAL(finished(QKeychain::Job*)), SLOT(slotWriteJobDone(QKeychain::Job*)));
    job->setKey(keychainKey(account->url().toString(), _user));
    job->setTextData(_password);
    job->start();
}

void HttpCredentials::slotWriteJobDone(QKeychain::Job *job)
{
    delete job->settings();
    switch (job->error()) {
    case NoError:
        break;
    default:
        qDebug() << "Error while writing password" << job->errorString();
    }
}

void HttpCredentials::slotAuthentication(QNetworkReply* reply, QAuthenticator* authenticator)
{
    Q_UNUSED(authenticator)
    // we cannot use QAuthenticator, because it sends username and passwords with latin1
    // instead of utf8 encoding. Instead, we send it manually. Thus, if we reach this signal,
    // those credentials were invalid and we terminate.
    qDebug() << "Stop request: Authentication failed for " << reply->url().toString();
    reply->close();
}

} // ns Mirall
