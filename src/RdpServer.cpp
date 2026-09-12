#include "RdpServer.h"
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

struct PamUserData {
    QByteArray username;
    QByteArray password;
};

static int pamConversation(int num_msg, const struct pam_message** msg,
                           struct pam_response** resp, void* appdata_ptr)
{
    if (num_msg <= 0 || !resp || !appdata_ptr) {
        return PAM_CONV_ERR;
    }
    struct pam_response* reply = (struct pam_response*)calloc(num_msg, sizeof(struct pam_response));
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

RdpServer::RdpServer(QObject *parent)
    : QObject(parent), m_listener(nullptr), m_listenerThread(nullptr), m_running(false),
      m_cliprdrContext(nullptr), m_rdpsndContext(nullptr),
      m_activePeer(nullptr), m_audioTimestamp(0), m_audioReady(false)
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
}

RdpServer::~RdpServer()
{
    stop();
}

void RdpServer::generateCertificate()
{
    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(certDir);
    QString certPath = certDir + "/server.crt";
    QString keyPath = certDir + "/server.key";

    if (QFile::exists(certPath) && QFile::exists(keyPath)) {
        return;
    }
    if (QFile::exists("server.crt") && QFile::exists("server.key")) {
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

bool RdpServer::authenticateUser(const QString& username, const QString& password)
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
        return false;
    }

    // 2. PAM system credentials check
    qInfo() << "Authenticating user" << username << "via PAM...";
    pam_handle_t* pamh = nullptr;
    PamUserData userdata{ username.toUtf8(), password.toUtf8() };
    struct pam_conv conv = { pamConversation, &userdata };

    // Prefer login or krdp PAM service
    const char* pamService = "login";
    if (QFile::exists("/etc/pam.d/krdp")) {
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

bool RdpServer::start(int port)
{
    if (m_running)
        return false;

    // Generate certificates if they do not exist
    generateCertificate();

    m_listener = freerdp_listener_new();
    if (!m_listener) {
        qWarning() << "Failed to create FreeRDP listener";
        return false;
    }

    m_listener->info = this;
    m_listener->PeerAccepted = peerAccepted;
    
    qInfo() << "Opening FreeRDP listener on primary port" << port;
    if (!m_listener->Open(m_listener, nullptr, port)) {
        qWarning() << "Failed to open FreeRDP listener on primary port" << port;
        freerdp_listener_free(m_listener);
        m_listener = nullptr;
        return false;
    }

    // Support dual listening only if started on standard RDP port 3389
    if (port == 3389) {
        int altPort = 3390;
        qInfo() << "Attempting to also open FreeRDP listener on dual port" << altPort;
        if (!m_listener->Open(m_listener, nullptr, altPort)) {
            qWarning() << "Could not bind to dual port" << altPort << "- continuing on primary port" << port;
        } else {
            qInfo() << "Successfully bound FreeRDP listener to dual port" << altPort;
        }
    }

    m_running = true;
    m_listenerThread = CreateThread(nullptr, 0, listenerThread, this, 0, nullptr);
    
    return true;
}

void RdpServer::stop()
{
    if (!m_running)
        return;

    if (m_networkAdaptTimer) {
        m_networkAdaptTimer->stop();
    }

    m_running = false;
    m_gfxChannel.close();
    m_activePeer = nullptr;
    {
        QMutexLocker locker(&m_cliprdrMutex);
        m_cliprdrContext = nullptr;
        m_cliprdrReady = false;
        for (auto& inf : m_incomingFiles) {
            if (inf.localFile) {
                inf.localFile->close();
                delete inf.localFile;
                inf.localFile = nullptr;
            }
        }
        m_incomingFiles.clear();
        m_completedIncomingFilePaths.clear();
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

BOOL RdpServer::peerAccepted(freerdp_listener* listener, freerdp_peer* peer)
{
    qInfo() << "RDP peer accepted from client:" << peer->hostname;
    RdpServer* server = static_cast<RdpServer*>(listener->info);
    
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
        // Derive DPI if physical dimensions are supplied
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
    
    // Configure secure protocols
    freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE);
    
    // Capabilities and graphics pipeline
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

    // Load SSL certificate and private key
    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString certPath = certDir + "/server.crt";
    QString keyPath = certDir + "/server.key";
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
    
    // Register Logon and Activation callbacks
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
    
    // Phase 2: Peer Negotiation Loop
    qInfo() << "Entering peer negotiation loop...";
    while (server->m_running && !ctx->activated) {
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
    
    // Phase 3: Client Active Packet Processing Loop
    if (ctx->activated && server->m_running) {
        qInfo() << "RDP peer connection is fully active!";
        
        while (server->m_running && ctx->activated) {
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
    
    if (server->m_activePeer == peer) {
        server->m_activePeer = nullptr;
        server->m_gfxChannel.close();
    }
    {
        QMutexLocker locker(&server->m_cliprdrMutex);
        if (server->m_activePeer == peer) {
            server->m_cliprdrContext = nullptr;
            server->m_cliprdrReady = false;
            for (auto& inf : server->m_incomingFiles) {
                if (inf.localFile) {
                    inf.localFile->close();
                    delete inf.localFile;
                    inf.localFile = nullptr;
                }
            }
            server->m_incomingFiles.clear();
            server->m_completedIncomingFilePaths.clear();
        }
    }
    if (server->m_networkAdaptTimer) {
        QMetaObject::invokeMethod(server->m_networkAdaptTimer, "stop", Qt::QueuedConnection);
    }
    server->m_smoothedRttMs = 0;
    server->m_lastRttMs = 0;
    server->m_currentFps = 60;
    server->m_currentQuality = 95;
    {
        QMutexLocker locker(&server->m_audioMutex);
        if (server->m_activePeer == peer) {
            server->m_rdpsndContext = nullptr;
            server->m_audioReady = false;
        }
    }
    server->m_cursorHidden = false;
    server->m_cursorCache.clear();
    server->m_lastUsedCursor = nullptr;
    
    if (ctx->activated) {
        ctx->activated = false;
        QMetaObject::invokeMethod(server, "clientDisconnected", Qt::QueuedConnection);
    }
    
    peer->Disconnect(peer);
    
    return 0;
}

BOOL RdpServer::peerLogon(freerdp_peer* peer, const SEC_WINNT_AUTH_IDENTITY* identity, BOOL automatic)
{
    // If no identity provided during early TLS negotiation, defer authentication to peerPostConnect
    if (!identity || (!identity->User && identity->UserLength == 0)) {
        qInfo() << "peerLogon: No identity in early handshake, deferring to PostConnect (automatic:" << automatic << ")";
        return TRUE;
    }

    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(peer->context);
    RdpServer* server = ctx->server;
    
    // Extract credentials securely based on ANSI vs Unicode encoding flags
    bool isUnicode = (identity->Flags & SEC_WINNT_AUTH_IDENTITY_UNICODE) || !(identity->Flags & SEC_WINNT_AUTH_IDENTITY_ANSI);
    QString username;
    QString password;
    if (isUnicode) {
        if (identity->User && identity->UserLength > 0)
            username = QString::fromUtf16(reinterpret_cast<const char16_t*>(identity->User), identity->UserLength);
        if (identity->Password && identity->PasswordLength > 0)
            password = QString::fromUtf16(reinterpret_cast<const char16_t*>(identity->Password), identity->PasswordLength);
    } else {
        if (identity->User && identity->UserLength > 0)
            username = QString::fromUtf8(reinterpret_cast<const char*>(identity->User), identity->UserLength);
        if (identity->Password && identity->PasswordLength > 0)
            password = QString::fromUtf8(reinterpret_cast<const char*>(identity->Password), identity->PasswordLength);
    }

    if (username.contains('\\')) {
        username = username.section('\\', -1);
    }

    if (username.isEmpty() || password.isEmpty()) {
        qInfo() << "peerLogon: Username or password empty in early handshake, deferring to PostConnect";
        return TRUE;
    }

    qInfo() << "peerLogon: authenticating user:" << username
            << "(password length:" << password.length() << "automatic:" << automatic << ")";
    
    bool authenticated = server->authenticateUser(username, password);
    return authenticated ? TRUE : FALSE;
}

BOOL RdpServer::peerActivate(freerdp_peer* peer)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(peer->context);
    RdpServer* server = ctx->server;
    
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
    
    // Open Virtual Channel Manager
    if (!ctx->vcm || !WTSVirtualChannelManagerOpen(ctx->vcm)) {
        qWarning() << "Failed to open virtual channel manager";
        return FALSE;
    }

    // Register Input Callbacks
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

    // Initialize Cliprdr channel
    CliprdrServerContext* cliprdr = cliprdr_server_context_new(ctx->vcm);
    if (cliprdr) {
        cliprdr->custom = server;
        cliprdr->rdpcontext = reinterpret_cast<rdpContext*>(ctx);
        cliprdr->useLongFormatNames = TRUE;
        cliprdr->streamFileClipEnabled = TRUE;
        cliprdr->fileClipNoFilePaths = TRUE;
        cliprdr->canLockClipData = TRUE;
        // Auto-send Capabilities + Monitor Ready PDUs (required by Mac and Windows clients)
        cliprdr->autoInitializationSequence = TRUE;
        cliprdr->ClientCapabilities = cliprdr_client_capabilities;
        cliprdr->ClientFormatList = cliprdr_client_format_list;
        cliprdr->ClientFormatListResponse = cliprdr_client_format_list_response;
        cliprdr->ClientFormatDataRequest = cliprdr_client_format_data_request;
        cliprdr->ClientFormatDataResponse = cliprdr_client_format_data_response;
        cliprdr->ClientFileContentsRequest = cliprdr_client_file_contents_request;
        cliprdr->ClientFileContentsResponse = cliprdr_client_file_contents_response;
        cliprdr->ClientLockClipboardData = cliprdr_client_lock_clipboard_data;
        cliprdr->ClientUnlockClipboardData = cliprdr_client_unlock_clipboard_data;
        if (cliprdr->Start(cliprdr) == CHANNEL_RC_OK) {
            QMutexLocker locker(&server->m_cliprdrMutex);
            server->m_cliprdrContext = cliprdr;
            server->m_cliprdrReady = false;
            qInfo() << "Clipboard (cliprdr) channel initialized with file transfer enabled";
        } else {
            cliprdr_server_context_free(cliprdr);
        }
    }

    if (server->m_networkAdaptTimer) {
        QMetaObject::invokeMethod(server->m_networkAdaptTimer, "start", Qt::QueuedConnection);
    }


    // Initialize RDPGFX channel via RdpGfxChannel component
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

    emit server->clientEncodingConfigured(targetFps, targetQuality);
    
    qInfo() << "Negotiated Dynamic resolution:" << width << "x" << height << "@ scale" << effectiveScale;
    
    server->m_clientScale = effectiveScale;
    server->m_inputSettings.reload();

    emit server->clientConnected(QSize(width, height), effectiveScale);

    // Client-side hardware cursor: Allow client to render its own cursor locally at native refresh rate.
    // Cursor shapes are synchronized dynamically via updateCursorShape.
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

    const char* u = freerdp_settings_get_string(settings, FreeRDP_Username);
    const char* p = freerdp_settings_get_string(settings, FreeRDP_Password);

    QString username = u ? QString::fromUtf8(u) : QString();
    QString password = p ? QString::fromUtf8(p) : QString();

    if (username.contains('\\')) {
        username = username.section('\\', -1);
    }

    if (username.isEmpty()) {
        qWarning() << "No username provided by client, rejecting authentication";
        return FALSE;
    }

    qInfo() << "peerPostConnect: verifying credentials for user:" << username
            << "(password length:" << password.length() << ")";

    bool authenticated = server->authenticateUser(username, password);
    if (!authenticated) {
        qWarning() << "Authentication failed in PostConnect for user:" << username;
        return FALSE;
    }

    qInfo() << "PostConnect stage reached successfully and user authenticated";
    return TRUE;
}
void RdpServer::sendVideoFrame(const QByteArray &data, bool isKeyFrame)
{
    m_gfxChannel.sendFrame(data, isKeyFrame);
}

static QByteArray createXorMask(const QImage &image)
{
    auto converted = image.convertToFormat(QImage::Format_ARGB32);
    converted.flip(Qt::Vertical);
    converted.rgbSwap();
    return QByteArray(reinterpret_cast<char *>(converted.bits()), converted.sizeInBytes());
}

void RdpServer::updateCursorShape(const QImage &image, const QPoint &hotspot)
{
    if (image.isNull()) return;

    freerdp_peer* peer = m_activePeer;
    if (!peer || !peer->context || !peer->context->update || !peer->context->update->pointer)
        return;

    // RDP cannot handle cursor images larger than 384x384 px. Discard and use system default cursor.
    if (image.width() > 384 || image.height() > 384) {
        POINTER_SYSTEM_UPDATE pointerSystemUpdate;
        memset(&pointerSystemUpdate, 0, sizeof(pointerSystemUpdate));
        pointerSystemUpdate.type = SYSPTR_DEFAULT;
        peer->context->update->pointer->PointerSystem(peer->context, &pointerSystemUpdate);
        return;
    }

    // If currently displayed cursor is identical, update timestamp and return
    if (m_lastUsedCursor && m_lastUsedCursor->image == image && m_lastUsedCursor->hotspot == hotspot) {
        m_lastUsedCursor->lastUsed = std::chrono::steady_clock::now();
        return;
    }

    auto updatePointer = peer->context->update->pointer;

    // Check if cursor is already cached
    auto itr = std::find_if(m_cursorCache.begin(), m_cursorCache.end(), [&image, &hotspot](const CursorCacheEntry &cached) {
        return cached.hotspot == hotspot && cached.image == image;
    });
    if (itr != m_cursorCache.end()) {
        m_lastUsedCursor = &itr.value();
        itr->lastUsed = std::chrono::steady_clock::now();
        POINTER_CACHED_UPDATE pointerCachedUpdate;
        memset(&pointerCachedUpdate, 0, sizeof(pointerCachedUpdate));
        pointerCachedUpdate.cacheIndex = itr->cacheId;
        updatePointer->PointerCached(peer->context, &pointerCachedUpdate);
        return;
    }

    // New cursor entry
    CursorCacheEntry newCursor;
    newCursor.hotspot = hotspot;
    newCursor.image = image;
    newCursor.cacheId = static_cast<uint32_t>(m_cursorCache.size());
    newCursor.lastUsed = std::chrono::steady_clock::now();

    // Evict least recently used cursor if cache limit reached
    UINT32 maxCacheSize = freerdp_settings_get_uint32(peer->context->settings, FreeRDP_PointerCacheSize);
    if (maxCacheSize == 0) maxCacheSize = 20;
    if (static_cast<UINT32>(m_cursorCache.size()) >= maxCacheSize) {
        auto lru = std::min_element(m_cursorCache.cbegin(), m_cursorCache.cend(), [](const CursorCacheEntry &first, const CursorCacheEntry &second) {
            return first.lastUsed < second.lastUsed;
        });
        newCursor.cacheId = lru->cacheId;
        m_cursorCache.erase(lru);
    }

    auto xorMask = createXorMask(image);

    if (image.width() < 96 && image.height() < 96) {
        POINTER_NEW_UPDATE pointerNewUpdate;
        memset(&pointerNewUpdate, 0, sizeof(pointerNewUpdate));
        pointerNewUpdate.xorBpp = 32;
        auto &colorUpdate = pointerNewUpdate.colorPtrAttr;
        colorUpdate.cacheIndex = static_cast<UINT16>(newCursor.cacheId);
        colorUpdate.hotSpotX = static_cast<UINT16>(qBound(0, hotspot.x(), image.width() - 1));
        colorUpdate.hotSpotY = static_cast<UINT16>(qBound(0, hotspot.y(), image.height() - 1));
        colorUpdate.width = static_cast<UINT16>(image.width());
        colorUpdate.height = static_cast<UINT16>(image.height());
        // For 32-bit ARGB cursors, lengthAndMask = 0 enables native 8-bit alpha blending without 1-bit raster stippling
        colorUpdate.lengthAndMask = 0;
        colorUpdate.andMaskData = nullptr;
        colorUpdate.lengthXorMask = static_cast<UINT16>(xorMask.size());
        colorUpdate.xorMaskData = reinterpret_cast<BYTE *>(xorMask.data());
        updatePointer->PointerNew(peer->context, &pointerNewUpdate);
    } else {
        POINTER_LARGE_UPDATE pointerLargeUpdate;
        memset(&pointerLargeUpdate, 0, sizeof(pointerLargeUpdate));
        pointerLargeUpdate.xorBpp = 32;
        pointerLargeUpdate.cacheIndex = static_cast<UINT16>(newCursor.cacheId);
        pointerLargeUpdate.hotSpotX = static_cast<UINT16>(qBound(0, hotspot.x(), image.width() - 1));
        pointerLargeUpdate.hotSpotY = static_cast<UINT16>(qBound(0, hotspot.y(), image.height() - 1));
        pointerLargeUpdate.width = static_cast<UINT16>(image.width());
        pointerLargeUpdate.height = static_cast<UINT16>(image.height());
        pointerLargeUpdate.lengthAndMask = 0;
        pointerLargeUpdate.andMaskData = nullptr;
        pointerLargeUpdate.lengthXorMask = static_cast<UINT32>(xorMask.size());
        pointerLargeUpdate.xorMaskData = reinterpret_cast<BYTE *>(xorMask.data());
        updatePointer->PointerLarge(peer->context, &pointerLargeUpdate);
    }

    POINTER_CACHED_UPDATE pointerCachedUpdate;
    memset(&pointerCachedUpdate, 0, sizeof(pointerCachedUpdate));
    pointerCachedUpdate.cacheIndex = static_cast<UINT32>(newCursor.cacheId);
    updatePointer->PointerCached(peer->context, &pointerCachedUpdate);

    auto inserted = m_cursorCache.insert(newCursor.cacheId, newCursor);
    m_lastUsedCursor = &inserted.value();
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

BOOL RdpServer::peerSynchronizeEvent(rdpInput* input, UINT32 flags)
{
    Q_UNUSED(flags);
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server) return TRUE;

    const auto stuck = server->m_pressedKeys;
    server->m_pressedKeys.clear();
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

    // 1. Wheel and Horizontal Wheel Events
    // Per [MS-RDPBCGR] 2.2.8.1.1.3.1.1, when PTR_FLAGS_WHEEL or PTR_FLAGS_HWHEEL is set,
    // x and y MUST be set to 0 and NO mouse motion or button events should be processed.
    // Handling wheel packets in isolation prevents cursor teleportation to (0, 0)
    // and eliminates jitter during trackpad/mouse scrolling.
    if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) {
        int axis = flags & WheelRotationMask;
        if (axis & PTR_FLAGS_WHEEL_NEGATIVE) {
            axis = (~axis & WheelRotationMask) + 1;
        }
        int16_t rawDelta = (flags & PTR_FLAGS_WHEEL_NEGATIVE) ? -static_cast<int16_t>(axis) : static_cast<int16_t>(axis);

        if (rawDelta == 0) {
            return TRUE;
        }

        if (flags & PTR_FLAGS_WHEEL) {
            double dy = server->m_inputSettings.computeVerticalDelta(rawDelta);
            emit server->pointerAxis(0.0, dy);
        } else if (flags & PTR_FLAGS_HWHEEL) {
            double dx = server->m_inputSettings.computeHorizontalDelta(rawDelta);
            emit server->pointerAxis(dx, 0.0);
        }

        return TRUE;
    }

    // 2. Movement and Clicks
    if ((flags & PTR_FLAGS_MOVE) || (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3))) {
        emit server->pointerMotionAbsolute(static_cast<double>(x), static_cast<double>(y));
    }

    if (flags & PTR_FLAGS_BUTTON1) {
        uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(server->m_inputSettings.mapPointerButton(BTN_LEFT /* 272 */), state);
    }
    if (flags & PTR_FLAGS_BUTTON2) {
        uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(server->m_inputSettings.mapPointerButton(BTN_RIGHT /* 273 */), state);
    }
    if (flags & PTR_FLAGS_BUTTON3) {
        uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(BTN_MIDDLE /* 274 */, state);
    }

    return TRUE;
}

BOOL RdpServer::peerRelMouseEvent(rdpInput* input, UINT16 flags, INT16 xDelta, INT16 yDelta)
{
    Q_UNUSED(xDelta);
    Q_UNUSED(yDelta);
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server) return TRUE;

    emit server->clientActivity();

    if (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) {
        if (flags & PTR_FLAGS_BUTTON1) {
            uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
            emit server->pointerButton(server->m_inputSettings.mapPointerButton(BTN_LEFT /* 272 */), state);
        }
        if (flags & PTR_FLAGS_BUTTON2) {
            uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
            emit server->pointerButton(server->m_inputSettings.mapPointerButton(BTN_RIGHT /* 273 */), state);
        }
        if (flags & PTR_FLAGS_BUTTON3) {
            uint state = (flags & PTR_FLAGS_DOWN) ? 1 : 0;
            emit server->pointerButton(BTN_MIDDLE /* 274 */, state);
        }
    }
    return TRUE;
}

BOOL RdpServer::peerExtendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server) return TRUE;

    emit server->clientActivity();

    if ((flags & PTR_FLAGS_MOVE) || (flags & (PTR_XFLAGS_BUTTON1 | PTR_XFLAGS_BUTTON2))) {
        emit server->pointerMotionAbsolute(static_cast<double>(x), static_cast<double>(y));
    }

    if (flags & PTR_XFLAGS_BUTTON1) {
        uint state = (flags & PTR_XFLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(BTN_SIDE /* 275 */, state);
    }
    if (flags & PTR_XFLAGS_BUTTON2) {
        uint state = (flags & PTR_XFLAGS_DOWN) ? 1 : 0;
        emit server->pointerButton(BTN_EXTRA /* 276 */, state);
    }
    return TRUE;
}

BOOL RdpServer::peerKeyboardEvent(rdpInput* input, UINT16 flags, UINT8 code)
{
    MyPeerContext* ctx = reinterpret_cast<MyPeerContext*>(input->context);
    RdpServer* server = ctx ? ctx->server : static_cast<RdpServer*>(input->param1);
    if (!server) return TRUE;

    emit server->clientActivity();

    auto virtualCode = GetVirtualKeyCodeFromVirtualScanCode(flags & KBD_FLAGS_EXTENDED ? code | KBDEXT : code, 4);
    virtualCode = flags & KBD_FLAGS_EXTENDED ? virtualCode | KBDEXT : virtualCode;

    quint32 keycode = GetKeycodeFromVirtualKeyCode(virtualCode, WINPR_KEYCODE_TYPE_EVDEV);

    uint state = (flags & KBD_FLAGS_RELEASE) ? 0 : 1;
    if (state == 0) {
        server->m_pressedKeys.remove(keycode);
    } else {
        server->m_pressedKeys.insert(keycode);
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
    if (!server) return TRUE;

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

UINT RdpServer::cliprdr_client_capabilities(CliprdrServerContext* context, const CLIPRDR_CAPABILITIES* capabilities)
{
    Q_UNUSED(capabilities);
    RdpServer* server = static_cast<RdpServer*>(context->custom);
    if (!server) return CHANNEL_RC_OK;

    qInfo() << "CLIPRDR: ClientCapabilities received! Clipboard channel is now ready.";
    server->m_cliprdrReady = true;

    // If host has clipboard content, announce it now to the client
    QMutexLocker locker(&server->m_cliprdrMutex);
    if (!server->m_outgoingFiles.isEmpty()) {
        CLIPRDR_FORMAT_LIST formatList;
        memset(&formatList, 0, sizeof(formatList));
        CLIPRDR_FORMAT formats[4];
        formats[0].formatId = CF_UNICODETEXT;
        formats[0].formatName = nullptr;
        formats[1].formatId = CF_TEXT;
        formats[1].formatName = nullptr;
        formats[2].formatId = server->m_formatFileGroupDescriptorW;
        formats[2].formatName = const_cast<char*>("FileGroupDescriptorW");
        formats[3].formatId = server->m_formatFileContents;
        formats[3].formatName = const_cast<char*>("FileContents");

        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.common.msgFlags = 0;
        formatList.numFormats = 4;
        formatList.formats = formats;

        context->ServerFormatList(context, &formatList);
    } else if (!server->m_lastHostClipboardText.isEmpty()) {
        CLIPRDR_FORMAT_LIST formatList;
        memset(&formatList, 0, sizeof(formatList));
        CLIPRDR_FORMAT formats[2];
        formats[0].formatId = CF_UNICODETEXT;
        formats[0].formatName = nullptr;
        formats[1].formatId = CF_TEXT;
        formats[1].formatName = nullptr;

        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.common.msgFlags = 0;
        formatList.numFormats = 2;
        formatList.formats = formats;

        context->ServerFormatList(context, &formatList);
    }
    return CHANNEL_RC_OK;
}

UINT RdpServer::cliprdr_client_format_list_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
    Q_UNUSED(context);
    qInfo() << "CLIPRDR: ClientFormatListResponse received with flags:" << formatListResponse->common.msgFlags;
    return CHANNEL_RC_OK;
}

UINT RdpServer::cliprdr_client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* formatList)
{
    RdpServer* server = static_cast<RdpServer*>(context->custom);
    if (!server) return CHANNEL_RC_OK;

    qInfo() << "CLIPRDR: Client advertised" << formatList->numFormats << "clipboard formats";
    server->m_clientFileGroupDescriptorFormatId = 0;
    UINT32 requestedId = 0;

    for (UINT32 i = 0; i < formatList->numFormats; i++) {
        const char* name = formatList->formats[i].formatName;
        UINT32 id = formatList->formats[i].formatId;
        qInfo() << "  Format [" << i << "]: id=" << id
                << "name=" << (name ? name : "standard");
        if (name && (strcasecmp(name, "FileGroupDescriptorW") == 0 ||
                     strcasecmp(name, "FileGroupDescriptor") == 0)) {
            server->m_clientFileGroupDescriptorFormatId = id;
        }
    }

    CLIPRDR_FORMAT_LIST_RESPONSE response;
    memset(&response, 0, sizeof(response));
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    context->ServerFormatListResponse(context, &response);

    if (server->m_clientFileGroupDescriptorFormatId != 0) {
        // Client has files on clipboard! Request the descriptor
        CLIPRDR_FORMAT_DATA_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.common.msgType = CB_FORMAT_DATA_REQUEST;
        req.requestedFormatId = server->m_clientFileGroupDescriptorFormatId;
        context->lastRequestedFormatId = server->m_clientFileGroupDescriptorFormatId;
        context->ServerFormatDataRequest(context, &req);
        qInfo() << "CLIPRDR: Client has files on clipboard, requested FileGroupDescriptorW formatId:" << server->m_clientFileGroupDescriptorFormatId;
        return CHANNEL_RC_OK;
    }

    // Fall back to text formats
    for (UINT32 i = 0; i < formatList->numFormats; i++) {
        if (formatList->formats[i].formatId == CF_UNICODETEXT) {
            requestedId = CF_UNICODETEXT;
            break;
        } else if (formatList->formats[i].formatId == CF_TEXT && requestedId == 0) {
            requestedId = CF_TEXT;
        }
    }

    if (requestedId != 0) {
        CLIPRDR_FORMAT_DATA_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.common.msgType = CB_FORMAT_DATA_REQUEST;
        req.requestedFormatId = requestedId;
        context->lastRequestedFormatId = requestedId;
        context->ServerFormatDataRequest(context, &req);
        qInfo() << "CLIPRDR: Requested format data for formatId:" << requestedId;
    }

    return CHANNEL_RC_OK;
}

UINT RdpServer::cliprdr_client_format_data_request(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
    RdpServer* server = static_cast<RdpServer*>(context->custom);
    if (!server) return CHANNEL_RC_OK;

    qInfo() << "CLIPRDR: ClientFormatDataRequest for formatId:" << formatDataRequest->requestedFormatId;

    CLIPRDR_FORMAT_DATA_RESPONSE resp;
    memset(&resp, 0, sizeof(resp));
    resp.common.msgType = CB_FORMAT_DATA_RESPONSE;

    if (formatDataRequest->requestedFormatId == server->m_formatFileGroupDescriptorW) {
        QMutexLocker locker(&server->m_cliprdrMutex);
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.common.dataLen = server->m_outgoingFgdData.size();
        resp.requestedFormatData = reinterpret_cast<const BYTE*>(server->m_outgoingFgdData.constData());
        context->ServerFormatDataResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded with FileGroupDescriptorW (" << server->m_outgoingFgdData.size() << "bytes)";
        return CHANNEL_RC_OK;
    }

    if (formatDataRequest->requestedFormatId == CF_UNICODETEXT) {
        QByteArray utf16;
        {
            QMutexLocker locker(&server->m_cliprdrMutex);
            const ushort* utf16Data = server->m_lastHostClipboardText.utf16();
            int len = (server->m_lastHostClipboardText.length() + 1) * sizeof(char16_t);
            utf16 = QByteArray(reinterpret_cast<const char*>(utf16Data), len);
        }
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.common.dataLen = utf16.size();
        resp.requestedFormatData = reinterpret_cast<const BYTE*>(utf16.constData());
        context->ServerFormatDataResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded with CF_UNICODETEXT (" << utf16.size() << "bytes)";
    } else if (formatDataRequest->requestedFormatId == CF_TEXT) {
        QByteArray utf8;
        {
            QMutexLocker locker(&server->m_cliprdrMutex);
            utf8 = server->m_lastHostClipboardText.toUtf8();
            utf8.append('\0');
        }
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.common.dataLen = utf8.size();
        resp.requestedFormatData = reinterpret_cast<const BYTE*>(utf8.constData());
        context->ServerFormatDataResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded with CF_TEXT (" << utf8.size() << "bytes)";
    } else {
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        context->ServerFormatDataResponse(context, &resp);
        qWarning() << "CLIPRDR: Unsupported formatId requested:" << formatDataRequest->requestedFormatId;
    }

    return CHANNEL_RC_OK;
}

UINT RdpServer::cliprdr_client_format_data_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
    RdpServer* server = static_cast<RdpServer*>(context->custom);
    if (!server) return CHANNEL_RC_OK;

    qInfo() << "CLIPRDR: ClientFormatDataResponse, flags:" << formatDataResponse->common.msgFlags
            << "dataLen:" << formatDataResponse->common.dataLen;

    if (!(formatDataResponse->common.msgFlags & CB_RESPONSE_OK) || formatDataResponse->common.dataLen == 0) {
        return CHANNEL_RC_OK;
    }

    if (server->m_clientFileGroupDescriptorFormatId != 0 &&
        context->lastRequestedFormatId == server->m_clientFileGroupDescriptorFormatId) {
        // We received FileGroupDescriptorW!
        if (formatDataResponse->common.dataLen < sizeof(UINT32)) {
            qWarning() << "CLIPRDR: Received invalid FileGroupDescriptorW payload (too short):" << formatDataResponse->common.dataLen;
            return CHANNEL_RC_OK;
        }

        const BYTE* data = formatDataResponse->requestedFormatData;
        UINT32 cItems = *reinterpret_cast<const UINT32*>(data);
        qInfo() << "CLIPRDR: FileGroupDescriptorW contains" << cItems << "files";

        // Clean up previous incoming files if any
        for (auto& inf : server->m_incomingFiles) {
            if (inf.localFile) {
                inf.localFile->close();
                delete inf.localFile;
                inf.localFile = nullptr;
            }
        }
        server->m_incomingFiles.clear();
        server->m_completedIncomingFilePaths.clear();
        server->m_currentIncomingFileIndex = 0;

        QString targetDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/rdp-clipboard";
        if (targetDir.isEmpty() || !QDir().mkpath(targetDir)) {
            targetDir = QDir::tempPath() + "/rdp-clipboard";
            QDir().mkpath(targetDir);
        }

        const size_t fdSize = sizeof(FILEDESCRIPTORW);
        const BYTE* currentFd = data + sizeof(UINT32);

        for (UINT32 i = 0; i < cItems; i++) {
            if ((size_t)(currentFd - data + fdSize) > (size_t)formatDataResponse->common.dataLen) {
                qWarning() << "CLIPRDR: Truncated FILEDESCRIPTORW at item" << i;
                break;
            }

            const FILEDESCRIPTORW* fd = reinterpret_cast<const FILEDESCRIPTORW*>(currentFd);
            QString baseName = QString::fromUtf16(reinterpret_cast<const char16_t*>(fd->cFileName));
            // Sanitize filename to avoid path traversal
            baseName = QFileInfo(baseName).fileName();
            if (baseName.isEmpty()) {
                baseName = QString("clipboard_file_%1").arg(i);
            }

            uint64_t fileSize = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
            QString localPath = targetDir + "/" + baseName;

            IncomingFileTransfer transfer;
            transfer.fileName = baseName;
            transfer.localPath = localPath;
            transfer.fileSize = fileSize;
            transfer.receivedBytes = 0;
            transfer.localFile = nullptr;

            server->m_incomingFiles.append(transfer);
            qInfo() << "CLIPRDR: Queued incoming file [" << i << "]:" << baseName << "size:" << fileSize << "bytes -> target:" << localPath;

            currentFd += fdSize;
        }

        if (!server->m_incomingFiles.isEmpty()) {
            server->startNextIncomingFile(context);
        }
        return CHANNEL_RC_OK;
    }

    // Fall back to text formats
    QString text;
    if (context->lastRequestedFormatId == CF_UNICODETEXT) {
        text = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(formatDataResponse->requestedFormatData),
            formatDataResponse->common.dataLen / sizeof(char16_t)
        );
    } else {
        text = QString::fromUtf8(
            reinterpret_cast<const char*>(formatDataResponse->requestedFormatData),
            formatDataResponse->common.dataLen
        );
    }
    while (!text.isEmpty() && text.endsWith(QChar('\0'))) {
        text.chop(1);
    }
    if (!text.isEmpty()) {
        qInfo() << "CLIPRDR: Received text from client (" << text.length() << "chars):" << text.left(40);
        {
            QMutexLocker locker(&server->m_cliprdrMutex);
            server->m_lastHostClipboardText = text;
            server->m_outgoingFiles.clear();
            server->m_outgoingFgdData.clear();
        }
        emit server->clientClipboardReceived(text);
    }

    return CHANNEL_RC_OK;
}

