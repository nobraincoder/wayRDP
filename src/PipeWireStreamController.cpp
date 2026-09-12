#include "PipeWireStreamController.h"
#include <QDebug>

PipeWireStreamController::PipeWireStreamController(QObject *parent)
    : QObject(parent), m_stream(nullptr), m_framerate(60), m_quality(80), m_fpsFrameCount(0), m_lastFpsLogTime(0)
{
    if (qEnvironmentVariableIsSet("RDP_QUALITY")) {
        bool ok = false;
        int envQuality = qEnvironmentVariableIntValue("RDP_QUALITY", &ok);
        if (ok && envQuality >= 30 && envQuality <= 100) {
            m_quality = envQuality;
        }
    }

    if (qEnvironmentVariableIsSet("RDP_IDLE_TIMEOUT_SEC")) {
        bool ok = false;
        int sec = qEnvironmentVariableIntValue("RDP_IDLE_TIMEOUT_SEC", &ok);
        if (ok && sec > 0) {
            m_idleTimeoutMs = sec * 1000;
        } else if (ok && sec == 0) {
            m_idleTimeoutMs = 0; // Disabled
        }
    }

    m_activeFramerate = m_framerate;
    m_lastActivityTimer.start();

    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(2000);
    connect(m_idleTimer, &QTimer::timeout, this, &PipeWireStreamController::checkIdleTimeout);
}

PipeWireStreamController::~PipeWireStreamController()
{
    onStreamStopped();
}

void PipeWireStreamController::setEncodingParameters(uint32_t fps, int quality)
{
    m_activeFramerate = fps;
    if (!m_isIdle) {
        m_framerate = fps;
    }
    if (qEnvironmentVariableIsSet("RDP_QUALITY")) {
        bool ok = false;
        int envQuality = qEnvironmentVariableIntValue("RDP_QUALITY", &ok);
        if (ok && envQuality >= 30 && envQuality <= 100) {
            m_quality = envQuality;
        } else {
            m_quality = quality;
        }
    } else {
        m_quality = quality;
    }
    qInfo() << "PipeWireStreamController: Setting encoding parameters to" << fps << "fps, quality" << m_quality;
    if (m_stream) {
        if (!m_isIdle) {
            m_stream->setMaxFramerate(fps);
        }
        m_stream->setQuality(m_quality);
    }
}

void PipeWireStreamController::setMaxFramerate(uint32_t fps)
{
    m_activeFramerate = fps;
    if (!m_isIdle) {
        m_framerate = fps;
        if (m_stream) {
            m_stream->setMaxFramerate(fps);
        }
    }
}

void PipeWireStreamController::setQuality(int quality)
{
    m_quality = quality;
    if (m_stream) {
        m_stream->setQuality(quality);
    }
}

void PipeWireStreamController::setTargetResolution(const QSize &size)
{
    m_targetResolution = size;
}

void PipeWireStreamController::onStreamStarted(uint nodeId, int fd, const QSize &size)
{
    qInfo() << "PipeWireStreamController: Starting encoding for nodeId:" << nodeId << "fd:" << fd << "size:" << size
            << "with fps:" << m_framerate << "quality:" << m_quality;

    m_fpsTimer.restart();
    m_fpsFrameCount = 0;
    m_lastFpsLogTime = 0;

    m_targetResolution = size;
    m_currentStreamResolution = QSize();

    m_lastActivityTimer.restart();
    m_isIdle = false;
    if (m_idleTimeoutMs > 0 && m_idleTimer) {
        m_idleTimer->start();
    }

    if (!m_stream) {
        m_stream = new PipeWireEncodedStream(this);
        connect(m_stream, &PipeWireEncodedStream::sizeChanged, this, &PipeWireStreamController::onStreamSizeChanged);
        connect(m_stream, &PipeWireEncodedStream::newPacket, this, &PipeWireStreamController::onNewPacket);
        connect(m_stream, &PipeWireEncodedStream::cursorChanged, this, &PipeWireStreamController::onCursorChanged);
        connect(m_stream, &PipeWireEncodedStream::errorFound, this, &PipeWireStreamController::onErrorFound);
    } else {
        m_stream->stop();
    }

    // Configurable codec profile: default is H264Baseline (matches KRdp default for immediate zero-latency decoding)
    QString codec = qEnvironmentVariable("RDP_CODEC").trimmed().toLower();
    if (codec == "h264_main" || codec == "main") {
        m_stream->setEncoder(PipeWireBaseEncodedStream::H264Main);
        qInfo() << "PipeWireStreamController: Using H264Main profile";
    } else if (codec == "vp8") {
        m_stream->setEncoder(PipeWireBaseEncodedStream::VP8);
        qInfo() << "PipeWireStreamController: Using VP8 codec";
    } else if (codec == "vp9") {
        m_stream->setEncoder(PipeWireBaseEncodedStream::VP9);
        qInfo() << "PipeWireStreamController: Using VP9 codec";
    } else {
        m_stream->setEncoder(PipeWireBaseEncodedStream::H264Baseline);
        qInfo() << "PipeWireStreamController: Using H264Baseline profile for minimal latency and universal compatibility";
    }

    // Encoding preference: default MUST be Speed (sets async_depth=1 in VAAPI and zerolatency in x264),
    // otherwise async_depth > 1 leaves the final frame of closing windows queued indefinitely in the GPU!
    QString pref = qEnvironmentVariable("RDP_ENCODER_PREFERENCE").trimmed().toLower();
    if (pref == "quality") {
        m_stream->setEncodingPreference(PipeWireBaseEncodedStream::Quality);
        qInfo() << "PipeWireStreamController: Using Quality encoding preference";
    } else if (pref == "size") {
        m_stream->setEncodingPreference(PipeWireBaseEncodedStream::Size);
        qInfo() << "PipeWireStreamController: Using Size encoding preference";
    } else {
        m_stream->setEncodingPreference(PipeWireBaseEncodedStream::Speed);
        qInfo() << "PipeWireStreamController: Using Speed encoding preference (async_depth=1 / zerolatency) to eliminate trailing frame ghosting";
    }

    m_stream->setColorRange(PipeWireBaseEncodedStream::ColorRange::Full);
    m_stream->setMaxFramerate(m_framerate);
    m_stream->setQuality(m_quality);
    m_stream->setMaxPendingFrames(25);

    const auto suggested = m_stream->suggestedEncoders();
    qInfo() << "PipeWireStreamController: Video encoder configured with quality:" << m_quality << "% | Suggested encoders:" << suggested;

    // Set properties
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    m_stream->setNodeId(nodeId);
#pragma GCC diagnostic pop
    m_stream->setFd(fd); // Ownership of fd is transferred to PipeWireEncodedStream and it will close it!

    qInfo() << "PipeWireStreamController: Starting hardware-accelerated GPU stream...";
    m_stream->start();
}

