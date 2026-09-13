#pragma once

#include <QObject>
#include <QProcess>

struct DependencyStatus {
    bool zeroTier = false;
    bool npcap = false;
    bool winPcap = false;
};

class DependencyInstaller : public QObject {
    Q_OBJECT
public:
    explicit DependencyInstaller(QObject *parent = nullptr);
    DependencyStatus status() const;
    void installMissing();
signals:
    void progress(const QString &message);
    void completed(const QString &message);
    void failed(const QString &message);
private:
#ifdef Q_OS_WIN
    enum class Package { ZeroTier, Npcap };
    void installNext();
    bool downloadAndVerify(Package package, QString *file, QString *error);
    bool verifyPublisherSignature(const QString &file, QString *error) const;
    QList<Package> pending;
    QProcess installer;
#endif
};
