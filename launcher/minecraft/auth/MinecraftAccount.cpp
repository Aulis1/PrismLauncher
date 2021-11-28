/* Copyright 2013-2021 MultiMC Contributors
 *
 * Authors: Orochimarufan <orochimarufan.x3@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MinecraftAccount.h"
#include "flows/AuthContext.h"

#include <QUuid>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegExp>
#include <QStringList>
#include <QJsonDocument>

#include <QDebug>

#include <QPainter>
#include "flows/MSASilent.h"
#include "flows/MSAInteractive.h"

#include "flows/MojangRefresh.h"
#include "flows/MojangLogin.h"

MinecraftAccount::MinecraftAccount(QObject* parent) : QObject(parent) {
    m_internalId = QUuid::createUuid().toString().remove(QRegExp("[{}-]"));
}


MinecraftAccountPtr MinecraftAccount::loadFromJsonV2(const QJsonObject& json) {
    MinecraftAccountPtr account(new MinecraftAccount());
    if(account->data.resumeStateFromV2(json)) {
        return account;
    }
    return nullptr;
}

MinecraftAccountPtr MinecraftAccount::loadFromJsonV3(const QJsonObject& json) {
    MinecraftAccountPtr account(new MinecraftAccount());
    if(account->data.resumeStateFromV3(json)) {
        return account;
    }
    return nullptr;
}

MinecraftAccountPtr MinecraftAccount::createFromUsername(const QString &username)
{
    MinecraftAccountPtr account = new MinecraftAccount();
    account->data.type = AccountType::Mojang;
    account->data.yggdrasilToken.extra["userName"] = username;
    account->data.yggdrasilToken.extra["clientToken"] = QUuid::createUuid().toString().remove(QRegExp("[{}-]"));
    return account;
}

MinecraftAccountPtr MinecraftAccount::createBlankMSA()
{
    MinecraftAccountPtr account(new MinecraftAccount());
    account->data.type = AccountType::MSA;
    return account;
}


QJsonObject MinecraftAccount::saveToJson() const
{
    return data.saveState();
}

AccountStatus MinecraftAccount::accountStatus() const {
    if(data.type == AccountType::Mojang) {
        if (data.accessToken().isEmpty()) {
            return NotVerified;
        }
        else {
            return Verified;
        }
    }
    // MSA
    // FIXME: this is extremely crude and probably wrong
    if(data.msaToken.token.isEmpty()) {
        return NotVerified;
    }
    else {
        return Verified;
    }
}

bool MinecraftAccount::isExpired() const {
    switch(data.type) {
        case AccountType::Mojang: {
            return data.accessToken().isEmpty();
        }
        break;
        case AccountType::MSA: {
            return data.msaToken.validity == Katabasis::Validity::None;
        }
        break;
        default: {
            return true;
        }
    }
}


QPixmap MinecraftAccount::getFace() const {
    QPixmap skinTexture;
    if(!skinTexture.loadFromData(data.minecraftProfile.skin.data, "PNG")) {
        return QPixmap();
    }
    QPixmap skin = QPixmap(8, 8);
    QPainter painter(&skin);
    painter.drawPixmap(0, 0, skinTexture.copy(8, 8, 8, 8));
    painter.drawPixmap(0, 0, skinTexture.copy(40, 8, 8, 8));
    return skin.scaled(64, 64, Qt::KeepAspectRatio);
}


shared_qobject_ptr<AccountTask> MinecraftAccount::login(AuthSessionPtr session, QString password)
{
    Q_ASSERT(m_currentTask.get() == nullptr);

    // take care of the true offline status
    if (accountStatus() == NotVerified && password.isEmpty())
    {
        if (session)
        {
            session->status = AuthSession::RequiresPassword;
            fillSession(session);
        }
        return nullptr;
    }

    if(accountStatus() == Verified && !session->wants_online)
    {
        session->status = AuthSession::PlayableOffline;
        session->auth_server_online = false;
        fillSession(session);
        return nullptr;
    }
    else
    {
        if (password.isEmpty())
        {
            m_currentTask.reset(new MojangRefresh(&data));
        }
        else
        {
            m_currentTask.reset(new MojangLogin(&data, password));
        }
        m_currentTask->assignSession(session);

        connect(m_currentTask.get(), SIGNAL(succeeded()), SLOT(authSucceeded()));
        connect(m_currentTask.get(), SIGNAL(failed(QString)), SLOT(authFailed(QString)));
        emit activityChanged(true);
    }
    return m_currentTask;
}

shared_qobject_ptr<AccountTask> MinecraftAccount::loginMSA(AuthSessionPtr session) {
    Q_ASSERT(m_currentTask.get() == nullptr);

    if(accountStatus() == Verified && !session->wants_online)
    {
        session->status = AuthSession::PlayableOffline;
        session->auth_server_online = false;
        fillSession(session);
        return nullptr;
    }
    else
    {
        m_currentTask.reset(new MSAInteractive(&data));
        m_currentTask->assignSession(session);

        connect(m_currentTask.get(), SIGNAL(succeeded()), SLOT(authSucceeded()));
        connect(m_currentTask.get(), SIGNAL(failed(QString)), SLOT(authFailed(QString)));
        emit activityChanged(true);
    }
    return m_currentTask;
}

shared_qobject_ptr<AccountTask> MinecraftAccount::refresh(AuthSessionPtr session) {
    Q_ASSERT(m_currentTask.get() == nullptr);

    // take care of the true offline status
    if (accountStatus() == NotVerified)
    {
        if (session)
        {
            if(data.type == AccountType::MSA) {
                session->status = AuthSession::RequiresOAuth;
            }
            else {
                session->status = AuthSession::RequiresPassword;
            }
            fillSession(session);
        }
        return nullptr;
    }

    if(accountStatus() == Verified && !session->wants_online)
    {
        session->status = AuthSession::PlayableOffline;
        session->auth_server_online = false;
        fillSession(session);
        return nullptr;
    }
    else
    {
        if(data.type == AccountType::MSA) {
            m_currentTask.reset(new MSASilent(&data));
        }
        else {
            m_currentTask.reset(new MojangRefresh(&data));
        }
        m_currentTask->assignSession(session);

        connect(m_currentTask.get(), SIGNAL(succeeded()), SLOT(authSucceeded()));
        connect(m_currentTask.get(), SIGNAL(failed(QString)), SLOT(authFailed(QString)));
        emit activityChanged(true);
    }
    return m_currentTask;
}


void MinecraftAccount::authSucceeded()
{
    auto session = m_currentTask->getAssignedSession();
    if (session)
    {
        /*
            session->status = AuthSession::RequiresProfileSetup;
            session->auth_server_online = true;
        */
        if(data.profileId().size() == 0) {
            session->status = AuthSession::RequiresProfileSetup;
        }
        else {
            if(session->wants_online) {
                session->status = AuthSession::PlayableOnline;
            }
            else {
                session->status = AuthSession::PlayableOffline;
            }
        }
        fillSession(session);
        session->auth_server_online = true;
    }
    m_currentTask.reset();
    emit changed();
    emit activityChanged(false);
}