void RdpServer::startNextIncomingFile(CliprdrServerContext* context)
{
    while (m_currentIncomingFileIndex < (uint32_t)m_incomingFiles.size()) {
        IncomingFileTransfer& item = m_incomingFiles[m_currentIncomingFileIndex];

        if (!item.localFile) {
            item.localFile = new QFile(item.localPath);
            if (!item.localFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                qWarning() << "CLIPRDR: Failed to open target file for writing:" << item.localPath;
                delete item.localFile;
                item.localFile = nullptr;
                m_currentIncomingFileIndex++;
                continue;
            }
        }

        if (item.fileSize == 0) {
            item.localFile->close();
            delete item.localFile;
            item.localFile = nullptr;
            m_completedIncomingFilePaths.append(item.localPath);
            m_currentIncomingFileIndex++;
            continue;
        }

        // Request initial chunk
        CLIPRDR_FILE_CONTENTS_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.common.msgType = CB_FILECONTENTS_REQUEST;
        req.streamId = ++m_fileStreamId;
        req.listIndex = m_currentIncomingFileIndex;
        req.dwFlags = FILECONTENTS_RANGE;
        req.nPositionLow = 0;
        req.nPositionHigh = 0;
        req.cbRequested = (UINT32)std::min((uint64_t)65536, item.fileSize);

        qInfo() << "CLIPRDR: Requesting initial chunk for file [" << m_currentIncomingFileIndex << "]"
                << item.fileName << "size:" << req.cbRequested << "streamId:" << req.streamId;
        context->ServerFileContentsRequest(context, &req);
        return;
    }

    // All files completed!
    if (!m_completedIncomingFilePaths.isEmpty()) {
        qInfo() << "CLIPRDR: All incoming files received (" << m_completedIncomingFilePaths.size() << "files):" << m_completedIncomingFilePaths;
        emit clientFilesReceived(m_completedIncomingFilePaths);
        // Free memory and temporary structures now that transmission is finished
        m_incomingFiles.clear();
        m_completedIncomingFilePaths.clear();
    }
}

