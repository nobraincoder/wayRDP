#include "SystemInputSettings.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <linux/input-event-codes.h>

SystemInputSettings::SystemInputSettings(QObject *parent)
    : QObject(parent)
{
    m_watcher = new QFileSystemWatcher(this);
    QString configPath = QDir::homePath() + "/.config/kcminputrc";
    QString globalsPath = QDir::homePath() + "/.config/kdeglobals";
    if (QFile::exists(configPath)) {
        m_watcher->addPath(configPath);
    }
    if (QFile::exists(globalsPath)) {
        m_watcher->addPath(globalsPath);
    }
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this, configPath, globalsPath](const QString &path) {
        qInfo() << "Detected change in" << path << "- reloading system input settings...";
        if (!m_watcher->files().contains(configPath) && QFile::exists(configPath)) {
            m_watcher->addPath(configPath);
        }
        if (!m_watcher->files().contains(globalsPath) && QFile::exists(globalsPath)) {
            m_watcher->addPath(globalsPath);
        }
        reload();
        emit settingsChanged();
    });

    reload();
}

void SystemInputSettings::reload()
{
    // Default baseline values
    m_naturalScroll = false;
    m_scrollFactor = 1.0;
    m_invertHScroll = false;
    m_invertVScroll = false;
    m_hasCustomScale = false;
    m_leftHanded = false;
    m_cursorTheme = "breeze_cursors";
    m_cursorSize = 24;

    // 1. Query live KWin D-Bus
    queryKWinDBus();

    // 2. Parse ~/.config/kcminputrc and kdeglobals
    parseConfigFile();

    // 3. Apply Environment overrides
    applyEnvironmentOverrides();

    qInfo() << QString("SystemInputSettings: NaturalScroll=%1, ScrollFactor=%2, LeftHanded=%3, Cursor=%4(%5px), EffectiveScale=%6")
                   .arg(m_naturalScroll ? "true" : "false")
                   .arg(m_scrollFactor)
                   .arg(m_leftHanded ? "true" : "false")
                   .arg(m_cursorTheme)
                   .arg(m_cursorSize)
                   .arg(effectiveScrollScale());
}

void SystemInputSettings::queryKWinDBus()
{
    QDBusInterface manager("org.kde.KWin", "/org/kde/KWin/InputDevice", "org.kde.KWin.InputDeviceManager", QDBusConnection::sessionBus());
    if (!manager.isValid()) return;

    QDBusReply<QStringList> devReply = manager.call("devicesSysNames");
    if (!devReply.isValid()) return;

    QStringList devices = devReply.value();
    bool foundTouchpad = false;

    for (const QString &sysName : devices) {
        QDBusInterface dev("org.kde.KWin", "/org/kde/KWin/InputDevice/" + sysName, "org.freedesktop.DBus.Properties", QDBusConnection::sessionBus());
        if (!dev.isValid()) continue;

        QDBusReply<QVariant> tpReply = dev.call("Get", "org.kde.KWin.InputDevice", "touchpad");
        bool isTouchpad = (tpReply.isValid() && tpReply.value().toBool());

        QDBusReply<QVariant> ptrReply = dev.call("Get", "org.kde.KWin.InputDevice", "pointer");
        bool isPointer = (ptrReply.isValid() && ptrReply.value().toBool());

        if (isTouchpad || (!foundTouchpad && isPointer)) {
            QDBusReply<QVariant> nsReply = dev.call("Get", "org.kde.KWin.InputDevice", "naturalScroll");
            if (nsReply.isValid()) {
                m_naturalScroll = nsReply.value().toBool();
            }
            QDBusReply<QVariant> sfReply = dev.call("Get", "org.kde.KWin.InputDevice", "scrollFactor");
            if (sfReply.isValid()) {
                bool ok = false;
                double sf = sfReply.value().toDouble(&ok);
                if (ok && sf > 0.0) {
                    m_scrollFactor = sf;
                }
            }
            QDBusReply<QVariant> lhReply = dev.call("Get", "org.kde.KWin.InputDevice", "leftHanded");
            if (lhReply.isValid() && lhReply.value().toBool()) {
                m_leftHanded = true;
            }

            if (isTouchpad) {
                foundTouchpad = true;
                break; // Touchpad settings take precedence for trackpad/smooth scrolling
            }
        }
    }
}

