#ifndef RDPSERVER_H
#define RDPSERVER_H

#include <QObject>
#include <QSize>
#include <QImage>
#include <QPoint>
#include <QMutex>
#include <QSet>
#include <QTimer>
#include <QStringList>
#include <atomic>
#include <chrono>

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/disp.h>
#include <freerdp/input.h>
#include <freerdp/update.h>

#include "core/SystemInputSettings.h"
#include "core/AuthManager.h"
#include "core/RdpCursorManager.h"
#include "channels/RdpGfxChannel.h"
#include "channels/RdpCliprdrChannel.h"

struct MyPeerContext {
    rdpContext common;
    class RdpServer* server;
    freerdp_peer* peer;
    HANDLE thread;
    HANDLE vcm;
    DispServerContext* disp;
    bool dispOpened;
    bool activated;
    bool authenticated{false};
    bool isWindowsClient{false};
};

class RdpServer : public QObject
{
    Q_OBJECT
public:
    explicit RdpServer(QObject *parent = nullptr);
    ~RdpServer() override;

    bool start(int port = 3389);
    void stop();

    bool authenticateUser(const QString& username, const QString& password);

public slots:
    void sendVideoFrame(const QByteArray &data, bool isKeyFrame);
    void sendAudioSamples(const QByteArray &data);
    void onHostClipboardChanged(const QString &text);
    void onHostClipboardFilesChanged(const QStringList &filePaths);
    void onKlipperClipboardHistoryUpdated();
    void checkNetworkAdaptation();
    void updateCursorShape(const QImage &image, const QPoint &hotspot);
    void resetGraphicsSurface(UINT32 width, UINT32 height);
    void purgeStaleFrames();

signals:
    void clientConnected(const QSize &resolution, double scale);
    void clientDisconnected();
    void clientEncodingConfigured(uint32_t fps, int quality);
    void requestedResolutionChanged(const QSize &resolution, double scale);

    void pointerMotionAbsolute(double x, double y);
    void pointerButton(int button, uint state);
    void pointerAxis(double dx, double dy);
    void pointerAxisDiscrete(uint axis, int steps);
    void keyboardKeycode(int keycode, uint state);
    void keyboardKeysym(int keysym, uint state);

    void clientClipboardReceived(const QString &text);
    void clientFilesReceived(const QStringList &filePaths);
    void audioConfigured(uint32_t sampleRate);
    void clientActivity();

private:
    static BOOL peerAccepted(freerdp_listener* listener, freerdp_peer* peer);
    static BOOL peerContextNew(freerdp_peer* peer, rdpContext* context);
    static void peerContextFree(freerdp_peer* peer, rdpContext* context);

    static DWORD WINAPI listenerThread(LPVOID param);
    static DWORD WINAPI peerThread(LPVOID param);

    static BOOL peerLogon(freerdp_peer* peer, const SEC_WINNT_AUTH_IDENTITY* identity, BOOL automatic);
    static BOOL peerActivate(freerdp_peer* peer);
    static BOOL peerCapabilities(freerdp_peer* peer);
    static BOOL peerPostConnect(freerdp_peer* peer);

    static BOOL peerSynchronizeEvent(rdpInput* input, UINT32 flags);
    static BOOL peerMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
    static BOOL peerRelMouseEvent(rdpInput* input, UINT16 flags, INT16 xDelta, INT16 yDelta);
    static BOOL peerExtendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
    static BOOL peerKeyboardEvent(rdpInput* input, UINT16 flags, UINT8 code);
    static BOOL peerUnicodeKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code);

    static BOOL peerSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* rect);

    static void rdpsnd_activated(RdpsndServerContext* context);
    static UINT rdpsnd_confirm_block(RdpsndServerContext* context, BYTE confirmBlockNum, UINT16 wtimestamp);

    QString m_samFilePath;
    freerdp_listener* m_listener{nullptr};
    HANDLE m_listenerThread{nullptr};
    bool m_running{false};
    std::atomic<bool> m_peerStopRequested{false};

public:
    RdpGfxChannel m_gfxChannel;
    RdpCliprdrChannel m_cliprdrChannel;
    RdpCursorManager m_cursorManager;

    RdpsndServerContext* m_rdpsndContext{nullptr};
    freerdp_peer* m_activePeer{nullptr};
    QMutex m_peerMutex;
    QMutex m_audioMutex;
    UINT16 m_audioTimestamp{0};
    std::atomic<bool> m_audioReady{false};
    std::atomic<bool> m_clientConfirmsBlocks{false};
    std::atomic<uint8_t> m_lastConfirmedBlock{0};
    int16_t m_lastLeftSample{0};
    int16_t m_lastRightSample{0};
    bool m_audioDroppedPrevious{false};
    std::atomic<uint32_t> m_audioSampleRate{48000};
    std::atomic<uint64_t> m_audioFramesSent{0};
    AUDIO_FORMAT m_pcmFormats[2];
    std::atomic<double> m_clientScale{1.0};
    std::atomic<uint32_t> m_lastSentFrameId{0};
    std::atomic<uint32_t> m_lastAckedFrameId{0};
    std::atomic<int64_t> m_lastRttMs{0};
    QTimer* m_networkAdaptTimer{nullptr};
    std::atomic<uint32_t> m_currentFps{60};
    std::atomic<uint32_t> m_maxTargetFps{60};
    std::atomic<int> m_currentQuality{95};
    std::atomic<int64_t> m_smoothedRttMs{0};

    QSet<quint32> m_pressedKeys;
    QMutex m_pressedKeysMutex;
    double m_scrollAccumulatorX{0.0};
    double m_scrollAccumulatorY{0.0};
    SystemInputSettings m_inputSettings;
};

#endif // RDPSERVER_H
