#pragma once
#include "controller.h"
#include "dependencies.h"
#include <QMainWindow>
#include <QTabWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPlainTextEdit>

class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(bool preview = false);
    void selectPage(int page, int settingsPage = 0);
protected:
    void closeEvent(QCloseEvent *) override;
private:
    void refreshAdapters();
    void save();
    void updateState();
    void exportReport();
    void checkDependencies();
    void promptForDependencies();
    void setupDependencies();
    Preferences preferences;
    QList<Adapter> adapters;
    RelayController relay;
    DependencyInstaller dependencies;
    QSettings settings;
    QTabWidget *tabs, *settingsTabs;
    QComboBox *local, *overlay;
    QLineEdit *gateway, *executable;
    QCheckBox *diagnostics, *capture, *discovery;
    QLabel *status, *switchStatus, *address, *mask, *gatewayValue, *validation;
    QLabel *requirements = nullptr;
    QPushButton *start, *stop, *refresh, *setupRequirements = nullptr;
    QPlainTextEdit *log;
    QWidget *configuration;
    bool loading = true, closing = false, previewMode = false;
};



// TEMPORARY FOR TESTING



class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(bool preview = false);
    void selectPage(int page, int settingsPage = 0);
protected:
    void closeEvent(QCloseEvent *) override;
private:
    void refreshAdapters();
    void save();
    void updateState();
    void exportReport();
    void checkDependencies();
    void promptForDependencies();
    void setupDependencies();
    void scanArpTable();

    Preferences preferences;
    QList<Adapter> adapters;
    RelayController relay;
    DependencyInstaller dependencies;
    QSettings settings;
    QTabWidget *tabs, *settingsTabs;
    QComboBox *local, *overlay;
    QLineEdit *gateway, *executable;
    QCheckBox *diagnostics, *capture, *discovery;
    QLabel *status, *switchStatus, *address, *mask, *gatewayValue, *validation;
    QLabel *requirements = nullptr;
    QPushButton *start, *stop, *refresh, *setupRequirements = nullptr;
    QPushButton *scanArp = nullptr; // <--- ADD THIS BUTTON POINTER
    QPlainTextEdit *log;
    QWidget *configuration;
    bool loading = true, closing = false, previewMode = false;
};