UINT RdpServer::cliprdr_client_file_contents_request(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
    RdpServer* server = static_cast<RdpServer*>(context->custom);
    if (!server) return CHANNEL_RC_OK;

    qInfo() << "CLIPRDR: ClientFileContentsRequest: listIndex=" << fileContentsRequest->listIndex
            << "dwFlags=" << fileContentsRequest->dwFlags
            << "streamId=" << fileContentsRequest->streamId
            << "cbRequested=" << fileContentsRequest->cbRequested;

    CLIPRDR_FILE_CONTENTS_RESPONSE resp;
    memset(&resp, 0, sizeof(resp));
    resp.common.msgType = CB_FILECONTENTS_RESPONSE;
    resp.streamId = fileContentsRequest->streamId;

    QMutexLocker locker(&server->m_cliprdrMutex);
    if (fileContentsRequest->listIndex >= (UINT32)server->m_outgoingFiles.size()) {
        qWarning() << "CLIPRDR: Invalid listIndex in ClientFileContentsRequest:" << fileContentsRequest->listIndex;
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        context->ServerFileContentsResponse(context, &resp);
        return CHANNEL_RC_OK;
    }

    QString filePath = server->m_outgoingFiles[fileContentsRequest->listIndex];
    QFileInfo fi(filePath);
    if (!fi.exists()) {
        qWarning() << "CLIPRDR: File does not exist:" << filePath;
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        context->ServerFileContentsResponse(context, &resp);
        return CHANNEL_RC_OK;
    }

    if (fileContentsRequest->dwFlags & FILECONTENTS_SIZE) {
        uint64_t fileSize = fi.size();
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.cbRequested = sizeof(uint64_t);
        resp.requestedData = reinterpret_cast<const BYTE*>(&fileSize);
        context->ServerFileContentsResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded to FILECONTENTS_SIZE for file [" << fileContentsRequest->listIndex << "]:" << fileSize << "bytes";
        return CHANNEL_RC_OK;
    }

    if (fileContentsRequest->dwFlags & FILECONTENTS_RANGE) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "CLIPRDR: Could not open file for reading:" << filePath;
            resp.common.msgFlags = CB_RESPONSE_FAIL;
            context->ServerFileContentsResponse(context, &resp);
            return CHANNEL_RC_OK;
        }

        uint64_t offset = ((uint64_t)fileContentsRequest->nPositionHigh << 32) | fileContentsRequest->nPositionLow;
        if (!file.seek(offset)) {
            qWarning() << "CLIPRDR: Could not seek to offset:" << offset << "in" << filePath;
            resp.common.msgFlags = CB_RESPONSE_FAIL;
            context->ServerFileContentsResponse(context, &resp);
            return CHANNEL_RC_OK;
        }

        QByteArray buffer = file.read(fileContentsRequest->cbRequested);
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.cbRequested = buffer.size();
        resp.requestedData = reinterpret_cast<const BYTE*>(buffer.constData());
        context->ServerFileContentsResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded to FILECONTENTS_RANGE at offset:" << offset << "sent:" << buffer.size() << "bytes";
        return CHANNEL_RC_OK;
    }

    resp.common.msgFlags = CB_RESPONSE_FAIL;
    context->ServerFileContentsResponse(context, &resp);
    return CHANNEL_RC_OK;
}

