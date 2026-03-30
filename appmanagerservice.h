#ifndef APPMANAGERSERVICE_H
#define APPMANAGERSERVICE_H

#include "apptypes.h"
#include "docclienttypes.h"

#include <functional>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QVector>

/**
 * @brief 多应用管理服务。
 *
 * 负责：
 * 1) 读取本地配置并构建应用列表；
 * 2) 检查本地文件完整性与本地版本；
 * 3) 向服务端请求在线版本信息；
 * 4) 下载并执行“替换可执行文件”式升级。
 */
class AppManagerService : public QObject
{
    Q_OBJECT

public:
    using DownloadProgressCallback = std::function<void(qint64 receivedBytes, qint64 totalBytes)>;
    using StatusCallback = std::function<void(const QString &status)>;
    using InstallProgressCallback = std::function<void(int percent)>;
    using CancelCallback = std::function<bool()>;

    explicit AppManagerService(QObject *parent = nullptr);

    bool loadConfig(const QString &configPath, QString &errorMessage);

    QString appsRoot() const;
    QVector<AppConfig> apps() const;

    QString appAbsoluteDir(const AppConfig &app) const;
    QString appAbsoluteExePath(const AppConfig &app) const;
    QString appCurrentVersion(const AppConfig &app) const;

    bool checkRequiredFiles(const AppConfig &app, QStringList &missingFiles) const;

    /** 返回缺失的依赖文件列表（不含主 EXE，仅 requiredFiles 中未存在的条目）。 */
    QStringList missingRequiredDeps(const AppConfig &app) const;

    /**
     * @brief 在线检查一个应用是否有更新。
     */
    OnlineAppInfo checkOnlineInfo(const AppConfig &app,
                                  int timeoutMs = 20000,
                                  const CancelCallback &cancelCallback = CancelCallback());

    /**
     * @brief 升级指定应用。
     * @param app 要升级的应用配置。
     * @param online 在线信息（包含最新版本和下载地址）。
     * @param resultMessage 输出执行结果信息。
     * @return true 升级成功；false 升级失败。
     */
    bool upgradeApp(const AppConfig &app,
                    const OnlineAppInfo &online,
                    QString &resultMessage,
                    int timeoutMs = 30000,
                    const DownloadProgressCallback &progressCallback = DownloadProgressCallback(),
                    const StatusCallback &statusCallback = StatusCallback(),
                    const InstallProgressCallback &installProgressCallback = InstallProgressCallback(),
                    const CancelCallback &cancelCallback = CancelCallback());

    /**
     * @brief 检查依赖文件完整性，不完整时下载并解压完整包。
     * @return true 依赖完整（或已修复）；false 修复失败。
     */
    bool checkAndFixDependencies(const AppConfig &app,
                                 const OnlineAppInfo &online,
                                 QString &resultMessage,
                                 int timeoutMs = 180000,
                                 const DownloadProgressCallback &progressCallback = DownloadProgressCallback(),
                                 const StatusCallback &statusCallback = StatusCallback(),
                                 const InstallProgressCallback &installProgressCallback = InstallProgressCallback(),
                                 const CancelCallback &cancelCallback = CancelCallback());

    /// 获取服务器基础 URL
    QString serverBaseUrl() const;

    /// 尝试连接服务器
    bool tryConnectServer(int timeoutMs = 3000);

    /// 从服务器获取应用清单（authToken 非空时携带 Authorization 头）
    QJsonArray fetchAppCatalog(const QString &authToken = QString(),
                               int timeoutMs = 10000,
                               const CancelCallback &cancelCallback = CancelCallback());

    /// 获取某个应用的历史版本列表
    QJsonObject fetchHistoryVersions(const QString &appId,
                                     int timeoutMs = 10000,
                                     const CancelCallback &cancelCallback = CancelCallback());

