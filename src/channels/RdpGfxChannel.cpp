#include "channels/RdpGfxChannel.h"
#include "video/CodecHooks.h"
#include <QDebug>
#include <QDateTime>
#include <winpr/wtsapi.h>

RdpGfxChannel::RdpGfxChannel(QObject *parent)
    : QObject(parent)
{
    startSubmissionThread();
}

RdpGfxChannel::~RdpGfxChannel()
{
    close();
    stopSubmissionThread();
}

bool RdpGfxChannel::initialize(HANDLE vcm, rdpContext* rdpcontext)
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_context && m_context->rdpcontext == rdpcontext && m_gfxReady) {
            qInfo() << "RdpGfxChannel: Already initialized and ready for this peer context, skipping.";
            return true;
        }
    }

    close();

    if (!vcm) {
        qWarning() << "RdpGfxChannel: Cannot initialize without Virtual Channel Manager";
        return false;
    }

    RdpgfxServerContext* gfx = rdpgfx_server_context_new(vcm);
    if (!gfx) {
        qWarning() << "RdpGfxChannel: Failed to create rdpgfx server context";
        return false;
    }

    gfx->rdpcontext = rdpcontext;
    gfx->custom = this;
    gfx->ChannelIdAssigned = channelIdAssignedCallback;
    gfx->CapsAdvertise = capsAdvertiseCallback;
    gfx->FrameAcknowledge = frameAcknowledgeCallback;
    gfx->QoeFrameAcknowledge = qoeFrameAcknowledgeCallback;

    if (!gfx->Initialize(gfx, FALSE)) {
        qWarning() << "RdpGfxChannel: Failed to initialize rdpgfx server context";
        rdpgfx_server_context_free(gfx);
        return false;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_context = gfx;
        m_gfxOpened = false;
        m_gfxReady = false;
        m_surfaceId = 0;
        m_hasActiveSurface = false;
        m_surfaceWidth = 0;
        m_surfaceHeight = 0;
        m_frameId = 0;
        m_lastSentFrameId = 0;
        m_lastAckedFrameId = 0;
        m_lastRttMs = 0;
    }

    return true;
}

void RdpGfxChannel::markOpened()
{
    QMutexLocker locker(&m_mutex);
    if (m_context && !m_gfxOpened) {
        if (m_context->Open(m_context)) {
            m_gfxOpened = true;
            qInfo() << "RdpGfxChannel: RDPGFX dynamic virtual channel opened successfully!";
        } else {
            qWarning() << "RdpGfxChannel: Failed to open RDPGFX dynamic virtual channel!";
        }
    }
}

void RdpGfxChannel::close()
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_context) {
            if (m_gfxOpened) {
                m_context->Close(m_context);
            }
            rdpgfx_server_context_free(m_context);
            m_context = nullptr;
        }
        m_gfxOpened = false;
        m_gfxReady = false;
        m_surfaceId = 0;
        m_hasActiveSurface = false;
        m_surfaceWidth = 0;
        m_surfaceHeight = 0;
    }

    {
        std::lock_guard<std::mutex> lock(m_frameQueueMutex);
        m_frameQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_pendingFramesMutex);
        m_pendingFrames.clear();
        m_pendingFrameTimestamps.clear();
    }
    m_frameQueueCond.notify_all();

    m_waitingForKeyFrame = false;
    m_droppedFramesWaitingForKey = 0;
}

void RdpGfxChannel::setOutputSuppressed(bool suppressed)
{
    m_outputSuppressed = suppressed;
    if (suppressed) {
        std::lock_guard<std::mutex> lock(m_frameQueueMutex);
        m_frameQueue.clear();
    }
}

void RdpGfxChannel::purgeStaleFrames()
{
    {
        std::lock_guard<std::mutex> lock(m_frameQueueMutex);
        m_frameQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_pendingFramesMutex);
        m_pendingFrames.clear();
        m_pendingFrameTimestamps.clear();
    }
    m_waitingForKeyFrame = true;
    CodecHooks_requestKeyframe();
    m_frameQueueCond.notify_all();
    qInfo() << "RdpGfxChannel: Purged stale frame queue, waiting for clean IDR keyframe";
}