UINT RdpServer::cliprdr_client_file_contents_response(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse)
{
    RdpServer* server = static_cast<RdpServer*>(context->custom);
    if (!server) return CHANNEL_RC_OK;

    if (server->m_currentIncomingFileIndex >= (uint32_t)server->m_incomingFiles.size()) {
        qWarning() << "CLIPRDR: Unexpected file contents response, no active file";
        return CHANNEL_RC_OK;
    }

    IncomingFileTransfer& item = server->m_incomingFiles[server->m_currentIncomingFileIndex];

    if (!(fileContentsResponse->common.msgFlags & CB_RESPONSE_OK)) {
        qWarning() << "CLIPRDR: File contents response failed for file" << item.fileName;
        if (item.localFile) {
            item.localFile->close();
            delete item.localFile;
            item.localFile = nullptr;
        }
        server->m_currentIncomingFileIndex++;
        server->startNextIncomingFile(context);
        return CHANNEL_RC_OK;
    }

    if (item.localFile && fileContentsResponse->cbRequested > 0 && fileContentsResponse->requestedData) {
        qint64 written = item.localFile->write(reinterpret_cast<const char*>(fileContentsResponse->requestedData),
                                               fileContentsResponse->cbRequested);
        if (written > 0) {
            item.receivedBytes += written;
        }
    }

    if (item.receivedBytes >= item.fileSize) {
        // File is complete!
        qInfo() << "CLIPRDR: Completed transfer of file:" << item.localPath << "(" << item.receivedBytes << "bytes)";
        if (item.localFile) {
            item.localFile->close();
            delete item.localFile;
            item.localFile = nullptr;
        }
        server->m_completedIncomingFilePaths.append(item.localPath);
        server->m_currentIncomingFileIndex++;
        server->startNextIncomingFile(context);
    } else {
        // Request next chunk
        uint64_t remaining = item.fileSize - item.receivedBytes;
        UINT32 chunkSize = (UINT32)std::min((uint64_t)65536, remaining);

        CLIPRDR_FILE_CONTENTS_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.common.msgType = CB_FILECONTENTS_REQUEST;
        req.streamId = ++server->m_fileStreamId;
        req.listIndex = server->m_currentIncomingFileIndex;
        req.dwFlags = FILECONTENTS_RANGE;
        req.nPositionLow = (UINT32)(item.receivedBytes & 0xFFFFFFFF);
        req.nPositionHigh = (UINT32)(item.receivedBytes >> 32);
        req.cbRequested = chunkSize;

        context->ServerFileContentsRequest(context, &req);
    }

    return CHANNEL_RC_OK;
}

