#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>
#include <clocale>

#include "Gamepad.h"

// MakeMKV's libmmbd stands in for libaacs/libbdplus when it's installed
// (see docs/spike-results.md, 0a). An explicit environment wins.
static void setupDecryption()
{
    static const char *const dirs[] = { "/usr/lib", "/usr/local/lib", "/app/lib",
                                        "/app/extra/lib" };
    for (const char *dir : dirs) {
        if (QFileInfo::exists(QString::fromLatin1(dir) + "/libmmbd.so.0")) {
            if (!qEnvironmentVariableIsSet("LIBAACS_PATH"))
                qputenv("LIBAACS_PATH", "libmmbd");
            if (!qEnvironmentVariableIsSet("LIBBDPLUS_PATH"))
                qputenv("LIBBDPLUS_PATH", "libmmbd");
            return;
        }
    }
}

static void sendKey(int key)
{
    QWindow *w = QGuiApplication::focusWindow();
    if (!w && !QGuiApplication::topLevelWindows().isEmpty())
        w = QGuiApplication::topLevelWindows().first();
    if (!w)
        return;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QGuiApplication::sendEvent(w, &press);
    QGuiApplication::sendEvent(w, &release);
}

// --script "2,down,enter,3,shot,quit": numbers wait (seconds), names press
// keys, "shot" saves a screenshot, "quit" exits. For headless UI tests.
static void runScript(QQuickWindow *win, QStringList steps, const QString &shotDir, int shot = 0)
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
            QTimer::singleShot(int(secs * 1000), win, [=] { runScript(win, steps, shotDir, shot); });
            return;
        }
        if (step == "quit") {
            QGuiApplication::quit();
            return;
        }
        if (step == "shot") {
            const QString path = QDir(shotDir).filePath(QStringLiteral("shot-%1.png").arg(++shot, 2, 10, QChar('0')));
            win->grabWindow().save(path);
            qInfo("script: saved %s", qPrintable(path));
        } else if (keys.contains(step)) {
            sendKey(keys.value(step));
        } else {
            qWarning("script: unknown step '%s'", qPrintable(step));
        }
    }
}

int main(int argc, char *argv[])
{
    setupDecryption();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL); // libmpv render API

    QGuiApplication app(argc, argv);
    app.setApplicationName("discplayer");
    // libmpv requires the C numeric locale; Qt sets the user's
    std::setlocale(LC_NUMERIC, "C");

    QCommandLineParser args;
    args.addHelpOption();
    args.addOptions({
        { "fullscreen", "Start fullscreen." },
        { "script", "Scripted input for tests.", "steps" },
        { "shot-dir", "Directory for script screenshots.", "dir", "." },
    });
    args.process(app);

    Gamepad gamepad;
    QObject::connect(&gamepad, &Gamepad::keyPressed, &app, [](int key) { sendKey(key); });
    gamepad.start();

    QQmlApplicationEngine engine;
    engine.setInitialProperties({ { "startFullscreen", args.isSet("fullscreen") } });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("DiscPlayer", "Main");

    if (args.isSet("script") && !engine.rootObjects().isEmpty()) {
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (win)
            runScript(win, args.value("script").split(','), args.value("shot-dir"));
    }

    const int ret = app.exec();
    gamepad.requestInterruption();
    gamepad.wait();
    return ret;
}
