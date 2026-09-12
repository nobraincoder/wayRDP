#include "SessionLifecycleController.h"

SessionLifecycleController::SessionLifecycleController(QObject *parent)
    : QObject(parent)
{
    // Auto-lock on disconnect is configurable via RDP_LOCK_ON_DISCONNECT
    // Enabled by default if set to "1" or "true"
    m_lockOnDisconnect = (qEnvironmentVariable("RDP_LOCK_ON_DISCONNECT") == "1" ||
                          qEnvironmentVariable("RDP_LOCK_ON_DISCONNECT").compare("true", Qt::CaseInsensitive) == 0);
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
    QDBusInterface screenSaver("org.freedesktop.ScreenSaver",
                               "/ScreenSaver",
                               "org.freedesktop.ScreenSaver",
                               QDBusConnection::sessionBus());

    if (screenSaver.isValid()) {
        QDBusReply<void> reply = screenSaver.call("Lock");
        if (reply.isValid()) {
            qInfo() << "SessionLifecycleController: Session locked successfully via KScreenLocker";
        } else {
            qWarning() << "SessionLifecycleController: Failed to lock screen:" << reply.error().message();
        }
    } else {
        qWarning() << "SessionLifecycleController: ScreenSaver D-Bus interface not found";
    }
}
