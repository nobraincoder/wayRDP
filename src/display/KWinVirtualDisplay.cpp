#include "display/KWinVirtualDisplay.h"
#include "input/EiConnection.h"
#include "video/CodecHooks.h"
#include <cmath>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QDBusArgument>
#include <unistd.h>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusPendingCallWatcher>
#include <QDBusInterface>
#include <QVariantMap>
#include <QUuid>
#include <QSettings>
#include <QDir>
#include <QProcess>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QStandardPaths>

QDBusArgument &operator<<(QDBusArgument &arg, const PortalStream &stream) {
    arg.beginStructure();
    arg << stream.id << stream.map;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalStream &stream) {
    arg.beginStructure();
    arg >> stream.id;
    arg.beginMap();
    while (!arg.atEnd()) {
        QString key;
        QVariant val;
        arg.beginMapEntry();
        arg >> key >> val;
        arg.endMapEntry();
        stream.map.insert(key, val);
    }
    arg.endMap();
    arg.endStructure();
    return arg;
}

KWinVirtualDisplay::KWinVirtualDisplay(QObject *parent)
    : IVirtualDisplayBackend(parent), m_displayActive(false), m_requestedScale(1.0)
{
    qDBusRegisterMetaType<PortalStream>();
    qDBusRegisterMetaType<QList<PortalStream>>();
}

KWinVirtualDisplay::~KWinVirtualDisplay()
{
    cancelPointerNudges();
    destroyDisplay();
}

bool KWinVirtualDisplay::createDisplay(const QString& name, const QSize& size, double scale)
{
    m_resolutionRetryCount = 0;
    if (m_displayActive || !m_sessionPath.isEmpty()) {
        qInfo() << "Display session is already active or in progress. Re-initializing display...";
        destroyDisplay();
    }

    m_displayName = name;
    m_requestedSize = size;
    m_requestedScale = scale;

    qInfo() << "Initiating Portal session for virtual output:" << name << "with size:" << size << "scale:" << scale;

    // Step 1: CreateSession
    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "CreateSession"
    );

    QVariantMap options;
    QString sessionToken = QUuid::createUuid().toString(QUuid::Id128);
    QString requestToken = QUuid::createUuid().toString(QUuid::Id128);
    options.insert("session_handle_token", sessionToken);
    options.insert("handle_token", requestToken);

    message.setArguments({options});

    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onCreateSessionResponse(uint,QVariantMap,QDBusMessage))
    );

    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "CreateSession DBus call failed:" << reply.error().message();
            QDBusConnection::sessionBus().disconnect(
                "org.freedesktop.portal.Desktop",
                QString(),
                "org.freedesktop.portal.Request",
                "Response",
                this,
                SLOT(onCreateSessionResponse(uint,QVariantMap,QDBusMessage))
            );
            return;
        }
        m_currentRequestPath = reply.value();
        qInfo() << "CreateSession request path:" << m_currentRequestPath.path();
    });

    return true;
}

void KWinVirtualDisplay::onCreateSessionResponse(uint code, const QVariantMap &results, const QDBusMessage &msg)
{
    if (m_currentRequestPath.path().isEmpty() || msg.path() != m_currentRequestPath.path()) {
        qWarning() << "Ignoring stale or mismatched CreateSession response from path:" << msg.path()
                   << "(expected:" << m_currentRequestPath.path() << ")";
        return;
    }

    QDBusConnection::sessionBus().disconnect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onCreateSessionResponse(uint,QVariantMap,QDBusMessage))
    );
    m_currentRequestPath = QDBusObjectPath();

    if (code != 0) {
        qWarning() << "CreateSession failed or was cancelled by user. Code:" << code;
        return;
    }

    m_sessionPath = results.value("session_handle").toString();
    qInfo() << "Session created successfully with path:" << m_sessionPath;

    // Step 2: SelectDevices
    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "SelectDevices"
    );

    QVariantMap options;
    QString requestToken = QUuid::createUuid().toString(QUuid::Id128);
    options.insert("handle_token", requestToken);
    // Keyboard (1) | Pointer (2) | Touchscreen (4) = 7u
    options.insert("types", QVariant::fromValue(7u));
    // Persist mode: 2 (PermissionsPersistUntilExplicitlyRevoked)
    options.insert("persist_mode", QVariant::fromValue(2u));

    QString configPath = QDir::homePath() + "/.local/state/wayrdp-serverstaterc";
    QSettings settings(configPath, QSettings::IniFormat);
    QString restoreToken = settings.value("restorationToken").toString();
    if (restoreToken.isEmpty()) {
        restoreToken = settings.value("General/restorationToken").toString();
    }
    // Fallback to krdp-serverstaterc if not yet migrated
    if (restoreToken.isEmpty()) {
        QSettings fallbackSettings(QDir::homePath() + "/.local/state/krdp-serverstaterc", QSettings::IniFormat);
        restoreToken = fallbackSettings.value("restorationToken").toString();
    }
    if (!restoreToken.isEmpty()) {
        qInfo() << "Using restore token for RemoteDesktop session:" << restoreToken;
        options.insert("restore_token", restoreToken);
    }

    message.setArguments({QDBusObjectPath(m_sessionPath), options});

    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onSelectDevicesResponse(uint,QVariantMap,QDBusMessage))
    );

    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "SelectDevices DBus call failed:" << reply.error().message();
            QDBusConnection::sessionBus().disconnect(
                "org.freedesktop.portal.Desktop",
                QString(),
                "org.freedesktop.portal.Request",
                "Response",
                this,
                SLOT(onSelectDevicesResponse(uint,QVariantMap,QDBusMessage))
            );
            return;
        }
        m_currentRequestPath = reply.value();
    });
}

