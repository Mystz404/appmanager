#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "appmanagerservice.h"

#include <QMainWindow>
#include <QHash>
#include <QQueue>
#include <QSet>

class DocBrowserPage;

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMovie;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QStackedWidget;
class QToolButton;
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
    void doActualLaunch(const AppConfig &app, bool forceNewWindow = false);
    QString logFilePath() const;
    void checkServerConnection();
    void trySilentAppManagerAutoUpdate();
    void tryStartupAppsUpdateCheck();
    void fetchRemoteCatalog();
    QIcon createDownloadIcon(int size) const;
    void refreshUpdateActions();
    QVector<AppConfig> collectUpdatableApps() const;
    void setInlineUpdateCheckState(bool checking,
                                   int current,
                                   int total,
                                   const QString &message);
    bool eventFilter(QObject *obj, QEvent *event) override;
    void showAppContextMenu(const QString &appId, const QPoint &globalPos);
    void processDownloadQueue();
    void doDownloadRemoteApp(const QString &appId);
    void updateDownloadItemBadge(const QString &appId, const QString &badge);
    void filterAppList(const QString &text);

private slots:
    void onAddLocalApp();
    void onCheckUpdates();
    void onUpdateAllApps();
    void onAppIconClicked(QListWidgetItem *item);
    void onOpenAppLocation(const QString &appId);
    void onDeleteApp(const QString &appId);
    void onDownloadHistoryVersion(const QString &appId);
    void onDownloadRemoteApp(const QString &appId);
    void onCheckAppManagerUpdate();
    void onAboutAppManager();
    void onLoginClicked();
    void onLogoutClicked();

private:
    Ui::MainWindow *ui;

    AppManagerService m_service;
    QString m_configPath;

    QLabel *m_titleLabel = nullptr;
    QStackedWidget *m_mainStack = nullptr;
    QToolButton *m_navLaunchButton = nullptr;
    QToolButton *m_navCommunityButton = nullptr;
    QToolButton *m_navHelpButton = nullptr;
    QToolButton *m_navLogButton = nullptr;
    QToolButton *m_loginButton  = nullptr;

    QListWidget *m_appList = nullptr;
    QLineEdit   *m_launchSearchBox = nullptr;
    QPushButton *m_addLocalAppButton = nullptr;
    QPushButton *m_checkUpdatesButton = nullptr;
    QPushButton *m_updateAllButton = nullptr;
    QMovie      *m_refreshMovie = nullptr;
    QProgressBar *m_updateCheckProgress = nullptr;
    QLabel *m_updateCheckHintLabel = nullptr;

    QPlainTextEdit *m_logView = nullptr;

    DocBrowserPage *m_docBrowserPage = nullptr;

    bool m_serverConnected = false;
    bool m_startupAutoUpdateChecked = false;
    bool m_startupAutoUpdateRunning = false;
    bool m_startupAppsUpdateCheckScheduled = false;
    bool m_isCheckingUpdates = false;

    // 登录状态
    QString m_authToken;
    QString m_authUser;

    QHash<QString, OnlineAppInfo> m_onlineCache;
    QHash<QString, AppConfig> m_appById;

    // AppManager 自身更新说明（从服务端获取）
    QString m_appManagerChangeLog;

    // 服务器清单中本地不存在的应用
    QHash<QString, QJsonObject> m_remoteCatalog;

    // 下载队列
    QQueue<QString> m_downloadQueue;
    QSet<QString>   m_downloadQueuedSet;  // 包含「已排队」和「下载中」的所有 id
};

#endif // MAINWINDOW_H
