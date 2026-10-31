// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

// Keeps the screensaver off while `inhibited` is true, through
// org.freedesktop.ScreenSaver on the session bus. In Game Mode that service is
// owned by Steam, whose screensaver otherwise covers the film after a while.
class ScreenSaverInhibitor : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool inhibited READ inhibited WRITE setInhibited NOTIFY inhibitedChanged)

public:
    using QObject::QObject;
    ~ScreenSaverInhibitor() override;

    bool inhibited() const { return m_inhibited; }
    void setInhibited(bool inhibited);

signals:
    void inhibitedChanged();

private:
    void uninhibit();

    bool m_inhibited = false;
    uint m_cookie = 0;
    quint64 m_generation = 0; // drops Inhibit replies that arrive too late
};
