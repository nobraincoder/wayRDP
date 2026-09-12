#include "input/EiConnection.h"

#include <QDebug>
#include <QScopeGuard>
#include <libei.h>
#include <linux/input.h>
#include <unistd.h>

EiDevice::EiDevice(struct ei_device *device)
    : m_device(ei_device_ref(device))
{
}

EiDevice::~EiDevice()
{
    if (m_device) {
        ei_device_unref(m_device);
    }
}

struct ei_device *EiDevice::device() const
{
    return m_device;
}

EisPointerDevice::EisPointerDevice(struct ei_device *device)
    : EiDevice(device)
{
    for (size_t i = 0; auto eiRegion = ei_device_get_region(device, i); ++i) {
        const Region region{
            .rect = QRectF{static_cast<qreal>(ei_region_get_x(eiRegion)),
                           static_cast<qreal>(ei_region_get_y(eiRegion)),
                           static_cast<qreal>(ei_region_get_width(eiRegion)),
                           static_cast<qreal>(ei_region_get_height(eiRegion))},
            .scale = static_cast<qreal>(ei_region_get_physical_scale(eiRegion)),
        };
        const auto mappingId = ei_region_get_mapping_id(eiRegion);
        if (mappingId) {
            m_regions.insert(QString::fromUtf8(mappingId), region);
            qInfo() << "EIS: Found pointer region mapping:" << mappingId << "rect:" << region.rect << "scale:" << region.scale;
        } else {
            m_regions.insert(QString(), region);
            qInfo() << "EIS: Found default pointer region rect:" << region.rect << "scale:" << region.scale;
        }
    }
}

std::optional<EisPointerDevice::Region> EisPointerDevice::regionForMapping(const QString &mappingId) const
{
    if (!mappingId.isEmpty()) {
        auto it = m_regions.constFind(mappingId);
        if (it != m_regions.cend()) {
            return *it;
        }
    }
    return std::nullopt;
}

EiConnection::EiConnection(int fd, QObject *parent)
    : QObject(parent)
{
    m_scrollStopTimer = new QTimer(this);
    m_scrollStopTimer->setSingleShot(true);
    connect(m_scrollStopTimer, &QTimer::timeout, this, &EiConnection::onScrollStopTimeout);

    m_ei = ei_new_sender(this);
    if (!m_ei) {
        qWarning() << "EiConnection: Could not create libei sender context";
        ::close(fd);
        return;
    }

    ei_configure_name(m_ei, "wayrdp");
    int rc = ei_setup_backend_fd(m_ei, fd);
    if (rc != 0) {
        qWarning() << "EiConnection: Could not set up libei backend with fd" << fd << "code:" << rc;
        ei_unref(m_ei);
        m_ei = nullptr;
        return;
    }

    int socketFd = ei_get_fd(m_ei);
    m_eisNotifier = std::make_unique<QSocketNotifier>(socketFd, QSocketNotifier::Read);
    connect(m_eisNotifier.get(), &QSocketNotifier::activated, this, &EiConnection::onEisReadyRead);
    qInfo() << "EiConnection: Successfully connected to EIS via fd" << fd << "(socket fd:" << socketFd << ")";
}

EiConnection::~EiConnection()
{
    if (!m_ei) {
        return;
    }

    ei_disconnect(m_ei);
    while (auto event = ei_get_event(m_ei)) {
        ei_event_unref(event);
    }
    m_ei = ei_unref(m_ei);
}

bool EiConnection::isValid() const
{
    return (m_ei != nullptr);
}

bool EiConnection::hasPointer() const
{
    return (m_ei != nullptr && !m_pointerDevices.empty());
}

bool EiConnection::hasKeyboard() const
{
    return (m_ei != nullptr && (m_keyboardDevice != nullptr || m_textDevice != nullptr));
}

EisPointerDevice *EiConnection::findPointerDeviceWithCapability(uint32_t capability)
{
    for (auto it = m_pointerDevices.rbegin(); it != m_pointerDevices.rend(); ++it) {
        if (ei_device_has_capability((*it)->device(), static_cast<ei_device_capability>(capability))) {
            return it->get();
        }
    }
    return nullptr;
}

