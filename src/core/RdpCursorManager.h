#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QHash>
#include <QSet>
#include <QByteArray>
#include <chrono>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/pointer.h>

class RdpCursorManager
{
public:
    RdpCursorManager() = default;
    ~RdpCursorManager() = default;

    // Updates or activates client-side hardware cursor cache for the given peer
    void updateCursorShape(freerdp_peer* peer, const QImage &image, const QPoint &hotspot);

    // Clears cached cursor entries on disconnect
    void reset();

private:
    struct CursorCacheEntry {
        uint32_t cacheId{0};
        QPoint hotspot;
        QImage image;
        std::chrono::steady_clock::time_point lastUsed;
    };

    static QRect findContentBounds(const QImage &image);
    static QByteArray createXorMask(const QImage &image);
    static QByteArray createAndMask(const QImage &image);

    QHash<uint32_t, CursorCacheEntry> m_cursorCache;
    bool m_hasLastUsed{false};
    uint32_t m_lastUsedCacheId{0};
    QPoint m_lastHotspot;
    qint64 m_lastCacheKey{0};
    QImage m_lastImage;
};

