#include "model.h"
#include "../src/capture-name.h"
#include "controller.h"
#include <QTemporaryDir>
#include <QFile>
#include <QtTest>
#include <QStandardPaths>
class Tests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
#ifdef Q_OS_WIN
    void liveWindowsAdapterResolution() {
        int tested = 0;
        for (const auto &i : QNetworkInterface::allInterfaces()) {
            if (i.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
            if (!i.isValid()) continue;
            const auto capture = windowsCaptureName(i.name());
            QVERIFY2(capture.startsWith("\\Device\\NPF_{"), qPrintable(i.name()));
            ++tested;
        }
        QVERIFY(tested > 0);
    }
    void windowsLaunchAndStop() {
        Preferences p; p.localInterface = "{01234567-abcd-4321-abcd-0123456789ab}"; p.overlayInterface = "{01234567-abcd-4321-abcd-0123456789ac}";
        p.relayPath = QCoreApplication::applicationDirPath() + "/zll-test-relay.exe";
        QList<Adapter> adapters{{"{01234567-abcd-4321-abcd-0123456789ab}", "Wi-Fi", "192.168.1.3", "255.255.255.0", "192.168.1.0/24", "192.168.1.1", false, true, true},
            {"{01234567-abcd-4321-abcd-0123456789ac}", "ZeroTier", "10.42.9.123", "255.255.255.0", "10.42.9.0/24", "10.42.9.1", true, false, true}};
        RelayController relay;
        QSignalSpy candidates(&relay, &RelayController::switchDetected);
        for (int session = 0; session < 2; ++session) {
            relay.start(p, adapters);
            QTRY_COMPARE_WITH_TIMEOUT(relay.state(), RelayController::Running, 5000);
            relay.stop();
            QTRY_COMPARE_WITH_TIMEOUT(relay.state(), RelayController::Idle, 7000);
            QFile log(relay.reportDirectory() + "/relay.log");
            QVERIFY(log.open(QIODevice::ReadOnly));
            QVERIFY(log.readAll().contains("Mock relay stopped"));
        }
        QCOMPARE(candidates.count(), 2);
        QSignalSpy messages(&relay, &RelayController::message);
        qputenv("ZLL_TEST_FAIL", "1");
        relay.start(p, adapters);
        QTRY_COMPARE_WITH_TIMEOUT(relay.state(), RelayController::Idle, 5000);
        qunsetenv("ZLL_TEST_FAIL");
        QVERIFY(messages.last().at(0).toString().contains("missing Npcap"));
    }
#endif
#ifdef Q_OS_LINUX
    void linuxStartWithoutPkexec() {
        Preferences p; p.localInterface = "test-lan"; p.overlayInterface = "test-zt";
        p.relayPath = QCoreApplication::applicationFilePath();
        QList<Adapter> adapters{{"test-lan", "Wired", "192.168.1.3", "255.255.255.0", "192.168.1.0/24", "192.168.1.1", false, false, true},
            {"test-zt", "ZeroTier", "10.42.9.123", "255.255.255.0", "10.42.9.0/24", "10.42.9.1", true, false, true}};
        QVERIFY(p.validate(adapters).isEmpty());
        RelayController relay;
        QSignalSpy messages(&relay, &RelayController::message);
        // No polkit on this machine: the app must hand back a runnable command
        // instead of sitting in Authorizing forever.
        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", "/nonexistent");
        relay.start(p, adapters);
        qputenv("PATH", path);
        QCOMPARE(relay.state(), RelayController::Idle);
        QVERIFY(!messages.isEmpty());
        const QString last = messages.last().at(0).toString();
        QVERIFY2(last.contains("sudo ") && last.contains("--zerotier-if"), qPrintable(last));
    }
#endif
    void windowsAdapterNames() {
        const QString guid = "{01234567-abcd-4321-abcd-0123456789ab}";
        const QString device = "\\Device\\NPF_" + guid.toUpper();
        QCOMPARE(windowsCaptureName(guid), device);
        QCOMPARE(windowsCaptureName(device), device);
        QVERIFY(windowsCaptureName("invalid_adapter_name").isEmpty());
        QString requested;
        auto lookup = [&](const QString &name) { requested = name; return guid; };
        QCOMPARE(windowsCaptureName("wireless_32768", lookup), device);
        QCOMPARE(requested, "wireless_32768");
        QCOMPARE(windowsCaptureName("ethernet_32769", lookup), device);
        QVERIFY(windowsCaptureName("wireless_32768", [](const QString &) { return QString(); }).isEmpty());
        QVERIFY(capture_names_equal(device.toUtf8().constData(), device.toLower().toUtf8().constData(), 1));
        QVERIFY(!capture_names_equal(device.toUtf8().constData(), device.toLower().toUtf8().constData(), 0));
        QVERIFY(!capture_names_equal("\\Device\\NPF_{abc}", "\\Device\\NPF_{abd}", 1));
    }
    void subnets() {
        QCOMPARE(subnetFor("10.42.9.123", "255.255.255.0"), "10.42.9.0/24");
        QCOMPARE(subnetFor("172.20.4.2", "255.255.0.0"), "172.20.0.0/16");
        QVERIFY(subnetFor("bad", "255.255.255.0").isEmpty());
        QVERIFY(subnetFor("10.1.1.1", "255.0.255.0").isEmpty());
    }
    void persistenceAndValidation() {
        QTemporaryDir temp;
        QSettings store(temp.filePath("prefs.ini"), QSettings::IniFormat);
        Preferences p; p.localInterface = "{01234567-abcd-4321-abcd-0123456789ab}"; p.overlayInterface = "{01234567-abcd-4321-abcd-0123456789ac}";
        p.capture = true; p.diagnostics = true; p.gateway = "10.42.9.1";
        p.relayPath = QCoreApplication::applicationFilePath(); p.save(store);
        Preferences restored; restored.load(store);
        QCOMPARE(restored.localInterface, p.localInterface); QCOMPARE(restored.capture, true);
        QList<Adapter> adapters{{"{01234567-abcd-4321-abcd-0123456789ab}", "Wi-Fi", "192.168.1.3", "255.255.255.0", "192.168.1.0/24", "192.168.1.1", false, true, true},
            {"{01234567-abcd-4321-abcd-0123456789ac}", "ZeroTier", "10.42.9.123", "255.255.255.0", "10.42.9.0/24", "10.42.9.1", true, false, true}};
        QVERIFY(restored.validate(adapters).isEmpty());
        auto args = restored.arguments(adapters, "/tmp/test with spaces");
        QVERIFY(args.contains("10.42.9.0/24")); QVERIFY(args.contains("--status-events"));
        QCOMPARE(args.last(), "/tmp/test with spaces");
        restored.localInterface = "missing"; QVERIFY(!restored.validate(adapters).isEmpty());
        restored = p; restored.gateway = "10.42.9.123"; QVERIFY(!restored.validate(adapters).isEmpty());
        restored = p; restored.gateway = "10.42.9.255"; QVERIFY(!restored.validate(adapters).isEmpty());
        restored = p; restored.overlayInterface = "{01234567-abcd-4321-abcd-0123456789ab}"; QVERIFY(!restored.validate(adapters).isEmpty());
    }
    void quoting() {
        QCOMPARE(shellQuote("a'b $(touch /tmp/no)"), "'a'\"'\"'b $(touch /tmp/no)'");
        QCOMPARE(appleScriptQuote("a\"b\\c"), "\"a\\\"b\\\\c\"");
    }
};
QTEST_GUILESS_MAIN(Tests)
#include "tests.moc"
