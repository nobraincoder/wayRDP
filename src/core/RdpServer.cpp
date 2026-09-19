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
    if (m_running.load())
        return false;

    m_peerStopRequested.store(false);
    m_running.store(true);

    AuthManager::generateCertificate();
    m_samFilePath = AuthManager::setupSamDatabase();

    m_listener = freerdp_listener_new();
    if (!m_listener) {
        qWarning() << "Failed to create FreeRDP listener";
        m_running.store(false);
        return false;
    }

    m_listener->info = this;
    m_listener->PeerAccepted = peerAccepted;

    qInfo() << "Opening FreeRDP listener on primary port" << port;
    if (!m_listener->Open(m_listener, nullptr, port)) {
        qWarning() << "Failed to open FreeRDP listener on primary port" << port;
        freerdp_listener_free(m_listener);
        m_listener = nullptr;
        m_running.store(false);
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
    if (!m_running.exchange(false))
        return;

    m_peerStopRequested.store(true);

    if (m_networkAdaptTimer) {
        m_networkAdaptTimer->stop();
    }

    m_gfxChannel.close();
    m_cliprdrChannel.close();
    m_cursorManager.reset();

    freerdp_peer* peerToClose = nullptr;
    {
        QMutexLocker locker(&m_peerMutex);
        if (m_activePeer) {
            peerToClose = m_activePeer;
            m_activePeer = nullptr;
        }
    }
    if (peerToClose) {
        peerToClose->Close(peerToClose);
    }

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

    while (server->m_running.load() && !server->m_peerStopRequested.load()) {
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

BOOL RdpServer::peerAccepted(freerdp_listener* listener, freerdp_peer* peer)
{
    qInfo() << "RDP peer accepted from client:" << peer->hostname;
    RdpServer* server = static_cast<RdpServer*>(listener->info);
    if (!server || !server->m_running.load() || server->m_peerStopRequested.load()) {
        qWarning() << "Rejecting connection: server is stopping or inactive";
        return FALSE;
    }

    {
        QMutexLocker locker(&server->m_peerMutex);
        if (server->m_activePeer != nullptr) {
            qInfo() << "Disconnecting previous/stale RDP session to accept new connection from" << peer->hostname;
            freerdp_peer* oldPeer = server->m_activePeer;
            server->m_activePeer = nullptr;
            oldPeer->Close(oldPeer);
        }
        server->m_activePeer = peer;
        server->m_scrollAccumulatorX = 0.0;
        server->m_scrollAccumulatorY = 0.0;
    }

    peer->ContextSize = sizeof(MyPeerContext);
    peer->ContextNew = peerContextNew;
    peer->ContextFree = peerContextFree;

    if (!freerdp_peer_context_new(peer)) {
        qWarning() << "Failed to create context for accepted peer";
        QMutexLocker locker(&server->m_peerMutex);
        if (server->m_activePeer == peer) {
            server->m_activePeer = nullptr;
        }
        return FALSE;
    }

    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(peer->context);
    ctx->server = server;
    ctx->peer = peer;
    ctx->activated = false;

    ctx->thread = CreateThread(nullptr, 0, peerThread, ctx, 0, nullptr);
    if (!ctx->thread) {
        qWarning() << "Failed to create thread for peer handling";
        freerdp_peer_context_free(peer);
        QMutexLocker locker(&server->m_peerMutex);
        if (server->m_activePeer == peer) {
            server->m_activePeer = nullptr;
        }
        return FALSE;
    }

    return TRUE;
}

BOOL RdpServer::peerContextNew(freerdp_peer* peer, rdpContext* context)
{
    qInfo() << "Initializing peer context settings";
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(context);
    ctx->disp = nullptr;
    ctx->dispOpened = false;
    ctx->vcm = WTSOpenServerA(reinterpret_cast<LPSTR>(peer->context));
    if (!ctx->vcm || ctx->vcm == INVALID_HANDLE_VALUE) {
        qWarning() << "Failed to open WTS server for peer";
        return FALSE;
    }

    freerdp_settings_set_uint32(peer->context->settings, FreeRDP_PointerCacheSize, 64);
    freerdp_settings_set_uint32(peer->context->settings, FreeRDP_LargePointerFlag, LARGE_POINTER_FLAG_96x96 | LARGE_POINTER_FLAG_384x384);
    return TRUE;
}

void RdpServer::peerContextFree(freerdp_peer* peer, rdpContext* context)
{
    qInfo() << "Freeing peer context";
    Q_UNUSED(peer);
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(context);
    if (ctx) {
        if (ctx->disp) {
            if (ctx->dispOpened) {
                ctx->disp->Close(ctx->disp);
                ctx->dispOpened = false;
            }
            disp_server_context_free(ctx->disp);
            ctx->disp = nullptr;
        }
        if (ctx->vcm && ctx->vcm != INVALID_HANDLE_VALUE) {
            WTSCloseServer(ctx->vcm);
            ctx->vcm = nullptr;
        }
    }
}

static BOOL my_disp_channel_id_assigned(DispServerContext* context, UINT32 channelId)
{
    qInfo() << "DisplayControl [MS-RDPEDISP]: Channel ID assigned:" << channelId << "- sending DisplayControlCaps...";
    if (context && context->DisplayControlCaps) {
        context->DisplayControlCaps(context);
    }
    return TRUE;
}

static UINT disp_monitor_layout(DispServerContext* context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU* pdu)
{
    if (!context || !pdu || pdu->NumMonitors == 0) {
        return CHANNEL_RC_BAD_CHANNEL;
    }

    RdpServer* server = static_cast<RdpServer*>(context->custom);
    if (!server) return CHANNEL_RC_OK;

    INT32 minX = pdu->Monitors[0].Left;
    INT32 minY = pdu->Monitors[0].Top;
    INT32 maxX = pdu->Monitors[0].Left + static_cast<INT32>(pdu->Monitors[0].Width);
    INT32 maxY = pdu->Monitors[0].Top + static_cast<INT32>(pdu->Monitors[0].Height);
    UINT32 desktopScale = pdu->Monitors[0].DesktopScaleFactor;
    UINT32 physicalWidth = pdu->Monitors[0].PhysicalWidth;

    if (pdu->NumMonitors > 1) {
        qInfo() << "DisplayControl [MS-RDPEDISP]: Received layout with" << pdu->NumMonitors << "monitors:";
        for (UINT32 i = 0; i < pdu->NumMonitors; ++i) {
            qInfo() << "  Monitor" << i << ":"
                    << "rect: (" << pdu->Monitors[i].Left << "," << pdu->Monitors[i].Top << ")"
                    << pdu->Monitors[i].Width << "x" << pdu->Monitors[i].Height
                    << "flags:" << pdu->Monitors[i].Flags
                    << "scale:" << pdu->Monitors[i].DesktopScaleFactor;
            if (i > 0) {
                minX = qMin(minX, pdu->Monitors[i].Left);
                minY = qMin(minY, pdu->Monitors[i].Top);
                maxX = qMax(maxX, pdu->Monitors[i].Left + static_cast<INT32>(pdu->Monitors[i].Width));
                maxY = qMax(maxY, pdu->Monitors[i].Top + static_cast<INT32>(pdu->Monitors[i].Height));
                if (pdu->Monitors[i].Flags & DISPLAY_CONTROL_MONITOR_PRIMARY) {
                    desktopScale = pdu->Monitors[i].DesktopScaleFactor;
                    physicalWidth = pdu->Monitors[i].PhysicalWidth;
                }
            }
        }
    }

    QSize monitorSize(maxX - minX, maxY - minY);

    double scale = server->m_clientScale.load();
    bool envScaleOk = false;
    double envScale = qEnvironmentVariable("RDP_SCALE").toDouble(&envScaleOk);
    if (envScaleOk && envScale >= 0.5 && envScale <= 5.0) {
        scale = envScale;
    } else if (desktopScale > 100 && desktopScale <= 500) {
        scale = static_cast<double>(desktopScale) / 100.0;
    } else if (physicalWidth > 0 && monitorSize.width() > 0) {
        double dpi = (static_cast<double>(monitorSize.width()) / (static_cast<double>(physicalWidth) / 25.4));
        if (dpi > 120.0) {
            scale = std::round((dpi / 96.0) * 4.0) / 4.0;
        } else {
            scale = 1.0;
        }
    }

    qInfo() << "DisplayControl [MS-RDPEDISP]: Client requested dynamic resolution change to:" << monitorSize
            << (pdu->NumMonitors > 1 ? "(unified multi-monitor canvas)" : "")
            << "desktop scale:" << desktopScale << "effective scale:" << scale;

    server->m_clientScale = scale;

    uint32_t maxFps = 60;
    if (qEnvironmentVariableIsSet("RDP_FPS")) {
        bool ok = false;
        int envFps = qEnvironmentVariableIntValue("RDP_FPS", &ok);
        if (ok && envFps >= 10 && envFps <= 120) {
            maxFps = envFps;
        }
    }
    server->m_maxTargetFps = maxFps;
    if (server->m_currentFps.load() > maxFps) {
        server->m_currentFps = maxFps;
        emit server->clientEncodingConfigured(maxFps, server->m_currentQuality.load());
    }

    emit server->requestedResolutionChanged(monitorSize, scale);

    return CHANNEL_RC_OK;
}

static void setup_and_open_disp(MyPeerContext* ctx, RdpServer* server)
{
    if (!ctx || !ctx->vcm) return;

    if (!ctx->disp) {
        ctx->disp = disp_server_context_new(ctx->vcm);
        if (ctx->disp) {
            ctx->disp->rdpcontext = &ctx->common;
            ctx->disp->custom = server;
            ctx->disp->DispMonitorLayout = disp_monitor_layout;
            ctx->disp->ChannelIdAssigned = my_disp_channel_id_assigned;
            ctx->disp->MaxNumMonitors = 16;
            ctx->disp->MaxMonitorAreaFactorA = 16384;
            ctx->disp->MaxMonitorAreaFactorB = 16384;
            qInfo() << "DisplayControl [MS-RDPEDISP]: Context created and configured for up to 16 monitors";
        }
    }

    if (ctx->disp && !ctx->dispOpened) {
        if (ctx->disp->Open(ctx->disp) == CHANNEL_RC_OK) {
            ctx->dispOpened = true;
            qInfo() << "DisplayControl [MS-RDPEDISP] dynamic virtual channel opened successfully!";
            if (ctx->disp->DisplayControlCaps) {
                ctx->disp->DisplayControlCaps(ctx->disp);
            }
        } else {
            qWarning() << "Failed to open DisplayControl dynamic virtual channel";
        }
    }
}

DWORD WINAPI RdpServer::peerThread(LPVOID param)
{
    MyPeerContext* ctx = static_cast<MyPeerContext*>(param);
    freerdp_peer* peer = ctx->peer;
    RdpServer* server = ctx->server;

    qInfo() << "Peer thread running for connection:" << peer->hostname;

    rdpSettings* settings = peer->context->settings;

    freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE);

    bool hasRdpPassword = !qEnvironmentVariable("RDP_PASSWORD").isEmpty();
    bool noAuth = AuthManager::isNoAuthEnabled();

    if (hasRdpPassword && !noAuth) {
        if (server->m_samFilePath.isEmpty() || !QFile::exists(server->m_samFilePath)) {
            server->m_samFilePath = AuthManager::setupSamDatabase();
        }
        if (!server->m_samFilePath.isEmpty()) {
            freerdp_settings_set_string(settings, FreeRDP_NtlmSamFile, server->m_samFilePath.toUtf8().constData());
            freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, TRUE);
            qInfo() << "Configured NLA security with SAM database for connection:" << peer->hostname;
        } else {
            freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
        }
    } else {
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
        if (noAuth) {
            qInfo() << "Operating in NO_AUTH mode (RDP_NO_AUTH set) for connection:" << peer->hostname;
            freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE);
        } else if (!hasRdpPassword) {
            qInfo() << "Operating in TLS / PAM mode (RDP_PASSWORD not set) for connection:" << peer->hostname;
        }
    }

    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicTimeZone, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE);

    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString certPath = certDir + "/server.crt";
    QString keyPath = certDir + "/server.key";
    if (!QFile::exists(certPath) || !QFile::exists(keyPath)) {
        AuthManager::generateCertificate();
    }
    if (!QFile::exists(certPath) || !QFile::exists(keyPath)) {
        if (QFile::exists("server.crt") && QFile::exists("server.key")) {
            certPath = "server.crt";
            keyPath = "server.key";
        } else if (QFile::exists("build/server.crt") && QFile::exists("build/server.key")) {
            certPath = "build/server.crt";
            keyPath = "build/server.key";
        }
    }
    rdpCertificate* cert = freerdp_certificate_new_from_file(certPath.toUtf8().constData());
    rdpPrivateKey* key = freerdp_key_new_from_file(keyPath.toUtf8().constData());
    if (cert && key) {
        if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
            qWarning() << "Failed to set RdpServerCertificate in settings";
            freerdp_certificate_free(cert);
        }
        if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1)) {
            qWarning() << "Failed to set RdpServerRsaKey in settings";
            freerdp_key_free(key);
        }
    } else {
        qWarning() << "Failed to load TLS cert/key from" << certPath << "/" << keyPath;
        if (cert) freerdp_certificate_free(cert);
        if (key) freerdp_key_free(key);
    }

    peer->Logon = peerLogon;
    peer->Activate = peerActivate;
    peer->Capabilities = peerCapabilities;
    peer->PostConnect = peerPostConnect;
    if (peer->context && peer->context->update) {
        peer->context->update->SuppressOutput = peerSuppressOutput;
    }
    if (peer->context && peer->context->input) {
        peer->context->input->param1 = server;
        peer->context->input->SynchronizeEvent = peerSynchronizeEvent;
        peer->context->input->MouseEvent = peerMouseEvent;
        peer->context->input->RelMouseEvent = peerRelMouseEvent;
        peer->context->input->ExtendedMouseEvent = peerExtendedMouseEvent;
        peer->context->input->KeyboardEvent = peerKeyboardEvent;
        peer->context->input->UnicodeKeyboardEvent = peerUnicodeKeyboardEvent;
    }

    if (!peer->Initialize(peer)) {
        qWarning() << "Failed to initialize peer";
        return 0;
    }

    qInfo() << "Entering peer negotiation loop...";
    while (server->m_running.load() && !server->m_peerStopRequested.load() && !ctx->activated) {
        HANDLE events[64];
        DWORD count = peer->GetEventHandles(peer, events, 64);
        if (count == 0) {
            qWarning() << "Failed to get event handles for peer during negotiation";
            break;
        }

        DWORD status = WaitForMultipleObjects(count, events, FALSE, 100);
        if (status == WAIT_FAILED) {
            qWarning() << "WaitForMultipleObjects failed during negotiation";
            break;
        }

        if (status != WAIT_TIMEOUT) {
            if (!peer->CheckFileDescriptor(peer)) {
                qWarning() << "Peer disconnected during handshake";
                break;
            }
        }
    }

    if (ctx->activated && server->m_running.load() && !server->m_peerStopRequested.load()) {
        qInfo() << "RDP peer connection is fully active!";

        while (server->m_running.load() && !server->m_peerStopRequested.load() && ctx->activated) {
            HANDLE events[64];
            DWORD count = peer->GetEventHandles(peer, events, 32);
            HANDLE vcmEvent = ctx->vcm ? WTSVirtualChannelManagerGetEventHandle(ctx->vcm) : nullptr;
            if (vcmEvent) {
                events[count++] = vcmEvent;
            }

            DWORD status = WaitForMultipleObjects(count, events, FALSE, 100);
            if (status == WAIT_FAILED) {
                qWarning() << "WaitForMultipleObjects failed inside active peer loop";
                break;
            }

            if (status != WAIT_TIMEOUT) {
                if (!peer->CheckFileDescriptor(peer)) {
                    qWarning() << "Active peer disconnected";
                    break;
                }
                if (vcmEvent && !WTSVirtualChannelManagerCheckFileDescriptor(ctx->vcm)) {
                    qWarning() << "VCM event check failed or peer disconnected";
                    break;
                }
            }

            if (ctx->vcm && (!server->m_gfxChannel.isOpened() || !ctx->dispOpened)) {
                if (WTSVirtualChannelManagerIsChannelJoined(ctx->vcm, "drdynvc")) {
                    UINT32 dvcState = WTSVirtualChannelManagerGetDrdynvcState(ctx->vcm);
                    if (dvcState == DRDYNVC_STATE_READY) {
                        server->m_gfxChannel.markOpened();
                        setup_and_open_disp(ctx, server);
                    }
                }
            }
        }
    }

    qInfo() << "Peer thread clean up for connection:" << peer->hostname;

    if (ctx->disp) {
        if (ctx->dispOpened) {
            ctx->disp->Close(ctx->disp);
            ctx->dispOpened = false;
        }
        disp_server_context_free(ctx->disp);
        ctx->disp = nullptr;
    }

    bool wasActive = false;
    {
        QMutexLocker locker(&server->m_peerMutex);
        if (server->m_activePeer == peer) {
            server->m_activePeer = nullptr;
            wasActive = true;
        }
    }

    if (wasActive) {
        server->m_gfxChannel.close();
        server->m_cliprdrChannel.close();
        server->m_cursorManager.reset();
        if (server->m_networkAdaptTimer) {
            QMetaObject::invokeMethod(server->m_networkAdaptTimer, "stop", Qt::QueuedConnection);
        }
        server->m_smoothedRttMs = 0;
        server->m_lastRttMs = 0;
        server->m_currentFps = 60;
        server->m_currentQuality = 95;
        {
            QMutexLocker locker(&server->m_audioMutex);
            server->m_rdpsndContext = nullptr;
            server->m_audioReady = false;
        }
    }

    if (ctx->activated) {
        ctx->activated = false;
        QMetaObject::invokeMethod(server, "clientDisconnected", Qt::QueuedConnection);
    }

    peer->Disconnect(peer);

    return 0;
}