UINT RdpServer::cliprdr_client_lock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_LOCK_CLIPBOARD_DATA* lockClipboardData)
{
    Q_UNUSED(context);
    Q_UNUSED(lockClipboardData);
    return CHANNEL_RC_OK;
}

UINT RdpServer::cliprdr_client_unlock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlockClipboardData)
{
    Q_UNUSED(context);
    Q_UNUSED(unlockClipboardData);
    return CHANNEL_RC_OK;
}

void RdpServer::onHostClipboardChanged(const QString &text)
{
    QMutexLocker locker(&m_cliprdrMutex);
    if (m_lastHostClipboardText == text && m_outgoingFiles.isEmpty()) return;
    m_lastHostClipboardText = text;
    m_outgoingFiles.clear();
    m_outgoingFgdData.clear();

    if (!m_cliprdrContext || !m_cliprdrReady) return;

    qInfo() << "CLIPRDR: Host clipboard changed (" << text.length() << "chars), announcing ServerFormatList";

    CLIPRDR_FORMAT_LIST formatList;
    memset(&formatList, 0, sizeof(formatList));
    CLIPRDR_FORMAT formats[2];
    formats[0].formatId = CF_UNICODETEXT;
    formats[0].formatName = nullptr;
    formats[1].formatId = CF_TEXT;
    formats[1].formatName = nullptr;

    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = 2;
    formatList.formats = formats;

    m_cliprdrContext->ServerFormatList(m_cliprdrContext, &formatList);
}

