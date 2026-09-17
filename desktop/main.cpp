#include "window.h"
#include <QApplication>
#include <QTimer>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QMessageBox>
#include <QPixmap>
#include <QStyleFactory>
#include <QFile>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

static void startupLog(const char *stage) {
#ifdef Q_OS_WIN
    // Available before QApplication, and usable even when the UI cannot open.
    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (!n || n >= MAX_PATH) return;
    const QString path = QString::fromWCharArray(temp) + "GRID0-Relay-startup.log";
    QFile log(path);
    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        log.write(QByteArray::number(GetCurrentProcessId()) + ": " + stage + '\n');
    }
#else
    Q_UNUSED(stage);
#endif
}

static QPixmap applicationIcon() {
    return QPixmap(":/branding/grid0-app-icon.png");
}
int grid0RelayMain(int argc, char **argv) {
    startupLog("entered application; creating QApplication");
#ifdef Q_OS_WIN
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        startupLog(message.toUtf8().constData());
    });
#endif
    QApplication app(argc, argv);
    startupLog("QApplication created");
    app.setOrganizationName("GRID0 Relay"); app.setApplicationName("GRID0Relay"); app.setApplicationVersion("0.6.10");
#ifdef Q_OS_WIN
    // Qt's Windows 11 style supports system light/dark and high-contrast themes.
    if (QStyleFactory::keys().contains("windows11", Qt::CaseInsensitive)) app.setStyle("windows11");
#endif
    const auto args = app.arguments(); bool preview = args.contains("--preview");
    auto icon = applicationIcon();
#ifdef Q_OS_WIN
    // macOS reads the bundled ICNS for the Dock. Setting an opaque bitmap at
    // runtime replaces it with an unmasked square while the app is open.
    app.setWindowIcon(QIcon(icon));
#endif
    int iconOutput = args.indexOf("--write-icon");
    if (iconOutput >= 0 && iconOutput + 1 < args.size()) {
        // An optional square size lets Linux packaging produce its 256px icon
        // without depending on an image tool being installed.
        const int size = iconOutput + 2 < args.size() ? args[iconOutput + 2].toInt() : 0;
        if (size > 0) icon = icon.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        return icon.save(args[iconOutput + 1]) ? 0 : 1;
    }
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(data);
    QLockFile lock(data + "/desktop.lock");
    if (!preview && !lock.tryLock(0)) { QMessageBox::information(nullptr, "GRID0 Relay", "The desktop app is already open."); return 1; }
    startupLog("creating main window");
    Window window(preview); window.show(); window.activateWindow();
    startupLog("main window shown");
    QTimer::singleShot(0, &app, [] { startupLog("event loop running"); });
    if (args.contains("--settings")) window.selectPage(1, args.contains("--advanced") ? 1 : 0);
    int shot = args.indexOf("--screenshot");
    if (preview && shot >= 0 && shot + 1 < args.size()) QTimer::singleShot(600, &app, [&] { window.grab().save(args[shot + 1]); app.quit(); });
    return app.exec();
}
#ifndef Q_OS_WIN
int main(int argc, char **argv) { return grid0RelayMain(argc, argv); }
#endif
