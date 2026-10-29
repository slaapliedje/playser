#include "DiscMonitor.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPointer>
#include <QThreadPool>

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

        // block device, label, object path and mount point per drive
        struct Block { QString device, label, path, mountPoint; };
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
                                       firstMountPoint(it.value().value(kFsIface).value("MountPoints")) });
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
            const QString kind = discKind(drive);

            QVariantMap d;
            d["path"] = path;
            d["model"] = QStringList{ drive.value("Vendor").toString().simplified(),
                                      drive.value("Model").toString().simplified() }
                             .join(' ').trimmed();
            d["device"] = device;
            d["hasMedia"] = drive.value("MediaAvailable").toBool();
            d["kind"] = kind;
            d["label"] = block.label;

            Identity id;
            if ((kind == "bluray" || kind == "dvd") && !block.path.isEmpty()) {
                present.insert(block.path);
                id = m_identities.value(block.path);
                if (block.mountPoint.isEmpty())
                    mount(block.path);
                else if (id.mountPoint != block.mountPoint)
                    identify(block.path, block.mountPoint);
                id = m_identities.value(block.path);
                d["identifying"] = id.pending;
                d["disc"] = id.disc;
                d["contentHash"] = id.contentHash;
            }
            const QString main = id.disc.value("main").toString();

            if (kind == "bluray") {
                // "01199.mpls" -> bd://mpls/1199
                d["url"] = main.endsWith(".mpls") ? QStringLiteral("bd://mpls/%1").arg(main.chopped(5).toInt())
                                                  : QStringLiteral("bd://");
                d["options"] = QVariantMap{ { "bluray-device", device } };
            } else if (kind == "dvd") {
                d["url"] = "dvd://";
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
    m_identities[blockPath] = Identity{};
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

void DiscMonitor::identify(const QString &blockPath, const QString &mountPoint)
{
    Identity &id = m_identities[blockPath];
    id = Identity{};
    id.mountPoint = mountPoint;
    // the first directory read can spin the disc up: keep it off the GUI thread
    QPointer<DiscMonitor> self(this);
    QThreadPool::globalInstance()->start([self, blockPath, mountPoint] {
        const QString hash = DiscIndex::contentHash(mountPoint);
        QVariantMap disc = hash.isEmpty() ? QVariantMap() : DiscIndex::lookup(hash);
        if (!disc.isEmpty())
            DiscIndex::resolvePlaylists(disc, DiscIndex::playlists(mountPoint));
        QMetaObject::invokeMethod(self, [self, blockPath, mountPoint, hash, disc] {
            if (!self)
                return;
            Identity &id = self->m_identities[blockPath];
            if (id.mountPoint != mountPoint) // ejected or remounted meanwhile
                return;
            id.pending = false;
            id.contentHash = hash;
            id.disc = disc;
            qInfo("disc %s: %s %s", qPrintable(mountPoint), qPrintable(hash),
                  disc.isEmpty() ? "(not in index)" : qPrintable(disc.value("title").toString()));
            self->refresh();
        });
    });
}
