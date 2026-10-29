#include "SystemCheck.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
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
    return QDir::home().filePath(".config/pipewire/pipewire-pulse.conf.d/90-discplayer-passthrough.conf");
}

const char kPassthroughRule[] =
    "# Disc Player: keep speech-dispatcher's silent dummy stream from holding the audio\n"
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