void RdpServer::onHostClipboardFilesChanged(const QStringList &filePaths)
{
    QStringList validFiles;
    for (const QString& path : filePaths) {
        if (QFileInfo::exists(path)) {
            validFiles.append(path);
        }
    }

    if (validFiles.isEmpty()) return;

    QMutexLocker locker(&m_cliprdrMutex);
    m_outgoingFiles = validFiles;

    // Build FileGroupDescriptorW payload
    UINT32 cItems = validFiles.size();
    size_t headerSize = sizeof(UINT32);
    size_t itemSize = sizeof(FILEDESCRIPTORW);
    size_t totalSize = headerSize + cItems * itemSize;

    m_outgoingFgdData.resize(totalSize);
    m_outgoingFgdData.fill(0);

    BYTE* ptr = reinterpret_cast<BYTE*>(m_outgoingFgdData.data());
    *reinterpret_cast<UINT32*>(ptr) = cItems;
    ptr += headerSize;

    for (int i = 0; i < validFiles.size(); i++) {
        QFileInfo fi(validFiles[i]);
        FILEDESCRIPTORW* fd = reinterpret_cast<FILEDESCRIPTORW*>(ptr + i * itemSize);
        fd->dwFlags = FD_FILESIZE | FD_WRITESTIME | FD_ATTRIBUTES;
        fd->dwFileAttributes = 0x00000080; // FILE_ATTRIBUTE_NORMAL
        uint64_t sz = fi.size();
        fd->nFileSizeLow = (DWORD)(sz & 0xFFFFFFFF);
        fd->nFileSizeHigh = (DWORD)(sz >> 32);

        QString fileName = fi.fileName();
        int copyLen = static_cast<int>(std::min<qsizetype>(fileName.length(), 259));
        memcpy(fd->cFileName, fileName.utf16(), copyLen * sizeof(char16_t));
        fd->cFileName[copyLen] = 0;
    }

    m_lastHostClipboardText = validFiles.join("\n");

    if (!m_cliprdrContext || !m_cliprdrReady) return;

    qInfo() << "CLIPRDR: Host clipboard files changed (" << validFiles.size() << "files), announcing ServerFormatList";

    CLIPRDR_FORMAT_LIST formatList;
    memset(&formatList, 0, sizeof(formatList));
    CLIPRDR_FORMAT formats[4];
    formats[0].formatId = CF_UNICODETEXT;
    formats[0].formatName = nullptr;
    formats[1].formatId = CF_TEXT;
    formats[1].formatName = nullptr;
    formats[2].formatId = m_formatFileGroupDescriptorW;
    formats[2].formatName = const_cast<char*>("FileGroupDescriptorW");
    formats[3].formatId = m_formatFileContents;
    formats[3].formatName = const_cast<char*>("FileContents");

    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = 4;
    formatList.formats = formats;

    m_cliprdrContext->ServerFormatList(m_cliprdrContext, &formatList);
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
        // EWMA: 70% previous, 30% new sample
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

    if (targetFps != m_currentFps.load() || targetQuality != m_currentQuality.load()) {
        m_currentFps = targetFps;
        m_currentQuality = targetQuality;
        qInfo() << "Network Adaptation: RTT" << rtt << "ms (smoothed" << smoothed << "ms) -> Adjusting encoding: FPS="
                << targetFps << "Quality=" << targetQuality;
        emit clientEncodingConfigured(targetFps, targetQuality);
    }
}

