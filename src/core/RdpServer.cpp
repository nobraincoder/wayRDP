#include "core/RdpServer.h"
#include <algorithm>
#include <QDebug>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QUrl>
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDateTime>
#include <QTime>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>
#include <freerdp/peer.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/codec/color.h>
#include <winpr/thread.h>
#include <winpr/wtsapi.h>
#include <winpr/sysinfo.h>
#include <winpr/input.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <security/pam_appl.h>
#include <winpr/ntlm.h>
#include <QTextStream>

static QString decodeCredential(const char* data, int length, bool unicode)
{
    if (!data || length <= 0) {
        return QString();
    }

    if (unicode) {
        const auto* u16 = reinterpret_cast<const char16_t*>(data);
        const int count = std::max(0, length / 2);
        return QString::fromRawData(u16, count);
    }

    return QString::fromLatin1(QByteArray(data, length));
}

RdpServer::RdpServer(QObject *parent)
    : QObject(parent), m_listener(nullptr), m_listenerThread(nullptr), m_running(false),
      m_rdpsndContext(nullptr), m_activePeer(nullptr), m_audioTimestamp(0), m_audioReady(false)
{
    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
    m_networkAdaptTimer = new QTimer(this);
    m_networkAdaptTimer->setInterval(1500);
    connect(m_networkAdaptTimer, &QTimer::timeout, this, &RdpServer::checkNetworkAdaptation);

    connect(&m_gfxChannel, &RdpGfxChannel::frameAcknowledged, this, [this](uint32_t frameId, int64_t rttMs) {
        m_lastAckedFrameId = frameId;
        if (rttMs > 0) {
            m_lastRttMs = rttMs;
        }
    });

    connect(&m_cliprdrChannel, &RdpCliprdrChannel::clientClipboardReceived,
            this, &RdpServer::clientClipboardReceived);
    connect(&m_cliprdrChannel, &RdpCliprdrChannel::clientFilesReceived,
            this, &RdpServer::clientFilesReceived);

    m_pcmFormats[0].wFormatTag = WAVE_FORMAT_PCM;
    m_pcmFormats[0].nChannels = 2;
    m_pcmFormats[0].nSamplesPerSec = 48000;
    m_pcmFormats[0].nAvgBytesPerSec = 48000 * 2 * 2;
    m_pcmFormats[0].nBlockAlign = 4;
    m_pcmFormats[0].wBitsPerSample = 16;
    m_pcmFormats[0].cbSize = 0;
    m_pcmFormats[0].data = nullptr;

    m_pcmFormats[1].wFormatTag = WAVE_FORMAT_PCM;
    m_pcmFormats[1].nChannels = 2;
    m_pcmFormats[1].nSamplesPerSec = 44100;
    m_pcmFormats[1].nAvgBytesPerSec = 44100 * 2 * 2;
    m_pcmFormats[1].nBlockAlign = 4;
    m_pcmFormats[1].wBitsPerSample = 16;
    m_pcmFormats[1].cbSize = 0;
    m_pcmFormats[1].data = nullptr;
}

RdpServer::~RdpServer()
{
    stop();
}

bool RdpServer::authenticateUser(const QString& username, const QString& password)
{
    return AuthManager::authenticateUser(username, password);
}

bool RdpServer::start(int port)
{
    if (m_running)
        return false;

    m_peerStopRequested = false;
    m_running = true;

    AuthManager::generateCertificate();
    m_samFilePath = AuthManager::setupSamDatabase();

    m_listener = freerdp_listener_new();
    if (!m_listener) {
        qWarning() << "Failed to create FreeRDP listener";
        m_running = false;
        return false;
    }

    m_listener->info = this;
    m_listener->PeerAccepted = peerAccepted;

    qInfo() << "Opening FreeRDP listener on primary port" << port;
    if (!m_listener->Open(m_listener, nullptr, port)) {
        qWarning() << "Failed to open FreeRDP listener on primary port" << port;
        freerdp_listener_free(m_listener);
        m_listener = nullptr;
        m_running = false;
        return false;
    }

    if (port == 3389) {
        int altPort = 3390;
        qInfo() << "Attempting to also open FreeRDP listener on dual port" << altPort;
        if (!m_listener->Open(m_listener, nullptr, altPort)) {
            qWarning() << "Could not bind to dual port" << altPort << "- continuing on primary port" << port;
        } else {
            qInfo() << "Successfully bound FreeRDP listener to dual port" << altPort;
        }
    }

    m_listenerThread = CreateThread(nullptr, 0, listenerThread, this, 0, nullptr);
    return true;
}

