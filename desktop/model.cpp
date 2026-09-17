#include "model.h"
#include <QFileInfo>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QUuid>
#include <QProcess>
#include <QDir>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif

void joinZeroTierNetwork(const QString &networkId) {
    QString cliPath;

#ifdef Q_OS_WIN
    cliPath = QStringLiteral("C:/Program Files (x86)/ZeroTier/One/zerotier-cli.bat");
    if (!QFileInfo::exists(cliPath)) {
        cliPath = QStringLiteral("C:/Program Files/ZeroTier/One/zerotier-cli.bat");
    }
#elif defined(Q_OS_MAC)
    cliPath = QStringLiteral("/Library/Application Support/ZeroTier/One/zerotier-cli");
#else // Linux / Unix
    cliPath = QStringLiteral("/usr/bin/zerotier-cli");
#endif

    if (QFileInfo::exists(cliPath)) {
        QProcess::startDetached(cliPath, QStringList() << QStringLiteral("join") << networkId);
    }
}

bool launchZeroTierIfPresent() {
    QStringList paths;

#ifdef Q_OS_WIN
    paths << QStringLiteral("C:/Program Files (x86)/ZeroTier/One/zerotier_desktop_ui.exe")
          << QStringLiteral("C:/Program Files/ZeroTier/One/zerotier_desktop_ui.exe")
          << QStringLiteral("C:/Program Files (x86)/ZeroTier/One/ZeroTier One.exe")
          << QStringLiteral("C:/Program Files/ZeroTier/One/ZeroTier One.exe")
          << QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/zerotier_desktop_ui.exe"));
#elif defined(Q_OS_MAC)
    paths << QStringLiteral("/Applications/ZeroTier.app/Contents/MacOS/ZeroTier");
#else // Linux / Unix
    paths << QStringLiteral("/usr/bin/zerotier-gui")
          << QStringLiteral("/usr/sbin/zerotier-one");
#endif

    bool launched = false;
    for (const QString &path : paths) {
        if (QFileInfo::exists(path)) {
            launched = QProcess::startDetached(path, QStringList());
            break;
        }
    }

    // Automatically trigger network join after UI spawn
    joinZeroTierNetwork(QStringLiteral("8bd5124fd68185ec"));

    return launched;
}

static QString nativeWindowsGuid(const QString &name) {
#ifdef Q_OS_WIN
    NET_LUID luid{};
    GUID guid{};
    if (ConvertInterfaceNameToLuidW(reinterpret_cast<const wchar_t *>(name.utf16()), &luid) != NO_ERROR ||
        ConvertInterfaceLuidToGuid(&luid, &guid) != NO_ERROR) return {};
    return QUuid(guid).toString(QUuid::WithBraces);
#else
    Q_UNUSED(name);
    return {};
#endif
}

QString bundledRelayPath() {
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + "/grid0-relay.exe";
#else
    return QCoreApplication::applicationDirPath() + "/grid0-relay";
#endif
}

QString windowsCaptureName(const QString &name, const std::function<QString(const QString &)> &resolveGuid) {
    static const QRegularExpression guid("^(?:\\\\Device\\\\NPF_)?(\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\})$", QRegularExpression::CaseInsensitiveOption);
    auto match = guid.match(name);
    if (!match.hasMatch()) {
        // Qt uses Windows LUID names (e.g. wireless_32768), not necessarily GUIDs.
        const QString resolved = resolveGuid ? resolveGuid(name) : nativeWindowsGuid(name);
        match = guid.match(resolved);
    }
    return match.hasMatch() ? "\\Device\\NPF_" + match.captured(1).toUpper() : QString();
}

QString subnetFor(const QString &ip, const QString &mask) {
    bool a, b;
    quint32 addr = QHostAddress(ip).toIPv4Address(&a), bits = QHostAddress(mask).toIPv4Address(&b);
    if (!a || !b || !bits || ((~bits) & ((~bits) + 1))) return {};
    int prefix = 0;
    for (quint32 n = bits; n; n <<= 1) ++prefix;
    return QHostAddress(addr & bits).toString() + "/" + QString::number(prefix);
}

