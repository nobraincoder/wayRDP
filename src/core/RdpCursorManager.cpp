#include "core/RdpCursorManager.h"
#include <algorithm>
#include <QDebug>

QRect RdpCursorManager::findContentBounds(const QImage &image)
{
    if (image.isNull()) return QRect();
    const int width = image.width();
    const int height = image.height();
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;

    for (int y = 0; y < height; ++y) {
        const QRgb* scanline = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(scanline[x]) > 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    if (minX > maxX || minY > maxY) {
        return QRect();
    }
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

QByteArray RdpCursorManager::createXorMask(const QImage &image)
{
    auto converted = image.convertToFormat(QImage::Format_ARGB32);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    converted.flip(Qt::Vertical);
    converted.rgbSwap();
#else
    converted = converted.mirrored(false, true).rgbSwapped();
#endif
    return QByteArray(reinterpret_cast<const char *>(converted.constBits()), converted.sizeInBytes());
}

QByteArray RdpCursorManager::createAndMask(const QImage &image)
{
    const int width = image.width();
    const int height = image.height();
    int andStep = ((width + 7) / 8);
    andStep = ((andStep + 1) / 2) * 2; // Padded to 2-byte boundary per RDP spec
    QByteArray andMask(height * andStep, 0);

    for (int y = 0; y < height; ++y) {
        // Bottom-up scanline order matching RDP XOR mask
        int srcY = height - 1 - y;
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(image.constScanLine(srcY));
        unsigned char* dstLine = reinterpret_cast<unsigned char*>(andMask.data() + y * andStep);
        unsigned char bitMask = 0x80;
        int byteIdx = 0;

        for (int x = 0; x < width; ++x) {
            if (qAlpha(srcLine[x]) == 0) {
                dstLine[byteIdx] |= bitMask;
            }
            bitMask >>= 1;
            if (bitMask == 0) {
                bitMask = 0x80;
                byteIdx++;
            }
        }
    }
    return andMask;
}

void RdpCursorManager::updateCursorShape(freerdp_peer* peer, const QImage &image, const QPoint &hotspot)
{
    if (image.isNull() || !peer) {
        qWarning() << "RdpCursorManager: null image or null peer";
        return;
    }

    if (!peer->context || !peer->context->update || !peer->context->update->pointer) {
        qWarning() << "RdpCursorManager: null context/update/pointer";
        return;
    }

    auto updatePointer = peer->context->update->pointer;

    // Fast-path: Check if identical to last applied cursor
    if (m_hasLastUsed && m_lastHotspot == hotspot &&
        (m_lastCacheKey == image.cacheKey() || m_lastImage == image)) {
        return;
    }

    // Handle completely transparent / empty cursor
    QRect contentBounds = findContentBounds(image);
    if (contentBounds.isEmpty()) {
        POINTER_SYSTEM_UPDATE pointerSystemUpdate;
        memset(&pointerSystemUpdate, 0, sizeof(pointerSystemUpdate));
        pointerSystemUpdate.type = SYSPTR_NULL;
        updatePointer->PointerSystem(peer->context, &pointerSystemUpdate);
        m_hasLastUsed = true;
        m_lastHotspot = hotspot;
        m_lastCacheKey = image.cacheKey();
        m_lastImage = image;
        return;
    }

    // Expand bounding box to contain hotspot so newHotspot is never negative
    QRect cropRect = contentBounds;
    if (hotspot.x() < cropRect.left()) cropRect.setLeft(hotspot.x());
    if (hotspot.x() > cropRect.right()) cropRect.setRight(hotspot.x());
    if (hotspot.y() < cropRect.top()) cropRect.setTop(hotspot.y());
    if (hotspot.y() > cropRect.bottom()) cropRect.setBottom(hotspot.y());

    QImage cropped = image.copy(cropRect);
    QPoint newHotspot = hotspot - cropRect.topLeft();

    // Check client capabilities for maximum allowed pointer dimensions
    UINT32 largePointerFlag = freerdp_settings_get_uint32(peer->context->settings, FreeRDP_LargePointerFlag);
    UINT32 maxAllowedSize = 32;
    if (largePointerFlag & LARGE_POINTER_FLAG_384x384) {
        maxAllowedSize = 384;
    } else if (largePointerFlag & LARGE_POINTER_FLAG_96x96) {
        maxAllowedSize = 96;
    }

    if (cropped.width() > static_cast<int>(maxAllowedSize) || cropped.height() > static_cast<int>(maxAllowedSize)) {
        qreal scale = qMin(static_cast<qreal>(maxAllowedSize) / cropped.width(),
                           static_cast<qreal>(maxAllowedSize) / cropped.height());
        QSize targetSize(qMax(1, qRound(cropped.width() * scale)),
                         qMax(1, qRound(cropped.height() * scale)));
        cropped = cropped.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        newHotspot = QPoint(qBound(0, qRound(newHotspot.x() * scale), cropped.width() - 1),
                            qBound(0, qRound(newHotspot.y() * scale), cropped.height() - 1));
    }

    // Check if processed cursor is already in client cache
    const qint64 targetCacheKey = cropped.cacheKey();
    const size_t targetHash = qHashBits(cropped.constBits(), cropped.sizeInBytes());
    auto itr = std::find_if(m_cursorCache.begin(), m_cursorCache.end(),
        [&cropped, &newHotspot, targetCacheKey, targetHash](const CursorCacheEntry &cached) {
            if (cached.hotspot != newHotspot || cached.image.size() != cropped.size()) {
                return false;
            }
            if (cached.image.cacheKey() == targetCacheKey) {
                return true;
            }
            if (cached.contentHash == targetHash) {
                // Defensively verify pixel exactness to guard against hash collisions
                if (cached.image == cropped) {
                    return true;
                }
                qWarning() << "RdpCursorManager: Hash collision detected for hash: 0x"
                           << QString::number(targetHash, 16) << "! Pixel content differs; bypassing cache match.";
                return false;
            }
            return cached.image == cropped;
        });

    qDebug().noquote() << QString("RdpCursorManager: Cursor cache lookup: hash=0x%1, match=%2, cacheSize=%3")
                              .arg(QString::number(targetHash, 16))
                              .arg(itr != m_cursorCache.end() ? "HIT" : "MISS")
                              .arg(m_cursorCache.size());

    if (itr != m_cursorCache.end()) {
        itr->lastUsed = std::chrono::steady_clock::now();
        POINTER_CACHED_UPDATE pointerCachedUpdate;
        memset(&pointerCachedUpdate, 0, sizeof(pointerCachedUpdate));
        pointerCachedUpdate.cacheIndex = itr->cacheId;
        BOOL resCached = updatePointer->PointerCached(peer->context, &pointerCachedUpdate);
        qInfo() << "RdpCursorManager: Activated cached cursor index:" << itr->cacheId << "result:" << resCached;

        m_hasLastUsed = true;
        m_lastUsedCacheId = itr->cacheId;
        m_lastHotspot = hotspot;
        m_lastCacheKey = image.cacheKey();
        m_lastImage = image;
        return;
    }

    // Allocate collision-free cache ID
    UINT32 maxCacheSize = freerdp_settings_get_uint32(peer->context->settings, FreeRDP_PointerCacheSize);
    if (maxCacheSize == 0) maxCacheSize = 20;

    uint32_t cacheId = 0;
    if (static_cast<UINT32>(m_cursorCache.size()) >= maxCacheSize) {
        auto lru = std::min_element(m_cursorCache.begin(), m_cursorCache.end(),
            [](const CursorCacheEntry &a, const CursorCacheEntry &b) {
                return a.lastUsed < b.lastUsed;
            });
        cacheId = lru->cacheId;
        m_cursorCache.erase(lru);
    } else {
        QSet<uint32_t> usedIds;
        for (const auto &entry : m_cursorCache) {
            usedIds.insert(entry.cacheId);
        }
        for (uint32_t id = 0; id < maxCacheSize; ++id) {
            if (!usedIds.contains(id)) {
                cacheId = id;
                break;
            }
        }
    }

    CursorCacheEntry newEntry;
    newEntry.cacheId = cacheId;
    newEntry.hotspot = newHotspot;
    newEntry.contentHash = targetHash;
    newEntry.image = cropped;
    newEntry.lastUsed = std::chrono::steady_clock::now();

    auto xorMask = createXorMask(cropped);
    auto andMask = createAndMask(cropped);
    BOOL res = FALSE;

    if (cropped.width() <= 96 && cropped.height() <= 96) {
        POINTER_NEW_UPDATE pointerNewUpdate;
        memset(&pointerNewUpdate, 0, sizeof(pointerNewUpdate));
        pointerNewUpdate.xorBpp = 32;
        auto &colorUpdate = pointerNewUpdate.colorPtrAttr;
        colorUpdate.cacheIndex = static_cast<UINT16>(cacheId);
        colorUpdate.hotSpotX = static_cast<UINT16>(qBound(0, newHotspot.x(), cropped.width() - 1));
        colorUpdate.hotSpotY = static_cast<UINT16>(qBound(0, newHotspot.y(), cropped.height() - 1));
        colorUpdate.width = static_cast<UINT16>(cropped.width());
        colorUpdate.height = static_cast<UINT16>(cropped.height());
        colorUpdate.lengthAndMask = static_cast<UINT16>(andMask.size());
        colorUpdate.andMaskData = reinterpret_cast<BYTE *>(andMask.data());
        colorUpdate.lengthXorMask = static_cast<UINT16>(xorMask.size());
        colorUpdate.xorMaskData = reinterpret_cast<BYTE *>(xorMask.data());
        res = updatePointer->PointerNew(peer->context, &pointerNewUpdate);
        qInfo() << "RdpCursorManager: PointerNew sent cacheIndex:" << cacheId
                << "size:" << cropped.width() << "x" << cropped.height()
                << "hotspot:" << colorUpdate.hotSpotX << "," << colorUpdate.hotSpotY
                << "xorLen:" << colorUpdate.lengthXorMask
                << "andLen:" << colorUpdate.lengthAndMask
                << "res:" << res;
    } else {
        POINTER_LARGE_UPDATE pointerLargeUpdate;
        memset(&pointerLargeUpdate, 0, sizeof(pointerLargeUpdate));
        pointerLargeUpdate.xorBpp = 32;
        pointerLargeUpdate.cacheIndex = static_cast<UINT16>(cacheId);
        pointerLargeUpdate.hotSpotX = static_cast<UINT16>(qBound(0, newHotspot.x(), cropped.width() - 1));
        pointerLargeUpdate.hotSpotY = static_cast<UINT16>(qBound(0, newHotspot.y(), cropped.height() - 1));
        pointerLargeUpdate.width = static_cast<UINT16>(cropped.width());
        pointerLargeUpdate.height = static_cast<UINT16>(cropped.height());
        pointerLargeUpdate.lengthAndMask = static_cast<UINT32>(andMask.size());
        pointerLargeUpdate.andMaskData = reinterpret_cast<BYTE *>(andMask.data());
        pointerLargeUpdate.lengthXorMask = static_cast<UINT32>(xorMask.size());
        pointerLargeUpdate.xorMaskData = reinterpret_cast<BYTE *>(xorMask.data());
        res = updatePointer->PointerLarge(peer->context, &pointerLargeUpdate);
        qInfo() << "RdpCursorManager: PointerLarge sent cacheIndex:" << cacheId
                << "size:" << cropped.width() << "x" << cropped.height()
                << "res:" << res;
    }

    m_cursorCache.insert(cacheId, newEntry);
    m_hasLastUsed = true;
    m_lastUsedCacheId = cacheId;
    m_lastHotspot = hotspot;
    m_lastCacheKey = image.cacheKey();
    m_lastImage = image;
}

void RdpCursorManager::reset()
{
    m_cursorCache.clear();
    m_hasLastUsed = false;
    m_lastUsedCacheId = 0;
    m_lastHotspot = QPoint();
    m_lastCacheKey = 0;
    m_lastImage = QImage();
}
