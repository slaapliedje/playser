// SPDX-License-Identifier: GPL-3.0-or-later
#include "DiscMonitor.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPointer>
#include <QThreadPool>

#include "DiscFs.h"
#include "DiscIndex.h"

namespace {

const QString kService = QStringLiteral("org.freedesktop.UDisks2");
const QString kRoot = QStringLiteral("/org/freedesktop/UDisks2");
const QString kDriveIface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString kBlockIface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString kFsIface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");

using InterfaceMap = QMap<QString, QVariantMap>;
using ObjectMap = QMap<QDBusObjectPath, InterfaceMap>;

// "ay" device paths come back NUL-terminated
QString byteString(const QVariant &v)
{
    QByteArray b = v.toByteArray();
    if (b.endsWith('\0'))
        b.chop(1);
    return QString::fromLocal8Bit(b);
}

QString objectPath(const QVariant &v)
{
    if (v.canConvert<QDBusObjectPath>())
        return v.value<QDBusObjectPath>().path();
    if (v.canConvert<QDBusArgument>()) {
        QDBusObjectPath p;
        v.value<QDBusArgument>() >> p;
        return p.path();
    }
    return v.toString();
}

// Filesystem.MountPoints is "aay": NUL-terminated paths
QString firstMountPoint(const QVariant &v)
{
    if (!v.canConvert<QDBusArgument>())
        return {};
    const QDBusArgument arg = v.value<QDBusArgument>();
    QString first;
    arg.beginArray();
    while (!arg.atEnd()) {
        QByteArray b;
        arg >> b;
        if (b.endsWith('\0'))
            b.chop(1);
        if (first.isEmpty())
            first = QString::fromLocal8Bit(b);
    }
    arg.endArray();
    return first;
}

QString discKind(const QVariantMap &drive)
{
    if (!drive.value("MediaAvailable").toBool())
        return "none";
    const QString media = drive.value("Media").toString();
    if (media.startsWith("optical_bd"))
        return "bluray";
    if (media.startsWith("optical_dvd") || media.startsWith("optical_hddvd"))
        return "dvd";
    if (media.startsWith("optical_cd") || media.startsWith("optical_mo"))
        return "cd";
    // some drives report media only after a read; treat unknown as data
    return media.isEmpty() ? "unknown" : media;
}

} // namespace

DiscMonitor::DiscMonitor(QObject *parent) : QObject(parent)
{
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ObjectMap>();

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, &DiscMonitor::refresh);

    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(kService, kRoot, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                this, SLOT(onInterfacesChanged()));
    bus.connect(kService, kRoot, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                this, SLOT(onInterfacesChanged()));
    // any object path: media changes arrive as Drive property changes
    bus.connect(kService, QString(), "org.freedesktop.DBus.Properties", "PropertiesChanged",
                this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));

    refresh();
}

void DiscMonitor::scheduleRefresh()
{
    m_debounce.start();
}

void DiscMonitor::onInterfacesChanged()
{
    scheduleRefresh();
}

void DiscMonitor::onPropertiesChanged(const QString &iface, const QVariantMap &,
                                      const QStringList &)
{
    if (iface == kDriveIface || iface == kBlockIface || iface == kFsIface)
        scheduleRefresh();
}

