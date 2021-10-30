#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDesktopServices>
#include <QMetaEnum>
#include <QDebug>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QUrlQuery>

#include <QPixmap>
#include <QPainter>

#include "AuthContext.h"
#include "katabasis/Globals.h"
#include "AuthRequest.h"

#include "Secrets.h"

#include "Env.h"

using OAuth2 = Katabasis::OAuth2;
using Activity = Katabasis::Activity;

AuthContext::AuthContext(AccountData * data, QObject *parent) :
    AccountTask(data, parent)
{
}

void AuthContext::beginActivity(Activity activity) {
    if(isBusy()) {
        throw 0;
    }
    m_activity = activity;
    changeState(STATE_WORKING, "Initializing");
    emit activityChanged(m_activity);
}

void AuthContext::finishActivity() {
    if(!isBusy()) {
        throw 0;
    }
    m_activity = Katabasis::Activity::Idle;
    setStage(AuthStage::Complete);
    m_data->validity_ = m_data->minecraftProfile.validity;
    emit activityChanged(m_activity);
}

void AuthContext::initMSA() {
    if(m_oauth2) {
        return;
    }

    auto clientId = Secrets::getMSAClientID('-');
    if(clientId.isEmpty()) {
        return;
    }

    Katabasis::OAuth2::Options opts;
    opts.scope = "XboxLive.signin offline_access";
    opts.clientIdentifier = clientId;
    opts.authorizationUrl = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
    opts.accessTokenUrl = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
    opts.listenerPorts = {28562, 28563, 28564, 28565, 28566};

    m_oauth2 = new OAuth2(opts, m_data->msaToken, this, &ENV.qnam());
    m_oauth2->setGrantFlow(Katabasis::OAuth2::GrantFlowDevice);

    connect(m_oauth2, &OAuth2::linkingFailedHard, this, &AuthContext::onOAuthLinkingFailedHard);
    connect(m_oauth2, &OAuth2::linkingFailedSoft, this, &AuthContext::onOAuthLinkingFailedSoft);
    connect(m_oauth2, &OAuth2::linkingSucceeded, this, &AuthContext::onOAuthLinkingSucceeded);
    connect(m_oauth2, &OAuth2::showVerificationUriAndCode, this, &AuthContext::showVerificationUriAndCode);
    connect(m_oauth2, &OAuth2::activityChanged, this, &AuthContext::onOAuthActivityChanged);
}

void AuthContext::initMojang() {
    if(m_yggdrasil) {
        return;
    }
    m_yggdrasil = new Yggdrasil(m_data, this);

    connect(m_yggdrasil, &Task::failed, this, &AuthContext::onMojangFailed);
    connect(m_yggdrasil, &Task::succeeded, this, &AuthContext::onMojangSucceeded);
}

void AuthContext::onMojangSucceeded() {
    doMinecraftProfile();
}


void AuthContext::onMojangFailed() {
    finishActivity();
    m_error = m_yggdrasil->m_error;
    m_aborted = m_yggdrasil->m_aborted;
    changeState(m_yggdrasil->accountState(), tr("Mojang user authentication failed."));
}

/*
bool AuthContext::signOut() {
    if(isBusy()) {
        return false;
    }

    start();

    beginActivity(Activity::LoggingOut);
    m_oauth2->unlink();
    m_account = AccountData();
    finishActivity();
    return true;
}
*/

void AuthContext::onOAuthLinkingFailedHard() {
    emit hideVerificationUriAndCode();
    finishActivity();
    changeState(STATE_FAILED_HARD, tr("Microsoft user authentication failed."));
}

void AuthContext::onOAuthLinkingFailedSoft() {
    emit hideVerificationUriAndCode();
    finishActivity();
    changeState(STATE_FAILED_SOFT, tr("Couldn't establish connection to Microsoft authentication server."));
    // TODO: here, we could still continue looking at the other tokens and see if we can talk to the services with them
}


