#include "core/AuthManager.h"
#include <QDebug>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>
#include <security/pam_appl.h>
#include <winpr/ntlm.h>
#include <unistd.h>

struct PamUserData {
    QByteArray username;
    QByteArray password;
};

static void freePamResponses(struct pam_response* resp, int count)
{
    if (!resp || count <= 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        if (resp[i].resp) {
            free(resp[i].resp);
            resp[i].resp = nullptr;
        }
    }
    free(resp);
}

static int pamConversation(int num_msg, const struct pam_message** msg,
                            struct pam_response** resp, void* appdata_ptr)
{
    if (num_msg <= 0 || !resp || !appdata_ptr) {
        return PAM_CONV_ERR;
    }
    struct pam_response* reply = static_cast<pam_response*>(calloc(num_msg, sizeof(struct pam_response)));
    if (!reply) return PAM_BUF_ERR;

    PamUserData* ud = static_cast<PamUserData*>(appdata_ptr);
    for (int i = 0; i < num_msg; ++i) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF) {
            reply[i].resp = strdup(ud->password.constData());
            reply[i].resp_retcode = 0;
        } else if (msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            reply[i].resp = strdup(ud->username.constData());
            reply[i].resp_retcode = 0;
        } else {
            reply[i].resp = nullptr;
            reply[i].resp_retcode = 0;
        }
    }
    *resp = reply;
    return PAM_SUCCESS;
}

void AuthManager::generateCertificate()
{
    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(certDir);
    QString certPath = certDir + "/server.crt";
    QString keyPath = certDir + "/server.key";

    if (QFile::exists(certPath) && QFile::exists(keyPath)) {
        return;
    }

    qInfo() << "Generating self-signed SSL/TLS certificate for FreeRDP...";
    QProcess proc;
    proc.start("openssl", QStringList() << "req" << "-x509" << "-newkey" << "rsa:2048"
                                         << "-keyout" << keyPath << "-out" << certPath
                                         << "-days" << "3650" << "-nodes"
                                         << "-subj" << "/CN=wayrdp"
                                         << "-addext" << "extendedKeyUsage=serverAuth,1.3.6.1.4.1.311.54.1.2"
                                         << "-addext" << "basicConstraints=critical,CA:TRUE"
                                         << "-addext" << "subjectAltName=DNS:localhost,IP:127.0.0.1");
    proc.waitForFinished();

    if (proc.exitCode() == 0) {
        qInfo() << "Successfully generated SSL certificates at" << certPath;
    } else {
        qWarning() << "Failed to generate certificates via OpenSSL:" << proc.readAllStandardError();
    }
}

bool AuthManager::authenticateUser(const QString& username, const QString& password)
{
    if (qEnvironmentVariable("RDP_NO_AUTH") == "1") {
        qInfo() << "Authentication bypassed due to RDP_NO_AUTH=1";
        return true;
    }

    // 1. Developer/testing override via environment variable
    QString envPassword = qEnvironmentVariable("RDP_PASSWORD");
    if (!envPassword.isEmpty() && password == envPassword) {
        qInfo() << "Authenticated using RDP_PASSWORD override for user:" << username;
        return true;
    }

    if (password.isEmpty()) {
        qWarning() << "Empty password provided, rejecting authentication for user:" << username;
        if (envPassword.isEmpty()) {
            qInfo() << "Notice: For Windows mstsc client compatibility, set RDP_PASSWORD in ~/.config/wayrdp.env to enable native Network Level Authentication (NLA).";
        }
        return false;
    }

    // 2. PAM system credentials check
    qInfo() << "Authenticating user" << username << "via PAM...";
    pam_handle_t* pamh = nullptr;
    PamUserData userdata{ username.toUtf8(), password.toUtf8() };
    struct pam_conv conv = { pamConversation, &userdata };

    // Prefer wayrdp, krdp, krdpserver, or login PAM service
    const char* pamService = "login";
    if (QFile::exists("/etc/pam.d/wayrdp")) {
        pamService = "wayrdp";
    } else if (QFile::exists("/etc/pam.d/krdp")) {
        pamService = "krdp";
    } else if (QFile::exists("/etc/pam.d/krdpserver")) {
        pamService = "krdpserver";
    }

    int retval = pam_start(pamService, userdata.username.constData(), &conv, &pamh);
    if (retval != PAM_SUCCESS) {
        qWarning() << "pam_start failed with code" << retval;
        return false;
    }

    retval = pam_authenticate(pamh, 0);
    bool success = (retval == PAM_SUCCESS);
    if (success) {
        retval = pam_acct_mgmt(pamh, 0);
        success = (retval == PAM_SUCCESS);
        if (!success) {
            qWarning() << "pam_acct_mgmt failed with code" << retval << "-" << pam_strerror(pamh, retval);
        }
    } else {
        qWarning() << "pam_authenticate failed with code" << retval << "-" << pam_strerror(pamh, retval);
    }

    pam_end(pamh, retval);
    return success;
}

QString AuthManager::setupSamDatabase()
{
    QString rdpPassword = qEnvironmentVariable("RDP_PASSWORD");
    if (rdpPassword.isEmpty() || qEnvironmentVariable("RDP_NO_AUTH") == "1") {
        return QString();
    }

    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty()) {
        runtimeDir = QString("/run/user/%1").arg(getuid());
    }
    QDir().mkpath(runtimeDir + "/wayrdp");

    QString samFilePath = runtimeDir + "/wayrdp/sam";
    QFile samFile(samFilePath);
    if (!samFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Failed to open SAM database file for writing:" << samFilePath;
        return QString();
    }

    samFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QByteArray passBytes = rdpPassword.toUtf8();
    BYTE hash[16] = {0};
    if (!NTOWFv1A(passBytes.constData(), passBytes.length(), hash)) {
        qWarning() << "NTOWFv1A failed to compute NTLM hash";
        samFile.close();
        samFile.remove();
        return QString();
    }

    QString hexHash;
    for (int i = 0; i < 16; ++i) {
        hexHash.append(QString::asprintf("%02x", hash[i]));
    }

    QStringList users;
    QString localUser = qEnvironmentVariable("USER");
    if (localUser.isEmpty()) localUser = qEnvironmentVariable("LOGNAME");
    if (localUser.isEmpty()) {
        char* l = getlogin();
        if (l) localUser = QString::fromUtf8(l);
    }
    if (!localUser.isEmpty()) {
        users << localUser;
        if (localUser != localUser.toLower()) {
            users << localUser.toLower();
        }
    }
    QString customUser = qEnvironmentVariable("RDP_USERNAME");
    if (!customUser.isEmpty() && !users.contains(customUser)) {
        users << customUser;
        if (customUser != customUser.toLower()) {
            users << customUser.toLower();
        }
    }

    QTextStream out(&samFile);
    for (const QString& user : users) {
        out << user << ":::" << hexHash << ":::\n";
    }
    samFile.close();
    qInfo() << "NLA SAM database generated at" << samFilePath << "for users:" << users;
    return samFilePath;
}

void AuthManager::cleanupSamDatabase(QString& samFilePath)
{
    if (!samFilePath.isEmpty() && QFile::exists(samFilePath)) {
        QFile::remove(samFilePath);
        samFilePath.clear();
    }
}
