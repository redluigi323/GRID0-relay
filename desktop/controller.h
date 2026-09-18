#pragma once
#include "model.h"
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

class Controller : public QObject {
    Q_OBJECT
public:
    explicit Controller(Preferences *prefs, QObject *parent = nullptr);
    ~Controller() override = default;
public slots:
    void handleRefreshAdapters();
signals:
    void adaptersRefreshed(const QList<Adapter> &adapters);
private:
    Preferences *m_prefs = nullptr;
};
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
    void relayEvent(const QString &, const QString &);
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
#ifdef Q_OS_LINUX
    QString manualCommand;
#endif
    QByteArray pending;
    QTimer startupTimeout;
    bool ready = false;
#ifdef Q_OS_WIN
    void *windowsStopEvent = nullptr;
    QTimer stopTimeout;
#endif
};