void AuthContext::onOAuthLinkingSucceeded() {
    emit hideVerificationUriAndCode();
    auto *o2t = qobject_cast<OAuth2 *>(sender());
    if (!o2t->linked()) {
        finishActivity();
        changeState(STATE_FAILED_HARD, tr("Microsoft user authentication ended with an impossible state (succeeded, but not succeeded at the same time)."));
        return;
    }
    QVariantMap extraTokens = o2t->extraTokens();
#ifndef NDEBUG
    if (!extraTokens.isEmpty()) {
        qDebug() << "Extra tokens in response:";
        foreach (QString key, extraTokens.keys()) {
            qDebug() << "\t" << key << ":" << extraTokens.value(key);
        }
    }
#endif
    doUserAuth();
}

void AuthContext::onOAuthActivityChanged(Katabasis::Activity activity) {
    // respond to activity change here
}

void AuthContext::doUserAuth() {
    setStage(AuthStage::UserAuth);
    changeState(STATE_WORKING, tr("Starting user authentication"));

    QString xbox_auth_template = R"XXX(
{
    "Properties": {
        "AuthMethod": "RPS",
        "SiteName": "user.auth.xboxlive.com",
        "RpsTicket": "d=%1"
    },
    "RelyingParty": "http://auth.xboxlive.com",
    "TokenType": "JWT"
}
)XXX";
    auto xbox_auth_data = xbox_auth_template.arg(m_data->msaToken.token);

    QNetworkRequest request = QNetworkRequest(QUrl("https://user.auth.xboxlive.com/user/authenticate"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    auto *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onUserAuthDone);
    requestor->post(request, xbox_auth_data.toUtf8());
    qDebug() << "First layer of XBox auth ... commencing.";
}

namespace {
bool getDateTime(QJsonValue value, QDateTime & out) {
    if(!value.isString()) {
        return false;
    }
    out = QDateTime::fromString(value.toString(), Qt::ISODate);
    return out.isValid();
}

bool getString(QJsonValue value, QString & out) {
    if(!value.isString()) {
        return false;
    }
    out = value.toString();
    return true;
}

bool getNumber(QJsonValue value, double & out) {
    if(!value.isDouble()) {
        return false;
    }
    out = value.toDouble();
    return true;
}

bool getNumber(QJsonValue value, int64_t & out) {
    if(!value.isDouble()) {
        return false;
    }
    out = (int64_t) value.toDouble();
    return true;
}

bool getBool(QJsonValue value, bool & out) {
    if(!value.isBool()) {
        return false;
    }
    out = value.toBool();
    return true;
}

/*
{
   "IssueInstant":"2020-12-07T19:52:08.4463796Z",
   "NotAfter":"2020-12-21T19:52:08.4463796Z",
   "Token":"token",
   "DisplayClaims":{
      "xui":[
         {
            "uhs":"userhash"
         }
      ]
   }
 }
*/
// TODO: handle error responses ...
/*
{
    "Identity":"0",
    "XErr":2148916238,
    "Message":"",
    "Redirect":"https://start.ui.xboxlive.com/AddChildToFamily"
}
// 2148916233 = missing XBox account
// 2148916238 = child account not linked to a family
*/

bool parseXTokenResponse(QByteArray & data, Katabasis::Token &output, const char * name) {
    qDebug() << "Parsing" << name <<":";
#ifndef NDEBUG
    qDebug() << data;
#endif
    QJsonParseError jsonError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &jsonError);
    if(jsonError.error) {
        qWarning() << "Failed to parse response from user.auth.xboxlive.com as JSON: " << jsonError.errorString();
        return false;
    }