BOOL RdpServer::peerLogon(freerdp_peer* peer, const SEC_WINNT_AUTH_IDENTITY* identity, BOOL automatic)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(peer->context);
    RdpServer* server = ctx->server;

    if (AuthManager::isNoAuthEnabled()) {
        qInfo() << "peerLogon: Authentication bypassed due to RDP_NO_AUTH";
        ctx->authenticated = true;
        return TRUE;
    }

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

BOOL RdpServer::peerActivate(freerdp_peer* peer)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(peer->context);
    RdpServer* server = ctx->server;

    if (ctx->activated) {
        qInfo() << "peerActivate: Peer already activated, skipping re-initialization.";
        return TRUE;
    }

    qInfo() << "Client activation capability exchange completed. Initializing Graphics Pipeline (RDPGFX)...";

    rdpSettings* settings = peer->context->settings;
    UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    UINT32 scale = freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
    if (scale == 0) scale = 100;

    const rdpMonitor* monitors = static_cast<const rdpMonitor*>(freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, 0));
    UINT32 monitorCount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
    if (monitorCount > 0 && monitors && freerdp_settings_get_bool(settings, FreeRDP_HasMonitorAttributes)) {
        if (monitors[0].attributes.desktopScaleFactor >= 100) {
            scale = monitors[0].attributes.desktopScaleFactor;
        }
    }

    if (monitorCount > 1 && monitors) {
        qInfo() << "Multi-monitor connection detected at activation with" << monitorCount << "monitors:";
        INT32 minX = monitors[0].x;
        INT32 minY = monitors[0].y;
        INT32 maxX = monitors[0].x + monitors[0].width;
        INT32 maxY = monitors[0].y + monitors[0].height;
        for (UINT32 i = 0; i < monitorCount; ++i) {
            qInfo() << "  Monitor" << i << ":"
                    << "(" << monitors[i].x << "," << monitors[i].y << ")"
                    << monitors[i].width << "x" << monitors[i].height
                    << (monitors[i].is_primary ? "[Primary]" : "")
                    << "scale:" << monitors[i].attributes.desktopScaleFactor;
            if (i > 0) {
                minX = qMin(minX, static_cast<INT32>(monitors[i].x));
                minY = qMin(minY, static_cast<INT32>(monitors[i].y));
                maxX = qMax(maxX, static_cast<INT32>(monitors[i].x + monitors[i].width));
                maxY = qMax(maxY, static_cast<INT32>(monitors[i].y + monitors[i].height));
                if (monitors[i].is_primary && monitors[i].attributes.desktopScaleFactor >= 100) {
                    scale = monitors[i].attributes.desktopScaleFactor;
                }
            }
        }
        width = maxX - minX;
        height = maxY - minY;
        qInfo() << "Unified multi-monitor bounding desktop:" << width << "x" << height;
    }

    double effectiveScale = static_cast<double>(scale) / 100.0;
    bool envScaleOk = false;
    double envScale = qEnvironmentVariable("RDP_SCALE").toDouble(&envScaleOk);
    if (envScaleOk && envScale >= 0.5 && envScale <= 5.0) {
        effectiveScale = envScale;
        qInfo() << "Using user configured RDP_SCALE override:" << effectiveScale;
    } else if (effectiveScale <= 1.0) {
        UINT32 physWidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalWidth);
        if (physWidth > 0 && width > 0) {
            double dpi = (static_cast<double>(width) / (static_cast<double>(physWidth) / 25.4));
            if (dpi > 120.0) {
                effectiveScale = std::round((dpi / 96.0) * 4.0) / 4.0;
                qInfo() << "Auto-detected HiDPI display from client physical dimensions. Calculated DPI:" << dpi << "-> setting scale:" << effectiveScale;
            }
        }
    }

    if (!ctx->vcm || !WTSVirtualChannelManagerOpen(ctx->vcm)) {
        qWarning() << "Failed to open virtual channel manager";
        return FALSE;
    }

    if (peer->context && peer->context->input) {
        peer->context->input->param1 = server;
        peer->context->input->SynchronizeEvent = peerSynchronizeEvent;
        peer->context->input->MouseEvent = peerMouseEvent;
        peer->context->input->RelMouseEvent = peerRelMouseEvent;
        peer->context->input->ExtendedMouseEvent = peerExtendedMouseEvent;
        peer->context->input->KeyboardEvent = peerKeyboardEvent;
        peer->context->input->UnicodeKeyboardEvent = peerUnicodeKeyboardEvent;
    }

    if (peer->context && peer->context->update) {
        peer->context->update->SuppressOutput = peerSuppressOutput;
    }

    server->m_cliprdrChannel.initialize(ctx->vcm, reinterpret_cast<rdpContext*>(ctx));

    bool audioEnabled = true;
    if (qEnvironmentVariableIsSet("RDP_AUDIO")) {
        QString a = qEnvironmentVariable("RDP_AUDIO").trimmed().toLower();
        if (a == "0" || a == "false" || a == "off") {
            audioEnabled = false;
        }
    }

    if (audioEnabled) {
        server->m_clientConfirmsBlocks = false;
        server->m_lastConfirmedBlock = 0;
        server->m_lastLeftSample = 0;
        server->m_lastRightSample = 0;
        server->m_audioDroppedPrevious = false;
        RdpsndServerContext* rdpsnd = rdpsnd_server_context_new(ctx->vcm);
        if (rdpsnd) {
            rdpsnd->data = server;
            rdpsnd->server_formats = server->m_pcmFormats;
            rdpsnd->num_server_formats = 2;
            rdpsnd->src_format = &server->m_pcmFormats[0];
            rdpsnd->latency = 20;
            rdpsnd->Activated = rdpsnd_activated;
            rdpsnd->ConfirmBlock = rdpsnd_confirm_block;

            if (rdpsnd->Initialize(rdpsnd, TRUE) == CHANNEL_RC_OK) {
                QMutexLocker locker(&server->m_audioMutex);
                server->m_rdpsndContext = rdpsnd;
                qInfo() << "Audio output (rdpsnd) channel initialized with 20ms buffer";
            } else {
                qWarning() << "Failed to initialize rdpsnd channel";
                rdpsnd_server_context_free(rdpsnd);
            }
        }
    }

    if (server->m_networkAdaptTimer) {
        QMetaObject::invokeMethod(server->m_networkAdaptTimer, "start", Qt::QueuedConnection);
    }

    if (!server->m_gfxChannel.initialize(ctx->vcm, reinterpret_cast<rdpContext*>(ctx))) {
        qWarning() << "Failed to initialize RDPGFX channel";
        return FALSE;
    }

    if (ctx->vcm && WTSVirtualChannelManagerIsChannelJoined(ctx->vcm, "drdynvc")) {
        if (WTSVirtualChannelManagerGetDrdynvcState(ctx->vcm) == DRDYNVC_STATE_READY) {
            server->m_gfxChannel.markOpened();
            setup_and_open_disp(ctx, server);
        }
    }

    ctx->activated = true;

    UINT32 connectionType = freerdp_settings_get_uint32(settings, FreeRDP_ConnectionType);
    uint32_t targetFps = 60;
    int targetQuality = 80;
    switch (connectionType) {
        case CONNECTION_TYPE_MODEM:
            targetFps = 15;
            targetQuality = 50;
            break;
        case CONNECTION_TYPE_BROADBAND_LOW:
            targetFps = 24;
            targetQuality = 65;
            break;
        case CONNECTION_TYPE_SATELLITE:
            targetFps = 20;
            targetQuality = 60;
            break;
        case CONNECTION_TYPE_BROADBAND_HIGH:
        case CONNECTION_TYPE_WAN:
            targetFps = 30;
            targetQuality = 75;
            break;
        case CONNECTION_TYPE_LAN:
            targetFps = 60;
            targetQuality = 85;
            break;
        default:
            targetFps = 60;
            targetQuality = 80;
            break;
    }

    uint32_t maxFps = 60;
    if (qEnvironmentVariableIsSet("RDP_FPS")) {
        bool ok = false;
        int envFps = qEnvironmentVariableIntValue("RDP_FPS", &ok);
        if (ok && envFps >= 10 && envFps <= 120) {
            maxFps = envFps;
            qInfo() << "Using user configured RDP_FPS:" << maxFps;
        }
    }

    server->m_maxTargetFps = maxFps;
    targetFps = std::min(targetFps, maxFps);
    server->m_currentFps = targetFps;

    if (qEnvironmentVariableIsSet("RDP_QUALITY")) {
        bool ok = false;
        int envQuality = qEnvironmentVariableIntValue("RDP_QUALITY", &ok);
        if (ok && envQuality >= 10 && envQuality <= 100) {
            targetQuality = envQuality;
            qInfo() << "Using user configured RDP_QUALITY:" << targetQuality;
        }
    } else {
        targetQuality = 95;
    }
    server->m_currentQuality = targetQuality;

    emit server->clientEncodingConfigured(targetFps, targetQuality);

    qInfo() << "Negotiated Dynamic resolution:" << width << "x" << height << "@ scale" << effectiveScale;

    server->m_clientScale = effectiveScale;
    server->m_inputSettings.reload();

    emit server->clientConnected(QSize(width, height), effectiveScale);

    return TRUE;
}