    /// 下载文件到指定路径
    bool downloadToFile(const QUrl &url,
                        const QString &filePath,
                        QString &errorMessage,
                        int timeoutMs = 30000,
                        const DownloadProgressCallback &progressCallback = DownloadProgressCallback(),
                        const StatusCallback &statusCallback = StatusCallback(),
                        const CancelCallback &cancelCallback = CancelCallback());

    /// 添加应用条目
    void addAppEntry(const AppConfig &app);

    /// 移除应用条目
    bool removeAppEntry(const QString &appId);

    /// 保存配置到文件
    bool saveConfig(QString &errorMessage);

    // ==================== AppManager 自身升级支持 ====================

    /// 获取AppManager应用名称
    QString appManagerName() const { return QStringLiteral("AppManager"); }

    /// 获取AppManager当前版本
    QString appManagerVersion() const;

    /// 检查AppManager是否有新版本
    OnlineAppInfo checkAppManagerUpdate(int timeoutMs = 10000);

    /// 升级AppManager（下载安装程序并启动）
    bool upgradeAppManager(const OnlineAppInfo &online,
                          QString &resultMessage,
                          int timeoutMs = 30000,
                          const DownloadProgressCallback &progressCallback = DownloadProgressCallback(),
                          const StatusCallback &statusCallback = StatusCallback());

    // ==================== 文档管理支持 ====================

    /// 从服务器获取文档目录（/docs/catalog）
    QVector<ClientDocEntry> fetchDocCatalog(int timeoutMs = 15000,
                                             const CancelCallback &cancelCallback = CancelCallback());

    /// 本地文档缓存目录（exe同级 docs/{docId}/）
    QString localDocCacheDir() const;

    /// 某文档的本地缓存路径
    QString localDocFilePath(const ClientDocEntry &doc) const;

    /// 文档文件是否已下载到本地
    bool isDocDownloaded(const ClientDocEntry &doc) const;

    /// 本地文档文件的 SHA256 是否与服务器一致（用于检测更新）
    bool isDocUpToDate(const ClientDocEntry &doc) const;

    /// 下载文档到本地缓存目录（可带进度回调）
    bool downloadDoc(const ClientDocEntry &doc,
                     QString &errorMessage,
                     int timeoutMs = 60000,
                     const DownloadProgressCallback &progressCallback = DownloadProgressCallback(),
                     const CancelCallback &cancelCallback = CancelCallback());

private:
    QByteArray httpGet(const QUrl &url,
                       QString &errorMessage,
                       int timeoutMs,
                       const DownloadProgressCallback &progressCallback = DownloadProgressCallback(),
                       const CancelCallback &cancelCallback = CancelCallback(),
                       const QByteArray &authToken = QByteArray()) const;
    bool downloadFileWithResume(const QUrl &url,
                                const QString &targetFilePath,
                                QString &errorMessage,
                                int timeoutMs,
                                const DownloadProgressCallback &progressCallback = DownloadProgressCallback(),
                                const StatusCallback &statusCallback = StatusCallback(),
                                const CancelCallback &cancelCallback = CancelCallback()) const;
    QStringList loadZipReplaceFileList(QString &errorMessage) const;
    QString resolvePath(const QString &relativePath) const;
    bool upgradeByExeReplace(const AppConfig &app,
                             const OnlineAppInfo &online,
                             const QString &packageFilePath,
                             QString &resultMessage,
                             const StatusCallback &statusCallback,
                             const InstallProgressCallback &installProgressCallback) const;
    bool upgradeByZipExtract(const AppConfig &app,
                             const OnlineAppInfo &online,
                             const QString &zipFilePath,
                             QString &resultMessage,
                             const StatusCallback &statusCallback,
                             const InstallProgressCallback &installProgressCallback) const;

private:
    QString m_configPath;
    QString m_appListPath;
    QString m_appsRootRaw;
    QString m_appsRoot;
    QString m_serverBaseUrl;
    QVector<AppConfig> m_apps;

    // QNAM 复用连接池，提升多个应用检查/下载时的效率。
    mutable QNetworkAccessManager m_networkManager;
};

#endif // APPMANAGERSERVICE_H