    auto obj = doc.object();
    if(!getDateTime(obj.value("IssueInstant"), output.issueInstant)) {
        qWarning() << "User IssueInstant is not a timestamp";
        return false;
    }
    if(!getDateTime(obj.value("NotAfter"), output.notAfter)) {
        qWarning() << "User NotAfter is not a timestamp";
        return false;
    }
    if(!getString(obj.value("Token"), output.token)) {
        qWarning() << "User Token is not a timestamp";
        return false;
    }
    auto arrayVal = obj.value("DisplayClaims").toObject().value("xui");
    if(!arrayVal.isArray()) {
        qWarning() << "Missing xui claims array";
        return false;
    }
    bool foundUHS = false;
    for(auto item: arrayVal.toArray()) {
        if(!item.isObject()) {
            continue;
        }
        auto obj = item.toObject();
        if(obj.contains("uhs")) {
            foundUHS = true;
        } else {
            continue;
        }
        // consume all 'display claims' ... whatever that means
        for(auto iter = obj.begin(); iter != obj.end(); iter++) {
            QString claim;
            if(!getString(obj.value(iter.key()), claim)) {
                qWarning() << "display claim " << iter.key() << " is not a string...";
                return false;
            }
            output.extra[iter.key()] = claim;
        }

        break;
    }
    if(!foundUHS) {
        qWarning() << "Missing uhs";
        return false;
    }
    output.validity = Katabasis::Validity::Certain;
    qDebug() << name << "is valid.";
    return true;
}

}

void AuthContext::onUserAuthDone(
    QNetworkReply::NetworkError error,
    QByteArray replyData,
    QList<QNetworkReply::RawHeaderPair> headers
) {
    if (error != QNetworkReply::NoError) {
        qWarning() << "Reply error:" << error;
        finishActivity();
        changeState(STATE_FAILED_HARD, tr("XBox user authentication failed."));
        return;
    }

    Katabasis::Token temp;
    if(!parseXTokenResponse(replyData, temp, "UToken")) {
        qWarning() << "Could not parse user authentication response...";
        finishActivity();
        changeState(STATE_FAILED_HARD, tr("XBox user authentication response could not be understood."));
        return;
    }
    m_data->userToken = temp;

    setStage(AuthStage::XboxAuth);
    changeState(STATE_WORKING, tr("Starting XBox authentication"));

    doSTSAuthMinecraft();
    doSTSAuthGeneric();
}
/*
        url = "https://xsts.auth.xboxlive.com/xsts/authorize"
        headers = {"x-xbl-contract-version": "1"}
        data = {
            "RelyingParty": relying_party,
            "TokenType": "JWT",
            "Properties": {
                "UserTokens": [self.user_token.token],
                "SandboxId": "RETAIL",
            },
        }
*/
void AuthContext::doSTSAuthMinecraft() {
    QString xbox_auth_template = R"XXX(
{
    "Properties": {
        "SandboxId": "RETAIL",
        "UserTokens": [
            "%1"
        ]
    },
    "RelyingParty": "rp://api.minecraftservices.com/",
    "TokenType": "JWT"
}
)XXX";
    auto xbox_auth_data = xbox_auth_template.arg(m_data->userToken.token);

    QNetworkRequest request = QNetworkRequest(QUrl("https://xsts.auth.xboxlive.com/xsts/authorize"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    AuthRequest *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onSTSAuthMinecraftDone);
    requestor->post(request, xbox_auth_data.toUtf8());
    qDebug() << "Getting Minecraft services STS token...";
}

void AuthContext::processSTSError(QNetworkReply::NetworkError error, QByteArray data, QList<QNetworkReply::RawHeaderPair> headers) {
    if(error == QNetworkReply::AuthenticationRequiredError) {
    QJsonParseError jsonError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &jsonError);
        if(jsonError.error) {
            qWarning() << "Cannot parse error XSTS response as JSON: " << jsonError.errorString();
            return;
        }

        int64_t errorCode = -1;
        auto obj = doc.object();
        if(!getNumber(obj.value("XErr"), errorCode)) {
            qWarning() << "XErr is not a number";
            return;
        }
        stsErrors.insert(errorCode);
        stsFailed = true;
    }
}


void AuthContext::onSTSAuthMinecraftDone(
    QNetworkReply::NetworkError error,
    QByteArray replyData,
    QList<QNetworkReply::RawHeaderPair> headers
) {
#ifndef NDEBUG
    qDebug() << replyData;
#endif
    if (error != QNetworkReply::NoError) {
        qWarning() << "Reply error:" << error;
        processSTSError(error, replyData, headers);
        failResult(m_mcAuthSucceeded);
        return;
    }

    Katabasis::Token temp;
    if(!parseXTokenResponse(replyData, temp, "STSAuthMinecraft")) {
        qWarning() << "Could not parse authorization response for access to mojang services...";
        failResult(m_mcAuthSucceeded);
        return;
    }

    if(temp.extra["uhs"] != m_data->userToken.extra["uhs"]) {
        qWarning() << "Server has changed user hash in the reply... something is wrong. ABORTING";
        failResult(m_mcAuthSucceeded);
        return;
    }
    m_data->mojangservicesToken = temp;

    doMinecraftAuth();
}

