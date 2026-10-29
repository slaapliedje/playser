#include "DiscMonitor.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {

const QString kService = QStringLiteral("org.freedesktop.UDisks2");
const QString kRoot = QStringLiteral("/org/freedesktop/UDisks2");
const QString kDriveIface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString kBlockIface = QStringLiteral("org.freedesktop.UDisks2.Block");

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
    if (iface == kDriveIface || iface == kBlockIface)
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

        // block device + label per drive
        QHash<QString, QPair<QString, QString>> blocks;
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
                                       block.value("IdLabel").toString() });
        }

        QVariantList drives;
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const QVariantMap drive = it.value().value(kDriveIface);
            if (drive.isEmpty() || !drive.value("Optical").toBool() &&
                                       !drive.value("MediaCompatibility").toStringList().join(',').contains("optical"))
                continue;
            const QString path = it.key().path();
            const auto [device, label] = blocks.value(path);
            const QString kind = discKind(drive);

            QVariantMap d;
            d["path"] = path;
            d["model"] = QStringList{ drive.value("Vendor").toString().simplified(),
                                      drive.value("Model").toString().simplified() }
                             .join(' ').trimmed();
            d["device"] = device;
            d["hasMedia"] = drive.value("MediaAvailable").toBool();
            d["kind"] = kind;
            d["label"] = label;
            if (kind == "bluray") {
                d["url"] = "bd://";
                d["options"] = QVariantMap{ { "bluray-device", device } };
            } else if (kind == "dvd") {
                d["url"] = "dvd://";
                d["options"] = QVariantMap{ { "dvd-device", device } };
            }
            drives.append(d);
        }
        std::sort(drives.begin(), drives.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value("device").toString() < b.toMap().value("device").toString();
        });
        if (drives != m_drives) {
            m_drives = drives;
            emit drivesChanged();
        }
    });
}