void KWinVirtualDisplay::onSelectDevicesResponse(uint code, const QVariantMap &results, const QDBusMessage &msg)
{
    Q_UNUSED(results);
    if (m_currentRequestPath.path().isEmpty() || msg.path() != m_currentRequestPath.path()) {
        qWarning() << "Ignoring stale or mismatched SelectDevices response from path:" << msg.path()
                   << "(expected:" << m_currentRequestPath.path() << ")";
        return;
    }

    QDBusConnection::sessionBus().disconnect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onSelectDevicesResponse(uint,QVariantMap,QDBusMessage))
    );
    m_currentRequestPath = QDBusObjectPath();

    if (code != 0) {
        qWarning() << "SelectDevices failed or was cancelled by user. Code:" << code;
        return;
    }

    qInfo() << "Devices selected successfully";

    // Step 3: SelectSources
    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "SelectSources"
    );

    QVariantMap options;
    QString requestToken = QUuid::createUuid().toString(QUuid::Id128);
    options.insert("handle_token", requestToken);
    // Types: 4u (Virtual)
    options.insert("types", QVariant::fromValue(4u));
    options.insert("multiple", false);
    options.insert("cursor_mode", QVariant::fromValue(4u)); // Metadata mode (cursor decoupled from video stream)

    message.setArguments({QDBusObjectPath(m_sessionPath), options});

    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onSelectSourcesResponse(uint,QVariantMap,QDBusMessage))
    );

    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "SelectSources DBus call failed:" << reply.error().message();
            QDBusConnection::sessionBus().disconnect(
                "org.freedesktop.portal.Desktop",
                QString(),
                "org.freedesktop.portal.Request",
                "Response",
                this,
                SLOT(onSelectSourcesResponse(uint,QVariantMap,QDBusMessage))
            );
            return;
        }
        m_currentRequestPath = reply.value();
    });
}

void KWinVirtualDisplay::onSelectSourcesResponse(uint code, const QVariantMap &results, const QDBusMessage &msg)
{
    Q_UNUSED(results);
    if (m_currentRequestPath.path().isEmpty() || msg.path() != m_currentRequestPath.path()) {
        qWarning() << "Ignoring stale or mismatched SelectSources response from path:" << msg.path()
                   << "(expected:" << m_currentRequestPath.path() << ")";
        return;
    }

    QDBusConnection::sessionBus().disconnect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onSelectSourcesResponse(uint,QVariantMap,QDBusMessage))
    );
    m_currentRequestPath = QDBusObjectPath();

    if (code != 0) {
        qWarning() << "SelectSources failed or was cancelled by user. Code:" << code;
        return;
    }

    qInfo() << "Sources selected successfully";

    // Step 4: Start
    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "Start"
    );

    QVariantMap options;
    QString requestToken = QUuid::createUuid().toString(QUuid::Id128);
    options.insert("handle_token", requestToken);

    message.setArguments({QDBusObjectPath(m_sessionPath), QString(""), options});

    QDBusConnection::sessionBus().connect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onStartResponse(uint,QVariantMap,QDBusMessage))
    );

    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "Start DBus call failed:" << reply.error().message();
            QDBusConnection::sessionBus().disconnect(
                "org.freedesktop.portal.Desktop",
                QString(),
                "org.freedesktop.portal.Request",
                "Response",
                this,
                SLOT(onStartResponse(uint,QVariantMap,QDBusMessage))
            );
            return;
        }
        m_currentRequestPath = reply.value();
    });
}

