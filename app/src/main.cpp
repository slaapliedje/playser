#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <clocale>

#include "Gamepad.h"
#include "Mpv.h"
#include "UiRenderer.h"

// MakeMKV's libmmbd stands in for libaacs/libbdplus (docs/spike-results.md,
// 0a). Prefer an installed MakeMKV Flatpak (com.makemkv.MakeMKV), so its
// updates reach us without rebuilding, then the copy bundled in ours, then a
// host install. An explicit LIBAACS_PATH wins.
static void setupDecryption()
{
    if (qEnvironmentVariableIsSet("LIBAACS_PATH"))
        return;
    const QString flatpakApp = QStringLiteral("app/com.makemkv.MakeMKV/current/active/files");
    const QStringList roots = {
        QDir::home().filePath(".local/share/flatpak/" + flatpakApp),
        "/var/lib/flatpak/" + flatpakApp,
        "/app", "/usr", "/usr/local",
    };
    for (const QString &root : roots) {
        const QString lib = root + "/lib";
        const QString con = root + "/bin/makemkvcon";
        if (!QFileInfo::exists(lib + "/libmmbd.so.0"))
            continue;
        // libbluray appends ".so.0" itself
        qputenv("LIBAACS_PATH", QFile::encodeName(lib + "/libmmbd"));
        qputenv("LIBBDPLUS_PATH", QFile::encodeName(lib + "/libmmbd"));
        // libmmbd only looks for its helper in /usr/bin and /usr/local/bin
        if (!qEnvironmentVariableIsSet("MAKEMKVCON") && QFileInfo::exists(con))
            qputenv("MAKEMKVCON", QFile::encodeName(con));
        // makemkvcon (a child process) must load its own libmakemkv/libdriveio
        // rather than another copy's; this process has already started
        const QByteArray path = qgetenv("LD_LIBRARY_PATH");
        qputenv("LD_LIBRARY_PATH", QFile::encodeName(lib) + (path.isEmpty() ? "" : ":" + path));
        qInfo("decryption: MakeMKV from %s", qPrintable(root));
        return;
    }
}

// mpv key names (from its window) -> the Qt keys the QML UI handles
static int qtKeyForMpv(const QString &name)
{
    static const QHash<QString, int> keys = {
        { "UP", Qt::Key_Up }, { "DOWN", Qt::Key_Down }, { "LEFT", Qt::Key_Left },
        { "RIGHT", Qt::Key_Right }, { "ENTER", Qt::Key_Return }, { "KP_ENTER", Qt::Key_Return },
        { "ESC", Qt::Key_Escape }, { "BS", Qt::Key_Escape }, { "SPACE", Qt::Key_Space },
        { "PGUP", Qt::Key_PageUp }, { "PGDWN", Qt::Key_PageDown }, { "MENU", Qt::Key_Menu },
        { "i", Qt::Key_I }, { "m", Qt::Key_Menu },
    };
    return keys.value(name);
}

// --script "2,down,enter,3,shot,quit": numbers wait (seconds), names press
// keys, "shot" saves the UI layer and an mpv window screenshot, "quit"
// exits. For headless UI tests.
static void runScript(UiRenderer *ui, Mpv *mpv, QStringList steps, const QString &shotDir, int shot = 0)
{
    static const QHash<QString, int> keys = {
        { "up", Qt::Key_Up }, { "down", Qt::Key_Down }, { "left", Qt::Key_Left },
        { "right", Qt::Key_Right }, { "enter", Qt::Key_Return }, { "back", Qt::Key_Escape },
        { "menu", Qt::Key_Menu }, { "info", Qt::Key_I }, { "pgup", Qt::Key_PageUp },
        { "pgdn", Qt::Key_PageDown }, { "space", Qt::Key_Space },
    };
    while (!steps.isEmpty()) {
        const QString step = steps.takeFirst().trimmed();
        bool isNumber = false;
        const double secs = step.toDouble(&isNumber);
        if (isNumber) {
            QTimer::singleShot(int(secs * 1000), ui, [=] { runScript(ui, mpv, steps, shotDir, shot); });
            return;
        }
        if (step == "quit") {
            QGuiApplication::quit();
            return;
        }
        if (step == "shot") {
            const QString base = QDir(shotDir).absoluteFilePath(QStringLiteral("shot-%1").arg(++shot, 2, 10, QChar('0')));
            ui->lastFrame().save(base + "-ui.png");
            mpv->screenshot(base + ".png"); // fails while idle (no video)
            qInfo("script: saved %s", qPrintable(base));
        } else if (keys.contains(step)) {
            ui->deliverKey(keys.value(step));
        } else {
            qWarning("script: unknown step '%s'", qPrintable(step));
        }
    }
}

int main(int argc, char *argv[])
{
    setupDecryption();
    // the UI is rendered on the CPU into an mpv overlay (see UiRenderer)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    QGuiApplication app(argc, argv);
    app.setApplicationName("discplayer");
    app.setOrganizationName("discplayer"); // settings: ~/.config/discplayer/
    // libmpv requires the C numeric locale; Qt sets the user's
    std::setlocale(LC_NUMERIC, "C");

    QCommandLineParser args;
    args.addHelpOption();
    args.addOptions({
        { "fullscreen", "Start fullscreen." },
        { "script", "Scripted input for tests.", "steps" },
        { "shot-dir", "Directory for script screenshots.", "dir", "." },
        { "play", "Play a file or URL directly (testing without a drive).", "url" },
    });
    args.process(app);

    Mpv mpv(args.isSet("fullscreen"));
    QQmlEngine engine;
    UiRenderer ui(&mpv);
    QObject::connect(&mpv, &Mpv::closeRequested, &app, &QGuiApplication::quit);
    QObject::connect(&mpv, &Mpv::keyPressed, &ui, [&ui](const QString &name) {
        if (int key = qtKeyForMpv(name))
            ui.deliverKey(key);
    });

    QQmlComponent component(&engine, "DiscPlayer", "Main");
    QObject *root = component.createWithInitialProperties({ { "mpv", QVariant::fromValue(&mpv) } });
    auto *rootItem = qobject_cast<QQuickItem *>(root);
    if (!rootItem) {
        qCritical("%s", qPrintable(component.errorString()));
        return 1;
    }
    ui.setRootItem(rootItem);
    if (args.isSet("play"))
        QMetaObject::invokeMethod(rootItem, "playUrl", QVariant(args.value("play")));

    Gamepad gamepad;
    QObject::connect(&gamepad, &Gamepad::keyPressed, &ui, &UiRenderer::deliverKey);
    gamepad.start();

    if (args.isSet("script"))
        runScript(&ui, &mpv, args.value("script").split(','), args.value("shot-dir"));

    const int ret = app.exec();
    gamepad.requestInterruption();
    gamepad.wait();
    return ret;
}
