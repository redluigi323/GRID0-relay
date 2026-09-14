#include "dependencies.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QNetworkInterface>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <winsvc.h>
#include <winhttp.h>
#endif

DependencyInstaller::DependencyInstaller(QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
    connect(&installer, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int exitCode, QProcess::ExitStatus exitStatus) {
            if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                emit failed("The installer did not finish successfully. Check its window, then reopen Grid0 Relay.");
                return;
            }
            installNext();
        });
#endif
}

#ifdef Q_OS_WIN
static bool serviceExists(const wchar_t *name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS);
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return service != nullptr;
}

static bool registryKeyExists(HKEY root, const wchar_t *path, REGSAM view) {
    HKEY key = nullptr;
    const LONG result = RegOpenKeyExW(root, path, 0, KEY_READ | view, &key);
    if (key) RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static bool winPcapRegistryPresent() {
    static constexpr wchar_t key[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinPcapInst";
    return registryKeyExists(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY) ||
           registryKeyExists(HKEY_LOCAL_MACHINE, key, KEY_WOW64_32KEY);
}

static bool downloadWithWinHttp(const QUrl &url, QSaveFile *output, QString *error) {
    const std::wstring host = url.host().toStdWString();
    const std::wstring path = (url.path(QUrl::FullyEncoded) +
                               (url.query().isEmpty() ? QString() : "?" + url.query(QUrl::FullyEncoded))).toStdWString();
    HINTERNET session = WinHttpOpen(L"GRID0 Relay/0.6.10", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = "Windows could not initialize its secure download service."; return false; }
    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, statusSize = sizeof(status);
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
                 status >= 200 && status < 300;
    if (!ok) {
        *error = "Could not download the official installer using Windows secure networking.";
    } else {
        while (ok) {
            DWORD available = 0;
            ok = WinHttpQueryDataAvailable(request, &available);
            if (!ok || available == 0) break;
            QByteArray bytes(static_cast<int>(available), Qt::Uninitialized);
            DWORD received = 0;
            ok = WinHttpReadData(request, bytes.data(), available, &received) &&
                 output->write(bytes.constData(), received) == received;
        }
        if (!ok) *error = "Windows could not finish saving the official installer download.";
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}
#endif

DependencyStatus DependencyInstaller::status() const {
    DependencyStatus result;
#ifdef Q_OS_WIN
    result.zeroTier = serviceExists(L"ZeroTierOneService");
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length && length < MAX_PATH) {
        const QString npcap = QString::fromWCharArray(systemDirectory) + "\\Npcap\\wpcap.dll";
        result.npcap = QFile::exists(npcap);
    }
    // Npcap's service is named "npcap". NPF and the legacy uninstall key are
    // WinPcap markers; do not offer a side-by-side driver install.
    result.winPcap = !result.npcap && (serviceExists(L"NPF") || winPcapRegistryPresent());
#elif defined(Q_OS_MACOS)
    for (const auto &adapter : QNetworkInterface::allInterfaces()) {
        const QString name = adapter.name() + " " + adapter.humanReadableName();
        if (name.contains("zerotier", Qt::CaseInsensitive) || adapter.name().startsWith("feth")) {
            result.zeroTier = true;
            break;
        }
    }
#else
    result.zeroTier = true;
    result.npcap = true;
#endif
    return result;
}

void DependencyInstaller::installMissing() {
#ifdef Q_OS_WIN
    const auto current = status();
    if (current.winPcap) {
        emit failed("WinPcap is installed. Remove it in Apps & Features, then reopen Grid0 Relay before installing Npcap.");
        return;
    }
    pending.clear();
    if (!current.zeroTier) pending.append(Package::ZeroTier);
    if (!current.npcap) pending.append(Package::Npcap);
    if (pending.isEmpty()) { emit completed("ZeroTier and Npcap are already installed."); return; }
    installNext();
#else
    emit completed("No automatic installer is available on this platform.");
#endif
}

#ifdef Q_OS_WIN
bool DependencyInstaller::verifyPublisherSignature(const QString &file, QString *error) const {
    WINTRUST_FILE_INFO info{};
    info.cbStruct = sizeof(info);
    info.pcwszFilePath = reinterpret_cast<const wchar_t *>(file.utf16());
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    const LONG verified = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);
    if (verified != ERROR_SUCCESS) {
        *error = "Windows could not validate the installer signature. The download was not run.";
        return false;
    }
    return true;
}

bool DependencyInstaller::downloadAndVerify(Package package, QString *file, QString *error) {
    const bool zeroTier = package == Package::ZeroTier;
    const QUrl url(zeroTier ? "https://download.zerotier.com/dist/ZeroTier%20One.msi"
                             : "https://npcap.com/dist/npcap-1.88.exe");
    emit progress("Downloading " + QString(zeroTier ? "ZeroTier One" : "Npcap") + " from its official publisher…");
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/installers";
    if (!QDir().mkpath(directory)) { *error = "Could not create the private installer folder."; return false; }
    *file = directory + (zeroTier ? "/ZeroTier-One.msi" : "/npcap-1.88.exe");
    QSaveFile output(*file);
    if (!output.open(QIODevice::WriteOnly) || !downloadWithWinHttp(url, &output, error) || !output.commit()) {
        if (error->isEmpty()) *error = "Could not save the downloaded installer.";
        return false;
    }
    return verifyPublisherSignature(*file, error);
}

void DependencyInstaller::installNext() {
    if (pending.isEmpty()) {
        emit completed("Required software is installed. Reopen Grid0 Relay so Windows can finish creating network adapters.");
        return;
    }
    const Package package = pending.takeFirst();
    QString file, error;
    if (!downloadAndVerify(package, &file, &error)) { emit failed(error); return; }
    const bool zeroTier = package == Package::ZeroTier;
    emit progress("Installing " + QString(zeroTier ? "ZeroTier One" : "Npcap") + "…");
    if (zeroTier) installer.start("msiexec.exe", {"/i", QDir::toNativeSeparators(file), "/passive", "/norestart"});
    // The public Npcap installer does not license silent installation. Launch
    // its official UI so the user can accept the driver installation terms.
    else installer.start(file);
    if (!installer.waitForStarted(10000)) emit failed("Windows could not start the installer.");
}
#endif
