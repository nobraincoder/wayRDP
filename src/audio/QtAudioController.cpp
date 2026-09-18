#include "audio/QtAudioController.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <QDebug>
#include <QProcess>

QtAudioController::QtAudioController(QObject *parent)
    : QObject(parent)
{
}

QtAudioController::~QtAudioController()
{
    stopAudioCapture();
}

void QtAudioController::startAudioCapture(uint32_t sampleRate)
{
    stopAudioCapture();

    m_sampleRate = sampleRate;

    // Check if virtual sink is enabled (enabled by default unless RDP_AUDIO_VIRTUAL_SINK=0)
    bool useVirtualSink = true;
    if (qEnvironmentVariableIsSet("RDP_AUDIO_VIRTUAL_SINK")) {
        QString v = qEnvironmentVariable("RDP_AUDIO_VIRTUAL_SINK").trimmed().toLower();
        if (v == "0" || v == "false" || v == "no") {
            useVirtualSink = false;
        }
    }

    if (useVirtualSink) {
        // Ensure any pre-existing wayrdp_sink modules from prior crashes or runs are cleanly removed
        QProcess cleanupProc;
        cleanupProc.start("pactl", QStringList() << "list" << "modules" << "short");
        if (cleanupProc.waitForFinished(1000)) {
            QString out = QString::fromUtf8(cleanupProc.readAllStandardOutput());
            for (const QString &line : out.split('\n')) {
                if (line.contains("wayrdp_sink")) {
                    QString modId = line.section('\t', 0, 0).trimmed();
                    if (!modId.isEmpty()) {
                        QProcess::execute("pactl", QStringList() << "unload-module" << modId);
                    }
                }
            }
        }

        // Save current default sink
        QProcess proc;
        proc.start("pactl", QStringList() << "get-default-sink");
        if (proc.waitForFinished(1000)) {
            QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            if (!out.isEmpty() && out != "wayrdp_sink") {
                m_originalDefaultSink = out;
            }
        }

        // Load dedicated virtual null-sink for wayRDP in s16le (matching our capture format)
        // to avoid PipeWire float32→s16le conversion that introduces dithering artifacts.
        proc.start("pactl", QStringList() << "load-module" << "module-null-sink"
                                          << "sink_name=wayrdp_sink"
                                          << "sink_properties=device.description=\"wayRDP_Audio\""
                                          << QString("rate=%1").arg(sampleRate)
                                          << "format=s16le"
                                          << "channels=2");
        if (proc.waitForFinished(1000)) {
            bool ok = false;
            uint32_t id = QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toUInt(&ok);
            if (ok && id > 0) {
                m_sinkModuleId = id;
                QProcess::execute("pactl", QStringList() << "set-default-sink" << "wayrdp_sink");
                qInfo() << "QtAudioController: Created dedicated virtual audio sink 'wayrdp_sink' (Module ID:"
                        << m_sinkModuleId << ") - desktop audio routed cleanly to remote session";
            }
        }
    }

    m_recording = true;
    m_workerThread = QThread::create([this]() {
        captureWorker();
    });
    m_workerThread->start();
}

void QtAudioController::stopAudioCapture()
{
    if (m_recording) {
        qInfo() << "QtAudioController: Stopping audio capture...";
        m_recording = false;
        if (m_workerThread) {
            m_workerThread->wait(1000);
            delete m_workerThread;
            m_workerThread = nullptr;
        }
    }

    if (m_sinkModuleId > 0) {
        if (!m_originalDefaultSink.isEmpty()) {
            QProcess::execute("pactl", QStringList() << "set-default-sink" << m_originalDefaultSink);
            qInfo() << "QtAudioController: Restored host default audio sink to:" << m_originalDefaultSink;
        }
        QProcess::execute("pactl", QStringList() << "unload-module" << QString::number(m_sinkModuleId));
        m_sinkModuleId = 0;
        m_originalDefaultSink.clear();
    }
}

