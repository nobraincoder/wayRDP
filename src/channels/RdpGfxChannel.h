#ifndef RDPGFXCHANNEL_H
#define RDPGFXCHANNEL_H

#include <QObject>
#include <QMutex>
#include <QSet>
#include <QByteArray>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/server/rdpgfx.h>
#include <winpr/wtsapi.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

struct QueuedVideoFrame {
    QByteArray data;
    bool isKeyFrame{false};
};

class RdpGfxChannel : public QObject
{
    Q_OBJECT
public:
    explicit RdpGfxChannel(QObject *parent = nullptr);
    ~RdpGfxChannel();

    bool initialize(HANDLE vcm, rdpContext* rdpcontext);
    void close();

    void resetSurface(UINT32 width, UINT32 height);
    void sendFrame(const QByteArray &data, bool isKeyFrame);
    void setOutputSuppressed(bool suppressed);
    void purgeStaleFrames();

    bool isReady() const { return m_gfxReady.load(); }
    bool isOpened() const { return m_gfxOpened.load(); }

    uint32_t lastSentFrameId() const { return m_lastSentFrameId.load(); }
    uint32_t lastAckedFrameId() const { return m_lastAckedFrameId.load(); }
    int64_t lastRttMs() const { return m_lastRttMs.load(); }

    RdpgfxServerContext* context() const { return m_context; }
    void markOpened();

signals:
    void frameAcknowledged(uint32_t frameId, int64_t rttMs);
    void keyFrameNeeded();

private:
    static BOOL channelIdAssignedCallback(RdpgfxServerContext* context, UINT32 channelId);
    static UINT capsAdvertiseCallback(RdpgfxServerContext* context, const RDPGFX_CAPS_ADVERTISE_PDU* capsAdvertise);
    static UINT frameAcknowledgeCallback(RdpgfxServerContext* context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU* frameAcknowledge);
    static UINT qoeFrameAcknowledgeCallback(RdpgfxServerContext* context, const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU* qoeFrameAcknowledge);

    void startSubmissionThread();
    void stopSubmissionThread();
    void submitFrame(const QueuedVideoFrame &frame);
    bool hasInFlightCapacity();

    RdpgfxServerContext* m_context{nullptr};
    mutable QMutex m_mutex;

    std::atomic<bool> m_gfxOpened{false};
    std::atomic<bool> m_gfxReady{false};
    std::atomic<bool> m_outputSuppressed{false};
    std::atomic<bool> m_waitingForKeyFrame{false};
    std::atomic<int> m_droppedFramesWaitingForKey{0};

    std::atomic<uint32_t> m_lastSentFrameId{0};
    std::atomic<uint32_t> m_lastAckedFrameId{0};
    std::atomic<int64_t> m_lastRttMs{0};

    UINT32 m_frameId{0};
    UINT16 m_surfaceId{0};
    UINT32 m_surfaceWidth{0};
    UINT32 m_surfaceHeight{0};
    bool m_hasActiveSurface{false};

    std::deque<QueuedVideoFrame> m_frameQueue;
    std::mutex m_frameQueueMutex;
    std::condition_variable m_frameQueueCond;

    QSet<uint32_t> m_pendingFrames;
    std::deque<std::pair<uint32_t, std::chrono::steady_clock::time_point>> m_pendingFrameTimestamps;
    std::mutex m_pendingFramesMutex;

    std::atomic<bool> m_submissionRunning{false};
    std::thread m_submissionThread;
};

#endif // RDPGFXCHANNEL_H
