#ifndef PIPEWIRESTREAMCONTROLLER_H
#define PIPEWIRESTREAMCONTROLLER_H

#include <QObject>
#include <QSize>
#include <QImage>
#include <QPoint>
#include <QElapsedTimer>
#include <QTimer>
#include <PipeWireEncodedStream>
#include <pipewiresourcestream.h>

class PipeWireStreamController : public QObject
{
    Q_OBJECT
public:
    explicit PipeWireStreamController(QObject *parent = nullptr);
    ~PipeWireStreamController() override;

public slots:
    void onStreamStarted(uint nodeId, int fd, const QSize &size);
    void onStreamStopped();
    void setEncodingParameters(uint32_t fps, int quality);
    void setMaxFramerate(uint32_t fps);
    void setQuality(int quality);
    void setTargetResolution(const QSize &size);
    void onClientActivity();
    void onMotionActivity();

signals:
    void videoPacketEncoded(const QByteArray &data, bool isKeyFrame);
    void cursorShapeChanged(const QImage &image, const QPoint &hotspot);
    void streamSizeChanged(const QSize &size);

private slots:
    void onNewPacket(const PipeWireEncodedStream::Packet &packet);
    void onCursorChanged(const PipeWireCursor &cursor);
    void onErrorFound(const QString &error);
    void onStreamSizeChanged(const QSize &size);
    void checkIdleTimeout();

private:
    void updateEffectiveQuality();

    PipeWireEncodedStream* m_stream;
    uint32_t m_framerate;
    int m_quality;
    int m_baseQuality{80};

    // Motion-Adaptive Quality (Optional dynamic quality drop during heavy motion/scrolling; default 0 for encoder stability)
    bool m_isMotionActive{false};
    int m_motionQualityDelta{0};
    QTimer *m_motionTimer{nullptr};

    QElapsedTimer m_fpsTimer;
    int m_fpsFrameCount;
    qint64 m_lastFpsLogTime;

    QSize m_targetResolution;
    QSize m_currentStreamResolution;

    QSize m_lastCursorSize;
    QPoint m_lastCursorHotspot;

    // Idle Power Saver (Dynamic FPS throttling)
    QTimer *m_idleTimer{nullptr};
    QElapsedTimer m_lastActivityTimer;
    bool m_isIdle{false};
    uint32_t m_activeFramerate{60};
    int m_idleTimeoutMs{30000};
};

#endif // PIPEWIRESTREAMCONTROLLER_H
