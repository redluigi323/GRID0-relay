#pragma once
#include "model.h"
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

class RelayController : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Authorizing, Starting, Running, Stopping };
    explicit RelayController(QObject *parent = nullptr);
    ~RelayController();
    State state() const { return current; }
    bool busy() const { return current != Idle; }
    QString reportDirectory() const { return report; }
    void start(const Preferences &, const QList<Adapter> &);
    void stop();
signals:
    void stateChanged();
    void message(const QString &);
    void lineReceived(const QString &);
    void switchDetected(const QString &);
private:
    void setState(State);
    void receive(QByteArray);
    void finish(int, QProcess::ExitStatus);
    State current = Idle;
    QProcess authorization;
    QLocalServer server;
    QLocalSocket *socket = nullptr;
    std::unique_ptr<QTemporaryDir> socketDirectory;
    QFile log;
    QString report;
    QString lastError;
    QByteArray pending;
    QTimer startupTimeout;
    bool ready = false;
#ifdef Q_OS_WIN
    void *windowsStopEvent = nullptr;
    QTimer stopTimeout;
#endif
};
