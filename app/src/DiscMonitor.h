#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QTimer>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

// Optical drives and their discs, from udisks2 over the system bus (works
// from a Flatpak with --system-talk-name=org.freedesktop.UDisks2).
//
// Each entry of `drives` is a map:
//   path, model, device ("/dev/sr0"), hasMedia, kind ("dvd"|"bluray"|"cd"|
//   "none"), label, url (mpv URL to play the disc), options (mpv options).
class DiscMonitor : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QVariantList drives READ drives NOTIFY drivesChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)

public:
    explicit DiscMonitor(QObject *parent = nullptr);

    QVariantList drives() const { return m_drives; }
    bool available() const { return m_available; }

    Q_INVOKABLE void refresh();

signals:
    void drivesChanged();
    void availableChanged();

private slots:
    void onInterfacesChanged();
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed,
                             const QStringList &invalidated);

private:
    void scheduleRefresh();

    QVariantList m_drives;
    bool m_available = false;
    QTimer m_debounce;
};