void KWinVirtualDisplay::onStartResponse(uint code, const QVariantMap &results, const QDBusMessage &msg)
{
    if (m_currentRequestPath.path().isEmpty() || msg.path() != m_currentRequestPath.path()) {
        qWarning() << "Ignoring stale or mismatched Start response from path:" << msg.path()
                   << "(expected:" << m_currentRequestPath.path() << ")";
        return;
    }

    QDBusConnection::sessionBus().disconnect(
        "org.freedesktop.portal.Desktop",
        QString(),
        "org.freedesktop.portal.Request",
        "Response",
        this,
        SLOT(onStartResponse(uint,QVariantMap,QDBusMessage))
    );
    m_currentRequestPath = QDBusObjectPath();

    if (code != 0) {
        qWarning() << "Start failed or was cancelled by user. Code:" << code;
        return;
    }

    uint grantedDevices = results.value("devices").toUInt();
    qInfo() << "Portal Session started successfully! Granted devices bitmask:" << grantedDevices;
    if (grantedDevices == 0) {
        qWarning() << "WARNING: No input devices were granted by the Portal!";
    }

    // Extract stream node ID and mapping ID
    m_streamNodeId = 0;
    m_streamMappingId.clear();
    const auto streams = qdbus_cast<QList<PortalStream>>(results.value("streams"));
    if (!streams.isEmpty()) {
        m_streamNodeId = streams.first().id;
        m_streamMappingId = streams.first().map.value("mapping_id").toString();
        if (m_streamMappingId.isEmpty()) {
            m_streamMappingId = streams.first().map.value("id").toString();
        }
    }

    qInfo() << "Extracted PipeWire stream node ID:" << m_streamNodeId << "mapping ID:" << m_streamMappingId;

    QString restoreToken = results.value("restore_token").toString();
    if (!restoreToken.isEmpty()) {
        qInfo() << "Saving new restore token:" << restoreToken;
        QString configPath = QDir::homePath() + "/.local/state/wayrdp-serverstaterc";
        QFileInfo configInfo(configPath);
        QDir().mkpath(configInfo.absolutePath());

        QSettings settings(configPath, QSettings::IniFormat);
        settings.setValue("restorationToken", restoreToken);
        settings.setValue("General/restorationToken", restoreToken);
        settings.sync();
    }

    m_displayActive = true;

    // Connect to low-latency libei EIS input engine
    connectToEis();

    // Wait a brief moment for KWin to register the virtual output, then configure resolution and start stream
    const uint64_t initialGen = ++m_resolutionGeneration;
    QTimer::singleShot(300, this, [this, initialGen]() {
        if (initialGen == m_resolutionGeneration) {
            setupVirtualDisplayResolution();
        }
    });
}

void KWinVirtualDisplay::connectToEis()
{
    qInfo() << "KWinVirtualDisplay: Connecting to EIS for session:" << m_sessionPath;

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "ConnectToEIS"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options});

    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "KWinVirtualDisplay: ConnectToEIS DBus call failed:" << reply.error().message() << "- using DBus input fallback.";
            return;
        }

        QDBusUnixFileDescriptor unixFd = reply.value();
        if (!unixFd.isValid()) {
            qWarning() << "KWinVirtualDisplay: Invalid file descriptor from ConnectToEIS - using DBus input fallback.";
            return;
        }

        int fd = unixFd.fileDescriptor();
        int dupedFd = dup(fd);
        if (dupedFd < 0) {
            qWarning() << "KWinVirtualDisplay: dup() failed for EIS FD:" << strerror(errno) << "- using DBus input fallback.";
            return;
        }
        qInfo() << "KWinVirtualDisplay: Received EIS FD:" << fd << "(duped:" << dupedFd << "), initializing low-latency EiConnection";

        m_eiConnection = std::make_unique<EiConnection>(dupedFd, this);
        connect(m_eiConnection.get(), &EiConnection::error, this, [this]() {
            qWarning() << "KWinVirtualDisplay: EiConnection error encountered! Resetting EIS connection to fallback to DBus.";
            m_eiConnection.reset();
        });
        connect(m_eiConnection.get(), &EiConnection::connected, this, [this]() {
            qInfo() << "KWinVirtualDisplay: Low-latency libei input engine initialized successfully!";
        });
    });
}

void KWinVirtualDisplay::openPipeWireRemote()
{
    qInfo() << "Opening PipeWire remote file descriptor for session:" << m_sessionPath;

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "OpenPipeWireRemote"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options});

    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "OpenPipeWireRemote DBus call failed:" << reply.error().message();
            return;
        }

        QDBusUnixFileDescriptor unixFd = reply.value();
        if (!unixFd.isValid()) {
            qWarning() << "Invalid file descriptor received from OpenPipeWireRemote";
            return;
        }

        int fd = unixFd.fileDescriptor();
        int dupedFd = dup(fd);
        if (dupedFd < 0) {
            qWarning() << "KWinVirtualDisplay: dup() failed for PipeWire Remote FD:" << strerror(errno);
            return;
        }
        qInfo() << "Successfully retrieved duplicated PipeWire Remote FD:" << dupedFd << "for stream node ID:" << m_streamNodeId;

        m_streamOpened = true;
        emit streamStarted(m_streamNodeId, dupedFd, m_requestedSize);
    });
}

static QProcessEnvironment getKScreenEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!env.contains("QT_QPA_PLATFORM")) {
        env.insert("QT_QPA_PLATFORM", "wayland");
    }
    if (!env.contains("XDG_RUNTIME_DIR")) {
        QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtimeDir.isEmpty()) {
            runtimeDir = QString("/run/user/%1").arg(getuid());
        }
        env.insert("XDG_RUNTIME_DIR", runtimeDir);
    }
    if (!env.contains("WAYLAND_DISPLAY")) {
        QString runtimeDir = env.value("XDG_RUNTIME_DIR");
        if (QFile::exists(runtimeDir + "/wayland-0")) {
            env.insert("WAYLAND_DISPLAY", "wayland-0");
        } else {
            QDir dir(runtimeDir);
            QStringList sockets = dir.entryList(QStringList() << "wayland-*", QDir::System | QDir::Files);
            if (!sockets.isEmpty()) {
                env.insert("WAYLAND_DISPLAY", sockets.first());
            }
        }
    }
    return env;
}

