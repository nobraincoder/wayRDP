#ifndef RDPSERVER_H
#define RDPSERVER_H

#include <QObject>
#include <QSize>
#include <QImage>
#include <QPoint>
#include <QMutex>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/disp.h>
#include <freerdp/input.h>
#include <freerdp/pointer.h>
#include <freerdp/update.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QFile>
#include <QStringList>
#include <winpr/shell.h>
#include "SystemInputSettings.h"
#include "RdpGfxChannel.h"

struct MyPeerContext {
    rdpContext common;
    class RdpServer* server;
    freerdp_peer* peer;
    HANDLE thread;
    HANDLE vcm;
    DispServerContext* disp;
    bool dispOpened;
    bool activated;
};

class RdpServer : public QObject
{
    Q_OBJECT
public:
    explicit RdpServer(QObject *parent = nullptr);
    ~RdpServer();

    bool start(int port = 3389);
    void stop();

    bool authenticateUser(const QString& username, const QString& password);

public slots:
    void sendVideoFrame(const QByteArray &data, bool isKeyFrame);
    void sendAudioSamples(const QByteArray &data);
    void onHostClipboardChanged(const QString &text);
    void onHostClipboardFilesChanged(const QStringList &filePaths);
    void checkNetworkAdaptation();
    void onKlipperClipboardUpdated();
    void updateCursorShape(const QImage &image, const QPoint &hotspot);
    void resetGraphicsSurface(UINT32 width, UINT32 height);

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
    static BOOL peerExtendedMouseEvent(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
    static BOOL peerKeyboardEvent(rdpInput* input, UINT16 flags, UINT8 code);
    static BOOL peerUnicodeKeyboardEvent(rdpInput* input, UINT16 flags, UINT16 code);

    static BOOL peerSuppressOutput(rdpContext* context, BYTE allow, const RECTANGLE_16* rect);

    static UINT cliprdr_client_capabilities(CliprdrServerContext* context, const CLIPRDR_CAPABILITIES* capabilities);
    static UINT cliprdr_client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* formatList);
    static UINT cliprdr_client_format_list_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse);
    static UINT cliprdr_client_format_data_request(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest);
    static UINT cliprdr_client_format_data_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse);
    static UINT cliprdr_client_file_contents_request(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest);
    static UINT cliprdr_client_file_contents_response(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse);
    static UINT cliprdr_client_lock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_LOCK_CLIPBOARD_DATA* lockClipboardData);
    static UINT cliprdr_client_unlock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlockClipboardData);

    static void rdpsnd_activated(RdpsndServerContext* context);

    void generateCertificate();

    freerdp_listener* m_listener;
    HANDLE m_listenerThread;
    bool m_running;

public:
    RdpGfxChannel m_gfxChannel;
    CliprdrServerContext* m_cliprdrContext;
    RdpsndServerContext* m_rdpsndContext;
    freerdp_peer* m_activePeer;
    QMutex m_peerMutex;
    QMutex m_audioMutex;
    QMutex m_cliprdrMutex;
    UINT16 m_audioTimestamp;
    std::atomic<bool> m_audioReady{false};
    std::atomic<bool> m_cliprdrReady{false};
    std::atomic<uint32_t> m_audioSampleRate{48000};
    std::atomic<uint64_t> m_audioFramesSent{0};
    AUDIO_FORMAT m_pcmFormats[2];
    std::atomic<bool> m_cursorHidden{false};
    std::atomic<double> m_clientScale{1.0};
    std::atomic<uint32_t> m_lastSentFrameId{0};
    std::atomic<uint32_t> m_lastAckedFrameId{0};
    std::atomic<int64_t> m_lastRttMs{0};
    QTimer* m_networkAdaptTimer{nullptr};
    std::atomic<uint32_t> m_currentFps{60};
    std::atomic<int> m_currentQuality{95};
    std::atomic<int64_t> m_smoothedRttMs{0};

    UINT32 m_formatFileGroupDescriptorW{0xC001};
    UINT32 m_formatFileContents{0xC002};
    UINT32 m_clientFileGroupDescriptorFormatId{0};
    QStringList m_outgoingFiles;
    QByteArray m_outgoingFgdData;

    struct IncomingFileTransfer {
        QString fileName;
        QString localPath;
        uint64_t fileSize{0};
        uint64_t receivedBytes{0};
        QFile* localFile{nullptr};
    };
    QList<IncomingFileTransfer> m_incomingFiles;
    uint32_t m_currentIncomingFileIndex{0};
    uint32_t m_fileStreamId{0};
    QStringList m_completedIncomingFilePaths;
    void startNextIncomingFile(CliprdrServerContext* context);

    QString m_lastHostClipboardText;
    struct CursorCacheEntry {
        uint32_t cacheId{0};
        QPoint hotspot;
        QImage image;
        std::chrono::steady_clock::time_point lastUsed;
    };
    QSet<quint32> m_pressedKeys;
    QHash<uint32_t, CursorCacheEntry> m_cursorCache;
    CursorCacheEntry* m_lastUsedCursor{nullptr};
    double m_scrollAccumulatorX{0.0};
    double m_scrollAccumulatorY{0.0};
    SystemInputSettings m_inputSettings;
};

#endif // RDPSERVER_H
