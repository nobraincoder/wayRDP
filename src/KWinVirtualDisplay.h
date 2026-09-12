#ifndef KWINVIRTUALDISPLAY_H
#define KWINVIRTUALDISPLAY_H

#include <QObject>
#include <QSize>
#include <QString>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QTimer>
#include <chrono>

#include "IVirtualDisplayBackend.h"

struct PortalStream {
    uint32_t id;
    QVariantMap map;
};
Q_DECLARE_METATYPE(PortalStream)

class KWinVirtualDisplay : public IVirtualDisplayBackend
{
    Q_OBJECT
public:
    explicit KWinVirtualDisplay(QObject *parent = nullptr);
    ~KWinVirtualDisplay() override;

    bool createDisplay(const QString& name, const QSize& size, double scale = 1.0) override;
    void destroyDisplay() override;
    bool isDisplayActive() const override { return m_displayActive; }
    QString backendName() const override { return QStringLiteral("KWin"); }

public slots:
    void onClientConnected(const QSize &resolution, double scale) override;
    void onClientDisconnected() override;
    void changeResolution(const QSize &newSize, double scale = 1.0) override;

    void sendPointerMotionAbsolute(double x, double y) override;
    void sendPointerButton(int button, uint state) override;
    void sendPointerAxis(double dx, double dy) override;
    void sendPointerAxisDiscrete(uint axis, int steps) override;
    void sendKeyboardKeycode(int keycode, uint state) override;
    void sendKeyboardKeysym(int keysym, uint state) override;

private slots:
    void onCreateSessionResponse(uint code, const QVariantMap &results);
    void onSelectDevicesResponse(uint code, const QVariantMap &results);
    void onSelectSourcesResponse(uint code, const QVariantMap &results);
    void onStartResponse(uint code, const QVariantMap &results);

private:
    void setupVirtualDisplayResolution();
    void openPipeWireRemote();
    void connectToEis();
    void doSendAxis(double dx, double dy);

    QString m_sessionPath;
    QDBusObjectPath m_currentRequestPath;
    bool m_displayActive;
    bool m_streamOpened{false};
    uint m_streamNodeId;
    QString m_streamMappingId;
    
    // Direct libei input connection
    std::unique_ptr<class EiConnection> m_eiConnection;

    // Requested dimensions
    QSize m_requestedSize;
    double m_requestedScale;
    QString m_displayName;
    QString m_originalPrimaryOutput;
    int m_resolutionRetryCount{0};

    // Sub-pixel accumulator and axis stabilization (D-Bus fallback)
    double m_accumulatedX{0.0};
    double m_accumulatedY{0.0};
    std::chrono::steady_clock::time_point m_lastAxisTime;
};

#endif // KWINVIRTUALDISPLAY_H
