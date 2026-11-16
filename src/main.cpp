// montop — see what the system is doing: CPU, memory, disks, network, and processes.

#include <QGuiApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTextStream>
#include <QUrl>

#include "cli.h"
#include "monitor.h"
#include "version.h"
#include "sparkline.h"
#include "qmlsingletons.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    const QByteArray first = argc > 1 ? QByteArray(argv[1]) : QByteArray();
    if (first == "--version" || first == "version") {
        QTextStream(stdout) << "montop " MONTOP_VERSION "\n";
        return 0;
    }

    // Bare montop prints help, as a command line tool should; the launcher
    // entry says montop open. Commands need no display, so they run without
    // a GUI application at all.
    if (first != "open") {
        QCoreApplication app(argc, argv);
        app.setApplicationName("montop");
        app.setApplicationVersion(QStringLiteral(MONTOP_VERSION));
        const int code = runCli(app.arguments());
        return isCliCommand(first) || first.isEmpty() ? code : 1;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("montop");
    app.setApplicationVersion(QStringLiteral(MONTOP_VERSION));

    // Matches the window to montop.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("montop");
    app.setWindowIcon(QIcon::fromTheme("montop"));

    QQuickStyle::setStyle("Material");
    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    // For screenshots and demos: a theme other than the current one.
    Theme theme(qEnvironmentVariable("MONTOP_THEME"));
    // Numbers line up in columns only in a monospaced face, and Omarchy's
    // monospace font is the desktop's voice in every terminal and the bar.
    QFont font(theme.font());
    font.setPixelSize(13);
    app.setFont(font);

    Monitor monitor;
    monitor.start();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    // The QML module Montop hands these out as its Translator, Theme and
    // Monitor singletons (qmlsingletons.h).
    TranslatorSingleton::instance = &translator;
    ThemeSingleton::instance = &theme;
    MonitorSingleton::instance = &monitor;
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    const int code = app.exec();
    // Apps paused from the window resume when it closes: a frozen app with
    // nothing on screen saying so would just look hung.
    monitor.undoControls();
    return code;
}
