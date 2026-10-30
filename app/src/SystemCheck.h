#pragma once

#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// What Blu-ray playback needs, for the setup screen. Never reads the MakeMKV
// key itself, only whether one is present.
class QProcess;

class SystemCheck : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // "flatpak" (com.makemkv.MakeMKV), "bundled", "system" or "" (none)
    Q_PROPERTY(QString makemkv READ makemkv NOTIFY changed)
    Q_PROPERTY(bool hasKey READ hasKey NOTIFY changed)
    // optical drives present / reachable through SCSI generic (/dev/sg*)
    Q_PROPERTY(int opticalDrives READ opticalDrives NOTIFY changed)
    Q_PROPERTY(bool scsiGeneric READ scsiGeneric NOTIFY changed)
    // everything Blu-ray playback needs is in place
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    // audio passthrough: our PipeWire rule is installed / was installed since
    // the last restart, so it isn't active yet (see setPassthroughRule)
    Q_PROPERTY(bool passthroughRule READ passthroughRule NOTIFY changed)
    Q_PROPERTY(bool passthroughRulePending READ passthroughRulePending NOTIFY changed)
    // 4K UHD needs a drive MakeMKV can put in LibreDrive mode. Per drive
    // ("/dev/sr0"): { name, state, version }, state "yes" | "no" | "checking"
    // | "unknown" (no Blu-ray in it yet to check with)
    Q_PROPERTY(QVariantMap uhdDrives READ uhdDrives NOTIFY changed)

public:
    explicit SystemCheck(QObject *parent = nullptr);

    QString makemkv() const { return m_makemkv; }
    bool hasKey() const { return m_hasKey; }
    int opticalDrives() const { return m_opticalDrives; }
    bool scsiGeneric() const { return m_scsiGeneric; }
    bool ready() const { return !m_makemkv.isEmpty() && m_hasKey && (m_opticalDrives == 0 || m_scsiGeneric); }

    bool passthroughRule() const { return m_passthroughRule; }
    bool passthroughRulePending() const { return m_passthroughRulePending; }
    QVariantMap uhdDrives() const { return m_uhdDrives; }

    Q_INVOKABLE void refresh();
    // installs or removes the rule; false if the file couldn't be written
    Q_INVOKABLE bool setPassthroughRule(bool installed);
    // asks MakeMKV about drives not checked before that have a disc in;
    // the answer is kept per drive model and firmware
    Q_INVOKABLE void checkUhdDrives();
    // before playback: MakeMKV can't share the drive with a check
    Q_INVOKABLE void stopUhdChecks();

signals:
    void changed();

private:
    void updateUhdDrives();
    void probeUhd(const QString &device, const QString &key);

    QString m_makemkv;
    bool m_hasKey = false;
    int m_opticalDrives = 0;
    bool m_scsiGeneric = false;
    bool m_passthroughRule = false;
    bool m_passthroughRulePending = false;
    QVariantMap m_uhdDrives;
    QHash<QString, QProcess *> m_probes; // device -> running makemkvcon
};
