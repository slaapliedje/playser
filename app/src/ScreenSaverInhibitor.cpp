#include "ScreenSaverInhibitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {

QDBusMessage call(const QString &method)
{
    return QDBusMessage::createMethodCall("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                                          "org.freedesktop.ScreenSaver", method);
}

} // namespace

ScreenSaverInhibitor::~ScreenSaverInhibitor()
{
    uninhibit();
}

void ScreenSaverInhibitor::setInhibited(bool inhibited)
{
    if (inhibited == m_inhibited)
        return;
    m_inhibited = inhibited;
    emit inhibitedChanged();

    const quint64 generation = ++m_generation;
    if (!inhibited) {
        uninhibit();
        return;
    }
    QDBusMessage msg = call("Inhibit");
    msg << QStringLiteral("Disc Player") << QStringLiteral("Playing video");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<uint> reply = *w;
        if (reply.isError()) {
            qWarning("screensaver: Inhibit failed: %s", qPrintable(reply.error().message()));
            return;
        }
        if (generation == m_generation) {
            m_cookie = reply.value();
            return;
        }
        // switched off (or on again) while the call was out: release it
        QDBusMessage release = call("UnInhibit");
        release << reply.value();
        QDBusConnection::sessionBus().asyncCall(release);
    });
}

void ScreenSaverInhibitor::uninhibit()
{
    if (!m_cookie)
        return;
    QDBusMessage msg = call("UnInhibit");
    msg << m_cookie;
    m_cookie = 0;
    QDBusConnection::sessionBus().asyncCall(msg);
}
