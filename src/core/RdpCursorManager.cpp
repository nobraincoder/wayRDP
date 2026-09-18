#include "core/RdpCursorManager.h"
#include <algorithm>
#include <QDebug>

QByteArray RdpCursorManager::createXorMask(const QImage &image)
{
    auto converted = image.convertToFormat(QImage::Format_ARGB32);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    converted.flip(Qt::Vertical);
    converted.rgbSwap();
#else
    converted = converted.mirrored(false, true).rgbSwapped();
#endif
    return QByteArray(reinterpret_cast<char *>(converted.bits()), converted.sizeInBytes());
}

void RdpCursorManager::updateCursorShape(freerdp_peer* peer, const QImage &image, const QPoint &hotspot)
{
    if (image.isNull() || !peer) return;

    if (!peer->context || !peer->context->update || !peer->context->update->pointer)
        return;

    // RDP cannot handle cursor images larger than 384x384 px. Discard and use system default cursor.
    if (image.width() > 384 || image.height() > 384) {
        POINTER_SYSTEM_UPDATE pointerSystemUpdate;
        memset(&pointerSystemUpdate, 0, sizeof(pointerSystemUpdate));
        pointerSystemUpdate.type = SYSPTR_DEFAULT;
        peer->context->update->pointer->PointerSystem(peer->context, &pointerSystemUpdate);
        return;
    }

    // If currently displayed cursor is identical, update timestamp and return
    if (m_lastUsedCursor && m_lastUsedCursor->hotspot == hotspot &&
        (m_lastUsedCursor->image.cacheKey() == image.cacheKey() || m_lastUsedCursor->image == image)) {
        m_lastUsedCursor->lastUsed = std::chrono::steady_clock::now();
        return;
    }

    auto updatePointer = peer->context->update->pointer;

    // Check if cursor is already cached (fast-path via 64-bit cacheKey)
    const qint64 targetCacheKey = image.cacheKey();
    auto itr = std::find_if(m_cursorCache.begin(), m_cursorCache.end(), [&image, &hotspot, targetCacheKey](const CursorCacheEntry &cached) {
        return cached.hotspot == hotspot && (cached.image.cacheKey() == targetCacheKey || cached.image == image);
    });
    if (itr != m_cursorCache.end()) {
        m_lastUsedCursor = &itr.value();
        itr->lastUsed = std::chrono::steady_clock::now();
        POINTER_CACHED_UPDATE pointerCachedUpdate;
        memset(&pointerCachedUpdate, 0, sizeof(pointerCachedUpdate));
        pointerCachedUpdate.cacheIndex = itr->cacheId;
        updatePointer->PointerCached(peer->context, &pointerCachedUpdate);
        return;
    }

    // New cursor entry
    CursorCacheEntry newCursor;
    newCursor.hotspot = hotspot;
    newCursor.image = image;
    newCursor.cacheId = static_cast<uint32_t>(m_cursorCache.size());
    newCursor.lastUsed = std::chrono::steady_clock::now();

    // Evict least recently used cursor if cache limit reached
    UINT32 maxCacheSize = freerdp_settings_get_uint32(peer->context->settings, FreeRDP_PointerCacheSize);
    if (maxCacheSize == 0) maxCacheSize = 20;
    if (static_cast<UINT32>(m_cursorCache.size()) >= maxCacheSize) {
        auto lru = std::min_element(m_cursorCache.cbegin(), m_cursorCache.cend(), [](const CursorCacheEntry &first, const CursorCacheEntry &second) {
            return first.lastUsed < second.lastUsed;
        });
        newCursor.cacheId = lru->cacheId;
        m_cursorCache.erase(lru);
    }

    auto xorMask = createXorMask(image);

    if (image.width() < 96 && image.height() < 96) {
        POINTER_NEW_UPDATE pointerNewUpdate;
        memset(&pointerNewUpdate, 0, sizeof(pointerNewUpdate));
        pointerNewUpdate.xorBpp = 32;
        auto &colorUpdate = pointerNewUpdate.colorPtrAttr;
        colorUpdate.cacheIndex = static_cast<UINT16>(newCursor.cacheId);
        colorUpdate.hotSpotX = static_cast<UINT16>(qBound(0, hotspot.x(), image.width() - 1));
        colorUpdate.hotSpotY = static_cast<UINT16>(qBound(0, hotspot.y(), image.height() - 1));
        colorUpdate.width = static_cast<UINT16>(image.width());
        colorUpdate.height = static_cast<UINT16>(image.height());
        // For 32-bit ARGB cursors, lengthAndMask = 0 enables native 8-bit alpha blending without 1-bit raster stippling
        colorUpdate.lengthAndMask = 0;
        colorUpdate.andMaskData = nullptr;
        colorUpdate.lengthXorMask = static_cast<UINT16>(xorMask.size());
        colorUpdate.xorMaskData = reinterpret_cast<BYTE *>(xorMask.data());
        updatePointer->PointerNew(peer->context, &pointerNewUpdate);
    } else {
        POINTER_LARGE_UPDATE pointerLargeUpdate;
        memset(&pointerLargeUpdate, 0, sizeof(pointerLargeUpdate));
        pointerLargeUpdate.xorBpp = 32;
        pointerLargeUpdate.cacheIndex = static_cast<UINT16>(newCursor.cacheId);
        pointerLargeUpdate.hotSpotX = static_cast<UINT16>(qBound(0, hotspot.x(), image.width() - 1));
        pointerLargeUpdate.hotSpotY = static_cast<UINT16>(qBound(0, hotspot.y(), image.height() - 1));
        pointerLargeUpdate.width = static_cast<UINT16>(image.width());
        pointerLargeUpdate.height = static_cast<UINT16>(image.height());
        pointerLargeUpdate.lengthAndMask = 0;
        pointerLargeUpdate.andMaskData = nullptr;
        pointerLargeUpdate.lengthXorMask = static_cast<UINT32>(xorMask.size());
        pointerLargeUpdate.xorMaskData = reinterpret_cast<BYTE *>(xorMask.data());
        updatePointer->PointerLarge(peer->context, &pointerLargeUpdate);
    }

    POINTER_CACHED_UPDATE pointerCachedUpdate;
    memset(&pointerCachedUpdate, 0, sizeof(pointerCachedUpdate));
    pointerCachedUpdate.cacheIndex = static_cast<UINT32>(newCursor.cacheId);
    updatePointer->PointerCached(peer->context, &pointerCachedUpdate);

    auto inserted = m_cursorCache.insert(newCursor.cacheId, newCursor);
    m_lastUsedCursor = &inserted.value();
}

void RdpCursorManager::reset()
{
    m_cursorCache.clear();
    m_lastUsedCursor = nullptr;
}