void SystemInputSettings::parseConfigFile()
{
    QString configPath = QDir::homePath() + "/.config/kcminputrc";
    QFile file(configPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QString currentSection;
        bool inTouchpadSection = false;
        bool inMouseSection = false;

        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
                continue;

            if (line.startsWith('[') && line.endsWith(']')) {
                currentSection = line.mid(1, line.length() - 2);
                QString lowerSec = currentSection.toLower();
                inTouchpadSection = lowerSec.contains("touchpad");
                inMouseSection = (lowerSec == "mouse");
                continue;
            }

            int eqIdx = line.indexOf('=');
            if (eqIdx == -1) continue;

            QString key = line.left(eqIdx).trimmed();
            QString val = line.mid(eqIdx + 1).trimmed();

            if (inTouchpadSection) {
                if (key.compare("NaturalScroll", Qt::CaseInsensitive) == 0) {
                    m_naturalScroll = (val.compare("true", Qt::CaseInsensitive) == 0);
                } else if (key.compare("ScrollFactor", Qt::CaseInsensitive) == 0) {
                    bool ok = false;
                    double sf = val.toDouble(&ok);
                    if (ok && sf > 0.0) {
                        m_scrollFactor = sf;
                    }
                }
            } else if (inMouseSection) {
                if (key.compare("LeftHanded", Qt::CaseInsensitive) == 0) {
                    m_leftHanded = (val.compare("true", Qt::CaseInsensitive) == 0);
                } else if (key.compare("cursorTheme", Qt::CaseInsensitive) == 0) {
                    m_cursorTheme = val;
                } else if (key.compare("cursorSize", Qt::CaseInsensitive) == 0) {
                    bool ok = false;
                    int sz = val.toInt(&ok);
                    if (ok && sz > 0) {
                        m_cursorSize = sz;
                    }
                }
            }
        }
    }

    // Check kdeglobals for system cursor size if not set in kcminputrc
    QString globalsPath = QDir::homePath() + "/.config/kdeglobals";
    QFile globalsFile(globalsPath);
    if (globalsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&globalsFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.startsWith("cursorSize=", Qt::CaseInsensitive)) {
                bool ok = false;
                int sz = line.mid(11).trimmed().toInt(&ok);
                if (ok && sz > 0) {
                    m_cursorSize = sz;
                }
            } else if (line.startsWith("cursorTheme=", Qt::CaseInsensitive)) {
                QString theme = line.mid(12).trimmed();
                if (!theme.isEmpty()) {
                    m_cursorTheme = theme;
                }
            }
        }
    }
}

void SystemInputSettings::applyEnvironmentOverrides()
{
    if (qEnvironmentVariableIsSet("RDP_NATURAL_SCROLL")) {
        QString val = qEnvironmentVariable("RDP_NATURAL_SCROLL").trimmed();
        m_naturalScroll = (val == "1" || val.compare("true", Qt::CaseInsensitive) == 0);
    }

    if (qEnvironmentVariableIsSet("RDP_LEFT_HANDED")) {
        QString val = qEnvironmentVariable("RDP_LEFT_HANDED").trimmed();
        m_leftHanded = (val == "1" || val.compare("true", Qt::CaseInsensitive) == 0);
    }

    if (qEnvironmentVariable("RDP_INVERT_SCROLL") == "1") {
        m_invertHScroll = !m_invertHScroll;
        m_invertVScroll = !m_invertVScroll;
    }
    if (qEnvironmentVariable("RDP_INVERT_HSCROLL") == "1") {
        m_invertHScroll = !m_invertHScroll;
    }
    if (qEnvironmentVariable("RDP_INVERT_VSCROLL") == "1") {
        m_invertVScroll = !m_invertVScroll;
    }

    if (qEnvironmentVariableIsSet("RDP_SCROLL_SCALE")) {
        bool ok = false;
        double scale = qEnvironmentVariable("RDP_SCROLL_SCALE").toDouble(&ok);
        if (ok && scale > 0.0) {
            m_hasCustomScale = true;
            m_customScale = scale;
        }
    }
}

uint32_t SystemInputSettings::mapPointerButton(uint32_t button) const
{
    if (!m_leftHanded) return button;
    if (button == BTN_LEFT) return BTN_RIGHT;
    if (button == BTN_RIGHT) return BTN_LEFT;
    return button;
}

double SystemInputSettings::effectiveScrollScale() const
{
    if (m_hasCustomScale) {
        return m_customScale;
    }
    return 0.125 * m_scrollFactor;
}

double SystemInputSettings::computeVerticalDelta(int16_t rawDelta) const
{
    // Base natural gesture (trackpad swipe up -> rawDelta > 0 -> scroll up dy < 0):
    double dy = -static_cast<double>(rawDelta) * effectiveScrollScale();

    if (!m_naturalScroll) {
        dy = -dy;
    }

    if (m_invertVScroll) {
        dy = -dy;
    }

    return dy;
}

double SystemInputSettings::computeHorizontalDelta(int16_t rawDelta) const
{
    // Base natural gesture (trackpad swipe right -> rawDelta > 0 -> scroll right dx > 0):
    double dx = static_cast<double>(rawDelta) * effectiveScrollScale();

    if (!m_naturalScroll) {
        dx = -dx;
    }

    if (m_invertHScroll) {
        dx = -dx;
    }

    return dx;
}
