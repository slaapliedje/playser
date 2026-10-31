// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCommandLineParser>
#include <QDir>
#include <QFontDatabase>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QTimer>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <thread>
#include <unistd.h>

#include "Gamepad.h"
#include "Mpv.h"
#include "UiRenderer.h"

// SIGTERM (Steam's "Exit game") and SIGINT quit through the event loop, so
// the player stops and saves its resume point like any other exit.
static int signalPipe[2] = { -1, -1 };

static void quitOnSignals(QGuiApplication *app)
{
    if (pipe(signalPipe) != 0)
        return;
    auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, app);
    QObject::connect(notifier, &QSocketNotifier::activated, app, [notifier] {
        char c;
        if (read(signalPipe[0], &c, 1) == 1)
            qInfo("signal: quitting");
        notifier->setEnabled(false);
        QGuiApplication::quit();
    });
    struct sigaction sa = {};
    sa.sa_handler = [](int) {
        const char c = 1;
        [[maybe_unused]] ssize_t n = write(signalPipe[1], &c, 1);
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    // a helper ended at exit (endHelpers) while its disc was still opening:
    // libmmbd's next write to it gets EPIPE instead of killing us
    signal(SIGPIPE, SIG_IGN);
}

// libmmbd runs MakeMKV's helper (makemkvcon guiserver) as our child and
// ends it when the disc is closed; but a disc still opening at exit (a hung
// or slow drive) leaves it running, orphaned and holding the drive until it
// is killed. End our own helpers on the way out (not anyone else's, e.g.
// the MakeMKV app's).
static void endHelpers()
{
    DIR *proc = opendir("/proc");
    if (!proc)
        return;
    const pid_t self = getpid();
    while (const dirent *e = readdir(proc)) {
        const pid_t pid = atoi(e->d_name);
        if (pid <= 0)
            continue;
        char path[64], stat[512] = {};
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        const size_t n = fread(stat, 1, sizeof(stat) - 1, f);
        fclose(f);
        // "pid (comm) state ppid ..."; comm may hold spaces, so from the last ')'
        const char *close = n ? strrchr(stat, ')') : nullptr;
        char state;
        int ppid;
        if (close && !strncmp(stat + strcspn(stat, "(") + 1, "makemkvcon)", 11)
            && sscanf(close + 1, " %c %d", &state, &ppid) == 2 && ppid == self) {
            qInfo("ending MakeMKV helper %d", pid);
            kill(pid, SIGTERM);
        }
    }
    closedir(proc);
}

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
        { "i", Qt::Key_I }, { "m", Qt::Key_Menu }, { "t", Qt::Key_TopMenu },
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
        { "pgdn", Qt::Key_PageDown }, { "space", Qt::Key_Space }, { "title", Qt::Key_TopMenu },
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
            // mpv captures asynchronously: give it time before the next
            // step changes what's on screen
            QTimer::singleShot(300, ui, [=] { runScript(ui, mpv, steps, shotDir, shot); });
            return;
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
    app.setApplicationName("playser");
    app.setOrganizationName("playser"); // settings: ~/.config/playser/
    // the same type everywhere: SteamOS's runtime only has DejaVu Sans, and
    // desktop hosts vary (data/fonts, SIL OFL)
    const int font = QFontDatabase::addApplicationFont(":/fonts/InterVariable.ttf");
    if (font >= 0)
        app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).value(0)));
    // libmpv requires the C numeric locale; Qt sets the user's
    std::setlocale(LC_NUMERIC, "C");

    QCommandLineParser args;
    args.addHelpOption();
    args.addOptions({
        { "fullscreen", "Start fullscreen." },
        { "script", "Scripted input for tests.", "steps" },
        { "shot-dir", "Directory for script screenshots.", "dir", "." },
        { "play", "Play a file or URL directly (testing without a drive).", "url" },
        { "play-menu", "Play a DVD image or folder through its menus (testing).", "path" },
        { "play-playlists", "Play a Blu-ray's playlists in turn as episodes, e.g. /dev/sr0:12,10 (testing).",
          "device:list" },
        { "mpv-option", "Set an mpv option, e.g. audio-spdif=ac3 (testing).", "name=value" },
    });
    args.process(app);

    Mpv mpv(args.isSet("fullscreen"), args.values("mpv-option"));
    QQmlEngine engine;
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QGuiApplication::quit);
    quitOnSignals(&app);
    Gamepad gamepad; // outlives the UI, which binds to it
    UiRenderer ui(&mpv);
    QObject::connect(&mpv, &Mpv::closeRequested, &app, &QGuiApplication::quit);
    QObject::connect(&mpv, &Mpv::keyPressed, &ui, [&ui](const QString &name) {
        if (int key = qtKeyForMpv(name))
            ui.deliverKey(key);
    });

    QQmlComponent component(&engine, "Playser", "Main");
    QObject *root = component.createWithInitialProperties({
        { "mpv", QVariant::fromValue(&mpv) }, { "gamepad", QVariant::fromValue(&gamepad) } });
    auto *rootItem = qobject_cast<QQuickItem *>(root);
    if (!rootItem) {
        qCritical("%s", qPrintable(component.errorString()));
        return 1;
    }
    ui.setRootItem(rootItem);
    if (args.isSet("play"))
        QMetaObject::invokeMethod(rootItem, "playUrl", QVariant(args.value("play")));
    if (args.isSet("play-menu"))
        QMetaObject::invokeMethod(rootItem, "playMenu", QVariant(args.value("play-menu")));
    if (args.isSet("play-playlists"))
        QMetaObject::invokeMethod(rootItem, "playPlaylists", QVariant(args.value("play-playlists")));

    QObject::connect(&gamepad, &Gamepad::keyPressed, &ui, &UiRenderer::deliverKey);
    gamepad.start();

    if (args.isSet("script"))
        runScript(&ui, &mpv, args.value("script").split(','), args.value("shot-dir"));

    const int ret = app.exec();
    QMetaObject::invokeMethod(rootItem, "shutdown");
    gamepad.requestInterruption();
    gamepad.wait();
    endHelpers();
    // mpv's teardown (~Mpv, after the UI and its settings) waits for a disc
    // still opening, and libmmbd waits forever for the helper just ended:
    // give teardown a few seconds, then leave without it
    std::thread([ret] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        qWarning("exit: still tearing down after 5 s, leaving");
        _exit(ret);
    }).detach();
    return ret;
}