void RdpGfxChannel::resetSurface(UINT32 width, UINT32 height)
{
    QMutexLocker locker(&m_mutex);
    if (!m_context || !m_gfxOpened) return;

    if (m_surfaceWidth == width && m_surfaceHeight == height && m_hasActiveSurface) {
        qInfo() << "RdpGfxChannel: Surface already configured at" << width << "x" << height << ", skipping duplicate reset";
        return;
    }

    m_gfxReady = false;
    qInfo() << "RdpGfxChannel: Resetting surface to" << width << "x" << height << "(active surface:" << m_hasActiveSurface << ")";

    if (m_hasActiveSurface) {
        RDPGFX_DELETE_SURFACE_PDU deletePdu;
        deletePdu.surfaceId = m_surfaceId;
        m_context->DeleteSurface(m_context, &deletePdu);
        m_hasActiveSurface = false;
    }

    MONITOR_DEF monitorDef;
    monitorDef.left = 0;
    monitorDef.top = 0;
    monitorDef.right = width;
    monitorDef.bottom = height;
    monitorDef.flags = 1;

    RDPGFX_RESET_GRAPHICS_PDU resetPdu;
    resetPdu.width = width;
    resetPdu.height = height;
    resetPdu.monitorCount = 1;
    resetPdu.monitorDefArray = &monitorDef;

    UINT result = m_context->ResetGraphics(m_context, &resetPdu);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: ResetGraphics failed with error:" << result;
        return;
    }

    m_surfaceId = 0;
    RDPGFX_CREATE_SURFACE_PDU createPdu;
    createPdu.surfaceId = m_surfaceId;
    createPdu.width = width;
    createPdu.height = height;
    createPdu.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;

    result = m_context->CreateSurface(m_context, &createPdu);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: CreateSurface failed with error:" << result;
        return;
    }

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapPdu;
    mapPdu.surfaceId = m_surfaceId;
    mapPdu.reserved = 0;
    mapPdu.outputOriginX = 0;
    mapPdu.outputOriginY = 0;

    result = m_context->MapSurfaceToOutput(m_context, &mapPdu);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: MapSurfaceToOutput failed with error:" << result;
        return;
    }

    m_hasActiveSurface = true;
    m_surfaceWidth = width;
    m_surfaceHeight = height;
    m_gfxReady = true;
    m_waitingForKeyFrame = true;
    m_droppedFramesWaitingForKey = 0;
    CodecHooks_requestKeyframe();

    qInfo() << "RdpGfxChannel: Surface configured successfully for" << width << "x" << height;
}

void RdpGfxChannel::sendFrame(const QByteArray &data, bool isKeyFrame)
{
    if (data.isEmpty() || !m_gfxReady.load() || m_outputSuppressed.load()) {
        return;
    }

    if (m_waitingForKeyFrame) {
        if (!isKeyFrame) {
            return;
        }
        m_waitingForKeyFrame = false;
        qInfo() << "RdpGfxChannel: Received clean IDR keyframe after surface reset! Resuming video output.";
    }

    {
        std::lock_guard<std::mutex> lock(m_frameQueueMutex);

        // Hot-path optimization: identical back-to-back frames are a common artifact when the
        // desktop is static or the encoder is briefly repeating the same frame. Dropping those
        // duplicates reduces queue pressure without affecting correctness.
        if (!m_frameQueue.empty()) {
            const QueuedVideoFrame &last = m_frameQueue.back();
            if (last.data == data && last.isKeyFrame == isKeyFrame) {
                return;
            }
        }

        if (m_frameQueue.size() > 60) {
            // NEVER silently drop a delta P-frame without waiting for an IDR keyframe!
            // In H.264, dropping a delta frame breaks the reference chain in mstsc,
            // permanently freezing the client video stream until an IDR frame arrives.
            m_frameQueue.clear();
            m_waitingForKeyFrame = true;
            CodecHooks_requestKeyframe();
            qWarning() << "RdpGfxChannel: Frame queue overflow (" << m_frameQueue.size()
                       << "frames), cleared queue and requested IDR keyframe to resync stream";
            return;
        }
        m_frameQueue.push_back({data, isKeyFrame});
    }
    m_frameQueueCond.notify_one();
}

void RdpGfxChannel::startSubmissionThread()
{
    if (m_submissionRunning.exchange(true)) {
        return;
    }
    m_submissionThread = std::thread([this]() {
        while (m_submissionRunning) {
            QueuedVideoFrame frame;
            {
                std::unique_lock<std::mutex> lock(m_frameQueueMutex);
                m_frameQueueCond.wait(lock, [this]() {
                    return !m_submissionRunning || !m_frameQueue.empty();
                });

                if (!m_submissionRunning) {
                    break;
                }

                if (!hasInFlightCapacity()) {
                    m_frameQueueCond.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                        return !m_submissionRunning || hasInFlightCapacity();
                    });
                    if (!m_submissionRunning) {
                        break;
                    }
                    if (!hasInFlightCapacity()) {
                        continue;
                    }
                }

                if (m_frameQueue.empty() || !m_gfxReady || m_outputSuppressed) {
                    continue;
                }

                frame = std::move(m_frameQueue.front());
                m_frameQueue.pop_front();
            }

            submitFrame(frame);
        }
    });
}