void KWinVirtualDisplay::setupVirtualDisplayResolution()
{
    if (!m_displayActive) return;

    const uint64_t generation = m_resolutionGeneration;
    qInfo() << "Configuring virtual display resolution to" << m_requestedSize << "and scale" << m_requestedScale
            << "(generation:" << generation << ")";

    QProcess* proc = new QProcess(this);
    QProcessEnvironment env = getKScreenEnvironment();
    proc->setProcessEnvironment(env);
    proc->start("kscreen-doctor", QStringList() << "-j");

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, proc, env, generation](int exitCode, QProcess::ExitStatus exitStatus) {
        proc->deleteLater();
        if (generation != m_resolutionGeneration) {
            qInfo() << "Discarding stale resolution query (generation mismatch:" << generation << "vs current:" << m_resolutionGeneration << ")";
            return;
        }
        if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
            qWarning() << "Failed to run kscreen-doctor -j";
            openPipeWireRemote();
            return;
        }

        QByteArray output = proc->readAllStandardOutput();
        QJsonDocument doc = QJsonDocument::fromJson(output);
        if (doc.isNull() || !doc.isObject()) {
            qWarning() << "Invalid JSON output from kscreen-doctor";
            openPipeWireRemote();
            return;
        }

        QJsonObject root = doc.object();
        QJsonArray outputs = root.value("outputs").toArray();
        QString virtualOutputName;
        int virtualOutputId = 0;
        QString currentPrimaryOutput;
        int currentPrimaryOutputId = 0;
        QJsonObject virtualOutputObj;

        for (const QJsonValue& val : outputs) {
            QJsonObject outObj = val.toObject();
            QString name = outObj.value("name").toString();
            int id = outObj.value("id").toInt(0);
            bool connected = outObj.value("connected").toBool(true);
            bool isVirtual = name.contains("VIRTUAL", Qt::CaseInsensitive);

            if (isVirtual && connected) {
                virtualOutputName = name;
                virtualOutputId = id;
                virtualOutputObj = outObj;
            } else if (!isVirtual && connected) {
                if (currentPrimaryOutput.isEmpty() || outObj.value("priority").toInt(0) == 1) {
                    currentPrimaryOutput = name;
                    currentPrimaryOutputId = id;
                }
            }
        }

        if (virtualOutputName.isEmpty() || virtualOutputId == 0) {
            qWarning() << "Virtual output not found in kscreen-doctor yet, retrying in 300ms...";
            QTimer::singleShot(300, this, [this, generation]() {
                if (generation == m_resolutionGeneration) {
                    setupVirtualDisplayResolution();
                }
            });
            return;
        }

        if (!currentPrimaryOutput.isEmpty()) {
            m_originalPrimaryOutput = currentPrimaryOutput;
        }

        qInfo() << "Found virtual output:" << virtualOutputName << "(ID:" << virtualOutputId << ")"
                << "Original primary output:" << m_originalPrimaryOutput;

        // Find existing mode matching requested width and height (exact or CVT 8-pixel horizontal alignment)
        QString matchedModeSpec;
        QJsonArray modes = virtualOutputObj.value("modes").toArray();
        for (const QJsonValue& mVal : modes) {
            QJsonObject mObj = mVal.toObject();
            QJsonObject sObj = mObj.value("size").toObject();
            int w = sObj.value("width").toInt();
            int h = sObj.value("height").toInt();
            if (w == m_requestedSize.width() && h == m_requestedSize.height()) {
                matchedModeSpec = mObj.value("id").toString();
                if (matchedModeSpec.isEmpty()) {
                    matchedModeSpec = mObj.value("name").toString();
                }
                break;
            }
        }

        // If no exact match, check for existing CVT 8-pixel aligned mode
        if (matchedModeSpec.isEmpty()) {
            for (const QJsonValue& mVal : modes) {
                QJsonObject mObj = mVal.toObject();
                QJsonObject sObj = mObj.value("size").toObject();
                int w = sObj.value("width").toInt();
                int h = sObj.value("height").toInt();
                if (std::abs(w - m_requestedSize.width()) <= 8 && h == m_requestedSize.height()) {
                    matchedModeSpec = mObj.value("id").toString();
                    if (matchedModeSpec.isEmpty()) {
                        matchedModeSpec = mObj.value("name").toString();
                    }
                    qInfo() << "Found existing CVT 8-pixel aligned mode:" << w << "x" << h
                            << "(ID:" << matchedModeSpec << ") for requested" << m_requestedSize;
                    m_requestedSize.setWidth(w);
                    break;
                }
            }
        }

        auto applySettings = [this, virtualOutputName, virtualOutputId, currentPrimaryOutputId, env, generation](const QString &modeIdOrName) {
            if (generation != m_resolutionGeneration) {
                qInfo() << "Discarding stale applySettings (generation mismatch:" << generation << "vs current:" << m_resolutionGeneration << ")";
                return;
            }

            // Use integer ID for kscreen-doctor arguments so dots in names like
            // "Virtual-virtual-xdp-kde-org.kde.krdpserver" are not split as command line sub-properties!
            QString targetOutputSpec = QString::number(virtualOutputId);
            QStringList args;
            if (!modeIdOrName.isEmpty()) {
                args << QString("output.%1.mode.%2").arg(targetOutputSpec).arg(modeIdOrName);
            } else {
                qWarning() << "No explicit mode ID available to apply; retaining existing mode and applying scale/priority";
            }
            args << QString("output.%1.scale.%2").arg(targetOutputSpec).arg(m_requestedScale);
            args << QString("output.%1.priority.1").arg(targetOutputSpec);
            if (currentPrimaryOutputId > 0 && currentPrimaryOutputId != virtualOutputId) {
                args << QString("output.%1.priority.2").arg(currentPrimaryOutputId);
            } else if (!m_originalPrimaryOutput.isEmpty() && m_originalPrimaryOutput != virtualOutputName) {
                args << QString("output.%1.priority.2").arg(m_originalPrimaryOutput);
            }

            qInfo() << "Running: kscreen-doctor" << args.join(" ");
            QProcess* applyProc = new QProcess(this);
            applyProc->setProcessEnvironment(env);
            applyProc->start("kscreen-doctor", args);
            connect(applyProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, applyProc, virtualOutputName, env, generation](int exitCode, QProcess::ExitStatus) {
                QString out = QString::fromUtf8(applyProc->readAllStandardOutput()).trimmed();
                QString err = QString::fromUtf8(applyProc->readAllStandardError()).trimmed();
                qInfo() << "applyProc (kscreen-doctor mode/scale) finished with code:" << exitCode;
                if (!out.isEmpty()) qInfo() << "applyProc stdout:" << out;
                if (!err.isEmpty()) qWarning() << "applyProc stderr:" << err;
                applyProc->deleteLater();

                if (generation != m_resolutionGeneration) {
                    qInfo() << "Discarding stale applyProc finished (generation mismatch:" << generation << "vs current:" << m_resolutionGeneration << ")";
                    return;
                }

                // Check actual configured mode and scale
                QProcess* checkProc = new QProcess(this);
                checkProc->setProcessEnvironment(env);
                checkProc->start("kscreen-doctor", QStringList() << "-j");
                connect(checkProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, checkProc, virtualOutputName, generation](int, QProcess::ExitStatus) {
                    QJsonDocument doc = QJsonDocument::fromJson(checkProc->readAllStandardOutput());
                    checkProc->deleteLater();

                    if (generation != m_resolutionGeneration) {
                        qInfo() << "Discarding stale checkProc finished (generation mismatch:" << generation << "vs current:" << m_resolutionGeneration << ")";
                        return;
                    }

                    int configuredWidth = 0;
                    int configuredHeight = 0;
                    if (!doc.isNull()) {
                        for (const QJsonValue& val : doc.object().value("outputs").toArray()) {
                            QJsonObject obj = val.toObject();
                            if (obj.value("name").toString() == virtualOutputName) {
                                configuredWidth = obj.value("size").toObject().value("width").toInt();
                                configuredHeight = obj.value("size").toObject().value("height").toInt();
                                qInfo() << "Virtual display active mode ID:" << obj.value("currentModeId").toString()
                                        << "scale:" << obj.value("scale").toDouble()
                                        << "size:" << configuredWidth << "x" << configuredHeight;
                                break;
                            }
                        }
                    }

                    // Verify whether the mode actually took effect (exact or within 8-pixel CVT alignment)
                    bool modeMatches = (configuredWidth == m_requestedSize.width() || std::abs(configuredWidth - m_requestedSize.width()) <= 8) &&
                                       (configuredHeight == m_requestedSize.height());

                    if (!modeMatches && m_resolutionRetryCount < 3) {
                        m_resolutionRetryCount++;
                        qWarning() << "Virtual display mode not yet applied by KWin (currently" << configuredWidth << "x" << configuredHeight
                                   << ", requested" << m_requestedSize << "). Retrying in 250ms (attempt" << m_resolutionRetryCount << "/3)...";
                        QTimer::singleShot(250, this, [this, generation]() {
                            if (generation == m_resolutionGeneration) {
                                setupVirtualDisplayResolution();
                            }
                        });
                        return;
                    }

                    m_resolutionRetryCount = 0;

                    // Synchronize m_requestedSize to the actual active compositor output size so
                    // PipeWire and FreeRDP stay 100% in agreement even if KWin fell back to an existing mode.
                    if (configuredWidth > 0 && configuredHeight > 0) {
                        if (configuredWidth != m_requestedSize.width() || configuredHeight != m_requestedSize.height()) {
                            qInfo() << "Synchronizing virtual display target size from" << m_requestedSize
                                    << "to active compositor size:" << configuredWidth << "x" << configuredHeight;
                            m_requestedSize = QSize(configuredWidth, configuredHeight);
                        }
                    }

                    if (!m_streamOpened) {
                        // Give KWin Wayland compositor 250ms to finish reallocating screencast buffer pool
                        QTimer::singleShot(250, this, &KWinVirtualDisplay::openPipeWireRemote);
                    } else {
                        qInfo() << "Virtual display mode updated dynamically to" << configuredWidth << "x" << configuredHeight;
                    }
                });
            });
        };

        if (matchedModeSpec.isEmpty()) {
            // VESA CVT timings require horizontal resolution to be a multiple of 8 (e.g. 2732 -> 2728, 1364 -> 1360)
            int cvtWidth = (m_requestedSize.width() / 8) * 8;
            QString targetOutputSpec = QString::number(virtualOutputId);
            qInfo() << "Mode" << m_requestedSize << "does not exist on output ID" << targetOutputSpec
                    << "(requesting CVT 8-pixel aligned width:" << cvtWidth << "), adding custom mode...";
            QProcess* addModeProc = new QProcess(this);
            addModeProc->setProcessEnvironment(env);
            QStringList addArgs;
            addArgs << QString("output.%1.addCustomMode.%2.%3.60000.reduced")
                       .arg(targetOutputSpec).arg(cvtWidth).arg(m_requestedSize.height());
            qInfo() << "Running: kscreen-doctor" << addArgs.join(" ");
            addModeProc->start("kscreen-doctor", addArgs);
            connect(addModeProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, addModeProc, applySettings, virtualOutputName, env, cvtWidth, generation](int exitCode, QProcess::ExitStatus) {
                QString out = QString::fromUtf8(addModeProc->readAllStandardOutput()).trimmed();
                QString err = QString::fromUtf8(addModeProc->readAllStandardError()).trimmed();
                qInfo() << "addModeProc (kscreen-doctor addCustomMode) finished with code:" << exitCode;
                if (!out.isEmpty()) qInfo() << "addModeProc stdout:" << out;
                if (!err.isEmpty()) qWarning() << "addModeProc stderr:" << err;
                addModeProc->deleteLater();

                if (generation != m_resolutionGeneration) {
                    qInfo() << "Discarding stale addModeProc finished (generation mismatch:" << generation << "vs current:" << m_resolutionGeneration << ")";
                    return;
                }

                // Re-query modes from kscreen-doctor to find the newly assigned mode ID
                QProcess* recheckProc = new QProcess(this);
                recheckProc->setProcessEnvironment(env);
                recheckProc->start("kscreen-doctor", QStringList() << "-j");
                connect(recheckProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, recheckProc, applySettings, virtualOutputName, cvtWidth, generation](int, QProcess::ExitStatus) {
                    QString newModeId;
                    QJsonDocument doc = QJsonDocument::fromJson(recheckProc->readAllStandardOutput());
                    recheckProc->deleteLater();

                    if (generation != m_resolutionGeneration) {
                        qInfo() << "Discarding stale recheckProc finished (generation mismatch:" << generation << "vs current:" << m_resolutionGeneration << ")";
                        return;
                    }

                    if (!doc.isNull()) {
                        for (const QJsonValue& val : doc.object().value("outputs").toArray()) {
                            QJsonObject obj = val.toObject();
                            if (obj.value("name").toString() == virtualOutputName) {
                                for (const QJsonValue& mVal : obj.value("modes").toArray()) {
                                    QJsonObject mObj = mVal.toObject();
                                    QJsonObject sObj = mObj.value("size").toObject();
                                    int w = sObj.value("width").toInt();
                                    int h = sObj.value("height").toInt();
                                    if ((w == m_requestedSize.width() || w == cvtWidth || std::abs(w - m_requestedSize.width()) <= 8) &&
                                        h == m_requestedSize.height()) {
                                        newModeId = mObj.value("id").toString();
                                        if (newModeId.isEmpty()) {
                                            newModeId = mObj.value("name").toString();
                                        }
                                        m_requestedSize.setWidth(w);
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }
                    qInfo() << "Discovered newly added mode ID:" << newModeId << "for" << m_requestedSize;
                    applySettings(newModeId);
                });
            });
        } else {
            applySettings(matchedModeSpec);
        }
    });
}

