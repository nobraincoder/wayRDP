#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>
#include <KSystemClipboard>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDebug>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <openssl/provider.h>
#include "core/RdpServer.h"
#include "display/IVirtualDisplayBackend.h"
#include "display/VirtualDisplayFactory.h"
#include "video/PipeWireStreamController.h"
#include "audio/QtAudioController.h"
#include "session/SessionLifecycleController.h"
#include "video/CodecHooks.h"

static int sigFd[2];
static QtMessageHandler s_defaultLogHandler = nullptr;
static std::atomic<bool> s_queueSaturated{false};

extern "C" Q_DECL_EXPORT bool Main_checkAndResetQueueSaturation(void)
{
    return s_queueSaturated.exchange(false, std::memory_order_acq_rel);
}

static void wayrdpMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (msg.contains(QLatin1String("Filter queue is full")) || msg.contains(QLatin1String("Encode queue is full"))) {
        s_queueSaturated.store(true, std::memory_order_release);
    }
    if (s_defaultLogHandler) {
        s_defaultLogHandler(type, context, msg);
    } else {
        fprintf(stderr, "%s\n", qPrintable(msg));
    }
}

static void signalHandler(int)
{
    char a = 1;
    ::write(sigFd[0], &a, sizeof(a));
}

static void ensurePermissionsAuthorized()
{
    QDBusInterface permStore(
        "org.freedesktop.impl.portal.PermissionStore",
        "/org/freedesktop/impl/portal/PermissionStore",
        "org.freedesktop.impl.portal.PermissionStore",
        QDBusConnection::sessionBus()
    );
    if (!permStore.isValid()) {
        qWarning() << "PermissionStore interface not available on session bus";
        return;
    }

    const QStringList apps = {"", "wayrdp", "org.kde.wayrdp", "org.kde.krdpserver"};
    const QStringList perms = {"yes"};

    for (const QString &app : apps) {
        QDBusReply<void> reply = permStore.call("SetPermission", "kde-authorized", true, "remote-desktop", app, perms);
        if (!reply.isValid()) {
            qWarning() << "Failed to set kde-authorized permission for" << (app.isEmpty() ? "<generic/empty>" : app) << ":" << reply.error().message();
        } else {
            qInfo() << "Mega-authorization configured in PermissionStore for:" << (app.isEmpty() ? "<generic/empty>" : app);
        }
    }
}

static void loadEnvironmentConfig()
{
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (configDir.isEmpty()) {
        configDir = QDir::homePath() + "/.config";
    }
    QString configPath = configDir + "/wayrdp.env";
    if (!QFile::exists(configPath)) {
        QString legacyPath = configDir + "/kde-virtual-rdp.env";
        if (QFile::exists(legacyPath)) {
            configPath = legacyPath;
        }
    }

    if (!QFile::exists(configPath)) {
        return;
    }

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    qInfo() << "Loading environment configuration from:" << configPath;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        int eq = line.indexOf('=');
        if (eq > 0) {
            QString key = line.left(eq).trimmed();
            QString val = line.mid(eq + 1).trimmed();
            if ((val.startsWith('"') && val.endsWith('"')) || (val.startsWith('\'') && val.endsWith('\''))) {
                val = val.mid(1, val.length() - 2);
            }
            if (qEnvironmentVariableIsEmpty(key.toUtf8().constData())) {
                qputenv(key.toUtf8().constData(), val.toUtf8());
            }
        }
    }
}