void AuthContext::doMinecraftAuth() {
    QString mc_auth_template = R"XXX(
{
    "identityToken": "XBL3.0 x=%1;%2"
}
)XXX";
    auto data = mc_auth_template.arg(m_data->mojangservicesToken.extra["uhs"].toString(), m_data->mojangservicesToken.token);

    QNetworkRequest request = QNetworkRequest(QUrl("https://api.minecraftservices.com/authentication/login_with_xbox"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    AuthRequest *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onMinecraftAuthDone);
    requestor->post(request, data.toUtf8());
    qDebug() << "Getting Minecraft access token...";
}

namespace {
bool parseMojangResponse(QByteArray & data, Katabasis::Token &output) {
    QJsonParseError jsonError;
    qDebug() << "Parsing Mojang response...";
#ifndef NDEBUG
    qDebug() << data;
#endif
    QJsonDocument doc = QJsonDocument::fromJson(data, &jsonError);
    if(jsonError.error) {
        qWarning() << "Failed to parse response from api.minecraftservices.com/authentication/login_with_xbox as JSON: " << jsonError.errorString();
        return false;
    }

    auto obj = doc.object();
    double expires_in = 0;
    if(!getNumber(obj.value("expires_in"), expires_in)) {
        qWarning() << "expires_in is not a valid number";
        return false;
    }
    auto currentTime = QDateTime::currentDateTimeUtc();
    output.issueInstant = currentTime;
    output.notAfter = currentTime.addSecs(expires_in);

    QString username;
    if(!getString(obj.value("username"), username)) {
        qWarning() << "username is not valid";
        return false;
    }

    // TODO: it's a JWT... validate it?
    if(!getString(obj.value("access_token"), output.token)) {
        qWarning() << "access_token is not valid";
        return false;
    }
    output.validity = Katabasis::Validity::Certain;
    qDebug() << "Mojang response is valid.";
    return true;
}
}

void AuthContext::onMinecraftAuthDone(
    QNetworkReply::NetworkError error,
    QByteArray replyData,
    QList<QNetworkReply::RawHeaderPair> headers
) {
    if (error != QNetworkReply::NoError) {
        qWarning() << "Reply error:" << error;
#ifndef NDEBUG
        qDebug() << replyData;
#endif
        failResult(m_mcAuthSucceeded);
        return;
    }

    if(!parseMojangResponse(replyData, m_data->yggdrasilToken)) {
        qWarning() << "Could not parse login_with_xbox response...";
#ifndef NDEBUG
        qDebug() << replyData;
#endif
        failResult(m_mcAuthSucceeded);
        return;
    }

    succeedResult(m_mcAuthSucceeded);
}

void AuthContext::doSTSAuthGeneric() {
    QString xbox_auth_template = R"XXX(
{
    "Properties": {
        "SandboxId": "RETAIL",
        "UserTokens": [
            "%1"
        ]
    },
    "RelyingParty": "http://xboxlive.com",
    "TokenType": "JWT"
}
)XXX";
    auto xbox_auth_data = xbox_auth_template.arg(m_data->userToken.token);

    QNetworkRequest request = QNetworkRequest(QUrl("https://xsts.auth.xboxlive.com/xsts/authorize"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    AuthRequest *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onSTSAuthGenericDone);
    requestor->post(request, xbox_auth_data.toUtf8());
    qDebug() << "Getting generic STS token...";
}