void KWinVirtualDisplay::destroyDisplay()
{
    if (!m_currentRequestPath.path().isEmpty()) {
        QDBusMessage cancelMessage = QDBusMessage::createMethodCall(
            "org.freedesktop.portal.Desktop",
            m_currentRequestPath.path(),
            "org.freedesktop.portal.Request",
            "Close"
        );
        QDBusConnection::sessionBus().asyncCall(cancelMessage);

        QDBusConnection::sessionBus().disconnect(
            "org.freedesktop.portal.Desktop",
            QString(),
            "org.freedesktop.portal.Request",
            "Response",
            this,
            nullptr
        );
        m_currentRequestPath = QDBusObjectPath();
    }

    if (m_resolutionDebounceTimer) {
        m_resolutionDebounceTimer->stop();
    }
    m_resolutionGeneration++;

    if (m_sessionPath.isEmpty()) {
        return;
    }

    qInfo() << "Closing portal session" << m_sessionPath << "to destroy virtual display";

    if (!m_originalPrimaryOutput.isEmpty()) {
        qInfo() << "Restoring primary output priority to:" << m_originalPrimaryOutput;
        QProcessEnvironment env = getKScreenEnvironment();
        QProcess* restoreProc = new QProcess(this);
        restoreProc->setProcessEnvironment(env);
        restoreProc->start("kscreen-doctor", QStringList() << QString("output.%1.priority.1").arg(m_originalPrimaryOutput));
        connect(restoreProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), restoreProc, &QProcess::deleteLater);
        m_originalPrimaryOutput.clear();
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        m_sessionPath,
        "org.freedesktop.portal.Session",
        "Close"
    );

    QDBusConnection::sessionBus().asyncCall(message);

    m_sessionPath.clear();
    m_displayActive = false;
    m_streamOpened = false;
    m_eiConnection.reset();
    m_streamMappingId.clear();
}

