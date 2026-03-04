#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "appmanagerservice.h"

#include <QMainWindow>
#include <QHash>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;
class QPlainTextEdit;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void setupUiWidgets();
    void connectSignals();
    void applySimpleStyle();
    void logToFile(const QString &message);
    void appendLog(const QString &message);
    void loadConfig();
    void refreshAppIcons();
    bool launchAppById(const QString &appId);
    QString logFilePath() const;

private slots:
    void onCheckRequiredFiles();
    void onCheckUpdates();
    void onAppIconClicked(QListWidgetItem *item);

private:
    Ui::MainWindow *ui;

    AppManagerService m_service;
    QString m_configPath;

    QLabel *m_titleLabel = nullptr;
    QListWidget *m_appList = nullptr;
    QPushButton *m_checkRequiredButton = nullptr;
    QPushButton *m_checkUpdatesButton = nullptr;

    QPlainTextEdit *m_logView = nullptr;

    QHash<QString, OnlineAppInfo> m_onlineCache;
    QHash<QString, AppConfig> m_appById;
};
#endif // MAINWINDOW_H
