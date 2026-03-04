#ifndef UPDATEDIALOG_H
#define UPDATEDIALOG_H

#include "appmanagerservice.h"

#include <QDialog>
#include <QHash>
#include <QStringList>

class QLabel;
class QListWidget;
class QProgressBar;
class QStackedWidget;

/**
 * @brief 在线升级工作流对话框。
 *
 * 将"检测 → 选择 → 升级 → 结果"四阶段以向导形式呈现在单一对话框内，
 * 逻辑层调用 AppManagerService 完成实际检测与安装操作。
 */
class UpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UpdateDialog(AppManagerService *service,
                         const QVector<AppConfig> &apps,
                         QWidget *parent = nullptr);

    /** 返回在线检测结果缓存，供调用方刷新界面。 */
    QHash<QString, OnlineAppInfo> onlineCache() const { return m_onlineCache; }

    /** 返回成功连接服务器的应用数。 */
    int serverSuccessCount() const { return m_serverSuccessCount; }

signals:
    /** 输出日志条目（不含时间戳，由外部负责格式化）。 */
    void logMessage(const QString &message);

protected:
    void showEvent(QShowEvent *event) override;
    void reject() override;

private:
    // ---- UI 构建 ----
    void buildUi();
    QWidget *createCheckPage();
    QWidget *createSelectPage();
    QWidget *createUpgradePage();
    QWidget *createResultPage();

    // ---- 阶段流程 ----
    void runCheckPhase();
    void showSelectPhase();
    void runUpgradePhase(const QList<int> &selectedIndexes);
    void showResultPhase(const QString &title, const QString &detail);

    // ---- 工具 ----
    void emitLog(const QString &msg);
    static QString compactStatus(const QString &text, int maxLen = 90);

    // ---- 进程管理 ----
    bool isProcessRunning(const QString &exePath) const;
    bool terminateProcess(const QString &exePath);
    void relaunchClosedApps();

private:
    AppManagerService *m_service;
    QVector<AppConfig> m_apps;
    QHash<QString, OnlineAppInfo> m_onlineCache;

    // 可升级应用索引（check 阶段产出）
    QList<int> m_updatableIndexes;

    // 流程控制
    bool m_phaseStarted = false;
    bool m_canceled = false;

    // 统计
    int m_serverSuccessCount = 0;
    int m_serverFailCount = 0;
    int m_upgradeSuccessCount = 0;
    int m_totalToUpgrade = 0;

    // 全流程日志
    QStringList m_logEntries;

    // 升级前自动关闭的程序（exe绝对路径），升级后重启
    QStringList m_closedApps;

    // ---- UI 组件 ----
    enum PageIndex { PageCheck = 0, PageSelect = 1, PageUpgrade = 2, PageResult = 3 };
    QStackedWidget *m_stack = nullptr;

    // 检测页
    QLabel *m_checkLabel = nullptr;
    QProgressBar *m_checkProgress = nullptr;

    // 选择页
    QListWidget *m_selectList = nullptr;

    // 升级页
    QLabel *m_upgradeLabel = nullptr;
    QProgressBar *m_upgradeProgress = nullptr;

    // 结果页
    QLabel *m_resultTitle = nullptr;
    QLabel *m_resultDetail = nullptr;
};

#endif // UPDATEDIALOG_H
