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

public slots:
    void onClientConnected();
    void onClientDisconnected();

private:
    void inhibitSleep();
    void releaseSleepInhibit();
    void lockScreen();

    uint32_t m_powerInhibitCookie{0};
    bool m_lockOnDisconnect{false};
};