void EiConnection::sendPointerMotionAbsolute(double x, double y, const QSize &streamSize, const QString &mappingId)
{
    if (!m_ei || m_pointerDevices.empty() || streamSize.isEmpty()) {
        return;
    }

    if (!mappingId.isEmpty()) {
        m_lastMappingId = mappingId;
    }

    EisPointerDevice *pointerDevice = nullptr;
    std::optional<EisPointerDevice::Region> mappedRegion;

    if (!mappingId.isEmpty()) {
        for (auto it = m_pointerDevices.rbegin(); it != m_pointerDevices.rend(); ++it) {
            auto r = (*it)->regionForMapping(mappingId);
            if (r.has_value()) {
                pointerDevice = it->get();
                mappedRegion = r;
                break;
            }
        }
    }

    if (!pointerDevice) {
        pointerDevice = findPointerDeviceWithCapability(EI_DEVICE_CAP_POINTER_ABSOLUTE);
    }
    if (!pointerDevice) {
        return;
    }

    m_lastActivePointerDevice = pointerDevice;

    QPointF devicePosition;
    if (mappedRegion.has_value()) {
        const auto &region = *mappedRegion;
        double normX = x / streamSize.width();
        double normY = y / streamSize.height();
        devicePosition = QPointF(region.rect.x() + normX * region.rect.width(),
                                 region.rect.y() + normY * region.rect.height());
    } else {
        devicePosition = QPointF(x, y);
    }

    ei_device_pointer_motion_absolute(pointerDevice->device(), devicePosition.x(), devicePosition.y());
    ei_device_frame(pointerDevice->device(), ei_now(m_ei));
}

void EiConnection::sendPointerButton(int button, uint state)
{
    if (!m_ei) return;

    EisPointerDevice *pointerDevice = m_lastActivePointerDevice;
    if (!pointerDevice || !ei_device_has_capability(pointerDevice->device(), EI_DEVICE_CAP_BUTTON)) {
        pointerDevice = findPointerDeviceWithCapability(EI_DEVICE_CAP_BUTTON);
    }
    if (!pointerDevice) return;

    ei_device_button_button(pointerDevice->device(), static_cast<uint32_t>(button), state == 1);
    ei_device_frame(pointerDevice->device(), ei_now(m_ei));
}

void EiConnection::sendPointerAxis(double dx, double dy)
{
    if (!m_ei) return;

    EisPointerDevice *pointerDevice = m_lastActivePointerDevice;
    if (!pointerDevice || !ei_device_has_capability(pointerDevice->device(), EI_DEVICE_CAP_SCROLL)) {
        pointerDevice = findPointerDeviceWithCapability(EI_DEVICE_CAP_SCROLL);
    }
    if (!pointerDevice) return;

    if (m_scrollStopTimer) {
        m_scrollStopTimer->start(150);
    }
    m_isScrolling = true;

    // Pass computed Wayland deltas directly to libei (dx > 0: right, dy > 0: down)
    ei_device_scroll_delta(pointerDevice->device(), dx, dy);
    ei_device_frame(pointerDevice->device(), ei_now(m_ei));
}

void EiConnection::sendPointerAxisDiscrete(uint axis, int steps)
{
    if (!m_ei) return;

    EisPointerDevice *pointerDevice = m_lastActivePointerDevice;
    if (!pointerDevice || !ei_device_has_capability(pointerDevice->device(), EI_DEVICE_CAP_SCROLL)) {
        pointerDevice = findPointerDeviceWithCapability(EI_DEVICE_CAP_SCROLL);
    }
    if (!pointerDevice) return;

    if (m_scrollStopTimer) {
        m_scrollStopTimer->start(150);
    }
    m_isScrolling = true;

    int32_t x = (axis == 1) ? (steps * 120) : 0;
    int32_t y = (axis == 0) ? (steps * 120) : 0;

    ei_device_scroll_discrete(pointerDevice->device(), x, y);
    ei_device_frame(pointerDevice->device(), ei_now(m_ei));
}

void EiConnection::onScrollStopTimeout()
{
    if (!m_ei || !m_isScrolling) return;

    EisPointerDevice *pointerDevice = m_lastActivePointerDevice;
    if (!pointerDevice || !ei_device_has_capability(pointerDevice->device(), EI_DEVICE_CAP_SCROLL)) {
        pointerDevice = findPointerDeviceWithCapability(EI_DEVICE_CAP_SCROLL);
    }
    if (pointerDevice) {
        ei_device_scroll_stop(pointerDevice->device(), true, true);
        ei_device_frame(pointerDevice->device(), ei_now(m_ei));
    }
    m_isScrolling = false;
}