void RdpGfxChannel::stopSubmissionThread()
{
    if (!m_submissionRunning.exchange(false)) {
        return;
    }
    m_frameQueueCond.notify_all();
    if (m_submissionThread.joinable()) {
        m_submissionThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_frameQueueMutex);
        m_frameQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_pendingFramesMutex);
        m_pendingFrames.clear();
        m_pendingFrameTimestamps.clear();
    }
}

bool RdpGfxChannel::hasInFlightCapacity()
{
    std::lock_guard<std::mutex> lock(m_pendingFramesMutex);
    // To sustain true 60 FPS streaming, the in-flight frame window must accommodate
    // client decode, presentation, and ACK batching latency (typically 40-80ms on macOS).
    // Allowing 16-24 frames (266-400ms at 60 FPS) ensures the submission thread never
    // stalls or artificially caps throughput to ~30 FPS on high-performance clients.
    int64_t rtt = m_lastRttMs.load();
    size_t maxInFlight = 16;
    if (rtt > 50) {
        maxInFlight = 20;
    } else if (rtt > 100) {
        maxInFlight = 24;
    }

    if (m_pendingFrames.size() < maxInFlight) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    int64_t timeoutMs = std::max<int64_t>(120, rtt * 2);
    if (!m_pendingFrameTimestamps.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_pendingFrameTimestamps.front().second).count() > timeoutMs) {
        m_pendingFrames.clear();
        m_pendingFrameTimestamps.clear();
        return true;
    }
    return false;
}

void RdpGfxChannel::submitFrame(const QueuedVideoFrame &frame)
{
    QMutexLocker locker(&m_mutex);
    if (!m_context || !m_gfxReady || m_outputSuppressed) {
        return;
    }

    rdpContext* rdpctx = m_context->rdpcontext;
    if (!rdpctx || !rdpctx->peer || !rdpctx->peer->context || !rdpctx->peer->context->settings) {
        return;
    }
    freerdp_peer* peer = rdpctx->peer;
    rdpSettings* settings = peer->context->settings;
    UINT32 width = (m_surfaceWidth > 0) ? m_surfaceWidth : freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    UINT32 height = (m_surfaceHeight > 0) ? m_surfaceHeight : freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

    const auto now = QDateTime::currentDateTimeUtc().time();
    UINT32 timestamp = (now.hour() << 22) | (now.minute() << 16) | (now.second() << 10) | now.msec();

    const auto frameId = m_frameId++;
    m_lastSentFrameId = frameId;
    {
        std::lock_guard<std::mutex> lock(m_pendingFramesMutex);
        m_pendingFrames.insert(frameId);
        m_pendingFrameTimestamps.push_back({frameId, std::chrono::steady_clock::now()});
    }

    RDPGFX_START_FRAME_PDU startFrame;
    startFrame.timestamp = timestamp;
    startFrame.frameId = frameId;

    if (startFrame.frameId == 0 || frame.isKeyFrame) {
        qInfo() << "RdpGfxChannel: Transmitting" << (frame.isKeyFrame ? "keyframe" : "frame")
                << "id:" << startFrame.frameId << "size:" << frame.data.size() << "bytes";
    }

    RDPGFX_END_FRAME_PDU endFrame;
    endFrame.frameId = startFrame.frameId;

    RECTANGLE_16 regionRect;
    regionRect.left = 0;
    regionRect.top = 0;
    regionRect.right = width;
    regionRect.bottom = height;

    RDPGFX_H264_QUANT_QUALITY quantQuality;
    quantQuality.qpVal = 0;
    quantQuality.qualityVal = 100;
    quantQuality.qp = 20;
    quantQuality.r = 0;
    quantQuality.p = 0;

    RDPGFX_AVC420_BITMAP_STREAM avc420;
    memset(&avc420, 0, sizeof(avc420));
    avc420.meta.numRegionRects = 1;
    avc420.meta.regionRects = &regionRect;
    avc420.meta.quantQualityVals = &quantQuality;
    avc420.length = frame.data.size();
    avc420.data = reinterpret_cast<BYTE*>(const_cast<char*>(frame.data.data()));

    RDPGFX_SURFACE_COMMAND cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.surfaceId = m_surfaceId;
    cmd.codecId = RDPGFX_CODECID_AVC420;
    cmd.contextId = 0;
    cmd.format = PIXEL_FORMAT_BGRX32;
    cmd.left = 0;
    cmd.top = 0;
    cmd.right = width;
    cmd.bottom = height;
    cmd.width = width;
    cmd.height = height;
    cmd.length = 0;
    cmd.data = nullptr;
    cmd.extra = &avc420;

    UINT result = m_context->StartFrame(m_context, &startFrame);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: StartFrame failed with error:" << result;
        std::lock_guard<std::mutex> lock(m_pendingFramesMutex);
        m_pendingFrames.remove(frameId);
        return;
    }
    result = m_context->SurfaceCommand(m_context, &cmd);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: SurfaceCommand failed with error:" << result;
    }
    result = m_context->EndFrame(m_context, &endFrame);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: EndFrame failed with error:" << result;
    }
}