void AuthContext::onSTSAuthGenericDone(
    QNetworkReply::NetworkError error,
    QByteArray replyData,
    QList<QNetworkReply::RawHeaderPair> headers
) {
#ifndef NDEBUG
    qDebug() << replyData;
#endif
    if (error != QNetworkReply::NoError) {
        qWarning() << "Reply error:" << error;
        processSTSError(error, replyData, headers);
        failResult(m_xboxProfileSucceeded);
        return;
    }

    Katabasis::Token temp;
    if(!parseXTokenResponse(replyData, temp, "STSAuthGeneric")) {
        qWarning() << "Could not parse authorization response for access to xbox API...";
        failResult(m_xboxProfileSucceeded);
        return;
    }

    if(temp.extra["uhs"] != m_data->userToken.extra["uhs"]) {
        qWarning() << "Server has changed user hash in the reply... something is wrong. ABORTING";
        failResult(m_xboxProfileSucceeded);
        return;
    }
    m_data->xboxApiToken = temp;

    doXBoxProfile();
}

void AuthContext::doXBoxProfile() {
    auto url = QUrl("https://profile.xboxlive.com/users/me/profile/settings");
    QUrlQuery q;
    q.addQueryItem(
        "settings",
        "GameDisplayName,AppDisplayName,AppDisplayPicRaw,GameDisplayPicRaw,"
        "PublicGamerpic,ShowUserAsAvatar,Gamerscore,Gamertag,ModernGamertag,ModernGamertagSuffix,"
        "UniqueModernGamertag,AccountTier,TenureLevel,XboxOneRep,"
        "PreferredColor,Location,Bio,Watermarks,"
        "RealName,RealNameOverride,IsQuarantined"
    );
    url.setQuery(q);

    QNetworkRequest request = QNetworkRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("x-xbl-contract-version", "3");
    request.setRawHeader("Authorization", QString("XBL3.0 x=%1;%2").arg(m_data->userToken.extra["uhs"].toString(), m_data->xboxApiToken.token).toUtf8());
    AuthRequest *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onXBoxProfileDone);
    requestor->get(request);
    qDebug() << "Getting Xbox profile...";
}

void AuthContext::onXBoxProfileDone(
    QNetworkReply::NetworkError error,
    QByteArray replyData,
    QList<QNetworkReply::RawHeaderPair> headers
) {
    if (error != QNetworkReply::NoError) {
        qWarning() << "Reply error:" << error;
#ifndef NDEBUG
        qDebug() << replyData;
#endif
        failResult(m_xboxProfileSucceeded);
        return;
    }

#ifndef NDEBUG
    qDebug() << "XBox profile: " << replyData;
#endif

    succeedResult(m_xboxProfileSucceeded);
}

void AuthContext::succeedResult(bool& flag) {
    m_requestsDone ++;
    flag = true;
    checkResult();
}

void AuthContext::failResult(bool& flag) {
    m_requestsDone ++;
    flag = false;
    checkResult();
}

void AuthContext::checkResult() {
    qDebug() << "AuthContext::checkResult called";
    if(m_requestsDone != 2) {
        qDebug() << "Number of ready results:" << m_requestsDone;
        return;
    }
    if(m_mcAuthSucceeded && m_xboxProfileSucceeded) {
        doMinecraftProfile();
    }
    else {
        finishActivity();
        if(stsFailed) {
            if(stsErrors.contains(2148916233)) {
                changeState(
                    STATE_FAILED_HARD,
                    tr("This Microsoft account does not have an XBox Live profile. Buy the game on %1 first.")
                        .arg("<a href=\"https://www.minecraft.net/en-us/store/minecraft-java-edition\">minecraft.net</a>")
                );
            }
            else if (stsErrors.contains(2148916235)){
                // NOTE: this is the Grulovia error
                changeState(
                    STATE_FAILED_HARD,
                    tr("XBox Live is not available in your country. You've been blocked.")
                );
            }
            else if (stsErrors.contains(2148916238)){
                changeState(
                    STATE_FAILED_HARD,
                    tr("This Microsoft account is underaged and is not linked to a family.\n\nPlease set up your account according to %1.")
                        .arg("<a href=\"https://help.minecraft.net/hc/en-us/articles/4403181904525\">help.minecraft.net</a>")
                );
            }
            else {
                QStringList errorList;
                for(auto & error: stsErrors) {
                    errorList.append(QString::number(error));
                }
                changeState(
                    STATE_FAILED_HARD,
                    tr("XSTS authentication ended with unrecognized error(s):\n\n%1").arg(errorList.join("\n"))
                );
            }
        }
        else {
            changeState(STATE_FAILED_HARD, tr("XBox and/or Mojang authentication steps did not succeed"));
        }
    }
}