void DiscMonitor::refresh()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        kService, kRoot, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<ObjectMap> reply = *w;
        const bool ok = !reply.isError();
        if (ok != m_available) {
            m_available = ok;
            emit availableChanged();
        }
        if (!ok) {
            qWarning("udisks2 unavailable: %s", qPrintable(reply.error().message()));
            return;
        }
        const ObjectMap objects = reply.value();

        // block device, label, object path, mount point and which disc per drive
        struct Block { QString device, label, path, mountPoint, discId; qulonglong size = 0; };
        QHash<QString, Block> blocks;
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const QVariantMap block = it.value().value(kBlockIface);
            if (block.isEmpty())
                continue;
            const QString drive = objectPath(block.value("Drive"));
            if (drive.isEmpty() || drive == "/")
                continue;
            // prefer the whole-disc block (no partition table on discs)
            if (!blocks.contains(drive))
                blocks.insert(drive, { byteString(block.value("Device")),
                                       block.value("IdLabel").toString(), it.key().path(),
                                       firstMountPoint(it.value().value(kFsIface).value("MountPoints")),
                                       QStringList{ block.value("IdLabel").toString(), block.value("IdUUID").toString(),
                                                    block.value("Size").toString() }.join('|'),
                                       block.value("Size").toULongLong() });
        }

        QVariantList drives;
        QSet<QString> present;
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const QVariantMap drive = it.value().value(kDriveIface);
            if (drive.isEmpty() || !drive.value("Optical").toBool() &&
                                       !drive.value("MediaCompatibility").toStringList().join(',').contains("optical"))
                continue;
            const QString path = it.key().path();
            const Block block = blocks.value(path);
            const QString &device = block.device;
            // a disc taken out while a player holds the drive open keeps
            // MediaAvailable (and Media) set; only the device's size drops
            const bool ejected = !block.path.isEmpty() && block.size == 0;
            const QString kind = ejected ? QStringLiteral("none") : discKind(drive);

            QVariantMap d;
            d["path"] = path;
            d["model"] = QStringList{ drive.value("Vendor").toString().simplified(),
                                      drive.value("Model").toString().simplified() }
                             .join(' ').trimmed();
            d["device"] = device;
            d["hasMedia"] = drive.value("MediaAvailable").toBool() && !ejected;
            d["kind"] = kind;
            d["label"] = block.label;
            d["block"] = block.path;
            d["mountPoint"] = block.mountPoint;

            Identity id;
            if ((kind == "bluray" || kind == "dvd") && !block.path.isEmpty()) {
                present.insert(block.path);
                id = m_identities.value(block.path);
                // read from the device; mounted only if that fails
                if (!m_identities.contains(block.path) || id.discId != block.discId)
                    identify(block.path, device, block.discId, QString());
                else if (id.needsMount && block.mountPoint.isEmpty())
                    mount(block.path);
                else if (id.needsMount && id.mountPoint != block.mountPoint)
                    identify(block.path, device, block.discId, block.mountPoint);
                id = m_identities.value(block.path);
                d["identifying"] = id.pending;
                d["disc"] = id.disc;
                d["contentHash"] = id.contentHash;
            }
            // the catalogue's main feature, else mpv's choice (Blu-ray: the
            // first title; DVD: the longest)
            const QString main = id.disc.value("mainUrl").toString();

            if (kind == "bluray") {
                d["url"] = main.startsWith("bd://") ? main : QStringLiteral("bd://");
                d["options"] = QVariantMap{ { "bluray-device", device } };
            } else if (kind == "dvd") {
                d["url"] = main.startsWith("dvd://") ? main : QStringLiteral("dvd://");
                d["options"] = QVariantMap{ { "dvd-device", device } };
            }
            drives.append(d);
        }
        // forget discs that were ejected
        for (auto it = m_identities.begin(); it != m_identities.end();)
            it = present.contains(it.key()) ? std::next(it) : m_identities.erase(it);
        for (auto it = m_mountRequested.begin(); it != m_mountRequested.end();)
            it = present.contains(*it) ? std::next(it) : m_mountRequested.erase(it);

        std::sort(drives.begin(), drives.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value("device").toString() < b.toMap().value("device").toString();
        });
        if (drives != m_drives) {
            m_drives = drives;
            emit drivesChanged();
        }
    });
}

void DiscMonitor::mount(const QString &blockPath)
{
    if (m_mountRequested.contains(blockPath))
        return;
    m_mountRequested.insert(blockPath);
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, blockPath, kFsIface, "Mount");
    msg << QVariantMap{ { "options", QStringLiteral("ro") } };
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, blockPath](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) {
            // not identifiable, but still playable
            qWarning("udisks: mounting %s failed: %s", qPrintable(blockPath), qPrintable(reply.error().message()));
            m_identities[blockPath].pending = false;
        }
        scheduleRefresh(); // the new mount point arrives with the refresh
    });
}