void KWinVirtualDisplay::onClientConnected(const QSize &resolution, double scale)
{
    qInfo() << "KWinVirtualDisplay: RDP Client connected with resolution:" << resolution << "scale:" << scale;
    if (m_displayActive && m_requestedSize == resolution && qFuzzyCompare(m_requestedScale, scale)) {
        qInfo() << "KWinVirtualDisplay: Virtual display session is already active with matching resolution/scale, keeping existing session.";
        return;
    }
    createDisplay("VIRTUAL-1", resolution, scale);
}

void KWinVirtualDisplay::onClientDisconnected()
{
    qInfo() << "KWinVirtualDisplay: RDP Client disconnected";
    cancelPointerNudges();
    m_eiConnection.reset();
    m_accumulatedX = 0.0;
    m_accumulatedY = 0.0;
    destroyDisplay();
}

void KWinVirtualDisplay::setScreenLocked(bool locked)
{
    m_isScreenLocked = locked;
    cancelPointerNudges();
    qInfo() << "KWinVirtualDisplay: Screen lock state set to:" << (locked ? "LOCKED" : "UNLOCKED");
}

void KWinVirtualDisplay::cancelPointerNudges()
{
    for (QTimer *timer : m_nudgeTimers) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
    }
    m_nudgeTimers.clear();
}