BOOL RdpServer::peerCapabilities(freerdp_peer* peer)
{
    Q_UNUSED(peer);
    qInfo() << "Capabilities exchange triggered";
    return TRUE;
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

    if (AuthManager::isNoAuthEnabled()) {
        qInfo() << "peerPostConnect: Authentication bypassed due to RDP_NO_AUTH";
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
    if (!server || !server->m_running.load() || server->m_peerStopRequested.load()) return TRUE;

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
    if (!server || !server->m_running.load() || server->m_peerStopRequested.load()) return TRUE;

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

BOOL RdpServer::peerRelMouseEvent(rdpInput* input, UINT16 flags, INT16 xDelta, INT16 yDelta)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server || !server->m_running.load() || server->m_peerStopRequested.load()) return TRUE;

    emit server->clientActivity();

    if (xDelta != 0 || yDelta != 0) {
        emit server->pointerMotion(static_cast<double>(xDelta), static_cast<double>(yDelta));
    }

    if (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) {
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
    }
    return TRUE;
}

BOOL RdpServer::peerExtendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server || !server->m_running.load() || server->m_peerStopRequested.load()) return TRUE;

    emit server->clientActivity();

    if ((flags & PTR_FLAGS_MOVE) || (flags & (PTR_XFLAGS_BUTTON1 | PTR_XFLAGS_BUTTON2))) {
        emit server->pointerMotionAbsolute(static_cast<double>(x), static_cast<double>(y));
    }

    if (flags & PTR_XFLAGS_BUTTON1) {
        uint state = (flags & PTR_XFLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(BTN_SIDE, state);
    }
    if (flags & PTR_XFLAGS_BUTTON2) {
        uint state = (flags & PTR_XFLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(BTN_EXTRA, state);
    }
    return TRUE;
}

BOOL RdpServer::peerKeyboardEvent(rdpInput* input, UINT16 flags, UINT8 code)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server || !server->m_running.load() || server->m_peerStopRequested.load()) return TRUE;

    emit server->clientActivity();

    auto virtualCode = GetVirtualKeyCodeFromVirtualScanCode(flags & KBD_FLAGS_EXTENDED ? code | KBDEXT : code, 4);
    virtualCode = flags & KBD_FLAGS_EXTENDED ? virtualCode | KBDEXT : virtualCode;

    quint32 keycode = GetKeycodeFromVirtualKeyCode(virtualCode, WINPR_KEYCODE_TYPE_EVDEV);

    uint state = (flags & KBD_FLAGS_RELEASE) ? 0 : 1;
    {
        QMutexLocker locker(&server->m_pressedKeysMutex);
        if (state == 0) {
            server->m_pressedKeys.remove(keycode);
        } else {
            server->m_pressedKeys.insert(keycode);
        }
    }

    if (keycode > 0) {
        emit server->keyboardKeycode(static_cast<int>(keycode), state);
    }
    return TRUE;
}

