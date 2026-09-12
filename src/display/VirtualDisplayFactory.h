#pragma once

#include <memory>
#include <QObject>
#include <QString>
#include "display/IVirtualDisplayBackend.h"
#include "display/KWinVirtualDisplay.h"

class VirtualDisplayFactory
{
public:
    static std::unique_ptr<IVirtualDisplayBackend> createBackend(QObject *parent = nullptr)
    {
        QString backend = qEnvironmentVariable("RDP_BACKEND").trimmed().toLower();
        QString currentDesktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").trimmed().toLower();

        if (backend == "kwin" || currentDesktop.contains("kde")) {
            qInfo() << "VirtualDisplayFactory: Using KWin virtual display backend (Wayland RemoteDesktop + ScreenCast portal)";
            return std::make_unique<KWinVirtualDisplay>(parent);
        }

        // Default to KWin backend on this system
        qInfo() << "VirtualDisplayFactory: Defaulting to KWin virtual display backend";
        return std::make_unique<KWinVirtualDisplay>(parent);
    }
};
