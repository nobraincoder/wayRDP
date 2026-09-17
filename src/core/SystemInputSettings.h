#pragma once

#include <QObject>
#include <QString>
#include <QFileSystemWatcher>
#include <cstdint>

class SystemInputSettings : public QObject
{
    Q_OBJECT
public:
    explicit SystemInputSettings(QObject *parent = nullptr);
    ~SystemInputSettings() override = default;

    void reload();

    bool isNaturalScroll() const { return m_naturalScroll; }
    double scrollFactor() const { return m_scrollFactor; }
    double effectiveScrollScale() const;

    bool isLeftHanded() const { return m_leftHanded; }
    uint32_t mapPointerButton(uint32_t button) const;

    QString cursorTheme() const { return m_cursorTheme; }
    int cursorSize() const { return m_cursorSize; }

    // Translates FreeRDP rawDelta (positive for wheel up / right, negative for wheel down / left)
    // into Wayland (libei) axis deltas (positive y = down, positive x = right).
    double computeVerticalDelta(int16_t rawDelta) const;
    double computeHorizontalDelta(int16_t rawDelta) const;

signals:
    void settingsChanged();

private:
    void queryKWinDBus();
    void parseConfigFile();
    void applyEnvironmentOverrides();

    bool m_naturalScroll{true};
    double m_scrollFactor{1.0};
    bool m_invertHScroll{false};
    bool m_invertVScroll{false};
    bool m_hasCustomScale{false};
    double m_customScale{10.0 / 120.0};

    bool m_leftHanded{false};
    QString m_cursorTheme{"breeze_cursors"};
    int m_cursorSize{24};

    QFileSystemWatcher *m_watcher{nullptr};
};