BOOL RdpServer::peerUnicodeKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server || !server->m_running.load() || server->m_peerStopRequested.load()) return TRUE;

    emit server->clientActivity();

    auto text = QString(QChar::fromUcs2(code));
    if (text.isEmpty()) {
        return TRUE;
    }

    auto keysym = xkb_utf32_to_keysym(text.toUcs4().first());
    if (!keysym) {
        return TRUE;
    }

    uint state = (flags & KBD_FLAGS_RELEASE) ? 0 : 1;
    emit server->keyboardKeysym(static_cast<int>(keysym), state);
    return TRUE;
}

BOOL RdpServer::peerSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* rect)
{
    Q_UNUSED(rect);
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(context);
    if (!ctx || !ctx->server) {
        return TRUE;
    }
    bool suppressed = (allow == 0);
    qInfo() << "SuppressOutput PDU received: allow =" << (int)allow << "(suppressed =" << suppressed << ")";
    ctx->server->m_gfxChannel.setOutputSuppressed(suppressed);
    return TRUE;
}

void RdpServer::onHostClipboardChanged(const QString &text)
{
    m_cliprdrChannel.onHostClipboardChanged(text);
}

void RdpServer::onHostClipboardFilesChanged(const QStringList &filePaths)
{
    m_cliprdrChannel.onHostClipboardFilesChanged(filePaths);
}

