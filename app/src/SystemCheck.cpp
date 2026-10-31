// SPDX-License-Identifier: GPL-3.0-or-later
#include "SystemCheck.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <fcntl.h>
#include <unistd.h>

namespace {

// Passthrough needs the audio output to itself (WirePlumber won't link it
// "exclusively" next to another stream), and SteamOS keeps speech-
// dispatcher's silent dummy module connected to it. This keeps only that
// stream unconnected; real voices (screen readers) are unaffected. The host
// path: the Flatpak grants just this folder.
QString passthroughRulePath()
{
    return QDir::home().filePath(".config/pipewire/pipewire-pulse.conf.d/90-playser-passthrough.conf");
}

const char kPassthroughRule[] =
    "# Playser: keep speech-dispatcher's silent dummy stream from holding the audio\n"
    "# output, so Dolby/DTS passthrough can use it exclusively. Delete to undo.\n"
    "pulse.rules = [\n"
    "    {\n"
    "        matches = [ { application.process.binary = \"sd_dummy\" } ]\n"
    "        actions = { update-props = { node.autoconnect = false } }\n"
    "    }\n"
    "]\n";

// changes with every boot; PipeWire reads the rule when it starts
QByteArray bootId()
{
    QFile f("/proc/sys/kernel/random/boot_id");
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
}

QString sysfsValue(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).simplified() : QString();
}

// MakeMKV's DRV flags: Blu-ray files present
constexpr int kDrvBluray = 4;

} // namespace

SystemCheck::SystemCheck(QObject *parent) : QObject(parent)
{
    refresh();
}

void SystemCheck::refresh()
{
    // setupDecryption() (main.cpp) chose a libmmbd at startup
    const QString aacs = qEnvironmentVariable("LIBAACS_PATH");
    m_makemkv = aacs.isEmpty() ? QString()
              : aacs.contains("com.makemkv.MakeMKV") ? QStringLiteral("flatpak")
              : aacs.startsWith("/app/") ? QStringLiteral("bundled")
                                         : QStringLiteral("system");

    // MakeMKV keeps the key as app_Key in ~/.MakeMKV/settings.conf; only
    // check that the setting has a value
    m_hasKey = false;
    QFile settings(QDir::home().filePath(".MakeMKV/settings.conf"));
    if (settings.open(QIODevice::ReadOnly)) {
        while (!settings.atEnd()) {
            const QByteArray line = settings.readLine().trimmed();
            if (!line.startsWith("app_Key"))
                continue;
            const int eq = line.indexOf('=');
            const QByteArray value = eq < 0 ? QByteArray() : line.mid(eq + 1).trimmed();
            m_hasKey = value.size() > 2; // more than ""
            break;
        }
    }

    // optical drives are SCSI type 5; MakeMKV talks to them through their
    // generic device, which needs the sg module (not loaded on SteamOS)
    m_opticalDrives = 0;
    m_scsiGeneric = true;
    const QDir devices("/sys/class/block");
    for (const QString &name : devices.entryList({ "sr*" }, QDir::Dirs | QDir::System)) {
        m_opticalDrives++;
        const QDir generic(devices.filePath(name + "/device/scsi_generic"));
        const QStringList sg = generic.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (sg.isEmpty()) {
            m_scsiGeneric = false;
            continue;
        }
        const int fd = ::open(QFile::encodeName("/dev/" + sg.first()).constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            m_scsiGeneric = false;
        else
            ::close(fd);
    }
    if (m_opticalDrives == 0)
        m_scsiGeneric = false;

    updateUhdDrives();

    m_passthroughRule = QFileInfo::exists(passthroughRulePath());
    m_passthroughRulePending = m_passthroughRule
        && QSettings().value("audio/passthroughRuleBoot").toByteArray() == bootId();
    emit changed();
}

bool SystemCheck::setPassthroughRule(bool installed)
{
    const QString path = passthroughRulePath();
    bool ok = true;
    if (!installed) {
        ok = !QFileInfo::exists(path) || QFile::remove(path);
    } else if (!QFileInfo::exists(path)) {
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly) && f.write(kPassthroughRule) == qint64(sizeof kPassthroughRule - 1);
        if (ok)
            QSettings().setValue("audio/passthroughRuleBoot", bootId());
    }
    if (!ok)
        qWarning("audio: couldn't %s %s", installed ? "write" : "remove", qPrintable(path));
    refresh();
    return ok;
}