void RdpServer::stop()
{
    if (!m_running)
        return;

    m_running = false;
    m_peerStopRequested = true;

    if (m_networkAdaptTimer) {
        m_networkAdaptTimer->stop();
    }

    m_gfxChannel.close();
    m_cliprdrChannel.close();
    m_cursorManager.reset();
    m_activePeer = nullptr;

    {
        QMutexLocker locker(&m_audioMutex);
        m_rdpsndContext = nullptr;
        m_audioReady = false;
    }

    if (m_listener) {
        m_listener->Close(m_listener);
        if (m_listenerThread) {
            WaitForSingleObject(m_listenerThread, INFINITE);
            CloseHandle(m_listenerThread);
            m_listenerThread = nullptr;
        }
        freerdp_listener_free(m_listener);
        m_listener = nullptr;
    }

    AuthManager::cleanupSamDatabase(m_samFilePath);
}

DWORD WINAPI RdpServer::listenerThread(LPVOID param)
{
    RdpServer* server = static_cast<RdpServer*>(param);
    qInfo() << "FreeRDP Listener thread started";

    while (server->m_running) {
        HANDLE events[32];
        DWORD count = server->m_listener->GetEventHandles(server->m_listener, events, 32);
        if (count > 0) {
            DWORD status = WaitForMultipleObjects(count, events, FALSE, 100);
            if (status != WAIT_TIMEOUT && status != WAIT_FAILED) {
                if (!server->m_listener->CheckFileDescriptor(server->m_listener)) {
                    qWarning() << "Failed to check file descriptor on listener";
                    break;
                }
            }
        } else {
            Sleep(50);
        }
    }

    qInfo() << "FreeRDP Listener thread stopped";
    return 0;
}

BOOL RdpServer::peerLogon(freerdp_peer* peer, const SEC_WINNT_AUTH_IDENTITY* identity, BOOL automatic)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(peer->context);
    RdpServer* server = ctx->server;

    if (freerdp_settings_get_bool(peer->context->settings, FreeRDP_NlaSecurity)) {
        qInfo() << "peerLogon: Connection authorized via NLA SAM database";
        ctx->authenticated = true;
        return TRUE;
    }

    if (!identity || (!identity->User && identity->UserLength == 0)) {
        qInfo() << "peerLogon: No identity in early handshake, deferring to PostConnect (automatic:" << automatic << ")";
        return TRUE;
    }

    bool isUnicode = (identity->Flags & SEC_WINNT_AUTH_IDENTITY_UNICODE) ||
                     !(identity->Flags & SEC_WINNT_AUTH_IDENTITY_ANSI);

    QString username;
    QString password;
    if (identity->User && identity->UserLength > 0) {
        username = decodeCredential(reinterpret_cast<const char*>(identity->User), identity->UserLength, isUnicode);
    }
    if (identity->Password && identity->PasswordLength > 0) {
        password = decodeCredential(reinterpret_cast<const char*>(identity->Password), identity->PasswordLength, isUnicode);
    }

    if (username.contains('\\')) {
        username = username.section('\\', -1);
    }
    if (username.contains('@')) {
        username = username.section('@', 0, 0);
    }

    if (username.isEmpty()) {
        username = qEnvironmentVariable("USER");
        if (username.isEmpty()) username = qEnvironmentVariable("LOGNAME");
    }

    if (password.isEmpty()) {
        qInfo() << "peerLogon: Password empty in early handshake, deferring to PostConnect";
        return TRUE;
    }

    qInfo() << "peerLogon: authenticating user:" << username
            << "(password length:" << password.length() << "automatic:" << automatic << ")";

    bool authenticated = server->authenticateUser(username, password);
    if (authenticated) {
        ctx->authenticated = true;
        return TRUE;
    }
    return FALSE;
}