void MinecraftAccount::authFailed(QString reason)
{
    auto session = m_currentTask->getAssignedSession();
    // This is emitted when the yggdrasil tasks time out or are cancelled.
    // -> we treat the error as no-op
    switch (m_currentTask->accountState()) {
        case AccountTask::STATE_FAILED_SOFT: {
            if (session)
            {
                if(accountStatus() == Verified) {
                    session->status = AuthSession::PlayableOffline;
                }
                else {
                    if(data.type == AccountType::MSA) {
                        session->status = AuthSession::RequiresOAuth;
                    }
                    else {
                        session->status = AuthSession::RequiresPassword;
                    }
                }
                session->auth_server_online = false;
                fillSession(session);
            }
        }
        break;
        case AccountTask::STATE_FAILED_HARD: {
            // FIXME: MSA data clearing
            data.yggdrasilToken.token = QString();
            data.yggdrasilToken.validity = Katabasis::Validity::None;
            data.validity_ = Katabasis::Validity::None;
            emit changed();
            if (session)
            {
                if(data.type == AccountType::MSA) {
                    session->status = AuthSession::RequiresOAuth;
                }
                else {
                    session->status = AuthSession::RequiresPassword;
                }
                session->auth_server_online = true;
                fillSession(session);
            }
        }
        break;
        case AccountTask::STATE_FAILED_GONE: {
            data.validity_ = Katabasis::Validity::None;
            emit changed();
            if (session)
            {
                session->status = AuthSession::GoneOrMigrated;
                session->auth_server_online = true;
                fillSession(session);
            }
        }
        break;
        case AccountTask::STATE_CREATED:
        case AccountTask::STATE_WORKING:
        case AccountTask::STATE_SUCCEEDED: {
            // Not reachable here, as they are not failures.
        }
    }
    m_currentTask.reset();
    emit activityChanged(false);
}

bool MinecraftAccount::isActive() const {
    return m_currentTask;
}

bool MinecraftAccount::shouldRefresh() const {
    /*
     * Never refresh accounts that are being used by the game, it breaks the game session.
     * Always refresh accounts that have not been refreshed yet during this session.
     * Don't refresh broken accounts.
     * Refresh accounts that would expire in the next 12 hours (fresh token validity is 24 hours).
     */
    if(isInUse()) {
        return false;
    }
    switch(data.validity_) {
        case Katabasis::Validity::Certain: {
            break;
        }
        case Katabasis::Validity::None: {
            return false;
        }
        case Katabasis::Validity::Assumed: {
            return true;
        }
    }
    auto now = QDateTime::currentDateTimeUtc();
    auto issuedTimestamp = data.yggdrasilToken.issueInstant;
    auto expiresTimestamp = data.yggdrasilToken.notAfter;

    if(!expiresTimestamp.isValid()) {
        expiresTimestamp = issuedTimestamp.addSecs(24 * 3600);
    }
    if (now.secsTo(expiresTimestamp) < 12 * 3600) {
        return true;
    }
    return false;
}

void MinecraftAccount::fillSession(AuthSessionPtr session)
{
    // the user name. you have to have an user name
    // FIXME: not with MSA
    session->username = data.userName();
    // volatile auth token
    session->access_token = data.accessToken();
    // the semi-permanent client token
    session->client_token = data.clientToken();
    // profile name
    session->player_name = data.profileName();
    // profile ID
    session->uuid = data.profileId();
    // 'legacy' or 'mojang', depending on account type
    session->user_type = typeString();
    if (!session->access_token.isEmpty())
    {
        session->session = "token:" + data.accessToken() + ":" + data.profileId();
    }
    else
    {
        session->session = "-";
    }
}

void MinecraftAccount::decrementUses()
{
    Usable::decrementUses();
    if(!isInUse())
    {
        emit changed();
        // FIXME: we now need a better way to identify accounts...
        qWarning() << "Profile" << data.profileId() << "is no longer in use.";
    }
}

void MinecraftAccount::incrementUses()
{
    bool wasInUse = isInUse();
    Usable::incrementUses();
    if(!wasInUse)
    {
        emit changed();
        // FIXME: we now need a better way to identify accounts...
        qWarning() << "Profile" << data.profileId() << "is now in use.";
    }
}