void KWinVirtualDisplay::changeResolution(const QSize &newSize, double scale)
{
    if (!m_displayActive || newSize.isEmpty())
        return;

    bool sizeChanged = (newSize != m_requestedSize &&
                        !(std::abs(newSize.width() - m_requestedSize.width()) <= 8 && newSize.height() == m_requestedSize.height()));
    bool scaleChanged = (scale > 0.0 && std::abs(scale - m_requestedScale) > 0.01);

    if (!sizeChanged && !scaleChanged)
        return;

    m_requestedSize = newSize;
    if (scale > 0.0) {
        m_requestedScale = scale;
    }
    m_resolutionRetryCount = 0;

    const uint64_t generation = ++m_resolutionGeneration;
    qInfo() << "KWinVirtualDisplay: Debouncing dynamic resolution adjustment from" << m_requestedSize << "@ scale" << m_requestedScale
            << "to" << newSize << "@ scale" << (scale > 0.0 ? scale : m_requestedScale)
            << "(generation:" << generation << ")";

    if (!m_resolutionDebounceTimer) {
        m_resolutionDebounceTimer = new QTimer(this);
        m_resolutionDebounceTimer->setSingleShot(true);
        connect(m_resolutionDebounceTimer, &QTimer::timeout, this, [this]() {
            setupVirtualDisplayResolution();
        });
    }
    m_resolutionDebounceTimer->start(150);
}

void KWinVirtualDisplay::sendPointerMotionAbsolute(double x, double y)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    if (x == m_lastPointerX && y == m_lastPointerY)
        return;

    cancelPointerNudges();

    m_lastPointerX = x;
    m_lastPointerY = y;

    if (m_eiConnection && m_eiConnection->hasPointer()) {
        m_eiConnection->sendPointerMotionAbsolute(x, y, m_requestedSize, m_streamMappingId);
        return;
    }

    if (m_streamNodeId == 0)
        return;

    // The RemoteDesktop portal expects logical coordinates for the virtual display stream.
    // When Retina display is enabled, the RDP client reports physical pixel coordinates (e.g. 3456x2168),
    // but KWin's logical space for the output is (width / scale, height / scale) (e.g. 1728x1084).
    double logicalX = (m_requestedScale > 0.0) ? (x / m_requestedScale) : x;
    double logicalY = (m_requestedScale > 0.0) ? (y / m_requestedScale) : y;

    // Rate-limit D-Bus fallback to 120 Hz to prevent flooding the session bus with 1000 Hz mouse events
    auto now = std::chrono::steady_clock::now();
    if (m_lastDbusMotionTime.time_since_epoch().count() > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastDbusMotionTime).count() < 8) {
        return;
    }
    m_lastDbusMotionTime = now;

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "NotifyPointerMotionAbsolute"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options, m_streamNodeId, logicalX, logicalY});
    QDBusConnection::sessionBus().asyncCall(message);
}

void KWinVirtualDisplay::sendPointerMotion(double dx, double dy)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    cancelPointerNudges();

    if (m_eiConnection && m_eiConnection->hasPointer()) {
        m_eiConnection->sendPointerMotion(dx, dy);
        return;
    }

    // D-Bus fallback via org.freedesktop.portal.RemoteDesktop.NotifyPointerMotion
    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "NotifyPointerMotion"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options, dx, dy});
    QDBusConnection::sessionBus().asyncCall(message);
}

