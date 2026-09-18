#ifndef QTAUDIOCONTROLLER_H
#define QTAUDIOCONTROLLER_H

#include <QObject>
#include <QByteArray>
#include <QThread>
#include <atomic>

class QtAudioController : public QObject
{
    Q_OBJECT
public:
    explicit QtAudioController(QObject *parent = nullptr);
    ~QtAudioController() override;

public slots:
    void startAudioCapture(uint32_t sampleRate = 48000);
    void stopAudioCapture();

signals:
    void audioSamplesReady(const QByteArray &data);

private:
    void captureWorker();

    std::atomic<bool> m_recording{false};
    std::atomic<uint32_t> m_sampleRate{48000};
    QThread *m_workerThread{nullptr};
    QString m_originalDefaultSink;
    uint32_t m_sinkModuleId{0};
};

#endif // QTAUDIOCONTROLLER_H
