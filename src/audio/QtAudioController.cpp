#include "audio/QtAudioController.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <QDebug>

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
}

void QtAudioController::captureWorker()
{
    uint32_t rate = m_sampleRate.load();
    if (rate == 0) rate = 48000;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = rate;
    ss.channels = 2;

    // 50ms chunk = (rate * 50 / 1000) frames * 4 bytes/frame (matches RDP latency buffer)
    uint32_t chunkSize = (rate * 50 / 1000) * 4;

    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
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
        dev = "@DEFAULT_MONITOR@";
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

    float volumeScale = 0.70f;
    if (qEnvironmentVariableIsSet("RDP_AUDIO_VOLUME")) {
        bool ok = false;
        double v = qEnvironmentVariable("RDP_AUDIO_VOLUME").toDouble(&ok);
        if (ok && v >= 0.0 && v <= 2.0) {
            volumeScale = static_cast<float>(v);
        }
    }

    qInfo() << "QtAudioController: PulseAudio recording started for desktop audio at" << rate
            << "Hz 16-bit stereo (50ms buffer:" << chunkSize << "bytes, volume headroom scale:" << volumeScale << ")";

    QByteArray buffer(chunkSize, 0);

    while (m_recording) {
        if (pa_simple_read(s, buffer.data(), chunkSize, &error) < 0) {
            if (m_recording) {
                qWarning() << "QtAudioController: pa_simple_read() failed:" << pa_strerror(error);
            }
            break;
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
