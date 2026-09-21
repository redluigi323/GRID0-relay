#include "window.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMenuBar>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QGridLayout>
#include <QGuiApplication>
#include <QScrollArea>
#ifdef Q_OS_MACOS
#include <QtLiquidGlass/QtLiquidGlass.h>
#endif

static QLabel *text(const QString &s, QWidget *parent = nullptr) {
    auto *l = new QLabel(s, parent); l->setWordWrap(true); l->setTextFormat(Qt::PlainText); return l;
}
static void title(QLabel *l, int size) { auto f = l->font(); f.setPointSize(size); f.setWeight(QFont::DemiBold); l->setFont(f); }
#ifdef Q_OS_MACOS
static void addMacGlass(QWidget *surface, QtLiquidGlass::Material material, double radius) {
    // Tests and screenshots use Qt's offscreen platform, which deliberately
    // has no AppKit view to host the native effect.
    if (QGuiApplication::platformName() != "cocoa") return;
    QtLiquidGlass::Options options;
    options.cornerRadius = radius;
    options.appearance = QtLiquidGlass::AdaptiveAppearance::Auto;
    options.titlebarStyle = QtLiquidGlass::TitlebarStyle::Preserve;
    options.dragBehavior = QtLiquidGlass::WindowDragBehavior::Preserve;
    options.blendingMode = QtLiquidGlass::BlendingMode::WithinWindow;
    QtLiquidGlass::addGlassEffect(surface, material, options);
}
#endif
Window::Window(bool preview) : previewMode(preview) {
    setWindowTitle("GRID0 Relay"); resize(740, 720); setMinimumSize(640, 590);
    auto *appMenu = menuBar()->addMenu("GRID0 Relay");
    auto *preferencesAction = appMenu->addAction("Settings…");
    preferencesAction->setMenuRole(QAction::PreferencesRole); preferencesAction->setShortcut(QKeySequence::Preferences);
    connect(preferencesAction, &QAction::triggered, this, [this] { selectPage(1); });
    auto *quit = appMenu->addAction("Quit GRID0 Relay"); quit->setMenuRole(QAction::QuitRole); quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    if (!preview) preferences.load(settings);
    const QString bundled = bundledRelayPath();
    if (preferences.relayPath.isEmpty()) preferences.relayPath = bundled;
    auto *central = new QWidget; setCentralWidget(central);
    auto *layout = new QVBoxLayout(central); layout->setContentsMargins(24, 22, 24, 20); layout->setSpacing(16);
    auto *brand = new QHBoxLayout;
    auto *logo = new QLabel;
    logo->setPixmap(QPixmap(":/branding/grid0-logo.png").scaled(44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setFixedSize(44, 44);
    logo->setAlignment(Qt::AlignCenter);
    brand->addWidget(logo);
    auto *heading = text("GRID0 Relay"); title(heading, 23); brand->addWidget(heading);
    brand->addStretch();
    layout->addLayout(brand);
    layout->addWidget(text("Nintendo Switch LAN play over ZeroTier"));
    // Let QMacStyle draw its native rounded tabs. Document mode intentionally
    // uses square browser/editor tabs, inappropriate for this utility.
    tabs = new QTabWidget; layout->addWidget(tabs);

    auto *play = new QWidget; auto *playLayout = new QVBoxLayout(play); playLayout->setContentsMargins(16, 24, 16, 12); playLayout->setSpacing(18);
    auto *summary = new QFrame; summary->setObjectName("relaySummary");
    auto *summaryLayout = new QVBoxLayout(summary); summaryLayout->setContentsMargins(16, 14, 16, 14); summaryLayout->setSpacing(8);
    status = text("Ready to connect"); status->setObjectName("relayStatus"); title(status, 18); summaryLayout->addWidget(status);
    switchStatus = text("Start the relay to look for your Switch."); summaryLayout->addWidget(switchStatus);
    auto *actions = new QHBoxLayout;
    // Keep native control heights: forcing tall buttons makes QMacStyle fall
    // back to a rectangular bezel rather than the standard macOS button.
    start = new QPushButton("Start relay"); start->setAutoDefault(true); start->setDefault(true);
    stop = new QPushButton("Stop relay");
    actions->addWidget(start); actions->addWidget(stop); actions->addStretch(); summaryLayout->addLayout(actions); playLayout->addWidget(summary);
    auto *group = new QGroupBox("Enter these settings on your Switch"); auto *form = new QGridLayout(group);
    group->setObjectName("switchSettings");
    form->setSizeConstraint(QLayout::SetMinimumSize);
    group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    form->setColumnStretch(1, 1);
    form->setHorizontalSpacing(20);
    form->setContentsMargins(18, 22, 18, 18); form->setVerticalSpacing(14);
    address = text("—"); mask = text("—"); gatewayValue = text("—");
    address->setObjectName("switchIP"); mask->setObjectName("switchMask"); gatewayValue->setObjectName("switchGateway");
    for (auto *l : {address, mask, gatewayValue}) { l->setTextInteractionFlags(Qt::TextSelectableByMouse); title(l, 14); }
    int row = 0;
    for (auto pair : {qMakePair(QString("IP address"), address), qMakePair(QString("Subnet mask"), mask), qMakePair(QString("Gateway"), gatewayValue)}) {
        auto *label = new QLabel(pair.first);
        pair.second->setWordWrap(false);
        pair.second->setMinimumHeight(pair.second->fontMetrics().height() + 4);
        pair.second->setMinimumWidth(pair.second->fontMetrics().horizontalAdvance("255.255.255.255") + 12);
        form->addWidget(label, row, 0); form->addWidget(pair.second, row++, 1);
    }
    auto *copy = new QPushButton("Copy Switch settings");
    auto *copyRow = new QHBoxLayout; copyRow->addWidget(copy); copyRow->addStretch();
    form->addLayout(copyRow, 3, 0, 1, 2); playLayout->addWidget(group);
#ifdef Q_OS_MACOS
    // A QFrame gives the native effect an independent host. QGroupBox uses a
    // shared Qt backing view, which would place the AppKit layer over its text.
    // Keep the window and Switch settings themselves in the native Qt style.
    addMacGlass(summary, QtLiquidGlass::Material::ClearGlass, 16.0);
#endif
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText("IP address: " + address->text() + "\nSubnet mask: " + mask->text() + "\nGateway: " + gatewayValue->text());
    });
    settingsHint = text("Use the exact subnet mask shown here. After changing network settings, reconnect your Switch and restart the game before entering LAN mode.");
    playLayout->addWidget(settingsHint);
    dhcpHint = text("Set your Switch to Automatic. When the relay starts it runs a DHCP server that gives Nintendo consoles a ZeroTier-subnet address — nothing to type in.");
    playLayout->addWidget(dhcpHint);
    validation = text(""); playLayout->addWidget(validation);
    auto *configure = new QPushButton("Connection settings…"); playLayout->addWidget(configure, 0, Qt::AlignLeft);
    connect(configure, &QPushButton::clicked, this, [this] { tabs->setCurrentIndex(1); });
    playLayout->addStretch();
    playLayout->setSizeConstraint(QLayout::SetMinimumSize);
    auto *playScroll = new QScrollArea; playScroll->setWidgetResizable(true);
    playScroll->setFrameShape(QFrame::NoFrame); playScroll->setWidget(play);
    tabs->addTab(playScroll, "Play");

    settingsTabs = new QTabWidget; tabs->addTab(settingsTabs, "Settings");
    configuration = new QWidget; auto *network = new QVBoxLayout(configuration); network->setContentsMargins(18, 24, 18, 16); network->setSpacing(18);
    network->addWidget(text("Choose your connection once. Your choices are saved for the next session."));
    auto *networkForm = new QFormLayout; networkForm->setVerticalSpacing(16);
    networkForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    local = new QComboBox; local->setObjectName("localAdapter"); overlay = new QComboBox; overlay->setObjectName("overlayAdapter");
    local->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); overlay->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    local->setMinimumContentsLength(24); overlay->setMinimumContentsLength(24);
    networkForm->addRow("Switch connection", local); networkForm->addRow("ZeroTier connection", overlay);
    gateway = new QLineEdit; gateway->setPlaceholderText("Automatic from ZeroTier subnet"); networkForm->addRow("Fake gateway", gateway); network->addLayout(networkForm);
    network->addWidget(text("Connect the computer and Switch to the same local network. Connect ZeroTier on the computer before selecting its adapter. The game subnet comes from that adapter."));
    refresh = new QPushButton("Refresh adapters"); network->addWidget(refresh, 0, Qt::AlignLeft);
    network->addWidget(text("The desktop relay supports one Switch on a /24 ZeroTier network. Check the selected adapter’s address against the ZeroTier app."));
