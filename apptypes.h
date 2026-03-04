#ifndef APPTYPES_H
#define APPTYPES_H

#include <QString>
#include <QStringList>
#include <QUrl>

/**
 * @brief 单个受管应用的静态配置。
 *
 * 这些字段来自本地配置文件 `apps.json`，用于描述应用位置、版本、必需文件和在线升级元数据地址。
 *
 * 设计说明：
 * 1) 本结构仅描述“不会频繁变化”的配置数据；
 * 2) 运行时状态（是否缺文件、是否可升级等）应放到 AppRuntimeState；
 * 3) 所有路径字段均建议使用相对 appsRoot 的路径，以便迁移目录时无需改配置。
 */
struct AppConfig
{
    /**
     * @brief 应用唯一标识符。
     *
     * - 来源：apps.json -> apps[i].id
     * - 用途：作为哈希键、缓存键、UI 项目绑定键；
     * - 约束：同一配置中必须唯一，建议仅使用字母/数字/下划线；
     * - 示例："app_report_center"。
     */
    QString id;

    /**
     * @brief 应用显示名称（面向用户）。
     *
     * - 来源：apps.json -> apps[i].name
     * - 用途：界面显示、日志打印、消息框提示；
     * - 约束：允许中文，建议可读且简短。
     */
    QString name;

    /**
     * @brief 主程序可执行文件路径（相对 appsRoot）。
     *
     * - 来源：apps.json -> apps[i].exe
     * - 用途：启动应用、EXE 替换升级、图标提取；
     * - 约束：应指向真实可执行文件；在 Windows 下一般为 .exe。
     */
    QString exeRelativePath;

    /**
     * @brief 必需文件列表（相对 appsRoot）。
     *
     * - 来源：apps.json -> apps[i].requiredFiles[]
     * - 用途：启动前健康检查；缺失时阻止启动并记录日志；
     * - 约束：可包含配置、资源、模板、动态库等关键文件。
     */
    QStringList requiredRelativeFiles;

    /**
     * @brief 在线升级元数据地址。
     *
     * - 来源：apps.json -> apps[i].updateMetaUrl
     * - 用途：拉取 latestVersion、downloadUrl、packageType 等在线信息；
     * - 约束：必须是有效 URL（http/https）；服务端应返回 JSON。
     */
    QUrl updateMetaUrl;
};

/**
 * @brief 单个应用的在线版本信息。
 *
 * 这些字段来自服务端返回的升级元数据，用于判断是否可升级和如何升级。
 *
 * 典型元数据示例：
 * {
 *   "latestVersion": "1.3.0",
 *   "downloadUrl": "https://server/app.exe",
 *   "sha256": "...",
 *   "packageType": "exe",
 *   "installRelativeDir": "managed_apps/AppA"
 * }
 */
struct OnlineAppInfo
{
    /**
     * @brief 在线请求是否成功。
     *
     * - true：网络请求与 JSON 解析都成功；
     * - false：发生超时、网络错误、格式错误或字段缺失。
     */
    bool requestSuccess = false;

    /**
     * @brief 在线请求失败原因。
     *
     * - 当 requestSuccess=false 时用于记录具体错误；
     * - 可能来自网络层（超时/连接失败）或业务层（字段不合法）。
     */
    QString errorMessage;

    /**
     * @brief 服务端声明的最新版本号。
     *
     * - 用途：与本地版本比较，决定是否需要升级；
     * - 约束：建议使用语义化版本号，例如 2.0.15。
     */
    QString latestVersion;

    /**
     * @brief 升级包下载地址。
     *
     * - 用途：下载升级包（exe 或 zip）；
     * - 约束：应可直接 GET 访问，且返回体为完整二进制内容。
     */
    QUrl downloadUrl;

    /**
     * @brief 升级包 SHA256（可选）。
     *
     * - 用途：下载后完整性校验，防止文件损坏或篡改；
     * - 约束：十六进制字符串，建议小写；为空表示跳过校验。
     */
    QString sha256;

    // 升级包类型："exe"（默认）或 "zip"。
    /**
     * @brief 升级包类型。
     *
     * - 取值："exe" 或 "zip"；
     * - exe：执行“替换可执行文件”升级；
     * - zip：下载后解压覆盖目录升级。
     */
    QString packageType = QStringLiteral("exe");

    // 当 packageType=zip 时生效，表示解压覆盖的目标目录（相对 appsRoot）。
    // 若为空则默认覆盖到 exe 所在目录。
    /**
     * @brief ZIP 解压目标目录（相对 appsRoot，可选）。
     *
     * - 仅在 packageType=zip 时有效；
     * - 为空时默认使用 exe 所在目录；
     * - 用于“程序与资源分离”场景下指定统一安装目录。
     */
    QString installRelativeDir;

    /**
     * @brief 安装模式。
     *
     * - 取值："single" 或 "allApps"；
     * - single：仅安装当前 appId 对应应用；
     * - allApps：ZIP 解压后按客户端 apps.json 中所有应用的 exe 路径逐一替换。
     */
    QString installMode = QStringLiteral("single");

    /**
     * @brief ZIP 升级时是否递归替换目标目录下所有 EXE。
     *
     * - 仅在 packageType=zip 时有效；
     * - true：解压后按相对路径递归替换所有 .exe；
     * - false：仅执行普通解压覆盖（兼容旧行为）。
     */
    bool zipReplaceExeRecursively = true;

    /**
     * @brief 服务端声明的依赖文件列表（相对 appsRoot）。
     *
     * - 升级前用于检查本地文件完整性；
     * - 如有缺失，提示用户下载完整包。
     */
    QStringList requiredFiles;

    /**
     * @brief 完整软件包下载地址。
     *
     * - 依赖文件不完整时，下载此包解压到目标目录后再升级；
     * - 为空表示服务端未配置完整包。
     */
    QUrl fullPackageUrl;
};

/**
 * @brief 单个应用在界面中的动态状态快照。
 *
 * 说明：该结构用于 UI 展示和批处理决策，不应回写到配置文件。
 */
struct AppRuntimeState
{
    /**
     * @brief 本地当前版本号（通过 Windows API 从 EXE 文件读取）。
     */
    QString currentVersion;

    /**
     * @brief 在线最新版本号（来自在线元数据）。
     */
    QString onlineVersion;

    /**
     * @brief 是否存在必需文件缺失。
     *
     * - true：至少缺失一个必需文件；
     * - false：必需文件完整。
     */
    bool missingFiles = false;

    /**
     * @brief 是否检测到可升级。
     *
     * 判定依据通常为 compareVersions(currentVersion, onlineVersion) < 0。
     */
    bool hasUpdate = false;

    /**
     * @brief 人类可读状态文本。
     *
     * 示例："可运行"、"文件不完整"、"在线检测失败"、"可升级"。
     */
    QString statusText;
};

#endif // APPTYPES_H
