#include "QtAudioController.h"
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

    // Ensure desktop sink is unmuted and at 100% volume so monitor stream has full dynamic range
    system("wpctl set-volume @DEFAULT_AUDIO_SINK@ 1.0 2>/dev/null; wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 2>/dev/null;");

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

    // 20ms chunk = (rate * 20 / 1000) frames * 4 bytes/frame
    uint32_t chunkSize = (rate * 20 / 1000) * 4;

    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = chunkSize;

    int error = 0;
    // Capture directly from the desktop audio output monitor
    pa_simple* s = pa_simple_new(nullptr, "wayrdp", PA_STREAM_RECORD,
                                 "@DEFAULT_MONITOR@", "Desktop Audio", &ss, nullptr, &attr, &error);
    if (!s) {
        qWarning() << "QtAudioController: Failed to open @DEFAULT_MONITOR@, trying fallback:" << pa_strerror(error);
        s = pa_simple_new(nullptr, "wayrdp", PA_STREAM_RECORD,
                          nullptr, "Desktop Audio", &ss, nullptr, &attr, &error);
    }

    if (!s) {
        qWarning() << "QtAudioController: Failed to initialize PulseAudio capture:" << pa_strerror(error);
        m_recording = false;
        return;
    }

    qInfo() << "QtAudioController: PulseAudio recording started for desktop audio at" << rate << "Hz 16-bit stereo (chunk size:" << chunkSize << "bytes)";

    QByteArray buffer(chunkSize, 0);
    while (m_recording) {
        if (pa_simple_read(s, buffer.data(), chunkSize, &error) < 0) {
            if (m_recording) {
                qWarning() << "QtAudioController: pa_simple_read() failed:" << pa_strerror(error);
            }
            break;
        }
        emit audioSamplesReady(buffer);
    }

    pa_simple_free(s);
    qInfo() << "QtAudioController: PulseAudio recording stopped";
}
