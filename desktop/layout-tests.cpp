#include "window.h"
#include <QApplication>
#include <QGroupBox>
#include <QScrollArea>
#include <QtTest>
class LayoutTests : public QObject {
    Q_OBJECT
private slots:
    void switchSettingsStayReadable() {
        Window window(true);
        window.resize(640, 590);
        window.show();
        auto *status = window.findChild<QLabel *>("relayStatus");
        auto *group = window.findChild<QGroupBox *>("switchSettings");
        QVERIFY(status && group);
        status->setText("Local capture failed: the selected network adapter could not be opened. Refresh adapters in Settings and inspect the capture error.");
        for (const char *name : {"switchIP", "switchMask", "switchGateway"}) {
            auto *value = window.findChild<QLabel *>(name);
            QVERIFY(value);
            value->setText("255.255.255.255");
        }
        QTest::qWait(50);
        for (auto *label : group->findChildren<QLabel *>()) {
            QVERIFY2(label->height() >= label->fontMetrics().height(), qPrintable(label->text()));
            QVERIFY2(label->width() >= label->fontMetrics().horizontalAdvance(label->text()), qPrintable(label->text()));
            QVERIFY(group->rect().contains(label->geometry()));
        }
        for (auto *button : group->findChildren<QPushButton *>()) {
            QVERIFY(button->height() >= button->sizeHint().height());
            QVERIFY(group->rect().contains(button->geometry()));
        }
    }
};
// This console test has its own main, not a WinMain/Qt entry-point wrapper.
#ifdef main
#undef main
#endif
QTEST_MAIN(LayoutTests)
#include "layout-tests.moc"