void RdpServer::onKlipperClipboardHistoryUpdated()
{
    QDBusInterface klipper("org.kde.klipper", "/klipper", "org.kde.klipper.klipper", QDBusConnection::sessionBus());
    if (klipper.isValid()) {
        QDBusReply<QString> reply = klipper.call("getClipboardContents");
        if (reply.isValid()) {
            QString text = reply.value().trimmed();
            if (text.isEmpty()) return;

            QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            QStringList files;
            bool allFiles = true;

            for (QString line : lines) {
                line = line.trimmed();
                if (line.startsWith(QLatin1String("file://"))) {
                    QString path = QUrl(line).toLocalFile();
                    if (!path.isEmpty() && QFileInfo::exists(path)) {
                        files.append(path);
                        continue;
                    }
                } else if (line.startsWith(QLatin1Char('/')) && QFileInfo::exists(line)) {
                    files.append(line);
                    continue;
                }
                allFiles = false;
                break;
            }

            if (allFiles && !files.isEmpty()) {
                qInfo() << "Klipper contains files:" << files;
                onHostClipboardFilesChanged(files);
            } else {
                onHostClipboardChanged(text);
            }
        }
    }
}

void RdpServer::checkNetworkAdaptation()
{
    int64_t rtt = m_lastRttMs.load();
    if (rtt <= 0) {
        return;
    }

    int64_t smoothed = m_smoothedRttMs.load();
    if (smoothed <= 0) {
        smoothed = rtt;
    } else {
        smoothed = (smoothed * 7 + rtt * 3) / 10;
    }
    m_smoothedRttMs = smoothed;

    uint32_t targetFps = 60;
    int targetQuality = 90;

    if (smoothed < 35) {
        targetFps = 60;
        targetQuality = 95;
    } else if (smoothed < 75) {
        targetFps = 60;
        targetQuality = 85;
    } else if (smoothed < 130) {
        targetFps = 45;
        targetQuality = 75;
    } else if (smoothed < 220) {
        targetFps = 30;
        targetQuality = 65;
    } else {
        targetFps = 20;
        targetQuality = 50;
    }

    targetFps = std::min(targetFps, m_maxTargetFps.load());
    if (qEnvironmentVariableIsSet("RDP_QUALITY")) {
        bool ok = false;
        int envQuality = qEnvironmentVariableIntValue("RDP_QUALITY", &ok);
        if (ok && envQuality >= 10 && envQuality <= 100) {
            targetQuality = envQuality;
        }
    }

    if (targetFps != m_currentFps.load() || targetQuality != m_currentQuality.load()) {
        m_currentFps = targetFps;
        m_currentQuality = targetQuality;
        qInfo() << "Network Adaptation: RTT" << rtt << "ms (smoothed" << smoothed << "ms) -> Adjusting encoding: FPS="
                << targetFps << "Quality=" << targetQuality;
        emit clientEncodingConfigured(targetFps, targetQuality);
    }
}