// A drive's answer can't change without new firmware, so it's kept under
// vendor, model and firmware revision.
void SystemCheck::updateUhdDrives()
{
    m_uhdDrives.clear();
    const QSettings settings;
    const QDir devices("/sys/class/block");
    for (const QString &name : devices.entryList({ "sr*" }, QDir::Dirs | QDir::System)) {
        const QString dev = devices.filePath(name + "/device/");
        const QString drive = sysfsValue(dev + "vendor") + ' ' + sysfsValue(dev + "model");
        const QString key = QString(drive + ' ' + sysfsValue(dev + "rev")).replace('/', '_');
        const QString device = "/dev/" + name;
        const QString state = settings.value("uhd/" + key + "/state").toString();
        m_uhdDrives.insert(device, QVariantMap{
            { "name", drive },
            { "key", key },
            { "state", m_probes.contains(device) ? QStringLiteral("checking")
                       : state.isEmpty() ? QStringLiteral("unknown") : state },
            { "version", settings.value("uhd/" + key + "/version").toString() },
        });
    }
}

void SystemCheck::checkUhdDrives()
{
    if (m_makemkv.isEmpty() || !m_scsiGeneric)
        return;
    bool started = false;
    for (auto it = m_uhdDrives.cbegin(); it != m_uhdDrives.cend(); ++it) {
        const QVariantMap drive = it.value().toMap();
        const QString name = it.key().section('/', -1);
        // only drives with a disc in; the size is 0 without one
        if (drive.value("state").toString() != u"unknown" || sysfsValue("/sys/class/block/" + name + "/size").toLongLong() == 0)
            continue;
        probeUhd(it.key(), drive.value("key").toString());
        started = true;
    }
    if (started) {
        updateUhdDrives();
        emit changed();
    }
}

void SystemCheck::stopUhdChecks()
{
    for (QProcess *p : std::as_const(m_probes))
        p->terminate();
}

// makemkvcon reports "Using LibreDrive mode" (MSG:1011) as it opens the
// disc, before "Using direct disc access mode" (MSG:3007), which comes
// either way; stop it there rather than wait out the whole scan.
void SystemCheck::probeUhd(const QString &device, const QString &key)
{
    auto *proc = new QProcess(this);
    m_probes.insert(device, proc);
    proc->setProgram(qEnvironmentVariable("MAKEMKVCON", QStringLiteral("makemkvcon")));
    proc->setArguments({ "-r", "--cache=1", "info", "dev:" + device });
    proc->setStandardErrorFile(QProcess::nullDevice());

    struct Probe { QString version; bool bluray = false; bool done = false; };
    auto probe = std::make_shared<Probe>();
    auto finish = [this, proc, probe, device, key](const QString &state) {
        if (probe->done)
            return;
        probe->done = true;
        if (!state.isEmpty()) {
            QSettings settings;
            settings.setValue("uhd/" + key + "/state", state);
            settings.setValue("uhd/" + key + "/version", probe->version);
            qInfo("drive %s (%s): %s", qPrintable(device), qPrintable(key),
                  state == u"yes" ? qPrintable("LibreDrive " + probe->version) : "no LibreDrive");
        }
        if (proc->state() != QProcess::NotRunning) {
            proc->terminate();
            QTimer::singleShot(3000, proc, [proc] { proc->kill(); });
        }
    };

    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, probe, device, finish] {
        while (proc->canReadLine()) {
            const QByteArray line = proc->readLine().trimmed();
            if (line.startsWith("DRV:")) {
                // DRV:index,visible,enabled,flags,"drive","disc","device"
                const QList<QByteArray> f = line.mid(4).split(',');
                if (f.size() >= 7 && f.last() == '"' + device.toUtf8() + '"')
                    probe->bluray = f.value(3).toInt() & kDrvBluray;
            } else if (line.startsWith("MSG:1011,")) {
                static const QRegularExpression version("LibreDrive mode \\(v([0-9.]+)");
                probe->version = version.match(QString::fromUtf8(line)).captured(1);
            } else if (line.startsWith("MSG:3007,")) {
                // without Blu-ray files (a DVD) LibreDrive may not show:
                // no answer then rather than a wrong one
                finish(!probe->bluray ? QString() : probe->version.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"));
            }
        }
    });
    connect(proc, &QProcess::finished, this, [this, proc, device, finish] {
        finish(QString()); // ended before saying: no answer
        m_probes.remove(device);
        proc->deleteLater();
        updateUhdDrives();
        emit changed();
    });
    connect(proc, &QProcess::errorOccurred, this, [proc](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit proc->finished(-1, QProcess::CrashExit);
    });
    QTimer::singleShot(90000, proc, [finish] { finish(QString()); });
    proc->start();
}