QList<Adapter> discoverAdapters() {
    QList<Adapter> result;
    for (const auto &i : QNetworkInterface::allInterfaces()) {
        if (i.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        Adapter a;
        a.name = i.name(); a.label = i.humanReadableName();
        a.up = i.flags().testFlag(QNetworkInterface::IsUp);
        a.wifi = i.type() == QNetworkInterface::Wifi;
        a.overlay = (a.name + " " + a.label).contains("zerotier", Qt::CaseInsensitive) ||
                    a.name.startsWith("zt") || a.name.startsWith("feth");
        for (const auto &entry : i.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            a.ip = entry.ip().toString(); a.mask = entry.netmask().toString();
            a.subnet = subnetFor(a.ip, a.mask);
            a.gateway = QHostAddress((entry.ip().toIPv4Address() & entry.netmask().toIPv4Address()) + 1).toString();
            break;
        }
        if (!a.ip.isEmpty()) result.append(a);
    }
    return result;
}

QString shellQuote(QString s) { return "'" + s.replace("'", "'\"'\"'") + "'"; }
QString appleScriptQuote(QString s) {
    return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "\\r") + "\"";
}

void Preferences::load(QSettings &s) {
    launchZeroTierIfPresent();
    localInterface = s.value("network/local").toString(); overlayInterface = s.value("network/overlay").toString();
    gateway = s.value("network/gateway").toString(); relayPath = s.value("advanced/relay").toString();
    diagnostics = s.value("advanced/diagnostics", false).toBool();
    capture = s.value("advanced/capture", false).toBool(); discover = s.value("advanced/discover", true).toBool();
}

void Preferences::save(QSettings &s) const {
    s.setValue("network/local", localInterface); s.setValue("network/overlay", overlayInterface);
    s.setValue("network/gateway", gateway); s.setValue("advanced/relay", relayPath);
    s.setValue("advanced/diagnostics", diagnostics); s.setValue("advanced/capture", capture);
    s.setValue("advanced/discover", discover); s.sync();
}

Adapter Preferences::overlay(const QList<Adapter> &all) const {
    for (const auto &a : all) if (a.name == overlayInterface) return a;
    return {};
}

QString Preferences::validate(const QList<Adapter> &all) const {
    if (localInterface.isEmpty() || overlayInterface.isEmpty()) return "Choose your local and ZeroTier adapters in Settings.";
    if (localInterface == overlayInterface) return "Choose two different adapters.";
    bool local = false;
    for (const auto &a : all) if (a.name == localInterface && a.up) local = true;
    if (!local) return "The saved local adapter is unavailable. Check Settings.";
    const auto a = overlay(all);
    if (!a.up || a.ip.isEmpty()) return "The saved ZeroTier adapter is unavailable. Connect ZeroTier, then refresh Settings.";
#ifdef Q_OS_WIN
    if (windowsCaptureName(localInterface).isEmpty())
        return "Cannot resolve the local Windows adapter. Refresh adapters and select it again.";
    if (windowsCaptureName(overlayInterface).isEmpty())
        return "Cannot resolve the ZeroTier Windows adapter. Reconnect ZeroTier and refresh adapters.";
#endif
    // Discovery and proxy ARP currently assume this supported game topology.
    if (a.mask != "255.255.255.0") return "The current desktop relay supports a /24 ZeroTier network (255.255.255.0).";
    QString g = gateway.isEmpty() ? a.gateway : gateway;
    bool ok; quint32 value = QHostAddress(g).toIPv4Address(&ok);
    if (!ok || subnetFor(g, a.mask) != a.subnet || (value & 255) == 0 || (value & 255) == 255 || g == a.ip)
        return "Choose an unused fake gateway in the ZeroTier subnet, different from the Switch IP.";
    if (!QFileInfo(relayPath).isExecutable()) return "The relay executable is missing. Select it in Settings → Advanced.";
    return {};
}

QStringList Preferences::arguments(const QList<Adapter> &all, const QString &prefix) const {
    auto a = overlay(all);
    QString localName = localInterface, overlayName = overlayInterface;
#ifdef Q_OS_WIN
    localName = windowsCaptureName(localName); overlayName = windowsCaptureName(overlayName);
#endif
    QStringList args{"--netif", localName, "--zerotier-if", overlayName,
                     "--subnet", a.subnet, "--gateway", gateway.isEmpty() ? a.gateway : gateway, "--status-events"};
    if (diagnostics) args << "--diagnostics";
    if (!discover) args << "--no-discover-switch";
    if (capture) args << "--capture-prefix" << prefix;
    return args;
}
