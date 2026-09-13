#include "controller.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#ifdef Q_OS_MACOS
#include <sys/socket.h>
#include <unistd.h>
#endif

RelayController::RelayController(QObject *parent) : QObject(parent) {
    connect(&authorization, &QProcess::finished, this, &RelayController::finish);
    connect(&authorization, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { lastError = authorization.errorString(); emit message(lastError); finish(1, QProcess::CrashExit); }
    });
    connect(&server, &QLocalServer::newConnection, this, [this] {
        auto *incoming = server.nextPendingConnection();
#ifdef Q_OS_MACOS
        uid_t uid; gid_t gid;
        if (getpeereid(incoming->socketDescriptor(), &uid, &gid) || uid != 0 || socket || current == Stopping) {
            incoming->abort(); incoming->deleteLater(); return;
        }
#endif
        socket = incoming;
        connect(socket, &QLocalSocket::readyRead, this, [this] { if (socket) receive(socket->readAll()); });
        setState(Starting); emit message("Starting relay…"); startupTimeout.start(15000);
        receive(socket->readAll());
    });
#ifdef Q_OS_WIN
    authorization.setProcessChannelMode(QProcess::MergedChannels);
    authorization.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    connect(&authorization, &QProcess::readyReadStandardOutput, this, [this] { receive(authorization.readAllStandardOutput()); });
    stopTimeout.setSingleShot(true);
    connect(&stopTimeout, &QTimer::timeout, this, [this] {
        emit message("The relay did not stop in time; closing its process."); authorization.kill();
    });