void PipeWireStreamController::onStreamStopped()
{
    if (m_idleTimer) {
        m_idleTimer->stop();
    }
    m_isIdle = false;

    if (m_stream) {
        qInfo() << "PipeWireStreamController: Stopping stream...";
        m_stream->stop();
        // Do NOT call deleteLater() on m_stream! PipeWireBaseEncodedStream destructor
        // calls d->thread->wait(Forever), which deadlocks the main event loop if the
        // screencast node was already destroyed by the portal. Reusing m_stream avoids
        // the deadlock completely.
    }
    m_currentStreamResolution = QSize();
}

void PipeWireStreamController::onStreamSizeChanged(const QSize &size)
{
    qInfo() << "PipeWireStreamController: Stream size changed to" << size
            << "(target resolution is:" << m_targetResolution << ")";
    m_currentStreamResolution = size;

    if (m_targetResolution.isEmpty() || size == m_targetResolution) {
        emit streamSizeChanged(size);
    } else {
        qWarning() << "PipeWireStreamController: Stream size" << size
                   << "does not match target resolution" << m_targetResolution
                   << "- waiting for compositor mode to stabilize...";
    }
}

void PipeWireStreamController::onNewPacket(const PipeWireEncodedStream::Packet &packet)
{
    static int mismatchDropCount = 0;
    if (!m_targetResolution.isEmpty() && !m_currentStreamResolution.isEmpty() && m_currentStreamResolution != m_targetResolution) {
        if (mismatchDropCount++ < 15) {
            // Discard transitional packets for up to 15 frames (~250ms) while compositor changes mode
            return;
        }
        qWarning() << "PipeWireStreamController: Host-side resolution change confirmed to"
                   << m_currentStreamResolution << "(was" << m_targetResolution << ")";
        m_targetResolution = m_currentStreamResolution;
        mismatchDropCount = 0;
        emit streamSizeChanged(m_targetResolution);
    } else {
        mismatchDropCount = 0;
    }

    m_fpsFrameCount++;
    qint64 elapsed = m_fpsTimer.elapsed();
    if (elapsed - m_lastFpsLogTime >= 2000) {
        double fps = (m_fpsFrameCount * 1000.0) / (elapsed - m_lastFpsLogTime);
        qInfo().noquote() << QString("PipeWireStreamController: Video stream running at %1 FPS | packet: %2 KB | keyframe: %3")
                                .arg(fps, 0, 'f', 1)
                                .arg(packet.data().size() / 1024.0, 0, 'f', 1)
                                .arg(packet.isKeyFrame() ? "true" : "false");
        m_fpsFrameCount = 0;
        m_lastFpsLogTime = elapsed;
    }

    emit videoPacketEncoded(packet.data(), packet.isKeyFrame());
}

void PipeWireStreamController::onCursorChanged(const PipeWireCursor &cursor)
{
    if (!cursor.texture.isNull()) {
        if (cursor.texture.size() != m_lastCursorSize || cursor.hotspot != m_lastCursorHotspot) {
            m_lastCursorSize = cursor.texture.size();
            m_lastCursorHotspot = cursor.hotspot;
            qInfo() << "PipeWireStreamController: Cursor shape changed | size:" << cursor.texture.size()
                    << "hotspot:" << cursor.hotspot << "pos:" << cursor.position;
        }
        emit cursorShapeChanged(cursor.texture, cursor.hotspot);
    }
}

void PipeWireStreamController::onErrorFound(const QString &error)
{
    qWarning() << "PipeWireStreamController error:" << error;
}

void PipeWireStreamController::onClientActivity()
{
    m_lastActivityTimer.restart();
    if (m_isIdle) {
        m_isIdle = false;
        qInfo() << "PipeWireStreamController: Client activity detected, restoring framerate to" << m_activeFramerate << "FPS";
        if (m_stream) {
            m_stream->setMaxFramerate(m_activeFramerate);
        }
    }
}

void PipeWireStreamController::checkIdleTimeout()
{
    if (m_idleTimeoutMs <= 0 || m_isIdle) {
        return;
    }

    if (m_lastActivityTimer.elapsed() >= m_idleTimeoutMs) {
        m_isIdle = true;
        qInfo() << "PipeWireStreamController: Idle timeout reached (" << (m_idleTimeoutMs / 1000)
                << "s without client input), throttling framerate to 5 FPS to conserve power";
        if (m_stream) {
            m_stream->setMaxFramerate(5);
        }
    }
}
