#include "SystemCheck.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <fcntl.h>
#include <unistd.h>

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
    emit changed();
}