BOOL RdpGfxChannel::channelIdAssignedCallback(RdpgfxServerContext* context, UINT32 channelId)
{
    Q_UNUSED(context);
    qInfo() << "RdpGfxChannel: ChannelIdAssigned callback received with channelId:" << channelId;
    return TRUE;
}

UINT RdpGfxChannel::capsAdvertiseCallback(RdpgfxServerContext* context, const RDPGFX_CAPS_ADVERTISE_PDU* capsAdvertise)
{
    qInfo() << "RdpGfxChannel: CapsAdvertise received from client";
    if (!context || !context->rdpcontext) return CHANNEL_RC_OK;

    RdpGfxChannel* channel = static_cast<RdpGfxChannel*>(context->custom);
    rdpContext* rdpctx = context->rdpcontext;
    freerdp_peer* peer = rdpctx->peer;
    rdpSettings* settings = peer->context->settings;
    UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

    UINT32 selectedVersion = 0;
    UINT32 selectedFlags = 0;
    int bestPriority = -1;

    for (UINT16 i = 0; i < capsAdvertise->capsSetCount; i++) {
        const RDPGFX_CAPSET* capsSet = &capsAdvertise->capsSets[i];

        qInfo() << "RdpGfxChannel: Client advertised capSet[" << i << "] version:"
                << QString("0x%1").arg(capsSet->version, 8, 16, QChar('0'))
                << "flags:" << QString("0x%1").arg(capsSet->flags, 8, 16, QChar('0'));

        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) {
            continue;
        }

        int priority = -1;
        switch (capsSet->version) {
            case RDPGFX_CAPVERSION_107:
                // Cap at 10.6 for AVC420: 10.7 requires AVC444v2 which decoders reject when sent AVC420
                continue;
            case RDPGFX_CAPVERSION_106:
            case RDPGFX_CAPVERSION_106_ERR:
                priority = 1006;
                break;
            case RDPGFX_CAPVERSION_105:
                priority = 1005;
                break;
            case RDPGFX_CAPVERSION_104:
                priority = 1004;
                break;
            case RDPGFX_CAPVERSION_103:
                priority = 1003;
                break;
            case RDPGFX_CAPVERSION_102:
                priority = 1002;
                break;
            case RDPGFX_CAPVERSION_101:
                priority = 1001;
                break;
            case RDPGFX_CAPVERSION_10:
                priority = 1000;
                break;
            case RDPGFX_CAPVERSION_81:
                if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
                    priority = 810;
                }
                break;
            default:
                break;
        }

        if (priority > bestPriority) {
            bestPriority = priority;
            selectedVersion = capsSet->version;
            selectedFlags = capsSet->flags;
        }
    }

    if (selectedVersion == 0) {
        qWarning() << "RdpGfxChannel: Client does not support any AVC420 / H.264 profile!";
        if (capsAdvertise->capsSetCount > 0) {
            selectedVersion = capsAdvertise->capsSets[0].version;
            selectedFlags = capsAdvertise->capsSets[0].flags;
        }
    }

    RDPGFX_CAPS_CONFIRM_PDU confirm;
    RDPGFX_CAPSET confirmCapsSet;
    confirm.capsSet = &confirmCapsSet;
    confirmCapsSet.version = selectedVersion;
    confirmCapsSet.length = 4;
    confirmCapsSet.flags = selectedFlags;

    qInfo() << "RdpGfxChannel: Confirming capability version:" << QString("0x%1").arg(selectedVersion, 8, 16, QChar('0'))
            << "flags:" << selectedFlags;

    UINT result = context->CapsConfirm(context, &confirm);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: CapsConfirm failed with error:" << result;
        return result;
    }

    MONITOR_DEF monitorDef;
    monitorDef.left = 0;
    monitorDef.top = 0;
    monitorDef.right = width;
    monitorDef.bottom = height;
    monitorDef.flags = 1;

    RDPGFX_RESET_GRAPHICS_PDU resetPdu;
    resetPdu.width = width;
    resetPdu.height = height;
    resetPdu.monitorCount = 1;
    resetPdu.monitorDefArray = &monitorDef;

    result = context->ResetGraphics(context, &resetPdu);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: ResetGraphics failed with error:" << result;
        return result;
    }

    if (channel) {
        channel->m_surfaceId = 0;
    }
    RDPGFX_CREATE_SURFACE_PDU createPdu;
    createPdu.surfaceId = 0;
    createPdu.width = width;
    createPdu.height = height;
    createPdu.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;

    result = context->CreateSurface(context, &createPdu);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: CreateSurface failed with error:" << result;
        return result;
    }

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapPdu;
    mapPdu.surfaceId = 0;
    mapPdu.reserved = 0;
    mapPdu.outputOriginX = 0;
    mapPdu.outputOriginY = 0;

    result = context->MapSurfaceToOutput(context, &mapPdu);
    if (result != CHANNEL_RC_OK) {
        qWarning() << "RdpGfxChannel: MapSurfaceToOutput failed with error:" << result;
        return result;
    }

    if (channel) {
        channel->m_hasActiveSurface = true;
        channel->m_surfaceWidth = width;
        channel->m_surfaceHeight = height;
        channel->m_gfxReady = true;
    }

    qInfo() << "RdpGfxChannel: Pipeline initialized successfully for resolution:" << width << "x" << height;
    return CHANNEL_RC_OK;
}