#endif
    startupTimeout.setSingleShot(true);
    connect(&startupTimeout, &QTimer::timeout, this, [this] {
        emit message("The relay did not become ready. Open Advanced to inspect its log."); stop();
    });
}
RelayController::~RelayController() {
#ifdef Q_OS_WIN
    if (windowsStopEvent) SetEvent(windowsStopEvent);
    if (authorization.state() != QProcess::NotRunning && !authorization.waitForFinished(3000)) {
        authorization.kill(); authorization.waitForFinished(1500);
    }
    if (windowsStopEvent) { CloseHandle(windowsStopEvent); windowsStopEvent = nullptr; }
#endif
    if (socket) socket->abort(); // Supervisor stops/reaps its child on disconnect.
    server.close();
    if (authorization.state() != QProcess::NotRunning) {
        authorization.terminate(); authorization.waitForFinished(1500);
    }
}
void RelayController::setState(State s) { current = s; emit stateChanged(); }
void RelayController::start(const Preferences &p, const QList<Adapter> &adapters) {
    if (busy()) return;
    lastError.clear();
    auto error = p.validate(adapters);
    if (!error.isEmpty()) { emit message(error); return; }
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    emit message("The Linux launcher is not implemented yet.");
    return;
#else
#ifdef Q_OS_MACOS
    const QString helper = QCoreApplication::applicationDirPath() + "/grid0-relay-supervisor";
    if (!QFileInfo(helper).isExecutable()) { emit message("The bundled macOS launcher is missing. Rebuild the desktop app."); return; }
#endif
    const QString id = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    report = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/reports/" + id;
    if (!QDir().mkpath(report) || !QFile::setPermissions(report, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        emit message("Cannot create a private report directory."); return;
    }
    log.setFileName(report + "/relay.log");
    if (!log.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { emit message("Cannot create the relay log."); return; }
    log.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    QFile meta(report + "/session.json");
    if (meta.open(QIODevice::WriteOnly)) {
        meta.write(QJsonDocument(QJsonObject{{"appVersion", QCoreApplication::applicationVersion()},
            {"os", QSysInfo::prettyProductName()}, {"localInterface", p.localInterface},
            {"zeroTierInterface", p.overlayInterface}, {"switchIP", p.overlay(adapters).ip},
            {"switchMask", p.overlay(adapters).mask}, {"capture", p.capture}}).toJson());
    }
#ifdef Q_OS_WIN
    const QString eventName = "Local\\Grid0Relay.Stop." + QUuid::createUuid().toString(QUuid::Id128);
    windowsStopEvent = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<const wchar_t *>(eventName.utf16()));
    if (!windowsStopEvent) { log.close(); emit message("Cannot create the relay stop control."); return; }
    pending.clear(); ready = false;
    auto args = p.arguments(adapters, report + "/packets");
    receive(("Local adapter: " + p.localInterface + " -> " + args.value(1) + "\nZeroTier adapter: " + p.overlayInterface + " -> " + args.value(3) + "\n").toUtf8());
    args << "--stop-event" << eventName << "--parent-pid" << QString::number(QCoreApplication::applicationPid());
    setState(Starting); emit message("Starting relay…"); startupTimeout.start(15000);
    authorization.start(p.relayPath, args);
#else
    socketDirectory = std::make_unique<QTemporaryDir>("/tmp/zll-ui-XXXXXX");
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!socketDirectory->isValid() || !server.listen(socketDirectory->path() + "/control")) {
        log.close(); emit message("Cannot create the local relay control connection."); return;
    }
    QStringList command{helper, server.fullServerName(), QString::number(getuid()), p.relayPath};
    command += p.arguments(adapters, report + "/packets");
    for (auto &part : command) part = shellQuote(part);
    pending.clear(); ready = false;
    setState(Authorizing); emit message("Approve the macOS administrator prompt to start.");
    authorization.start("/usr/bin/osascript", {"-e", "with timeout of 604800 seconds\n do shell script " + appleScriptQuote(command.join(' ')) + " with administrator privileges\nend timeout"});
#endif
#endif
}
void RelayController::receive(QByteArray bytes) {
    if (log.isOpen() && log.size() < 16 * 1024 * 1024) { log.write(bytes); log.flush(); }
    pending += bytes;
    while (pending.contains('\n')) {
        auto pos = pending.indexOf('\n');
        QString line = QString::fromUtf8(pending.left(pos)).trimmed(); pending.remove(0, pos + 1);
        emit lineReceived(line);
        if (line.startsWith("Relay started (PID ") && current != Stopping) {
            ready = true; startupTimeout.stop(); setState(Running); emit message("Relay running. Open Splatoon’s LAN mode.");
        }
        const QString marker = "Detected local Switch candidate: ";
        if (line.contains(marker)) emit switchDetected(line.mid(line.indexOf(marker) + marker.size()));
        if (line.contains("LAN broadcast mismatch:")) emit message("Switch subnet mismatch. Copy the mask below, reconnect, and restart the game.");
        if (line.contains("[ERROR]") || line.startsWith("Failed to start")) { lastError = line; emit message(line); }
    }
    if (pending.size() > 65536) pending = pending.right(65536);
}
void RelayController::stop() {
    if (!busy() || current == Stopping) return;
    startupTimeout.stop(); setState(Stopping);
#ifdef Q_OS_WIN
    if (windowsStopEvent) SetEvent(windowsStopEvent);
    stopTimeout.start(5000);
#else
    if (socket) { socket->write("STOP\n"); socket->flush(); }
    else { server.close(); authorization.terminate(); }
#endif
}
void RelayController::finish(int code, QProcess::ExitStatus status) {
    startupTimeout.stop();
#ifdef Q_OS_WIN
    stopTimeout.stop();
    receive(authorization.readAllStandardOutput());
    if (windowsStopEvent) { CloseHandle(windowsStopEvent); windowsStopEvent = nullptr; }
#endif
    bool requested = current == Stopping;
    if (socket) { receive(socket->readAll()); socket->abort(); socket->deleteLater(); socket = nullptr; }
    server.close(); socketDirectory.reset();
#ifdef Q_OS_WIN
    const QString error = lastError;
#else
    const QString error = QString::fromUtf8(authorization.readAllStandardError()).trimmed();
#endif
    if (!error.isEmpty()) receive(error.toUtf8() + '\n');
    if (log.isOpen()) log.close();
    setState(Idle);
    if (requested) emit message("Relay stopped.");
    else if (!ready || code || status == QProcess::CrashExit)
        emit message(error.contains("-128") ? "Administrator authorization was cancelled." :
                     (error.isEmpty() ? "The relay exited. Open Advanced to inspect the log." : error));
    else emit message("Relay stopped.");
}
