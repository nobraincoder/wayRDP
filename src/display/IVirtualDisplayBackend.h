#pragma once

#include <QObject>
#include <QSize>
#include <QString>

class IVirtualDisplayBackend : public QObject
{
    Q_OBJECT
public:
    explicit IVirtualDisplayBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~IVirtualDisplayBackend() override = default;

    virtual bool createDisplay(const QString &name, const QSize &size, double scale = 1.0) = 0;
    virtual void destroyDisplay() = 0;
    virtual bool isDisplayActive() const = 0;
    virtual QString backendName() const = 0;

public slots:
    virtual void onClientConnected(const QSize &resolution, double scale) = 0;
    virtual void onClientDisconnected() = 0;
    virtual void changeResolution(const QSize &newSize, double scale = 1.0) = 0;
    virtual void setScreenLocked(bool locked) { Q_UNUSED(locked); }

    virtual void sendPointerMotionAbsolute(double x, double y) = 0;
    virtual void sendPointerButton(int button, uint state) = 0;
    virtual void sendPointerAxis(double dx, double dy) = 0;
    virtual void sendPointerAxisDiscrete(uint axis, int steps) = 0;
    virtual void sendKeyboardKeycode(int keycode, uint state) = 0;
    virtual void sendKeyboardKeysym(int keysym, uint state) = 0;

signals:
    void streamStarted(uint nodeId, int fd, const QSize &size);
    void streamStopped();
};