void RdpServer::rdpsnd_activated(RdpsndServerContext* context)
{
    if (!context) return;
    qInfo() << "RDPSND channel activated by client (version:" << context->clientVersion
            << "caps: 0x" + QString::number(context->capsFlags, 16) << ")";

    RdpServer* server = static_cast<RdpServer*>(context->data);
    if (!server) return;

    qInfo() << "Client advertised" << context->num_client_formats << "audio formats:";
    for (size_t i = 0; i < context->num_client_formats; i++) {
        const AUDIO_FORMAT* fmt = &context->client_formats[i];
        qInfo() << "  Format [" << i << "]: tag=" << fmt->wFormatTag
                << "channels=" << fmt->nChannels
                << "rate=" << fmt->nSamplesPerSec
                << "bits=" << fmt->wBitsPerSample;
    }

    int selectedIndex = -1;
    uint32_t selectedRate = 48000;

    for (size_t i = 0; i < context->num_client_formats; i++) {
        const AUDIO_FORMAT* fmt = &context->client_formats[i];
        if (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->nChannels == 2 &&
            fmt->wBitsPerSample == 16 && fmt->nSamplesPerSec == 48000) {
            selectedIndex = static_cast<int>(i);
            selectedRate = 48000;
            break;
        }
    }

    if (selectedIndex < 0) {
        for (size_t i = 0; i < context->num_client_formats; i++) {
            const AUDIO_FORMAT* fmt = &context->client_formats[i];
            if (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->nChannels == 2 &&
                fmt->wBitsPerSample == 16 && fmt->nSamplesPerSec == 44100) {
                selectedIndex = static_cast<int>(i);
                selectedRate = 44100;
                break;
            }
        }
    }

    if (selectedIndex < 0) {
        for (size_t i = 0; i < context->num_client_formats; i++) {
            const AUDIO_FORMAT* fmt = &context->client_formats[i];
            if (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->nChannels == 2 &&
                fmt->wBitsPerSample == 16) {
                selectedIndex = static_cast<int>(i);
                selectedRate = fmt->nSamplesPerSec;
                break;
            }
        }
    }

    if (selectedIndex < 0 && context->num_client_formats > 0) {
        selectedIndex = 0;
        selectedRate = context->client_formats[0].nSamplesPerSec > 0 ? context->client_formats[0].nSamplesPerSec : 44100;
    }

    if (selectedIndex >= 0) {
        qInfo() << "Selecting audio format index:" << selectedIndex << "with sample rate:" << selectedRate;

        context->latency = 20;
        if (selectedRate == 44100) {
            context->src_format = &server->m_pcmFormats[1];
        } else {
            context->src_format = &server->m_pcmFormats[0];
        }

        context->SelectFormat(context, static_cast<UINT16>(selectedIndex));

        server->m_audioSampleRate = selectedRate;
        server->m_audioFramesSent = 0;
        server->m_audioTimestamp = 0;
        server->m_audioReady = true;

        QMetaObject::invokeMethod(server, "audioConfigured", Qt::QueuedConnection,
                                  Q_ARG(uint32_t, selectedRate));
    }
}