void EiConnection::sendKeyboardKeycode(int keycode, uint state)
{
    if (!m_ei || !m_keyboardDevice) return;

    ei_device_keyboard_key(m_keyboardDevice->device(), static_cast<uint32_t>(keycode), state == 1);
    ei_device_frame(m_keyboardDevice->device(), ei_now(m_ei));
}

void EiConnection::sendKeyboardKeysym(int keysym, uint state)
{
    if (!m_ei || !m_textDevice) return;

    ei_device_text_keysym(m_textDevice->device(), static_cast<uint32_t>(keysym), state == 1);
    ei_device_frame(m_textDevice->device(), ei_now(m_ei));
}

void EiConnection::processEisEvents()
{
    while (auto event = ei_get_event(m_ei)) {
        auto cleanup = qScopeGuard([event] {
            ei_event_unref(event);
        });

        auto eventType = ei_event_get_type(event);
        auto device = ei_event_get_device(event);

        switch (eventType) {
        case EI_EVENT_CONNECT:
            qInfo() << "EiConnection: EIS session connected!";
            break;
        case EI_EVENT_DISCONNECT:
            qWarning() << "EiConnection: EIS session disconnected";
            emit error();
            break;
        case EI_EVENT_SEAT_ADDED: {
            auto seat = ei_event_get_seat(event);
            qInfo() << "EiConnection: Seat added, requesting unified remote device...";
            ei_seat_bind_capabilities(seat,
                                      EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                      EI_DEVICE_CAP_BUTTON,
                                      EI_DEVICE_CAP_SCROLL,
                                      EI_DEVICE_CAP_KEYBOARD,
                                      EI_DEVICE_CAP_TEXT,
                                      NULL);
            ei_seat_request_device_with_capabilities(seat,
                                                     EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                                     EI_DEVICE_CAP_BUTTON,
                                                     EI_DEVICE_CAP_SCROLL,
                                                     EI_DEVICE_CAP_KEYBOARD,
                                                     EI_DEVICE_CAP_TEXT,
                                                     NULL);
            break;
        }
        case EI_EVENT_DEVICE_ADDED:
            qInfo() << "EiConnection: Device added by EIS:" << ei_device_get_name(device);
            if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
                m_pointerDevices.push_back(std::make_unique<EisPointerDevice>(device));
            }
            if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                m_keyboardDevice = std::make_unique<EiDevice>(device);
            }
            if (ei_device_has_capability(device, EI_DEVICE_CAP_TEXT)) {
                m_textDevice = std::make_unique<EiDevice>(device);
            }
            ei_device_start_emulating(device, ei_now(m_ei));
            emit connected();
            break;
        case EI_EVENT_DEVICE_PAUSED:
            qInfo() << "EiConnection: Device paused by EIS:" << (device ? ei_device_get_name(device) : "unknown");
            if (device) {
                ei_device_stop_emulating(device);
            }
            break;
        case EI_EVENT_DEVICE_RESUMED:
            qInfo() << "EiConnection: Device resumed by EIS:" << (device ? ei_device_get_name(device) : "unknown");
            if (device) {
                ei_device_start_emulating(device, ei_now(m_ei));
            }
            break;
        case EI_EVENT_DEVICE_REMOVED:
            qInfo() << "EiConnection: Device removed by EIS:" << (device ? ei_device_get_name(device) : "unknown");
            std::erase_if(m_pointerDevices, [device, this](const auto &p) {
                if (p->device() == device) {
                    if (m_lastActivePointerDevice == p.get()) {
                        m_lastActivePointerDevice = nullptr;
                    }
                    return true;
                }
                return false;
            });
            if (m_keyboardDevice && m_keyboardDevice->device() == device) {
                m_keyboardDevice.reset();
            }
            if (m_textDevice && m_textDevice->device() == device) {
                m_textDevice.reset();
            }
            break;
        default:
            break;
        }
    }
}

void EiConnection::onEisReadyRead()
{
    if (!m_ei) return;
    ei_dispatch(m_ei);
    processEisEvents();
}