int main(int argc, char *argv[])
{
    s_defaultLogHandler = qInstallMessageHandler(wayrdpMessageHandler);
    loadEnvironmentConfig();

    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (runtimeDir.isEmpty()) {
            runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        }
        if (runtimeDir.isEmpty()) {
            runtimeDir = QString("/run/user/%1").arg(getuid());
        }
        if (QFile::exists(runtimeDir + "/wayland-0")) {
            qputenv("WAYLAND_DISPLAY", "wayland-0");
        } else {
            QDir dir(runtimeDir);
            QStringList sockets = dir.entryList(QStringList() << "wayland-*", QDir::System | QDir::Files);
            if (!sockets.isEmpty()) {
                qputenv("WAYLAND_DISPLAY", sockets.first().toUtf8());
            }
        }
    }

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "wayland");
    }

    if (qEnvironmentVariableIsSet("RDP_ENCODER")) {
        QString encoder = qEnvironmentVariable("RDP_ENCODER").trimmed().toLower();
        if (encoder == "vaapi" || encoder == "h264_vaapi") {
            qputenv("KPIPEWIRE_FORCE_ENCODER", "h264_vaapi");
        } else if (encoder == "nvenc" || encoder == "h264_nvenc") {
            qputenv("KPIPEWIRE_FORCE_ENCODER", "h264_nvenc");
        } else if (encoder == "x264" || encoder == "software" || encoder == "libx264") {
            qputenv("KPIPEWIRE_FORCE_ENCODER", "libx264");
        } else {
            qputenv("KPIPEWIRE_FORCE_ENCODER", encoder.toUtf8());
        }
    } else if (qEnvironmentVariableIsEmpty("KPIPEWIRE_FORCE_ENCODER")) {
        // Automatically probe hardware acceleration availability
        bool hasNvidia = (QFile::exists("/dev/nvidia0") || QFile::exists("/dev/nvidiactl"));
        bool hasDri = false;
        QDir driDir("/dev/dri");
        if (driDir.exists()) {
            const QStringList renders = driDir.entryList(QStringList() << "renderD*", QDir::System | QDir::Files);
            hasDri = !renders.isEmpty();
        }

        if (hasNvidia) {
            qInfo() << "Hardware auto-detection: Detected NVIDIA GPU node, selecting h264_nvenc";
            qputenv("KPIPEWIRE_FORCE_ENCODER", "h264_nvenc");
        } else if (hasDri) {
            qInfo() << "Hardware auto-detection: Detected DRI render node, selecting h264_vaapi";
            qputenv("KPIPEWIRE_FORCE_ENCODER", "h264_vaapi");
        } else {
            qInfo() << "Hardware auto-detection: No hardware encoder node detected, falling back to libx264";
            qputenv("KPIPEWIRE_FORCE_ENCODER", "libx264");
        }
    }

    QGuiApplication app(argc, argv);
    app.setDesktopFileName("org.kde.krdpserver");
    app.setApplicationName("org.kde.krdpserver");
    app.setApplicationDisplayName("wayRDP Server");
    app.setApplicationVersion("0.1.0");

    // Automatically configure mega-authorization so RemoteDesktop portal Start never stalls
    ensurePermissionsAuthorized();

    qInfo() << "Initializing OpenSSL legacy and default providers...";
    OSSL_PROVIDER* legacy = OSSL_PROVIDER_load(nullptr, "legacy");
    if (!legacy) {
        qWarning() << "Failed to load OpenSSL legacy provider. MD4/RC4 might be unavailable.";
    }
    OSSL_PROVIDER* deflt = OSSL_PROVIDER_load(nullptr, "default");
    if (!deflt) {
        qWarning() << "Failed to load OpenSSL default provider.";
    }

    qInfo() << "Starting wayRDP Server...";

    RdpServer server;
    auto virtualDisplay = VirtualDisplayFactory::createBackend(&app);
    PipeWireStreamController streamController;
    SessionLifecycleController lifecycleController;
    QtAudioController audioController;

    // Session lifecycle (Sleep inhibit during active RDP session, optional auto-lock on disconnect)
    QObject::connect(&server, &RdpServer::clientConnected,
                     &lifecycleController, &SessionLifecycleController::onClientConnected);
    QObject::connect(&server, &RdpServer::clientConnected,
                     [&lifecycleController, &virtualDisplay]() {
                         virtualDisplay->setScreenLocked(lifecycleController.isScreenLocked());
                     });
    QObject::connect(&server, &RdpServer::clientDisconnected,
                     &lifecycleController, &SessionLifecycleController::onClientDisconnected);

    // Dynamic lock/unlock transitions (Purge stale queue and request fresh IDR keyframe to prevent flash)
    QObject::connect(&lifecycleController, &SessionLifecycleController::sessionUnlocked,
                     [&virtualDisplay, &streamController, &server]() {
                         qInfo() << "Main: Session unlocked! Purging stale frames and requesting keyframe...";
                         virtualDisplay->setScreenLocked(false);
                         streamController.onClientActivity();
                         CodecHooks_requestKeyframe();
                         server.purgeStaleFrames();
                         // KScreenLocker teardown takes 150-250ms for KWin to finish compositing the unlocked desktop.
                         // Schedule a second purge and keyframe request after 250ms to ensure the clean desktop is transmitted
                         // and eradicate any lingering lockscreen frame from the hardware encoder pipeline.
                         QTimer::singleShot(250, &server, [&server, &streamController]() {
                             qInfo() << "Main: Post-unlock stabilization timer fired: requesting clean desktop keyframe";
                             streamController.onClientActivity();
                             server.purgeStaleFrames();
                             CodecHooks_requestKeyframe();
                         });
                     });
    QObject::connect(&lifecycleController, &SessionLifecycleController::sessionLocked,
                     [&virtualDisplay, &server]() {
                         qInfo() << "Main: Session locked! Updating display lock status and purging frames...";
                         virtualDisplay->setScreenLocked(true);
                         server.purgeStaleFrames();
                     });

    // Idle power saver (Dynamic FPS throttling when no input activity from client)
    QObject::connect(&server, &RdpServer::clientActivity,
                     &streamController, &PipeWireStreamController::onClientActivity);

    // Connect RdpServer and IVirtualDisplayBackend / PipeWireStreamController
    QObject::connect(&server, &RdpServer::clientConnected,
                     virtualDisplay.get(), &IVirtualDisplayBackend::onClientConnected);
    // When client disconnects, stop PipeWire stream FIRST, then tear down virtual display session
    QObject::connect(&server, &RdpServer::clientDisconnected,
                     &streamController, &PipeWireStreamController::onStreamStopped);
    QObject::connect(&server, &RdpServer::clientDisconnected,
                     virtualDisplay.get(), &IVirtualDisplayBackend::onClientDisconnected);

    // Dynamic resolution changes via DisplayControl ([MS-RDPEDISP])
    QObject::connect(&server, &RdpServer::requestedResolutionChanged,
                     virtualDisplay.get(), &IVirtualDisplayBackend::changeResolution);
    QObject::connect(&server, &RdpServer::requestedResolutionChanged,
                     [&streamController](const QSize &res, double) {
                         streamController.setTargetResolution(res);
                     });
    QObject::connect(&streamController, &PipeWireStreamController::streamSizeChanged,
                     [&server](const QSize &newSize) {
                         server.resetGraphicsSurface(newSize.width(), newSize.height());
                     });

    // Input events wiring
    QObject::connect(&server, &RdpServer::pointerMotionAbsolute,
                     virtualDisplay.get(), &IVirtualDisplayBackend::sendPointerMotionAbsolute);
    QObject::connect(&server, &RdpServer::pointerButton,
                     virtualDisplay.get(), &IVirtualDisplayBackend::sendPointerButton);
    QObject::connect(&server, &RdpServer::pointerAxis,
                     virtualDisplay.get(), &IVirtualDisplayBackend::sendPointerAxis);
    QObject::connect(&server, &RdpServer::pointerAxis,
                     &streamController, [&streamController](double, double) {
                         streamController.onMotionActivity();
                     });
    QObject::connect(&server, &RdpServer::pointerAxisDiscrete,
                     virtualDisplay.get(), &IVirtualDisplayBackend::sendPointerAxisDiscrete);
    QObject::connect(&server, &RdpServer::pointerAxisDiscrete,
                     &streamController, [&streamController](double, double) {
                         streamController.onMotionActivity();
                     });
    QObject::connect(&server, &RdpServer::keyboardKeycode,
                     virtualDisplay.get(), &IVirtualDisplayBackend::sendKeyboardKeycode);
    QObject::connect(&server, &RdpServer::keyboardKeysym,
                     virtualDisplay.get(), &IVirtualDisplayBackend::sendKeyboardKeysym);

    // Dynamic encoding parameters
    QObject::connect(&server, &RdpServer::clientEncodingConfigured,
                     &streamController, &PipeWireStreamController::setEncodingParameters);

    // Wire PipeWire screen capture stream
    QObject::connect(virtualDisplay.get(), &IVirtualDisplayBackend::streamStarted,
                     &streamController, &PipeWireStreamController::onStreamStarted);
    QObject::connect(&streamController, &PipeWireStreamController::videoPacketEncoded,
                     &server, &RdpServer::sendVideoFrame);
    QObject::connect(&streamController, &PipeWireStreamController::cursorShapeChanged,
                     &server, &RdpServer::updateCursorShape);

    // Audio output channel wiring
    QObject::connect(&server, &RdpServer::audioConfigured,
                     &audioController, &QtAudioController::startAudioCapture);
    QObject::connect(&audioController, &QtAudioController::audioSamplesReady,
                     &server, &RdpServer::sendAudioSamples, Qt::DirectConnection);
    QObject::connect(&server, &RdpServer::clientDisconnected,
                     &audioController, &QtAudioController::stopAudioCapture);


    // Wire Clipboard via KSystemClipboard & Klipper D-Bus
    // On Wayland, background services without an active focused window are rejected from calling
    // wl_data_device.set_selection directly. Setting clipboard contents via org.kde.klipper D-Bus
    // uses Plasma's privileged compositor connection, ensuring text is placed on the host clipboard reliably.
    KSystemClipboard *sysClipboard = KSystemClipboard::instance();

    QObject::connect(&server, &RdpServer::clientClipboardReceived, [sysClipboard](const QString &text) {
        qInfo() << "Applying received client clipboard text:" << text.left(40);
        QDBusInterface klipper("org.kde.klipper", "/klipper", "org.kde.klipper.klipper", QDBusConnection::sessionBus());
        if (klipper.isValid()) {
            klipper.call(QDBus::NoBlock, "setClipboardContents", text);
        }
        if (sysClipboard) {
            QMimeData *mime = new QMimeData();
            mime->setText(text);
            sysClipboard->setMimeData(mime, QClipboard::Clipboard);
        }
    });

    QObject::connect(&server, &RdpServer::clientFilesReceived, [sysClipboard](const QStringList &filePaths) {
        qInfo() << "Applying received client files to KSystemClipboard:" << filePaths;
        if (sysClipboard) {
            QList<QUrl> urls;
            for (const QString &path : filePaths) {
                urls.append(QUrl::fromLocalFile(path));
            }
            QMimeData *mime = new QMimeData();
            mime->setUrls(urls);
            mime->setText(filePaths.join("\n"));
            sysClipboard->setMimeData(mime, QClipboard::Clipboard);
        }
    });

    if (sysClipboard) {
        QObject::connect(sysClipboard, &KSystemClipboard::changed, [&server, sysClipboard](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard) {
                const QMimeData *mime = sysClipboard->mimeData(QClipboard::Clipboard);
                if (!mime) return;
                if (mime->hasUrls()) {
                    QStringList files;
                    for (const QUrl &url : mime->urls()) {
                        if (url.isLocalFile()) {
                            files.append(url.toLocalFile());
                        }
                    }
                    if (!files.isEmpty()) {
                        qInfo() << "KSystemClipboard contains files:" << files;
                        server.onHostClipboardFilesChanged(files);
                        return;
                    }
                }
                QString text = mime->text();
                if (!text.isEmpty()) {
                    server.onHostClipboardChanged(text);
                }
            }
        });
    }

    // Connect to Klipper's clipboardHistoryUpdated signal for host->client clipboard sync
    QDBusConnection::sessionBus().connect(
        "org.kde.klipper",
        "/klipper",
        "org.kde.klipper.klipper",
        "clipboardHistoryUpdated",
        &server,
        SLOT(onKlipperClipboardHistoryUpdated())
    );

    // Initial sync from Klipper
    server.onKlipperClipboardHistoryUpdated();

    int port = 3390;
    bool ok = false;
    if (qEnvironmentVariableIsSet("RDP_PORT")) {
        int envPort = qEnvironmentVariableIntValue("RDP_PORT", &ok);
        if (ok && envPort > 0 && envPort < 65536) {
            port = envPort;
        }
    }
    if (app.arguments().size() > 1) {
        int parsedPort = app.arguments().at(1).toInt(&ok);
        if (ok && parsedPort > 0 && parsedPort < 65536) {
            port = parsedPort;
        }
    }

    // Install SIGINT / SIGTERM signal handler to restore primary display and close sessions cleanly
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigFd) == 0) {
        QSocketNotifier *sn = new QSocketNotifier(sigFd[1], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, [sn, &app]() {
            if (sn) {
                sn->setEnabled(false);
            }
            char a;
            ::read(sigFd[1], &a, sizeof(a));
            qInfo() << "Received termination signal (SIGINT/SIGTERM), shutting down gracefully...";
            app.quit();
        });

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        if (virtualDisplay) {
            virtualDisplay->destroyDisplay();
        }
        server.stop();
    });

    if (!server.start(port)) {
        qCritical() << "Failed to start RdpServer on port" << port;
        return 1;
    }

    qInfo() << "wayRDP Server started and listening on port" << port;

    int ret = app.exec();
    if (virtualDisplay) {
        virtualDisplay->destroyDisplay();
    }
    server.stop();
    ::_exit(ret);
}
