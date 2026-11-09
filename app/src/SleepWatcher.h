// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QtQml/qqmlregistration.h>

// The system going to sleep and waking again, from logind on the system bus
// (a Flatpak needs --system-talk-name=org.freedesktop.login1). A delay lock
// holds sleep off for a moment after sleeping(), so the film can be paused
// and its place kept (and sent to a media server) first.
class SleepWatcher : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit SleepWatcher(QObject *parent = nullptr);

    // for tests: as if the system were going to sleep (true) or waking
    void simulate(bool sleeping);

signals:
    void sleeping();
    void woke();

private slots:
    void onPrepareForSleep(bool start);

private:
    void takeLock();

    QDBusUnixFileDescriptor m_lock; // sleep waits while it's open
};