void DiscMonitor::ensureMounted(const QVariantMap &drive)
{
    const QString device = drive.value("device").toString();
    const QString mountPoint = drive.value("mountPoint").toString();
    const QString blockPath = drive.value("block").toString();
    if (!mountPoint.isEmpty() || blockPath.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, device, mountPoint] {
            if (mountPoint.isEmpty())
                emit mountFailed(device);
            else
                emit mounted(device, mountPoint);
        }, Qt::QueuedConnection);
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, blockPath, kFsIface, "Mount");
    msg << QVariantMap{ { "options", QStringLiteral("ro") } };
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, device](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) {
            qWarning("udisks: mounting %s failed: %s", qPrintable(device), qPrintable(reply.error().message()));
            emit mountFailed(device);
        } else {
            emit mounted(device, reply.value());
        }
        scheduleRefresh();
    });
}

void DiscMonitor::identify(const QString &blockPath, const QString &device, const QString &discId,
                           const QString &mountPoint)
{
    Identity &id = m_identities[blockPath];
    const bool needsMount = id.discId == discId && id.needsMount;
    id = Identity{};
    id.discId = discId;
    id.mountPoint = mountPoint;
    id.needsMount = needsMount;
    if (!needsMount)
        m_mountRequested.remove(blockPath); // a new disc may need its own mount
    // the first read can spin the disc up: keep it off the GUI thread
    QPointer<DiscMonitor> self(this);
    QThreadPool::globalInstance()->start([self, blockPath, device, discId, mountPoint] {
        // straight from the drive (UDF), so the disc needn't be mounted;
        // through the mount when that's been needed
        const std::unique_ptr<DiscFs> fs = DiscFs::open(mountPoint.isEmpty() ? device : mountPoint);
        if (!fs) {
            QMetaObject::invokeMethod(self, [self, blockPath, device, discId, mountPoint] {
                if (!self || self->m_identities.value(blockPath).discId != discId)
                    return;
                Identity &id = self->m_identities[blockPath];
                if (!mountPoint.isEmpty()) { // mounted and still unreadable
                    qWarning("disc %s: can't read %s", qPrintable(device), qPrintable(mountPoint));
                    id.pending = false;
                    self->refresh();
                    return;
                }
                qInfo("disc %s: can't read the device, reading it mounted", qPrintable(device));
                id.needsMount = true;
                self->refresh();
            });
            return;
        }
        const QString hash = DiscIndex::contentHash(*fs);
        QVariantMap disc = hash.isEmpty() ? QVariantMap() : DiscIndex::lookup(hash);
        const bool known = !disc.isEmpty();
        if (known)
            DiscIndex::resolveTitles(disc, *fs);
        // not in the catalogue: the title the disc gives itself (Blu-ray);
        // its art either way
        const QVariantMap meta = DiscIndex::discMeta(*fs);
        if (!known && meta.contains("title"))
            disc.insert("title", meta.value("title"));
        if (meta.contains("art"))
            disc.insert("art", meta.value("art"));
        if (!disc.contains("format") && meta.contains("format"))
            disc.insert("format", meta.value("format"));
        if (meta.contains("stereo"))
            disc.insert("stereo", true);
        const QString source = fs->source();
        QMetaObject::invokeMethod(self, [self, blockPath, discId, mountPoint, source, hash, disc, known] {
            if (!self)
                return;
            Identity &id = self->m_identities[blockPath];
            if (id.discId != discId || id.mountPoint != mountPoint) // changed meanwhile
                return;
            id.pending = false;
            id.contentHash = hash;
            id.disc = disc;
            const QString title = disc.value("title").toString();
            const QString what = known ? title
                : title.isEmpty() ? QStringLiteral("(not in index)")
                                  : QStringLiteral("(not in index; disc says \"%1\")").arg(title);
            const QString main = disc.value("mainUrl").toString();
            qInfo("disc %s: %s %s%s", qPrintable(source), qPrintable(hash), qPrintable(what),
                  main.isEmpty() ? "" : qPrintable(", main feature " + main));
            self->refresh();
        });
    });
}