UINT RdpServer::rdpsnd_confirm_block(RdpsndServerContext* context, BYTE confirmBlockNum, UINT16 wtimestamp)
{
    Q_UNUSED(wtimestamp);
    if (!context) return CHANNEL_RC_OK;
    RdpServer* server = static_cast<RdpServer*>(context->data);
    if (!server) return CHANNEL_RC_OK;
    server->m_clientConfirmsBlocks = true;
    server->m_lastConfirmedBlock = confirmBlockNum;
    return CHANNEL_RC_OK;
}

void RdpServer::sendAudioSamples(const QByteArray &data)
{
    if (!m_audioReady) return;
    QMutexLocker locker(&m_audioMutex);
    if (!m_rdpsndContext) return;

    size_t nframes = data.size() / 4;
    if (nframes == 0) return;

    if (m_clientConfirmsBlocks) {
        int inFlight = (m_rdpsndContext->block_no - m_lastConfirmedBlock.load() + 256) % 256;
        if (inFlight > 10) {
            m_audioDroppedPrevious = true;
            m_audioFramesSent += nframes;
            return;
        }
    }

    m_audioFramesSent += nframes;

    size_t totalSamples = data.size() / sizeof(int16_t);
    const char* payloadData = data.constData();
    size_t payloadSize = data.size();
    QByteArray modifiedData;

    if (m_audioDroppedPrevious && totalSamples >= 128) {
        modifiedData = data;
        int16_t* samples = reinterpret_cast<int16_t*>(modifiedData.data());
        for (size_t i = 0; i < 64; ++i) {
            float alpha = static_cast<float>(i) / 64.0f;
            samples[i * 2]     = static_cast<int16_t>(m_lastLeftSample  * (1.0f - alpha) + samples[i * 2]     * alpha);
            samples[i * 2 + 1] = static_cast<int16_t>(m_lastRightSample * (1.0f - alpha) + samples[i * 2 + 1] * alpha);
        }
        m_audioDroppedPrevious = false;
        m_lastLeftSample = samples[totalSamples - 2];
        m_lastRightSample = samples[totalSamples - 1];
        payloadData = modifiedData.constData();
        payloadSize = modifiedData.size();
    } else {
        const int16_t* samples = reinterpret_cast<const int16_t*>(data.constData());
        if (totalSamples >= 2) {
            m_lastLeftSample = samples[totalSamples - 2];
            m_lastRightSample = samples[totalSamples - 1];
        }
    }

    const UINT64 nowMs = GetTickCount64();
    const UINT16 timestamp16 = static_cast<UINT16>(nowMs % 65536);
    const UINT32 timestamp32 = static_cast<UINT32>(nowMs & 0xFFFFFFFF);

    if (m_rdpsndContext->clientVersion >= 8 && m_rdpsndContext->SendSamples2) {
        UINT rc = m_rdpsndContext->SendSamples2(m_rdpsndContext,
                                                m_rdpsndContext->selected_client_format,
                                                payloadData,
                                                payloadSize,
                                                timestamp16,
                                                timestamp32);
        if (rc == CHANNEL_RC_OK) {
            return;
        }
    }

    m_rdpsndContext->SendSamples(m_rdpsndContext, payloadData, nframes, timestamp16);
}

void RdpServer::updateCursorShape(const QImage &image, const QPoint &hotspot)
{
    m_cursorManager.updateCursorShape(m_activePeer, image, hotspot);
}

void RdpServer::resetGraphicsSurface(UINT32 width, UINT32 height)
{
    if (m_activePeer && m_activePeer->context) {
        if (m_activePeer->context->settings) {
            freerdp_settings_set_uint32(m_activePeer->context->settings, FreeRDP_DesktopWidth, width);
            freerdp_settings_set_uint32(m_activePeer->context->settings, FreeRDP_DesktopHeight, height);
        }
        if (m_activePeer->context->update && m_activePeer->context->update->DesktopResize) {
            m_activePeer->context->update->DesktopResize(m_activePeer->context);
        }
    }
    m_gfxChannel.resetSurface(width, height);
}

void RdpServer::purgeStaleFrames()
{
    m_gfxChannel.purgeStaleFrames();
}