void KWinVirtualDisplay::sendPointerButton(int button, uint state)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    bool sentViaEi = false;
    if (m_eiConnection && m_eiConnection->hasPointer()) {
        sentViaEi = m_eiConnection->sendPointerButton(button, state);
    }

    if (!sentViaEi) {
        QDBusMessage message = QDBusMessage::createMethodCall(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerButton"
        );

        QVariantMap options;
        message.setArguments({QDBusObjectPath(m_sessionPath), options, button, state});
        QDBusPendingCall pcall = QDBusConnection::sessionBus().asyncCall(message);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [button, state](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<> reply = *w;
            w->deleteLater();
            if (reply.isError()) {
                qWarning() << "NotifyPointerButton DBus error:" << reply.error().message() << "button:" << button << "state:" << state;
            }
        });
    }

    // When releasing a mouse button (e.g. desktop area selection rubberband, dragging window/files,
    // closing dropdown menus), Plasma/apps tear down the overlay/selection box with an OpacityAnimator (~250ms).
    // Request an IDR keyframe right after the animation to ensure any lingering sub-pixel opacity ghosts
    // are completely eliminated, without synthetic pointer movement that triggers KWin's shakecursor effect.
    if (state == 0 && !m_isScreenLocked) {
        cancelPointerNudges();
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, timer]() {
            CodecHooks_requestKeyframe();
            m_nudgeTimers.removeOne(timer);
            timer->deleteLater();
        });
        m_nudgeTimers.append(timer);
        timer->start(280);
    } else if (state == 1) {
        cancelPointerNudges();
    }
}

void KWinVirtualDisplay::doSendAxis(double dx, double dy)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "NotifyPointerAxis"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options, dx, dy});
    QDBusConnection::sessionBus().asyncCall(message);
}

void KWinVirtualDisplay::sendPointerAxis(double dx, double dy)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    if (m_eiConnection && m_eiConnection->hasPointer()) {
        m_eiConnection->sendPointerAxis(dx, dy);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    double dtMs = std::chrono::duration<double, std::milli>(now - m_lastAxisTime).count();
    m_lastAxisTime = now;

    // Reset accumulator after a pause (> 150ms) to ensure clean new gestures
    if (dtMs > 150.0) {
        m_accumulatedX = 0.0;
        m_accumulatedY = 0.0;
    }

    // Isolate axes: Standard trackpad scrolls are either vertical or horizontal.
    // Resetting the opposite accumulator prevents stale cross-axis accumulation.
    if (dy != 0.0) {
        dx = 0.0;
        m_accumulatedX = 0.0;
    } else if (dx != 0.0) {
        dy = 0.0;
        m_accumulatedY = 0.0;
    }

    // Sub-pixel accumulator: Accumulate fractional deltas and emit whole integer pixels.
    // This completely eliminates fractional application deadbands and up/down micro-jitter,
    // while allowing the client OS (macOS/Windows) to manage natural kinetic inertia directly.
    m_accumulatedX += dx;
    m_accumulatedY += dy;

    double sendX = 0.0;
    double sendY = 0.0;

    if (std::abs(m_accumulatedX) >= 1.0) {
        double wholeX = std::trunc(m_accumulatedX);
        sendX = wholeX;
        m_accumulatedX -= wholeX;
    }

    if (std::abs(m_accumulatedY) >= 1.0) {
        double wholeY = std::trunc(m_accumulatedY);
        sendY = wholeY;
        m_accumulatedY -= wholeY;
    }

    // Portal Workaround: xdg-desktop-portal-kde only handles ONE axis per call
    // (if dx != 0 it handles dx and ignores dy). Always emit separate DBus calls.
    if (sendX != 0.0) {
        qDebug() << "PORTAL_AXIS_X: sendX=" << sendX;
        doSendAxis(sendX, 0.0);
    }
    if (sendY != 0.0) {
        qDebug() << "PORTAL_AXIS_Y: sendY=" << sendY;
        doSendAxis(0.0, sendY);
    }
}

void KWinVirtualDisplay::sendPointerAxisDiscrete(uint axis, int steps)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    if (m_eiConnection && m_eiConnection->hasPointer()) {
        m_eiConnection->sendPointerAxisDiscrete(axis, steps);
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "NotifyPointerAxisDiscrete"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options, axis, steps});
    QDBusPendingCall pcall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [axis, steps](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
            qWarning() << "NotifyPointerAxisDiscrete DBus error:" << reply.error().message() << "axis:" << axis << "steps:" << steps;
        }
    });
}

void KWinVirtualDisplay::sendKeyboardKeycode(int keycode, uint state)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    if (m_eiConnection && m_eiConnection->hasKeyboard()) {
        m_eiConnection->sendKeyboardKeycode(keycode, state);
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "NotifyKeyboardKeycode"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options, keycode, state});
    QDBusPendingCall pcall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [keycode, state](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
            qWarning() << "NotifyKeyboardKeycode DBus error:" << reply.error().message() << "keycode:" << keycode << "state:" << state;
        }
    });
}

void KWinVirtualDisplay::sendKeyboardKeysym(int keysym, uint state)
{
    if (m_sessionPath.isEmpty() || !m_displayActive)
        return;

    if (m_eiConnection && m_eiConnection->hasKeyboard()) {
        m_eiConnection->sendKeyboardKeysym(keysym, state);
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        "NotifyKeyboardKeysym"
    );

    QVariantMap options;
    message.setArguments({QDBusObjectPath(m_sessionPath), options, keysym, state});
    QDBusPendingCall pcall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [keysym, state](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
            qWarning() << "NotifyKeyboardKeysym DBus error:" << reply.error().message() << "keysym:" << QString("0x%1").arg(keysym, 0, 16) << "state:" << state;
        }
    });
}

