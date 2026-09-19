#ifndef EICONNECTION_H
#define EICONNECTION_H

#include <QObject>
#include <QSize>
#include <QPointF>
#include <QRectF>
#include <QHash>
#include <QString>
#include <QSocketNotifier>
#include <QTimer>
#include <memory>
#include <vector>
#include <optional>

struct ei;
struct ei_device;

class EiDevice
{
public:
    explicit EiDevice(struct ei_device *device);
    virtual ~EiDevice();

    EiDevice(const EiDevice &) = delete;
    EiDevice &operator=(const EiDevice &) = delete;

    [[nodiscard]] struct ei_device *device() const;

private:
    struct ei_device *m_device{nullptr};
};

class EisPointerDevice : public EiDevice
{
public:
    struct Region {
        QRectF rect;
        qreal scale{1.0};
    };

    explicit EisPointerDevice(struct ei_device *device);

    [[nodiscard]] std::optional<Region> regionForMapping(const QString &mappingId) const;
    [[nodiscard]] const QHash<QString, Region> &regions() const { return m_regions; }

private:
    QHash<QString, Region> m_regions;
};

class EiConnection : public QObject
{
    Q_OBJECT
public:
    explicit EiConnection(int fd, QObject *parent = nullptr);
    ~EiConnection() override;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool hasPointer() const;
    [[nodiscard]] bool hasKeyboard() const;

    void sendPointerMotionAbsolute(double x, double y, const QSize &streamSize, const QString &mappingId = QString());
    void sendPointerMotion(double dx, double dy);
    bool sendPointerButton(int button, uint state);
    void sendPointerAxis(double dx, double dy);
    void sendPointerAxisDiscrete(uint axis, int steps);
    void sendKeyboardKeycode(int keycode, uint state);
    void sendKeyboardKeysym(int keysym, uint state);

signals:
    void error();
    void connected();

private slots:
    void onEisReadyRead();

private:
    void processEisEvents();
    EisPointerDevice *findPointerDeviceWithCapability(uint32_t capability);

    std::unique_ptr<QSocketNotifier> m_eisNotifier;
    struct ei *m_ei{nullptr};
    std::vector<std::unique_ptr<EisPointerDevice>> m_pointerDevices;
    EisPointerDevice *m_lastActivePointerDevice{nullptr};
    QString m_lastMappingId;
    std::unique_ptr<EiDevice> m_keyboardDevice;
    std::unique_ptr<EiDevice> m_textDevice;
};

#endif // EICONNECTION_H
