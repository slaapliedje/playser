// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

// Optical drives and their discs, from udisks2 over the system bus (works
// from a Flatpak with --system-talk-name=org.freedesktop.UDisks2).
//
// Each entry of `drives` is a map:
//   path, model, device ("/dev/sr0"), hasMedia, kind ("dvd"|"bluray"|"cd"|
//   "none"), label, url (mpv URL to play the disc), options (mpv options),
//   identifying (still working out which disc it is), disc (DiscIndex entry,
//   empty if unknown), contentHash, block (udisks object path), mountPoint
//   ("" if not mounted).
//
// To identify a DVD or Blu-ray it is mounted read-only (as a desktop would)
// and looked up in DiscIndex. A known Blu-ray's url then names its main
// playlist, which matters on discs with hundreds of decoy playlists.
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
    // Mounts the drive's disc read-only if it isn't, then mounted() (or
    // mountFailed()). Blu-ray menus need it: libbluray's BD-J can't read
    // everything a disc's Java wants straight from the device (UDF).
    Q_INVOKABLE void ensureMounted(const QVariantMap &drive);

signals:
    void drivesChanged();
    void availableChanged();
    void mounted(const QString &device, const QString &mountPoint);
    void mountFailed(const QString &device);

private slots:
    void onInterfacesChanged();
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed,
                             const QStringList &invalidated);

private:
    struct Identity {
        QString discId;     // which disc (label, UUID, size): a new one is identified
        QString mountPoint; // read through when the device couldn't be
        bool needsMount = false;
        bool pending = true;
        QString contentHash;
        QVariantMap disc;
    };

    void scheduleRefresh();
    void mount(const QString &blockPath);
    void identify(const QString &blockPath, const QString &device, const QString &discId,
                  const QString &mountPoint);

    QHash<QString, Identity> m_identities; // by block object path
    QSet<QString> m_mountRequested;

    QVariantList m_drives;
    bool m_available = false;
    QTimer m_debounce;
};
