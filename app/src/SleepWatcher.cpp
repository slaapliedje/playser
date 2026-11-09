// SPDX-License-Identifier: GPL-3.0-or-later
#include "SleepWatcher.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTimer>

namespace {

const QString kService = QStringLiteral("org.freedesktop.login1");
const QString kPath = QStringLiteral("/org/freedesktop/login1");
const QString kManager = QStringLiteral("org.freedesktop.login1.Manager");

// time for a media server to hear about the pause before sleep goes ahead
// (logind waits at most InhibitDelayMaxSec, 5 s by default)
constexpr int kReleaseDelayMs = 1000;

} // namespace

SleepWatcher::SleepWatcher(QObject *parent)
    : QObject(parent)
{
    QDBusConnection::systemBus().connect(kService, kPath, kManager, "PrepareForSleep", this,
                                         SLOT(onPrepareForSleep(bool)));
    takeLock();
}

void SleepWatcher::takeLock()
{
    if (m_lock.isValid())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kManager, "Inhibit");
    msg << QStringLiteral("sleep") << QStringLiteral("Playser")
        << QStringLiteral("Pausing the film and keeping its place") << QStringLiteral("delay");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
        if (reply.isError()) { // sleep still pauses the film, just without the moment's grace
            qWarning("sleep: no delay lock: %s", qPrintable(reply.error().message()));
            return;
        }
        m_lock = reply.value();
    });
}

void SleepWatcher::onPrepareForSleep(bool start)
{
    simulate(start);
}

void SleepWatcher::simulate(bool sleeping)
{
    if (sleeping) {
        qInfo("sleep: going to sleep");
        emit this->sleeping();
        QTimer::singleShot(kReleaseDelayMs, this, [this] { m_lock = QDBusUnixFileDescriptor(); });
    } else {
        qInfo("sleep: woke");
        takeLock();
        emit woke();
    }
}