void RdpServer::onKlipperClipboardUpdated()
{
    // No-op: Native QClipboard handles Wayland clipboard synchronization without blocking D-Bus calls
}

void RdpServer::rdpsnd_activated(RdpsndServerContext* context)
{
    qInfo() << "RDPSND channel activated by client";
    if (!context) return;

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

    // 1. First choice: 48000Hz 16-bit stereo PCM
    for (size_t i = 0; i < context->num_client_formats; i++) {
        const AUDIO_FORMAT* fmt = &context->client_formats[i];
        if (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->nChannels == 2 &&
            fmt->wBitsPerSample == 16 && fmt->nSamplesPerSec == 48000) {
            selectedIndex = static_cast<int>(i);
            selectedRate = 48000;
            break;
        }
    }

    // 2. Second choice: 44100Hz 16-bit stereo PCM
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

    // 3. Fallback: Any 16-bit stereo PCM
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

    // 4. Ultimate fallback: format 0
    if (selectedIndex < 0 && context->num_client_formats > 0) {
        selectedIndex = 0;
        selectedRate = context->client_formats[0].nSamplesPerSec > 0 ? context->client_formats[0].nSamplesPerSec : 44100;
    }

    if (selectedIndex >= 0) {
        qInfo() << "Selecting audio format index:" << selectedIndex << "with sample rate:" << selectedRate;

        // MUST set latency and src_format BEFORE SelectFormat, because SelectFormat
        // internally computes out_frames = src_format->nSamplesPerSec * latency / 1000
        context->latency = 20; // Match PulseAudio 20ms fragment size exactly

        // Update src_format to match the selected sample rate
        if (selectedRate == 44100) {
            context->src_format = &server->m_pcmFormats[1];
        } else {
            context->src_format = &server->m_pcmFormats[0];
        }

        context->SelectFormat(context, static_cast<UINT16>(selectedIndex));

        // Set volume to maximum after format is selected
        if (context->SetVolume) {
            context->SetVolume(context, 0xFFFF, 0xFFFF);
        }

        server->m_audioSampleRate = selectedRate;
        server->m_audioFramesSent = 0;
        server->m_audioReady = true;

        QMetaObject::invokeMethod(server, "audioConfigured", Qt::QueuedConnection,
                                  Q_ARG(uint32_t, selectedRate));
    }
}

void RdpServer::sendAudioSamples(const QByteArray &data)
{
    if (!m_audioReady) return;
    QMutexLocker locker(&m_audioMutex);
    if (!m_rdpsndContext) return;

    size_t nframes = data.size() / 4;
    if (nframes == 0) return;

    uint32_t rate = m_audioSampleRate.load();
    if (rate == 0) rate = 48000;

    // MS-RDPEA requires wTimeStamp to be the actual monotonic transmission time
    UINT16 timestamp = static_cast<UINT16>(GetTickCount64() % 65536);

    m_rdpsndContext->SendSamples(m_rdpsndContext, data.constData(), nframes, timestamp);
}

