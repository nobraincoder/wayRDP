#include "SessionLifecycleController.h"
#include <QProcess>

SessionLifecycleController::SessionLifecycleController(QObject *parent)
    : QObject(parent)
{
    // Auto-lock on disconnect: Enabled by default for security, can be disabled via RDP_LOCK_ON_DISCONNECT=0/false
    QString envLock = qEnvironmentVariable("RDP_LOCK_ON_DISCONNECT").trimmed().toLower();
    if (envLock == "0" || envLock == "false" || envLock == "no" || envLock == "off") {
        m_lockOnDisconnect = false;
        qInfo() << "SessionLifecycleController: Lock on disconnect disabled via RDP_LOCK_ON_DISCONNECT";
    } else {
        m_lockOnDisconnect = true;
        qInfo() << "SessionLifecycleController: Lock on disconnect enabled by default";
    }
}

SessionLifecycleController::~SessionLifecycleController()
{
    releaseSleepInhibit();
}

void SessionLifecycleController::onClientConnected()
{
    inhibitSleep();
}

void SessionLifecycleController::onClientDisconnected()
{
    releaseSleepInhibit();

    if (m_lockOnDisconnect) {
        lockScreen();
    }
}

void SessionLifecycleController::inhibitSleep()
{
    if (m_powerInhibitCookie != 0) {
        return; // Already inhibited
    }

    QDBusInterface pm("org.freedesktop.PowerManagement.Inhibit",
                      "/org/freedesktop/PowerManagement/Inhibit",
                      "org.freedesktop.PowerManagement.Inhibit",
                      QDBusConnection::sessionBus());

    if (!pm.isValid()) {
        qWarning() << "SessionLifecycleController: PowerManagement.Inhibit D-Bus interface not available";
        return;
    }

    QDBusReply<uint> reply = pm.call("Inhibit", "wayRDP", "Active remote desktop connection");
    if (reply.isValid()) {
        m_powerInhibitCookie = reply.value();
        qInfo() << "SessionLifecycleController: System sleep inhibited during active session (cookie:" << m_powerInhibitCookie << ")";
    } else {
        qWarning() << "SessionLifecycleController: Failed to inhibit sleep:" << reply.error().message();
    }
}

void SessionLifecycleController::releaseSleepInhibit()
{
    if (m_powerInhibitCookie == 0) {
        return;
    }

    QDBusInterface pm("org.freedesktop.PowerManagement.Inhibit",
                      "/org/freedesktop/PowerManagement/Inhibit",
                      "org.freedesktop.PowerManagement.Inhibit",
                      QDBusConnection::sessionBus());

    if (pm.isValid()) {
        pm.call("UnInhibit", m_powerInhibitCookie);
        qInfo() << "SessionLifecycleController: System sleep inhibition released (cookie:" << m_powerInhibitCookie << ")";
    }
    m_powerInhibitCookie = 0;
}

void SessionLifecycleController::lockScreen()
{
    qInfo() << "SessionLifecycleController: Locking session following RDP client disconnection...";
    bool locked = false;

    QDBusInterface screenSaver("org.freedesktop.ScreenSaver",
                               "/ScreenSaver",
                               "org.freedesktop.ScreenSaver",
                               QDBusConnection::sessionBus());

    if (screenSaver.isValid()) {
        QDBusReply<void> reply = screenSaver.call("Lock");
        if (reply.isValid()) {
            qInfo() << "SessionLifecycleController: Session locked successfully via KScreenLocker";
            locked = true;
        } else {
            qWarning() << "SessionLifecycleController: Failed to lock screen via DBus:" << reply.error().message();
        }
    } else {
        qWarning() << "SessionLifecycleController: ScreenSaver D-Bus interface not found";
    }

    if (!locked) {
        qInfo() << "SessionLifecycleController: Invoking loginctl lock-session fallback...";
        QProcess::startDetached("loginctl", QStringList() << "lock-session");
    }
}