namespace {
bool parseMinecraftProfile(QByteArray & data, MinecraftProfile &output) {
    qDebug() << "Parsing Minecraft profile...";
#ifndef NDEBUG
    qDebug() << data;
#endif

    QJsonParseError jsonError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &jsonError);
    if(jsonError.error) {
        qWarning() << "Failed to parse response from user.auth.xboxlive.com as JSON: " << jsonError.errorString();
        return false;
    }

    auto obj = doc.object();
    if(!getString(obj.value("id"), output.id)) {
        qWarning() << "Minecraft profile id is not a string";
        return false;
    }

    if(!getString(obj.value("name"), output.name)) {
        qWarning() << "Minecraft profile name is not a string";
        return false;
    }

    auto skinsArray = obj.value("skins").toArray();
    for(auto skin: skinsArray) {
        auto skinObj = skin.toObject();
        Skin skinOut;
        if(!getString(skinObj.value("id"), skinOut.id)) {
            continue;
        }
        QString state;
        if(!getString(skinObj.value("state"), state)) {
            continue;
        }
        if(state != "ACTIVE") {
            continue;
        }
        if(!getString(skinObj.value("url"), skinOut.url)) {
            continue;
        }
        if(!getString(skinObj.value("variant"), skinOut.variant)) {
            continue;
        }
        // we deal with only the active skin
        output.skin = skinOut;
        break;
    }
    auto capesArray = obj.value("capes").toArray();

    QString currentCape;
    for(auto cape: capesArray) {
        auto capeObj = cape.toObject();
        Cape capeOut;
        if(!getString(capeObj.value("id"), capeOut.id)) {
            continue;
        }
        QString state;
        if(!getString(capeObj.value("state"), state)) {
            continue;
        }
        if(state == "ACTIVE") {
            currentCape = capeOut.id;
        }
        if(!getString(capeObj.value("url"), capeOut.url)) {
            continue;
        }
        if(!getString(capeObj.value("alias"), capeOut.alias)) {
            continue;
        }

        output.capes[capeOut.id] = capeOut;
    }
    output.currentCape = currentCape;
    output.validity = Katabasis::Validity::Certain;
    return true;
}
}

void AuthContext::doMinecraftProfile() {
    setStage(AuthStage::MinecraftProfile);
    changeState(STATE_WORKING, tr("Starting minecraft profile acquisition"));

    auto url = QUrl("https://api.minecraftservices.com/minecraft/profile");
    QNetworkRequest request = QNetworkRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_data->yggdrasilToken.token).toUtf8());

    AuthRequest *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onMinecraftProfileDone);
    requestor->get(request);
}

void AuthContext::onMinecraftProfileDone(
    QNetworkReply::NetworkError error,
    QByteArray data,
    QList<QNetworkReply::RawHeaderPair> headers
) {
#ifndef NDEBUG
    qDebug() << data;
#endif
    if (error == QNetworkReply::ContentNotFoundError) {
        m_data->minecraftProfile = MinecraftProfile();
        finishActivity();
        changeState(STATE_FAILED_HARD, tr("Account is missing a Minecraft Java profile.\n\nWhile the Microsoft account is valid, it does not own the game.\n\nYou might own Bedrock on this account, but that does not give you access to Java currently."));
        return;
    }
    if (error != QNetworkReply::NoError) {
        finishActivity();
        changeState(STATE_FAILED_HARD, tr("Minecraft Java profile acquisition failed."));
        return;
    }
    if(!parseMinecraftProfile(data, m_data->minecraftProfile)) {
        m_data->minecraftProfile = MinecraftProfile();
        finishActivity();
        changeState(STATE_FAILED_HARD, tr("Minecraft Java profile response could not be parsed"));
        return;
    }

    if(m_data->type == AccountType::Mojang) {
        doMigrationEligibilityCheck();
    }
    else {
        doGetSkin();
    }
}

