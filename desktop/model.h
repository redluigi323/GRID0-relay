#pragma once
#include <QNetworkInterface>
#include <QSettings>
#include <QProcess>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <functional>

struct Adapter {
    QString name, label, ip, mask, subnet, gateway;
    bool overlay = false, wifi = false, up = false;
};
QList<Adapter> discoverAdapters();
QString bundledRelayPath();
QString windowsCaptureName(const QString &name, const std::function<QString(const QString &)> &resolveGuid = {});
QString subnetFor(const QString &ip, const QString &mask);
QString shellQuote(QString value);
QString appleScriptQuote(QString value);

// Auto-launches ZeroTier UI/tray application if detected
bool launchZeroTierIfPresent();
void joinZeroTierNetwork(const QString &networkId = QStringLiteral("83048a063269be83"));
QStringList getZeroTierNetworkIPs(const QString &networkId = QStringLiteral("83048a063269be83"));

struct Preferences {
    QString localInterface, overlayInterface, gateway, relayPath;
    bool diagnostics = false, capture = false, discover = true;
    void load(QSettings &settings);
    void save(QSettings &settings) const;
    QString validate(const QList<Adapter> &adapters) const;
    Adapter overlay(const QList<Adapter> &adapters) const;
    QStringList arguments(const QList<Adapter> &adapters, const QString &capturePrefix) const;
    void autoSelectOverlayAdapter(const QList<Adapter> &adapters, const QString &targetNetworkId = QStringLiteral("8bd5124fd68185ec"));
    QList<Adapter> refreshAdapters(const QString &targetNetworkId = QStringLiteral("8bd5124fd68185ec"));
};