UINT RdpGfxChannel::frameAcknowledgeCallback(RdpgfxServerContext* context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU* frameAcknowledge)
{
    if (!context || !context->custom) return CHANNEL_RC_OK;
    RdpGfxChannel* channel = static_cast<RdpGfxChannel*>(context->custom);

    int64_t rttMs = 0;
    channel->m_lastAckedFrameId = frameAcknowledge->frameId;
    {
        std::lock_guard<std::mutex> lock(channel->m_pendingFramesMutex);
        // Cumulative ACK: prune all pending frame IDs <= acknowledged frameId (MS-RDPEGFX 3.2.5.1)
        auto it = channel->m_pendingFrames.begin();
        while (it != channel->m_pendingFrames.end()) {
            if (*it <= frameAcknowledge->frameId) {
                it = channel->m_pendingFrames.erase(it);
            } else {
                ++it;
            }
        }
        while (!channel->m_pendingFrameTimestamps.empty() &&
               channel->m_pendingFrameTimestamps.front().first <= frameAcknowledge->frameId) {
            auto sentTime = channel->m_pendingFrameTimestamps.front().second;
            auto now = std::chrono::steady_clock::now();
            rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - sentTime).count();
            channel->m_lastRttMs = rttMs;
            channel->m_pendingFrameTimestamps.pop_front();
        }
    }
    channel->m_frameQueueCond.notify_one();
    emit channel->frameAcknowledged(frameAcknowledge->frameId, rttMs);
    return CHANNEL_RC_OK;
}

UINT RdpGfxChannel::qoeFrameAcknowledgeCallback(RdpgfxServerContext* context, const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU* qoeFrameAcknowledge)
{
    if (!context || !context->custom) return CHANNEL_RC_OK;
    RdpGfxChannel* channel = static_cast<RdpGfxChannel*>(context->custom);

    int64_t rttMs = 0;
    channel->m_lastAckedFrameId = qoeFrameAcknowledge->frameId;
    {
        std::lock_guard<std::mutex> lock(channel->m_pendingFramesMutex);
        // Cumulative ACK: prune all pending frame IDs <= acknowledged frameId (MS-RDPEGFX 3.2.5.1)
        auto it = channel->m_pendingFrames.begin();
        while (it != channel->m_pendingFrames.end()) {
            if (*it <= qoeFrameAcknowledge->frameId) {
                it = channel->m_pendingFrames.erase(it);
            } else {
                ++it;
            }
        }
        while (!channel->m_pendingFrameTimestamps.empty() &&
               channel->m_pendingFrameTimestamps.front().first <= qoeFrameAcknowledge->frameId) {
            auto sentTime = channel->m_pendingFrameTimestamps.front().second;
            auto now = std::chrono::steady_clock::now();
            rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - sentTime).count();
            channel->m_lastRttMs = rttMs;
            channel->m_pendingFrameTimestamps.pop_front();
        }
    }
    channel->m_frameQueueCond.notify_one();
    emit channel->frameAcknowledged(qoeFrameAcknowledge->frameId, rttMs);
    return CHANNEL_RC_OK;
}