void AuthContext::doMigrationEligibilityCheck() {
    setStage(AuthStage::MigrationEligibility);
    changeState(STATE_WORKING, tr("Starting check for migration eligibility"));

    auto url = QUrl("https://api.minecraftservices.com/rollout/v1/msamigration");
    QNetworkRequest request = QNetworkRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_data->yggdrasilToken.token).toUtf8());

    AuthRequest *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onMigrationEligibilityCheckDone);
    requestor->get(request);
}

bool parseRolloutResponse(QByteArray & data, bool& result) {
    qDebug() << "Parsing Rollout response...";
#ifndef NDEBUG
    qDebug() << data;
#endif

    QJsonParseError jsonError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &jsonError);
    if(jsonError.error) {
        qWarning() << "Failed to parse response from https://api.minecraftservices.com/rollout/v1/msamigration as JSON: " << jsonError.errorString();
        return false;
    }

    auto obj = doc.object();
    QString feature;
    if(!getString(obj.value("feature"), feature)) {
        qWarning() << "Rollout feature is not a string";
        return false;
    }
    if(feature != "msamigration") {
        qWarning() << "Rollout feature is not what we expected (msamigration), but is instead \"" << feature << "\"";
        return false;
    }
    if(!getBool(obj.value("rollout"), result)) {
        qWarning() << "Rollout feature is not a string";
        return false;
    }
    return true;
}

void AuthContext::onMigrationEligibilityCheckDone(
    QNetworkReply::NetworkError error,
    QByteArray data,
    QList<QNetworkReply::RawHeaderPair> headers
) {
    if (error == QNetworkReply::NoError) {
        parseRolloutResponse(data, m_data->canMigrateToMSA);
    }
    doGetSkin();
}

void AuthContext::doGetSkin() {
    setStage(AuthStage::Skin);
    changeState(STATE_WORKING, tr("Fetching player skin"));

    auto url = QUrl(m_data->minecraftProfile.skin.url);
    QNetworkRequest request = QNetworkRequest(url);
    AuthRequest *requestor = new AuthRequest(this);
    connect(requestor, &AuthRequest::finished, this, &AuthContext::onSkinDone);
    requestor->get(request);
}

void AuthContext::onSkinDone(
    QNetworkReply::NetworkError error,
    QByteArray data,
    QList<QNetworkReply::RawHeaderPair>
) {
    if (error == QNetworkReply::NoError) {
        m_data->minecraftProfile.skin.data = data;
    }
    m_data->validity_ = Katabasis::Validity::Certain;
    finishActivity();
    changeState(STATE_SUCCEEDED, tr("Finished all authentication steps"));
}

void AuthContext::setStage(AuthContext::AuthStage stage) {
    m_stage = stage;
    emit progress((int)m_stage, (int)AuthStage::Complete);
}


QString AuthContext::getStateMessage() const {
    switch (m_accountState)
    {
        case STATE_WORKING:
            switch(m_stage) {
                case AuthStage::Initial: {
                    QString loginMessage = tr("Logging in as %1 user");
                    if(m_data->type == AccountType::MSA) {
                        return loginMessage.arg("Microsoft");
                    }
                    else {
                        return loginMessage.arg("Mojang");
                    }
                }
                case AuthStage::UserAuth:
                    return tr("Logging in as XBox user");
                case AuthStage::XboxAuth:
                    return tr("Logging in with XBox and Mojang services");
                case AuthStage::MinecraftProfile:
                    return tr("Getting Minecraft profile");
                case AuthStage::MigrationEligibility:
                    return tr("Checking for migration eligibility");
                case AuthStage::Skin:
                    return tr("Getting Minecraft skin");
                case AuthStage::Complete:
                    return tr("Finished");
                default:
                    break;
            }
        default:
            return AccountTask::getStateMessage();
    }
}