void QtAudioController::captureWorker()
{
    uint32_t rate = m_sampleRate.load();
    if (rate == 0) rate = 48000;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = rate;
    ss.channels = 2;

    // 20ms chunk = (rate * 20 / 1000) frames * 4 bytes/frame (matches RDP latency buffer)
    uint32_t chunkSize = (rate * 20 / 1000) * 4;

    pa_buffer_attr attr;
    attr.maxlength = chunkSize * 10; // 200ms max buffer — matches our relaxed flush threshold
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = chunkSize;

    QByteArray deviceName;
    const char* dev = nullptr;
    if (qEnvironmentVariableIsSet("RDP_AUDIO_DEVICE")) {
        deviceName = qgetenv("RDP_AUDIO_DEVICE");
        if (!deviceName.isEmpty()) {
            dev = deviceName.constData();
        }
    }
    if (!dev) {
        if (m_sinkModuleId > 0) {
            dev = "wayrdp_sink.monitor";
        } else {
            dev = "@DEFAULT_MONITOR@";
        }
    }

    int error = 0;
    // Capture directly from the desktop audio output monitor
    pa_simple* s = pa_simple_new(nullptr, "wayrdp", PA_STREAM_RECORD,
                                 dev, "Desktop Audio", &ss, nullptr, &attr, &error);
    if (!s) {
        qWarning() << "QtAudioController: Failed to open" << dev << ", trying fallback:" << pa_strerror(error);
        s = pa_simple_new(nullptr, "wayrdp", PA_STREAM_RECORD,
                          nullptr, "Desktop Audio", &ss, nullptr, &attr, &error);
    }

    if (!s) {
        qWarning() << "QtAudioController: Failed to initialize PulseAudio capture:" << pa_strerror(error);
        m_recording = false;
        return;
    }

    float volumeScale = 1.0f;
    if (qEnvironmentVariableIsSet("RDP_AUDIO_VOLUME")) {
        bool ok = false;
        double v = qEnvironmentVariable("RDP_AUDIO_VOLUME").toDouble(&ok);
        if (ok && v >= 0.0 && v <= 2.0) {
            volumeScale = static_cast<float>(v);
        }
    }

    qInfo() << "QtAudioController: PulseAudio recording started for desktop audio at" << rate
            << "Hz 16-bit stereo (20ms buffer:" << chunkSize << "bytes, volume headroom scale:" << volumeScale << ")";

    QByteArray buffer(chunkSize, 0);

    while (m_recording) {
        if (pa_simple_read(s, buffer.data(), chunkSize, &error) < 0) {
            if (m_recording) {
                qWarning() << "QtAudioController: pa_simple_read() failed:" << pa_strerror(error);
            }
            break;
        }

        // Real-time synchronization: flush stale audio backlog only if buffer latency
        // exceeds 200ms, which indicates a significant drift (e.g. after a network
        // keyframe burst). The previous 80ms threshold was too aggressive and caused
        // audible gaps that made the audio sound choppy or "different".
        pa_usec_t latency = pa_simple_get_latency(s, &error);
        if (latency > 200000) {
            pa_simple_flush(s, &error);
        }

        // Apply volume headroom scaling with saturation clamping to prevent digital clipping/distortion
        if (volumeScale < 0.99f || volumeScale > 1.01f) {
            int16_t* samples = reinterpret_cast<int16_t*>(buffer.data());
            size_t sampleCount = buffer.size() / sizeof(int16_t);
            for (size_t i = 0; i < sampleCount; ++i) {
                int32_t val = static_cast<int32_t>(samples[i] * volumeScale);
                samples[i] = static_cast<int16_t>(std::clamp(val, -32768, 32767));
            }
        }

        emit audioSamplesReady(buffer);
    }

    pa_simple_free(s);
    qInfo() << "QtAudioController: PulseAudio recording stopped";
}