BOOL RdpServer::peerPostConnect(freerdp_peer* peer)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(peer->context);
    RdpServer* server = ctx->server;
    rdpSettings* settings = peer->context->settings;

    UINT32 osMajor = freerdp_settings_get_uint32(settings, FreeRDP_OsMajorType);
    ctx->isWindowsClient = (osMajor == 1);
    qInfo() << "peerPostConnect: Client OS:" << freerdp_peer_os_major_type_string(peer)
            << "(osMajor:" << osMajor << ") isWindows:" << ctx->isWindowsClient;

    if (qEnvironmentVariable("RDP_NO_AUTH") == "1") {
        qInfo() << "peerPostConnect: Authentication bypassed due to RDP_NO_AUTH=1";
        ctx->authenticated = true;
        return TRUE;
    }

    UINT32 selectedProtocol = freerdp_settings_get_uint32(settings, FreeRDP_SelectedProtocol);
    bool isNla = (selectedProtocol == 2 || selectedProtocol == 8);
    if (ctx->authenticated || (isNla && freerdp_settings_get_bool(settings, FreeRDP_NlaSecurity))) {
        qInfo() << "peerPostConnect: Peer authorized via NLA SAM database (selectedProtocol:" << selectedProtocol << ")";
        return TRUE;
    }

    const char* u = freerdp_settings_get_string(settings, FreeRDP_Username);
    const char* p = freerdp_settings_get_string(settings, FreeRDP_Password);

    QString username = u ? QString::fromUtf8(u) : QString();
    QString password = p ? QString::fromUtf8(p) : QString();

    if (username.contains('\\')) {
        username = username.section('\\', -1);
    }
    if (username.contains('@')) {
        username = username.section('@', 0, 0);
    }

    if (username.isEmpty()) {
        username = qEnvironmentVariable("USER");
        if (username.isEmpty()) username = qEnvironmentVariable("LOGNAME");
        if (username.isEmpty()) {
            char* login = getlogin();
            if (login) username = QString::fromUtf8(login);
        }
        if (username.isEmpty()) username = QStringLiteral("user");
        qInfo() << "peerPostConnect: No username provided by client, defaulting to session user:" << username;
    }

    qInfo() << "peerPostConnect: verifying credentials for user:" << username
            << "(password length:" << password.length() << ")";

    bool authenticated = server->authenticateUser(username, password);
    if (!authenticated) {
        qWarning() << "Authentication failed in PostConnect for user:" << username;
        if (peer->context && peer->context->rdp) {
            freerdp_set_error_info(peer->context->rdp, ERRINFO_SERVER_FRESH_CREDENTIALS_REQUIRED);
        }
        return FALSE;
    }

    qInfo() << "PostConnect stage reached successfully and user authenticated";
    return TRUE;
}

void RdpServer::sendVideoFrame(const QByteArray &data, bool isKeyFrame)
{
    m_gfxChannel.sendFrame(data, isKeyFrame);
}


BOOL RdpServer::peerSynchronizeEvent(rdpInput* input, UINT32 flags)
{
    Q_UNUSED(flags);
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server) return TRUE;

    QSet<quint32> stuck;
    {
        QMutexLocker locker(&server->m_pressedKeysMutex);
        stuck = server->m_pressedKeys;
        server->m_pressedKeys.clear();
    }
    for (auto keycode : stuck) {
        emit server->keyboardKeycode(static_cast<int>(keycode), 0);
    }
    return TRUE;
}

BOOL RdpServer::peerMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server) return TRUE;

    emit server->clientActivity();

    if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) {
        int axis = flags & WheelRotationMask;
        if (axis & PTR_FLAGS_WHEEL_NEGATIVE) {
            axis = (~axis & WheelRotationMask) + 1;
        }
        int16_t rawDelta = (flags & PTR_FLAGS_WHEEL_NEGATIVE) ? -static_cast<int16_t>(axis) : static_cast<int16_t>(axis);

        if (rawDelta == 0) {
            return TRUE;
        }

        double osScale = (ctx && ctx->isWindowsClient) ? server->m_inputSettings.windowsScrollScale() : 1.0;

        if (flags & PTR_FLAGS_WHEEL) {
            double dy = server->m_inputSettings.computeVerticalDelta(rawDelta) * osScale;
            emit server->pointerAxis(0.0, dy);
        } else if (flags & PTR_FLAGS_HWHEEL) {
            double dx = server->m_inputSettings.computeHorizontalDelta(rawDelta) * osScale;
            emit server->pointerAxis(dx, 0.0);
        }

        return TRUE;
    }

    if ((flags & PTR_FLAGS_MOVE) || (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3))) {
        emit server->pointerMotionAbsolute(static_cast<double>(x), static_cast<double>(y));
    }

    if (flags & PTR_FLAGS_BUTTON1) {
        uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(server->m_inputSettings.mapPointerButton(BTN_LEFT), state);
    }
    if (flags & PTR_FLAGS_BUTTON2) {
        uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(server->m_inputSettings.mapPointerButton(BTN_RIGHT), state);
    }
    if (flags & PTR_FLAGS_BUTTON3) {
        uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(BTN_MIDDLE, state);
    }

    return TRUE;
}

// rest of file unchanged
