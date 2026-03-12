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
    void validateLocalApps();
    void refreshAppIcons();
    bool launchAppById(const QString &appId);
    void startUpdateWorkflow(const QVector<AppConfig> &apps);
    QString logFilePath() const;
    void checkServerConnection();
    void trySilentAppManagerAutoUpdate();
    void tryStartupAppsUpdateCheck();
    void fetchRemoteCatalog();
    QIcon createDownloadIcon(int size) const;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void showAppContextMenu(const QString &appId, const QPoint &globalPos);

private slots:
    void onCheckUpdates();
    void onAppIconClicked(QListWidgetItem *item);
    void onOpenAppLocation(const QString &appId);
    void onDeleteApp(const QString &appId);
    void onDownloadHistoryVersion(const QString &appId);
    void onDownloadRemoteApp(const QString &appId);
    void onCheckAppManagerUpdate();
    void onAboutAppManager();

private:
    Ui::MainWindow *ui;

    AppManagerService m_service;
    QString m_configPath;

    QLabel *m_titleLabel = nullptr;
    QListWidget *m_appList = nullptr;
    QPushButton *m_checkUpdatesButton = nullptr;

    QPlainTextEdit *m_logView = nullptr;

    bool m_serverConnected = false;
    bool m_startupAutoUpdateChecked = false;
    bool m_startupAutoUpdateRunning = false;
    bool m_startupAppsUpdateCheckScheduled = false;

    QHash<QString, OnlineAppInfo> m_onlineCache;
    QHash<QString, AppConfig> m_appById;

    // 服务器清单中本地不存在的应用
    QHash<QString, QJsonObject> m_remoteCatalog;
};
#endif // MAINWINDOW_H
