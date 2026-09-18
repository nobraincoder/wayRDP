#pragma once

#include <QObject>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>

class SessionLifecycleController : public QObject
{
    Q_OBJECT
public:
    explicit SessionLifecycleController(QObject *parent = nullptr);
    ~SessionLifecycleController() override;

    bool isScreenLocked() const { return m_isScreenLocked; }

public slots:
    void onClientConnected();
    void onClientDisconnected();

signals:
    void screenLockChanged(bool locked);
    void sessionLocked();
    void sessionUnlocked();

private slots:
    void onScreenSaverActiveChanged(bool active);

private:
    void setupScreenSaverListener();
    void inhibitSleep();
    void releaseSleepInhibit();
    void lockScreen();

    uint32_t m_powerInhibitCookie{0};
    bool m_lockOnDisconnect{false};
    bool m_isScreenLocked{false};
};