#ifdef Q_OS_WIN
    network->addWidget(text("Install Npcap and ZeroTier before starting. This Windows preview uses your existing local network; hotspot setup is manual."));
#endif
    // Replaced by checkDependencies() at startup; this is what previews and
    // screenshots show, so it must not be an empty button.
    requirements = text("Checking for ZeroTier and packet capture support…"); network->addWidget(requirements);
    setupRequirements = new QPushButton("Check required software"); setupRequirements->setEnabled(false);
    network->addWidget(setupRequirements, 0, Qt::AlignLeft);
    network->addStretch(); settingsTabs->addTab(configuration, "Connection");

    auto *advanced = new QWidget; auto *av = new QVBoxLayout(advanced); av->setContentsMargins(18, 20, 18, 12); av->setSpacing(12);
    av->addWidget(text("Troubleshooting & reports"));
    diagnostics = new QCheckBox("Detailed traffic diagnostics"); capture = new QCheckBox("Save packet captures for the next relay session"); discovery = new QCheckBox("Find the Switch automatically");
    av->addWidget(diagnostics); av->addWidget(capture); av->addWidget(discovery);
    av->addWidget(text("Packet captures include game payloads and network addresses. Reports stay on this computer until you choose to share them."));
    auto *binaryRow = new QHBoxLayout; executable = new QLineEdit; executable->setPlaceholderText("Bundled relay (recommended)"); executable->setClearButtonEnabled(true); auto *choose = new QPushButton("Choose…");
    binaryRow->addWidget(executable); binaryRow->addWidget(choose); av->addWidget(text("Relay executable")); av->addLayout(binaryRow);
    log = new QPlainTextEdit; log->setReadOnly(true); log->setMaximumBlockCount(2000); log->setPlaceholderText("The relay log will appear here when you start a session.");
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); av->addWidget(log, 1);
    auto *reportButtons = new QHBoxLayout; auto *exportButton = new QPushButton("Export report…"); auto *openFolder = new QPushButton("Open reports folder");
    reportButtons->addWidget(exportButton); reportButtons->addWidget(openFolder); reportButtons->addStretch(); av->addLayout(reportButtons);
    settingsTabs->addTab(advanced, "Advanced");
    gateway->setText(preferences.gateway); executable->setText(preferences.relayPath == bundled ? QString() : preferences.relayPath);
    diagnostics->setChecked(preferences.diagnostics); capture->setChecked(preferences.capture); discovery->setChecked(preferences.discover);
    connect(refresh, &QPushButton::clicked, this, &Window::refreshAdapters);
    connect(setupRequirements, &QPushButton::clicked, this, &Window::setupDependencies);
    connect(&dependencies, &DependencyInstaller::progress, requirements, &QLabel::setText);
    connect(&dependencies, &DependencyInstaller::completed, this, [this](const QString &message) {
        checkDependencies();
        QMessageBox::information(this, "Setup complete", message + "\n\nClick OK to restart the application.");

        QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments());
        QCoreApplication::quit();
    });
    connect(&dependencies, &DependencyInstaller::failed, this, [this](const QString &message) {
        checkDependencies(); QMessageBox::warning(this, "Setup needs attention", message);
    });
    for (auto *combo : {local, overlay}) connect(combo, &QComboBox::currentIndexChanged, this, [this] { save(); });
    for (auto *edit : {gateway, executable}) connect(edit, &QLineEdit::textChanged, this, [this] { save(); });
    for (auto *check : {diagnostics, capture, discovery}) connect(check, &QCheckBox::toggled, this, [this] { save(); });
    manualMode->setChecked(!preferences.dhcp); autoMode->setChecked(preferences.dhcp);
    for (auto *mode : {manualMode, autoMode}) connect(mode, &QRadioButton::toggled, this, [this] { save(); });
    connect(choose, &QPushButton::clicked, this, [this] {
        if (relay.busy()) return;
        auto path = QFileDialog::getOpenFileName(this, "Choose relay executable", executable->text());
        if (!path.isEmpty()) executable->setText(path);
    });
    connect(&relay, &RelayController::stateChanged, choose, [this, choose] { choose->setEnabled(!relay.busy()); });
    connect(start, &QPushButton::clicked, this, [this] {
        if (previewMode) return;
        refreshAdapters(); if (!preferences.validate(adapters).isEmpty()) return;
        log->clear(); switchStatus->setText("Looking for your Switch…"); relay.start(preferences, adapters);
    });
    connect(stop, &QPushButton::clicked, &relay, &RelayController::stop);
    connect(&relay, &RelayController::lineReceived, log, &QPlainTextEdit::appendPlainText);
    connect(&relay, &RelayController::message, status, &QLabel::setText);
    connect(&relay, &RelayController::switchDetected, this, [this](const QString &s) { switchStatus->setText("Local console detected: " + s); });
    connect(&relay, &RelayController::stateChanged, this, [this] { updateState(); if (closing && !relay.busy()) close(); });
    connect(exportButton, &QPushButton::clicked, this, &Window::exportReport);
    connect(openFolder, &QPushButton::clicked, this, [] {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/reports";
        QDir().mkpath(dir); QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    loading = false; refreshAdapters();
    // Missing ZeroTier or Npcap is reported once the window is up, not silently
    // left in Settings: without them Start cannot work at all.
    if (!previewMode) QTimer::singleShot(0, this, [this] { checkDependencies(); promptForDependencies(); });
}
void Window::selectPage(int page, int sub) { tabs->setCurrentIndex(page); settingsTabs->setCurrentIndex(sub); }
void Window::refreshAdapters() {
    if (relay.busy()) return;
    loading = true;
    adapters = preferences.refreshAdapters();
    for (const auto &a : adapters) {
        if (preferences.localInterface.isEmpty() && a.up && !a.overlay && (a.wifi || a.name == "en0")) preferences.localInterface = a.name;
        if (preferences.overlayInterface.isEmpty() && a.up && a.overlay) preferences.overlayInterface = a.name;
    }
    for (auto pair : {qMakePair(local, preferences.localInterface), qMakePair(overlay, preferences.overlayInterface)}) {
        pair.first->clear(); pair.first->addItem("Choose an adapter", QString());
        for (const auto &a : adapters) {
            const QString label = a.label.isEmpty() ? a.name : a.label;
            pair.first->addItem(label + " — " + a.ip + (a.up ? "" : " (offline)"), a.name);
            pair.first->setItemData(pair.first->count() - 1, a.name, Qt::ToolTipRole);
        }
        int index = pair.first->findData(pair.second);
        if (index < 0 && !pair.second.isEmpty()) { pair.first->addItem(pair.second + " (unavailable)", pair.second); index = pair.first->count() - 1; }
        pair.first->setCurrentIndex(qMax(0, index));
    }
    loading = false; save();
}
void Window::save() {
    if (loading || relay.busy()) return;
    preferences.localInterface = local->currentData().toString(); preferences.overlayInterface = overlay->currentData().toString();
    preferences.gateway = gateway->text().trimmed(); preferences.relayPath = executable->text().trimmed();
    const QString bundled = bundledRelayPath();
    if (preferences.relayPath.isEmpty()) preferences.relayPath = bundled;
    preferences.diagnostics = diagnostics->isChecked(); preferences.capture = capture->isChecked(); preferences.discover = discovery->isChecked();
    preferences.dhcp = autoMode->isChecked();
    if (!previewMode) {
        auto stored = preferences;
        if (stored.relayPath == bundled) stored.relayPath.clear(); // Moving the app must not leave a stale path.
        stored.save(settings);
    }
    updateState();
}
void Window::updateState() {
    auto a = preferences.overlay(adapters);
    address->setText(a.ip.isEmpty() ? "—" : a.ip); mask->setText(a.mask.isEmpty() ? "—" : a.mask);
    gatewayValue->setText(preferences.gateway.isEmpty() ? (a.gateway.isEmpty() ? "—" : a.gateway) : preferences.gateway);
    auto error = preferences.validate(adapters); validation->setText(error);
    switchSettingsGroup->setVisible(!preferences.dhcp);
    settingsHint->setVisible(!preferences.dhcp);
    dhcpHint->setVisible(preferences.dhcp);
    if (!relay.busy()) status->setText(error.isEmpty() ? "Ready to connect" : "Finish connection setup");
    start->setEnabled(!relay.busy() && error.isEmpty()); stop->setEnabled(relay.busy() && relay.state() != RelayController::Stopping);
    stop->setText(relay.state() == RelayController::Authorizing ? "Cancel" : "Stop relay");
    configuration->setEnabled(!relay.busy());
    for (QWidget *w : std::initializer_list<QWidget *>{diagnostics, capture, discovery, executable, manualMode, autoMode}) w->setEnabled(!relay.busy());
}
void Window::checkDependencies() {
    if (!requirements || !setupRequirements) return;
    const DependencyStatus state = dependencies.status();
#ifdef Q_OS_WIN
    if (state.winPcap) {
        requirements->setText("WinPcap is installed. Remove it before installing Npcap, then reopen GRID0 Relay.");
        setupRequirements->setText("Open Apps & Features…"); setupRequirements->setEnabled(true);
        return;
    }
    QStringList missing;
    if (!state.zeroTier) missing << "ZeroTier One";
    if (!state.npcap) missing << "Npcap";
    if (missing.isEmpty()) {
        requirements->setText("ZeroTier One and Npcap are ready.");
        setupRequirements->setText("Required software installed"); setupRequirements->setEnabled(false);
    } else {
        requirements->setText("Required software missing: " + missing.join(" and ") + ".");
        setupRequirements->setText("Download and install…"); setupRequirements->setEnabled(true);
    }
#elif defined(Q_OS_MACOS)
    if (state.zeroTier) {
        requirements->setText("ZeroTier is ready. macOS already includes libpcap.");
        setupRequirements->setText("ZeroTier installed"); setupRequirements->setEnabled(false);
    } else {
        requirements->setText("ZeroTier is required. macOS already includes libpcap.");
        setupRequirements->setText("Get ZeroTier for macOS…"); setupRequirements->setEnabled(true);
    }
#elif defined(Q_OS_LINUX)
    if (state.zeroTier && state.npcap) {
        requirements->setText("ZeroTier and libpcap are ready.");
        setupRequirements->setText("Required software installed"); setupRequirements->setEnabled(false);
    } else if (!state.zeroTier) {
        requirements->setText(state.npcap ? "ZeroTier is required." :
            "ZeroTier is required, and libpcap is missing: install your distribution's package "
            "(libpcap0.8 on Debian and Ubuntu, libpcap on Fedora and Arch).");
        setupRequirements->setText("Get ZeroTier for Linux…"); setupRequirements->setEnabled(true);
    } else {
        requirements->setText("libpcap is missing. Install your distribution's package "
            "(libpcap0.8 on Debian and Ubuntu, libpcap on Fedora and Arch), then reopen GRID0 Relay.");
        setupRequirements->setText("Install libpcap with your package manager"); setupRequirements->setEnabled(false);
    }
#else
    requirements->hide(); setupRequirements->hide();
#endif
}
void Window::promptForDependencies() {
    if (previewMode) return;
    const DependencyStatus state = dependencies.status();
#ifdef Q_OS_WIN
    if (state.winPcap) {
        QMessageBox box(QMessageBox::Warning, "GRID0 Relay needs Npcap",
            "WinPcap is installed on this PC. Npcap cannot be installed beside it, and GRID0 Relay "
            "needs Npcap to see your Switch's traffic.\n\nRemove WinPcap in Apps & Features, then reopen "
            "GRID0 Relay and it will offer the Npcap installer.", QMessageBox::NoButton, this);
        auto *open = box.addButton("Open Apps & Features…", QMessageBox::AcceptRole);
        box.addButton("Not now", QMessageBox::RejectRole);
        box.setDefaultButton(open);
        box.exec();
        if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl("ms-settings:appsfeatures"));
        return;
    }
    if (state.zeroTier && state.npcap) return;
    QStringList missing;
    if (!state.zeroTier) missing << "ZeroTier One";
    if (!state.npcap) missing << "Npcap";
    const bool several = missing.size() > 1;
    QMessageBox box(QMessageBox::Warning, "Required software missing",
        "GRID0 Relay cannot start the relay without " + missing.join(" and ") + ".\n\n"
        "ZeroTier carries your Switch's LAN traffic to your friends, and Npcap lets the relay read and "
        "send that traffic on this PC.", QMessageBox::NoButton, this);
    box.setInformativeText("Installing downloads the official installer" + QString(several ? "s" : "") +
        ", asks Windows to validate " + (several ? "each signature" : "its signature") +
        ", and starts the installation. Npcap opens its own screen so you can approve its driver terms. "
        "Windows will ask for administrator permission.");
    auto *install = box.addButton("Install now…", QMessageBox::AcceptRole);
    box.addButton("Not now", QMessageBox::RejectRole);
    box.setDefaultButton(install);
    box.exec();
    if (box.clickedButton() == install) dependencies.installMissing();
#elif defined(Q_OS_MACOS)
    if (state.zeroTier) return;
    QMessageBox box(QMessageBox::Warning, "ZeroTier is required",
        "GRID0 Relay cannot start the relay without the ZeroTier client.\n\nZeroTier carries your "
        "Switch's LAN traffic to your friends. macOS already includes libpcap, so nothing else is needed.",
        QMessageBox::NoButton, this);
    auto *download = box.addButton("Get ZeroTier…", QMessageBox::AcceptRole);
    box.addButton("Not now", QMessageBox::RejectRole);
    box.setDefaultButton(download);
    box.exec();
    if (box.clickedButton() == download) QDesktopServices::openUrl(QUrl("https://www.zerotier.com/download/"));
#elif defined(Q_OS_LINUX)
    if (state.zeroTier && state.npcap) return;
    QStringList missing;
    if (!state.zeroTier) missing << "the ZeroTier client";
    if (!state.npcap) missing << "libpcap";
    QMessageBox box(QMessageBox::Warning, "Required software missing",
        "GRID0 Relay cannot start the relay without " + missing.join(" and ") + ".\n\n"
        "ZeroTier carries your Switch's LAN traffic to your friends, and libpcap lets the relay read and "
        "send that traffic on this computer.", QMessageBox::NoButton, this);
    if (!state.npcap)
        box.setInformativeText("libpcap comes from your distribution: install libpcap0.8 on Debian and "
            "Ubuntu, or libpcap on Fedora and Arch, then reopen GRID0 Relay.");
    QPushButton *download = state.zeroTier ? nullptr : box.addButton("Get ZeroTier…", QMessageBox::AcceptRole);
    auto *dismiss = box.addButton(download ? "Not now" : "OK", QMessageBox::RejectRole);
    box.setDefaultButton(download ? download : dismiss);
    box.exec();
    if (download && box.clickedButton() == download) QDesktopServices::openUrl(QUrl("https://www.zerotier.com/download/"));
#endif
}
void Window::setupDependencies() {
    const DependencyStatus state = dependencies.status();
#ifdef Q_OS_WIN
    if (state.winPcap) {
        if (QMessageBox::question(this, "Remove WinPcap", "GRID0 Relay needs Npcap and cannot install it beside WinPcap. Open Apps & Features to remove WinPcap now?") == QMessageBox::Yes) {
            QDesktopServices::openUrl(QUrl("ms-settings:appsfeatures"));
            QMessageBox::information(this, "Reopen GRID0 Relay", "After removing WinPcap, close and reopen GRID0 Relay. It will then offer the Npcap installer.");
        }
        return;
    }
    if (state.zeroTier && state.npcap) return;
    QStringList missing;
    if (!state.zeroTier) missing << "ZeroTier One";
    if (!state.npcap) missing << "Npcap";
    const QString prompt = "GRID0 Relay will download the official " + missing.join(" and ") +
        " installer" + (missing.size() == 1 ? QString() : "s") +
        ", validate each Windows signature, and start installation. Npcap opens its own installation screen so you can approve its driver terms. Windows administrator permission is required. Continue?";
    if (QMessageBox::question(this, "Install required software", prompt) == QMessageBox::Yes) dependencies.installMissing();
#elif defined(Q_OS_MACOS)
    if (!state.zeroTier && QMessageBox::question(this, "Get ZeroTier", "macOS already includes libpcap. Open ZeroTier’s official macOS download page?") == QMessageBox::Yes)
        QDesktopServices::openUrl(QUrl("https://www.zerotier.com/download/"));
#elif defined(Q_OS_LINUX)
    if (!state.zeroTier && QMessageBox::question(this, "Get ZeroTier", "libpcap comes from your distribution. Open ZeroTier’s official download page?") == QMessageBox::Yes)
        QDesktopServices::openUrl(QUrl("https://www.zerotier.com/download/"));
#endif
}
void Window::exportReport() {
    if (relay.busy()) { QMessageBox::information(this, "Finish the session first", "Stop the relay before exporting, so all packet captures are complete."); return; }
    QString source = relay.reportDirectory();
    if (source.isEmpty()) { QMessageBox::information(this, "No session report yet", "Start a relay session first. Earlier reports are available in the reports folder."); return; }
    QString parent = QFileDialog::getExistingDirectory(this, "Export report to folder"); if (parent.isEmpty()) return;
    QString dest = parent + "/Grid0-Relay-report-" + QFileInfo(source).fileName();
    if (QFileInfo::exists(dest) || !QDir().mkdir(dest)) { QMessageBox::warning(this, "Cannot export", "Choose a folder without an existing copy of this report."); return; }
    for (const auto &name : QDir(source).entryList(QDir::Files)) {
        if (!QFile::copy(source + "/" + name, dest + "/" + name)) {
            QMessageBox::warning(this, "Report incomplete", "Could not copy " + name + ". The original report is still available."); return;
        }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
}
void Window::closeEvent(QCloseEvent *event) {
    if (relay.busy()) {
        closing = true; status->setText("Stopping the relay before closing…"); relay.stop(); event->ignore();
    } else event->accept();
}
