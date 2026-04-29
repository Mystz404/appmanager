#include "appmanagerservice.h"

#include "versionutils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#include <TlHelp32.h>
#endif

namespace {

bool replaceFileWithRetry(const QString &srcFilePath,
                          const QString &dstFilePath,
                          QString &errorMessage)
{
    QFile dstFile(dstFilePath);
    if (dstFile.exists()) {
        dstFile.setPermissions(dstFile.permissions() | QFileDevice::WriteUser);
        if (!dstFile.remove()) {
            errorMessage = QStringLiteral("删除旧文件失败（可能被占用或无权限）: %1")
                               .arg(QDir::toNativeSeparators(dstFilePath));
            return false;
        }
    }

    if (!QFile::copy(srcFilePath, dstFilePath)) {
        errorMessage = QStringLiteral("复制文件失败: %1 -> %2")
                           .arg(QDir::toNativeSeparators(srcFilePath),
                                QDir::toNativeSeparators(dstFilePath));
        return false;
    }

    return true;
}

bool copyDirectoryNonExe(const QString &sourceRoot,
                         const QString &targetRoot,
                         QString &errorMessage,
                         const std::function<void(int done, int total, const QString &relPath)> &progressCallback = {})
{
    QStringList allFiles;
    QDirIterator it(sourceRoot,
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString filePath = it.next();
        if (filePath.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
            continue;
        }
        allFiles.push_back(filePath);
    }

    int done = 0;
    const int total = allFiles.size();
    for (const QString &srcFilePath : allFiles) {

        const QString relativePath = QDir(sourceRoot).relativeFilePath(srcFilePath);
        const QString dstFilePath = QDir(targetRoot).absoluteFilePath(relativePath);
        const QFileInfo dstInfo(dstFilePath);
        QDir().mkpath(dstInfo.absolutePath());

        if (!replaceFileWithRetry(srcFilePath, dstFilePath, errorMessage)) {
            return false;
        }

        ++done;
        if (progressCallback) {
            progressCallback(done, total, relativePath);
        }
    }
    return true;
}

bool syncMissingFilesAndDirs(const QString &sourceRoot,
                             const QString &targetRoot,
                             int &createdDirCount,
                             int &copiedFileCount,
                             QString &errorMessage,
                             const std::function<void(int done, int total, const QString &relPath)> &progressCallback = {})
{
    createdDirCount = 0;
    copiedFileCount = 0;

    // 1) 先补齐目录
    QDirIterator dirIt(sourceRoot, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (dirIt.hasNext()) {
        const QString srcDirPath = dirIt.next();
        const QString relDirPath = QDir(sourceRoot).relativeFilePath(srcDirPath);
        const QString dstDirPath = QDir(targetRoot).absoluteFilePath(relDirPath);
        QDir dstDir(dstDirPath);
        if (!dstDir.exists()) {
            if (!QDir().mkpath(dstDirPath)) {
                errorMessage = QStringLiteral("创建缺失目录失败: %1")
                                   .arg(QDir::toNativeSeparators(dstDirPath));
                return false;
            }
            ++createdDirCount;
        }
    }

    // 2) 再补齐文件（仅拷贝客户端缺失文件）
    QStringList allFiles;
    QDirIterator fileIt(sourceRoot, QDir::Files, QDirIterator::Subdirectories);
    while (fileIt.hasNext()) {
        allFiles.push_back(fileIt.next());
    }

    int done = 0;
    const int total = allFiles.size();
    for (const QString &srcFilePath : allFiles) {
        const QString relPath = QDir(sourceRoot).relativeFilePath(srcFilePath);
        const QString dstFilePath = QDir(targetRoot).absoluteFilePath(relPath);
        if (!QFileInfo::exists(dstFilePath)) {
            const QFileInfo dstInfo(dstFilePath);
            QDir().mkpath(dstInfo.absolutePath());
            if (!QFile::copy(srcFilePath, dstFilePath)) {
                errorMessage = QStringLiteral("补齐缺失文件失败: %1 -> %2")
                                   .arg(QDir::toNativeSeparators(srcFilePath),
                                        QDir::toNativeSeparators(dstFilePath));
                return false;
            }
            ++copiedFileCount;
        }

        ++done;
        if (progressCallback) {
            progressCallback(done, total, relPath);
        }
    }

    return true;
}

bool replaceExeFilesRecursively(const QString &sourceRoot,
                                const QString &targetRoot,
                                int &replacedCount,
                                QString &errorMessage)
{
    replacedCount = 0;
    QDirIterator it(sourceRoot,
                    QStringList() << QStringLiteral("*.exe") << QStringLiteral("*.EXE"),
                    QDir::Files,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        const QString srcExePath = it.next();
        const QString relativePath = QDir(sourceRoot).relativeFilePath(srcExePath);
        const QString dstExePath = QDir(targetRoot).absoluteFilePath(relativePath);
        const QFileInfo dstInfo(dstExePath);
        QDir().mkpath(dstInfo.absolutePath());

        const QString backupPath = dstExePath + QStringLiteral(".bak");
        const bool dstExists = QFileInfo::exists(dstExePath);

        QFile::remove(backupPath);
        if (dstExists && !QFile::copy(dstExePath, backupPath)) {
            errorMessage = QStringLiteral("备份 EXE 失败: %1")
                               .arg(QDir::toNativeSeparators(dstExePath));
            return false;
        }

        if (dstExists && !QFile::remove(dstExePath)) {
            errorMessage = QStringLiteral("替换 EXE 失败，目标文件可能被占用: %1")
                               .arg(QDir::toNativeSeparators(dstExePath));
            return false;
        }

        if (!QFile::copy(srcExePath, dstExePath)) {
            if (QFileInfo::exists(backupPath)) {
                QFile::copy(backupPath, dstExePath);
            }
            errorMessage = QStringLiteral("写入新 EXE 失败: %1")
                               .arg(QDir::toNativeSeparators(dstExePath));
            return false;
        }

        ++replacedCount;
    }

    return true;
}

QString requiredFileAbsolutePath(const QString &appInstallDir, const QString &relativeOrAbsolutePath)
{
    const QString cleaned = QDir::cleanPath(relativeOrAbsolutePath);
    const QFileInfo info(cleaned);
    if (info.isAbsolute()) {
        return cleaned;
    }
    return QDir(appInstallDir).absoluteFilePath(cleaned);
}

QUrl rebaseToConfiguredServer(const QString &rawUrlText,
                              const QUrl &rawUrl,
                              const QString &serverBaseUrl)
{
    const QUrl base(serverBaseUrl.trimmed());
    if (!base.isValid() || base.host().isEmpty()) {
        return rawUrl;
    }

    const QString text = rawUrlText.trimmed();

    // 相对 URL 或无 host 的 URL，按配置服务器地址解析。
    if (rawUrl.isEmpty() || rawUrl.isRelative() || rawUrl.host().isEmpty()) {
        if (text.isEmpty()) {
            return rawUrl;
        }
        const QUrl rel(text);
        return base.resolved(rel);
    }

    // 绝对 URL：保留 path/query，仅替换为 apps.json 配置的主机与端口。
    QUrl rebased = rawUrl;
    rebased.setScheme(base.scheme());
    rebased.setHost(base.host());
    rebased.setPort(base.port(-1));
    return rebased;
}

QUrl buildUpdateMetaUrlByAppId(const QString &serverBaseUrl, const QString &appId)
{
    const QUrl base(serverBaseUrl.trimmed());
    if (!base.isValid() || base.host().isEmpty() || appId.trimmed().isEmpty()) {
        return {};
    }

    QUrl url = base;
    QString path = url.path();
    if (!path.endsWith('/')) {
        path += '/';
    }
    path += QStringLiteral("updates/") + appId.trimmed() + QStringLiteral(".json");
    url.setPath(path);
    return url;
}

QUrl normalizeServerBaseUrl(const QString &serverBaseUrl)
{
    QString candidate = serverBaseUrl.trimmed();
    if (candidate.isEmpty()) {
        return {};
    }

    if (!candidate.contains(QStringLiteral("://"))) {
        candidate.prepend(QStringLiteral("http://"));
    }

    QUrl url(candidate);
    if (!url.isValid() || url.host().isEmpty()) {
        return {};
    }

    QString path = url.path();
    if (path.isEmpty()) {
        path = QStringLiteral("/");
    }
    if (!path.endsWith('/')) {
        path += '/';
    }
    url.setPath(path);
    return url;
}

bool parseAppsArray(const QJsonArray &appsArray,
                    QVector<AppConfig> &parsedApps,
                    QString &errorMessage)
{
    parsedApps.clear();
    parsedApps.reserve(appsArray.size());

    for (int i = 0; i < appsArray.size(); ++i) {
        if (!appsArray.at(i).isObject()) {
            errorMessage = QStringLiteral("第 %1 个应用配置格式无效").arg(i + 1);
            return false;
        }

        const QJsonObject obj = appsArray.at(i).toObject();
        AppConfig app;
        app.id = obj.value(QStringLiteral("id")).toString().trimmed();
        app.name = obj.value(QStringLiteral("name")).toString().trimmed();
        app.exeRelativePath = obj.value(QStringLiteral("exe")).toString().trimmed();
        app.isLocalApp = obj.value(QStringLiteral("isLocalApp")).toBool(false)
                         || obj.value(QStringLiteral("source")).toString().compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0;
        app.isHistoryVersion = obj.value(QStringLiteral("isHistoryVersion")).toBool(false);
        app.allowMultiInstance = obj.value(QStringLiteral("allowMultiInstance")).toBool(false);

        const QUrl configuredMetaUrl = QUrl(obj.value(QStringLiteral("updateMetaUrl")).toString().trimmed());
        if (configuredMetaUrl.isValid() && !configuredMetaUrl.isEmpty()) {
            app.updateMetaUrl = configuredMetaUrl;
        }

        const QJsonArray reqArray = obj.value(QStringLiteral("requiredFiles")).toArray();
        for (const QJsonValue &value : reqArray) {
            const QString relativeFile = value.toString().trimmed();
            if (!relativeFile.isEmpty()) {
                app.requiredRelativeFiles.append(relativeFile);
            }
        }

        if (app.id.isEmpty() || app.name.isEmpty() || app.exeRelativePath.isEmpty()) {
            errorMessage = QStringLiteral("第 %1 个应用配置不完整").arg(i + 1);
            return false;
        }

        parsedApps.push_back(app);
    }

    return true;
}

QString persistentDownloadCacheDir()
{
    const QString root = QDir(QCoreApplication::applicationDirPath())
                             .absoluteFilePath(QStringLiteral("download_cache"));
    QDir().mkpath(root);
    return root;
}

QString cachedUpgradePackagePath(const AppConfig &app, const OnlineAppInfo &online)
{
    QString packageExt = QStringLiteral(".bin");
    const QString pathPart = online.downloadUrl.path().toLower();
    if (pathPart.endsWith(QStringLiteral(".zip"))) {
        packageExt = QStringLiteral(".zip");
    } else if (pathPart.endsWith(QStringLiteral(".exe"))) {
        packageExt = QStringLiteral(".exe");
    }

    const QString dir = QDir(persistentDownloadCacheDir()).absoluteFilePath(QStringLiteral("upgrades"));
    QDir().mkpath(dir);
    return QDir(dir).absoluteFilePath(
        QStringLiteral("%1_%2%3").arg(app.id, online.latestVersion, packageExt));
}

QString cachedFullPackagePath(const AppConfig &app, const OnlineAppInfo &online)
{
    const QString dir = QDir(persistentDownloadCacheDir()).absoluteFilePath(QStringLiteral("full_packages"));
    QDir().mkpath(dir);
    return QDir(dir).absoluteFilePath(
        QStringLiteral("%1_%2_full.zip").arg(app.id, online.latestVersion));
}

QString resolvePowerShellProgram()
{
#ifdef Q_OS_WIN
    // 优先使用可验证存在的绝对路径；
    // 32 位进程访问 64 位 PowerShell 需要通过 Sysnative 虚拟目录。
    const QStringList absolutePaths = {
        QStringLiteral("C:/Windows/Sysnative/WindowsPowerShell/v1.0/powershell.exe"),
        QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
    };
    for (const QString &path : absolutePaths) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    // 绝对路径均不存在时回退到 PATH 查找
    return QStringLiteral("powershell.exe");
#else
    return QStringLiteral("powershell");
#endif
}

// ---------------------------------------------------------------------------
//  extractZipFile — 健壮的 ZIP 解压函数
//
//  改进点（对比之前的内联实现）：
//    1. 每个文件解压用 try/catch 包裹，单文件失败不中断整体解压。
//    2. 同时读取 stdout 和 stderr 管道，防止 stderr 缓冲区满导致进程死锁。
//    3. 路径中的单引号正确转义（''），防止 PowerShell 脚本注入/崩溃。
//    4. 支持解压超时（默认 10 分钟），防止挂死。
//    5. 使用 [Console]::Out.WriteLine 直接写 stdout，性能优于 Write-Output。
// ---------------------------------------------------------------------------
bool extractZipFile(const QString &zipFilePath,
                    const QString &destDir,
                    QString &errorMessage,
                    const std::function<void(int pct)> &progressCallback = {},
                    const std::function<void(const QString &msg)> &statusCallback = {},
                    int timeoutSecs = 600)
{
#ifdef Q_OS_WIN
    // 转义单引号用于 PowerShell 单引号字符串
    QString psZipPath = QDir::toNativeSeparators(zipFilePath);
    psZipPath.replace(QLatin1Char('\''), QStringLiteral("''"));
    QString psDestPath = QDir::toNativeSeparators(destDir);
    psDestPath.replace(QLatin1Char('\''), QStringLiteral("''"));

    QProcess process;
    process.setProgram(resolvePowerShellProgram());
    process.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-Command"),
        QStringLiteral(
            "$ErrorActionPreference='Stop';"
            "try{"
            "  Add-Type -AssemblyName System.IO.Compression.FileSystem;"
            "  $zip=[System.IO.Compression.ZipFile]::OpenRead('%1');"
            "  $entries=@($zip.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) });"
            "  $total=[Math]::Max($entries.Count,1);"
            "  $idx=0;$errCount=0;"
            "  foreach($e in $entries){"
            "    try{"
            "      $out=[System.IO.Path]::GetFullPath("
            "        [System.IO.Path]::Combine('%2',$e.FullName));"
            "      $dir=[System.IO.Path]::GetDirectoryName($out);"
            "      if(-not [string]::IsNullOrEmpty($dir)){"
            "        [void][System.IO.Directory]::CreateDirectory($dir)};"
            "      [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e,$out,$true)"
            "    }catch{"
            "      $errCount++;"
            "      [Console]::Out.WriteLine('EXTRACT_ERROR:'+$e.FullName+': '+$_.Exception.Message)"
            "    }"
            "    $idx++;"
            "    [Console]::Out.WriteLine('PROGRESS:'+[int](($idx*100)/$total))"
            "  }"
            "  $zip.Dispose();"
            "  if($errCount -gt 0){"
            "    [Console]::Out.WriteLine('TOTAL_ERRORS:'+$errCount)}"
            "}catch{"
            "  [Console]::Out.WriteLine('FATAL:'+$_.Exception.Message);"
            "  exit 1"
            "}")
            .arg(psZipPath, psDestPath)
    });
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000)) {
        errorMessage = QStringLiteral("无法启动解压进程（PowerShell），请确认系统 PowerShell 可用");
        return false;
    }

    QByteArray stdoutBuf;
    QByteArray stderrAccum;
    QStringList extractErrors;
    QElapsedTimer elapsed;
    elapsed.start();
    const qint64 timeoutMs = static_cast<qint64>(timeoutSecs) * 1000;

    while (process.state() != QProcess::NotRunning) {
        // 超时保护
        if (timeoutMs > 0 && elapsed.elapsed() > timeoutMs) {
            process.kill();
            process.waitForFinished(3000);
            errorMessage = QStringLiteral("解压超时（已等待 %1 秒），进程已终止").arg(timeoutSecs);
            return false;
        }

        process.waitForReadyRead(300);

        // **关键**：必须同时读取 stdout 和 stderr，防止管道缓冲区满导致进程死锁
        stdoutBuf.append(process.readAllStandardOutput());
        stderrAccum.append(process.readAllStandardError());

        // 解析完整行
        while (true) {
            const int eol = stdoutBuf.indexOf('\n');
            if (eol < 0) break;
            const QByteArray line = stdoutBuf.left(eol).trimmed();
            stdoutBuf.remove(0, eol + 1);

            if (line.startsWith("PROGRESS:")) {
                bool ok = false;
                const int pct = QString::fromLatin1(line.mid(9)).toInt(&ok);
                if (ok && progressCallback) {
                    progressCallback(pct);
                }
            } else if (line.startsWith("EXTRACT_ERROR:")) {
                const QString errDetail = QString::fromUtf8(line.mid(14));
                extractErrors.append(errDetail);
                if (statusCallback) {
                    statusCallback(QStringLiteral("解压文件出错（已跳过）: %1").arg(errDetail));
                }
            } else if (line.startsWith("FATAL:")) {
                errorMessage = QStringLiteral("解压致命错误: %1")
                                   .arg(QString::fromUtf8(line.mid(6)));
                process.waitForFinished(3000);
                return false;
            }
        }

        if (!process.waitForFinished(10)) {
            continue;
        }
    }

    // 读取剩余输出
    stdoutBuf.append(process.readAllStandardOutput());
    stderrAccum.append(process.readAllStandardError());
    while (true) {
        const int eol = stdoutBuf.indexOf('\n');
        if (eol < 0) break;
        const QByteArray line = stdoutBuf.left(eol).trimmed();
        stdoutBuf.remove(0, eol + 1);
        if (line.startsWith("EXTRACT_ERROR:")) {
            extractErrors.append(QString::fromUtf8(line.mid(14)));
        } else if (line.startsWith("FATAL:")) {
            errorMessage = QStringLiteral("解压致命错误: %1")
                               .arg(QString::fromUtf8(line.mid(6)));
            return false;
        }
    }

    if (process.exitStatus() != QProcess::NormalExit) {
        errorMessage = QStringLiteral("解压进程异常终止");
        return false;
    }

    if (process.exitCode() != 0) {
        const QString stderrStr = QString::fromLocal8Bit(stderrAccum).trimmed();
        errorMessage = QStringLiteral("解压失败（退出码 %1）: %2")
                           .arg(process.exitCode())
                           .arg(stderrStr.isEmpty() ? QStringLiteral("未知错误") : stderrStr);
        return false;
    }

    // 有部分文件出错但整体完成 — 记录警告，返回成功（调用方后续验证会发现缺失）
    if (!extractErrors.isEmpty()) {
        if (statusCallback) {
            statusCallback(QStringLiteral("解压完成，但有 %1 个文件出错（已跳过）")
                               .arg(extractErrors.size()));
        }
        errorMessage = QStringLiteral("解压过程中 %1 个文件出错（已跳过），后续校验可能发现缺失")
                           .arg(extractErrors.size());
    }

    return true;
#else
    Q_UNUSED(zipFilePath)
    Q_UNUSED(destDir)
    Q_UNUSED(progressCallback)
    Q_UNUSED(statusCallback)
    Q_UNUSED(timeoutSecs)
    errorMessage = QStringLiteral("当前平台暂不支持 ZIP 解压");
    return false;
#endif
}

// ---------------------------------------------------------------------------
//  unwrapSingleTopLevelDir — 剥离 ZIP 中常见的"唯一顶层目录"
//
//  许多 ZIP 打包后内部结构为 AppName_v1.0/file1.dll，解压到 extractedDir 后
//  实际文件在 extractedDir/AppName_v1.0/ 下。直接用 extractedDir 做源目录复制
//  会在目标目录多套一层。
//
//  本函数检测：如果 extractedDir 下恰好只有1个子目录且0个文件，则返回该
//  子目录路径作为真正的源目录；否则原样返回 extractedDir。
// ---------------------------------------------------------------------------
QString unwrapSingleTopLevelDir(const QString &extractedDir)
{
    const QDir dir(extractedDir);
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

    if (entries.size() == 1 && entries.first().isDir()) {
        return entries.first().absoluteFilePath();
    }
    return extractedDir;
}

bool loadPersistedApps(const QString &appListPath,
                       bool &exists,
                       QJsonArray &appsArray,
                       QString &errorMessage)
{
    exists = false;
    appsArray = QJsonArray();

    const QFileInfo fileInfo(appListPath);
    if (!fileInfo.exists()) {
        return true;
    }
    exists = true;

    QFile file(appListPath);
    if (!file.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("无法打开客户端应用列表: %1").arg(appListPath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError) {
        errorMessage = QStringLiteral("客户端应用列表 JSON 格式错误: %1").arg(parseError.errorString());
        return false;
    }

    if (doc.isArray()) {
        appsArray = doc.array();
        return true;
    }

    if (doc.isObject()) {
        appsArray = doc.object().value(QStringLiteral("apps")).toArray();
        return true;
    }

    errorMessage = QStringLiteral("客户端应用列表格式无效: %1").arg(appListPath);
    return false;
}

QJsonArray buildAppsArray(const QVector<AppConfig> &apps)
{
    QJsonArray appsArray;
    for (const AppConfig &app : apps) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), app.id);
        obj.insert(QStringLiteral("name"), app.name);
        obj.insert(QStringLiteral("exe"), app.exeRelativePath);
        if (app.isLocalApp) {
            obj.insert(QStringLiteral("source"), QStringLiteral("local"));
            obj.insert(QStringLiteral("isLocalApp"), true);
        }
        if (app.isHistoryVersion) {
            obj.insert(QStringLiteral("isHistoryVersion"), true);
        }
        if (app.allowMultiInstance) {
            obj.insert(QStringLiteral("allowMultiInstance"), true);
        }
        if (app.updateMetaUrl.isValid() && !app.updateMetaUrl.isEmpty()) {
            obj.insert(QStringLiteral("updateMetaUrl"), app.updateMetaUrl.toString());
        }
        if (!app.requiredRelativeFiles.isEmpty()) {
            QJsonArray reqArr;
            for (const QString &f : app.requiredRelativeFiles) {
                reqArr.append(f);
            }
            obj.insert(QStringLiteral("requiredFiles"), reqArr);
        }
        appsArray.append(obj);
    }
    return appsArray;
}

#ifdef Q_OS_WIN
static bool isAppProcessRunning(const QString &exePath)
{
    const QString exeName = QFileInfo(exePath).fileName();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (exeName.compare(QString::fromWCharArray(pe.szExeFile), Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool killAppProcess(const QString &exePath)
{
    const QString exeName = QFileInfo(exePath).fileName();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool anyFound = false;
    bool allOk = true;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (exeName.compare(QString::fromWCharArray(pe.szExeFile), Qt::CaseInsensitive) == 0) {
                anyFound = true;
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    if (TerminateProcess(hProc, 0)) {
                        WaitForSingleObject(hProc, 5000);
                    } else {
                        allOk = false;
                    }
                    CloseHandle(hProc);
                } else {
                    allOk = false;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return anyFound && allOk;
}
#endif

}

AppManagerService::AppManagerService(QObject *parent)
    : QObject(parent)
{
}

bool AppManagerService::loadConfig(const QString &configPath, QString &errorMessage)
{
    m_configPath = configPath;
    m_appListPath = QFileInfo(configPath).absoluteDir().absoluteFilePath(QStringLiteral("client_apps.json"));
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("无法打开配置文件: %1").arg(configPath);
        return false;
    }

    const QByteArray jsonBytes = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        errorMessage = QStringLiteral("配置文件 JSON 格式错误: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    m_appsRootRaw = root.value(QStringLiteral("appsRoot")).toString().trimmed();
    m_serverBaseUrl = root.value(QStringLiteral("serverBaseUrl")).toString().trimmed();

    const QUrl normalizedBase = normalizeServerBaseUrl(m_serverBaseUrl);
    if (normalizedBase.isValid() && !normalizedBase.host().isEmpty()) {
        m_serverBaseUrl = normalizedBase.toString(QUrl::FullyEncoded);
    }

    const QUrl baseServerUrl(m_serverBaseUrl);
    if (m_serverBaseUrl.isEmpty() || !baseServerUrl.isValid() || baseServerUrl.host().isEmpty()) {
        errorMessage = QStringLiteral("serverBaseUrl 未配置或格式无效，请在配置文件中填写有效服务器地址");
        return false;
    }

    const QString rootFromConfig = m_appsRootRaw;
    if (rootFromConfig.isEmpty()) {
        // 未设置 appsRoot 时，默认使用配置文件所在目录，便于示例和落地。
        m_appsRoot = QFileInfo(configPath).absolutePath();
    } else {
        QDir rootDir(rootFromConfig);
        if (rootDir.isRelative()) {
            rootDir = QDir(QFileInfo(configPath).absolutePath());
            if (!rootDir.cd(rootFromConfig)) {
                errorMessage = QStringLiteral("appsRoot 相对路径无效: %1").arg(rootFromConfig);
                return false;
            }
        }
        m_appsRoot = rootDir.absolutePath();
    }

    const QJsonArray legacyAppsArray = root.value(QStringLiteral("apps")).toArray();
    bool hasPersistedAppsFile = false;
    QJsonArray persistedAppsArray;
    if (!loadPersistedApps(m_appListPath, hasPersistedAppsFile, persistedAppsArray, errorMessage)) {
        return false;
    }

    const QJsonArray effectiveAppsArray = hasPersistedAppsFile ? persistedAppsArray : legacyAppsArray;

    QVector<AppConfig> parsedApps;
    if (!parseAppsArray(effectiveAppsArray, parsedApps, errorMessage)) {
        return false;
    }

    m_apps = parsedApps;

    // 统一使用 serverBaseUrl 生成 updateMetaUrl。
    for (AppConfig &app : m_apps) {
        if (app.isLocalApp) {
            continue;
        }
        const QUrl generated = buildUpdateMetaUrlByAppId(m_serverBaseUrl, app.id);
        if (!generated.isValid() || generated.isEmpty()) {
            errorMessage = QStringLiteral("无法根据 serverBaseUrl 生成应用元数据地址: %1").arg(app.id);
            return false;
        }
        app.updateMetaUrl = generated;
    }

    if (!hasPersistedAppsFile) {
        QString migrateError;
        if (!saveConfig(migrateError)) {
            errorMessage = QStringLiteral("迁移客户端应用列表失败: %1").arg(migrateError);
            return false;
        }
    }

    return true;
}

QString AppManagerService::appsRoot() const
{
    return m_appsRoot;
}

QVector<AppConfig> AppManagerService::apps() const
{
    return m_apps;
}

QString AppManagerService::resolvePath(const QString &relativePath) const
{
    QDir root(m_appsRoot);
    return QDir::cleanPath(root.absoluteFilePath(relativePath));
}

QString AppManagerService::appAbsoluteDir(const AppConfig &app) const
{
    const QString exePath = appAbsoluteExePath(app);
    return QFileInfo(exePath).absolutePath();
}

QString AppManagerService::appAbsoluteExePath(const AppConfig &app) const
{
    return resolvePath(app.exeRelativePath);
}

QString AppManagerService::appCurrentVersion(const AppConfig &app) const
{
    const QString exePath = appAbsoluteExePath(app);
    if (!QFileInfo::exists(exePath)) {
        return QStringLiteral("未知");
    }
    const QString version = getFileVersion(exePath);
    return version.isEmpty() ? QStringLiteral("未知") : version;
}

bool AppManagerService::checkRequiredFiles(const AppConfig &app, QStringList &missingFiles) const
{
    missingFiles.clear();

    const QString exePath = appAbsoluteExePath(app);
    if (!QFileInfo::exists(exePath)) {
        missingFiles.push_back(app.exeRelativePath);
    }

    const QString appInstallDir = appAbsoluteDir(app);
    for (const QString &relativeFile : app.requiredRelativeFiles) {
        if (!QFileInfo::exists(requiredFileAbsolutePath(appInstallDir, relativeFile))) {
            missingFiles.push_back(relativeFile);
        }
    }

    return missingFiles.isEmpty();
}

QStringList AppManagerService::missingRequiredDeps(const AppConfig &app) const
{
    QStringList missing;
    const QString appInstallDir = appAbsoluteDir(app);
    for (const QString &relFile : app.requiredRelativeFiles) {
        if (!QFileInfo::exists(requiredFileAbsolutePath(appInstallDir, relFile))) {
            missing.push_back(relFile);
        }
    }
    return missing;
}

QByteArray AppManagerService::httpGet(const QUrl &url,
                                      QString &errorMessage,
                                      int timeoutMs,
                                      const DownloadProgressCallback &progressCallback,
                                      const CancelCallback &cancelCallback,
                                      const QByteArray &authToken) const
{
    if (!url.isValid()) {
        errorMessage = QStringLiteral("无效 URL: %1").arg(url.toString());
        return {};
    }

    QNetworkRequest request(url);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "AppManager/" APP_VERSION " (Windows; Qt/5.15)");
    if (!authToken.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + authToken);
    }

    QNetworkReply *reply = m_networkManager.get(request);
    QEventLoop loop;
    QTimer timer;
    QTimer cancelPoll;
    timer.setSingleShot(true);
    cancelPoll.setSingleShot(false);

    bool canceled = false;

    if (progressCallback) {
        QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                         [&](qint64 received, qint64 total) {
                             progressCallback(received, total);
                         });
    }

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        if (reply->isRunning()) {
            reply->abort();
        }
        loop.quit();
    });

    if (cancelCallback) {
        QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
            if (!reply->isRunning()) {
                return;
            }
            if (cancelCallback()) {
                canceled = true;
                reply->abort();
                loop.quit();
            }
        });
        cancelPoll.start(80);
    }

    timer.start(timeoutMs);
    loop.exec();

    if (cancelPoll.isActive()) {
        cancelPoll.stop();
    }

    const bool timeout = !timer.isActive();
    const QNetworkReply::NetworkError netError = reply->error();
    const QByteArray response = reply->readAll();
    const QString netErrorStr = reply->errorString();
    reply->deleteLater();

    if (canceled) {
        errorMessage = QStringLiteral("请求已取消");
        return {};
    }

    if (timeout) {
        errorMessage = QStringLiteral("请求超时: %1").arg(url.toString());
        return {};
    }

    if (netError != QNetworkReply::NoError) {
        errorMessage = QStringLiteral("请求失败: %1").arg(netErrorStr);
        return {};
    }

    return response;
}

OnlineAppInfo AppManagerService::checkOnlineInfo(const AppConfig &app,
                                                 int timeoutMs,
                                                 const CancelCallback &cancelCallback)
{
    OnlineAppInfo info;

    QString errorMessage;
    const QByteArray body = httpGet(app.updateMetaUrl, errorMessage, timeoutMs,
                                    DownloadProgressCallback(), cancelCallback);
    if (!errorMessage.isEmpty()) {
        info.requestSuccess = false;
        info.errorMessage = errorMessage;
        return info;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        info.requestSuccess = false;
        info.errorMessage = QStringLiteral("在线元数据解析失败: %1").arg(parseError.errorString());
        return info;
    }

    const QJsonObject obj = doc.object();
    info.latestVersion = obj.value(QStringLiteral("latestVersion")).toString().trimmed();
    const QString downloadUrlText = obj.value(QStringLiteral("downloadUrl")).toString().trimmed();
    info.downloadUrl = rebaseToConfiguredServer(downloadUrlText,
                                                QUrl(downloadUrlText),
                                                m_serverBaseUrl);
    info.sha256 = obj.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    info.packageType = obj.value(QStringLiteral("packageType")).toString().trimmed().toLower();
    info.subDir = obj.value(QStringLiteral("subDir")).toString().trimmed();
    if (obj.contains(QStringLiteral("zipReplaceExeRecursively"))) {
        info.zipReplaceExeRecursively = obj.value(QStringLiteral("zipReplaceExeRecursively")).toBool(true);
    }

    // 解析依赖文件列表
    const QJsonArray reqArr = obj.value(QStringLiteral("requiredFiles")).toArray();
    for (const QJsonValue &v : reqArr) {
        const QString f = v.toString().trimmed();
        if (!f.isEmpty()) info.requiredFiles.append(f);
    }
    // 解析完整包下载地址
    const QString fullPkgStr = obj.value(QStringLiteral("fullPackageUrl")).toString().trimmed();
    if (!fullPkgStr.isEmpty()) {
        info.fullPackageUrl = rebaseToConfiguredServer(fullPkgStr,
                                                       QUrl(fullPkgStr),
                                                       m_serverBaseUrl);
    }

    // 解析单文件依赖下载基址
    const QString depsBaseStr = obj.value(QStringLiteral("depsBaseUrl")).toString().trimmed();
    if (!depsBaseStr.isEmpty()) {
        info.depsBaseUrl = rebaseToConfiguredServer(depsBaseStr,
                                                    QUrl(depsBaseStr),
                                                    m_serverBaseUrl);
    }

    // 解析当前版本更新说明
    info.changeLog = obj.value(QStringLiteral("changeLog")).toString().trimmed();

    if (info.packageType.isEmpty()) {
        info.packageType = QStringLiteral("exe");
    }

    if (info.latestVersion.isEmpty() || !info.downloadUrl.isValid()) {
        info.requestSuccess = false;
        info.errorMessage = QStringLiteral("在线元数据缺少 latestVersion 或 downloadUrl");
        return info;
    }

    if (info.packageType != QStringLiteral("exe") && info.packageType != QStringLiteral("zip")) {
        info.requestSuccess = false;
        info.errorMessage = QStringLiteral("packageType 仅支持 exe 或 zip");
        return info;
    }

    info.requestSuccess = true;
    return info;
}

QStringList AppManagerService::loadZipReplaceFileList(QString &errorMessage) const
{
    errorMessage.clear();

    const QString manifestPath = resolvePath(QStringLiteral("zip_replace_manifest.json"));
    QFile file(manifestPath);
    if (!file.exists()) {
        return {};
    }

    if (!file.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("无法读取ZIP替换清单文件: %1")
                           .arg(QDir::toNativeSeparators(manifestPath));
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        errorMessage = QStringLiteral("ZIP替换清单JSON格式错误: %1，文件: %2")
                           .arg(parseError.errorString(), QDir::toNativeSeparators(manifestPath));
        return {};
    }

    const QJsonObject root = doc.object();
    const QJsonArray files = root.value(QStringLiteral("files")).toArray();

    QStringList result;
    QSet<QString> dedup;
    for (const QJsonValue &value : files) {
        const QString relativePath = QDir::cleanPath(value.toString().trimmed());
        if (relativePath.isEmpty()) {
            continue;
        }
        if (!dedup.contains(relativePath)) {
            result.push_back(relativePath);
            dedup.insert(relativePath);
        }
    }

    return result;
}

// ============================================================================
//  依赖文件完整性检查 & 完整包修复
// ============================================================================

bool AppManagerService::checkAndFixDependencies(const AppConfig &app,
                                                 const OnlineAppInfo &online,
                                                 QString &resultMessage,
                                                 int timeoutMs,
                                                 const DownloadProgressCallback &progressCallback,
                                                 const StatusCallback &statusCallback,
                                                 const InstallProgressCallback &installProgressCallback,
                                                 const CancelCallback &cancelCallback,
                                                 bool forceRedownload)
{
    // 没有服务器指定的依赖文件列表，跳过检查
    if (online.requiredFiles.isEmpty()) {
        if (statusCallback) {
            statusCallback(QStringLiteral("服务器未配置依赖文件列表，跳过完整性检查"));
        }
        return true;
    }

    // 检查每个依赖文件是否存在（相对应用安装目录）
    const QString appInstallDir = appAbsoluteDir(app);
    QStringList missingFiles;
    for (const QString &relFile : online.requiredFiles) {
        const QString absPath = requiredFileAbsolutePath(appInstallDir, relFile);
        if (!QFileInfo::exists(absPath)) {
            missingFiles.append(relFile);
        }
    }

    if (missingFiles.isEmpty()) {
        if (statusCallback) {
            statusCallback(QStringLiteral("依赖文件完整性检查通过（%1 个文件）").arg(online.requiredFiles.size()));
        }
        return true;
    }

    // 依赖不完整
    if (statusCallback) {
        statusCallback(QStringLiteral("检测到 %1 个依赖文件缺失：%2")
                           .arg(missingFiles.size())
                           .arg(missingFiles.join(QStringLiteral(", "))));
    }

    // 策略选择：少量缺失（≤6）且服务端配置了依赖目录时，逐文件下载以减少下载量
    static constexpr int SINGLE_FILE_THRESHOLD = 6;
    if (missingFiles.size() <= SINGLE_FILE_THRESHOLD && online.depsBaseUrl.isValid()) {
        if (statusCallback) {
            statusCallback(QStringLiteral("缺失文件 %1 个（≤%2），使用逐文件下载模式")
                               .arg(missingFiles.size()).arg(SINGLE_FILE_THRESHOLD));
        }
        if (installProgressCallback) {
            installProgressCallback(5);
        }

        int downloaded = 0;
        QStringList failedFiles;
        QStringList notFoundFiles;
        for (int i = 0; i < missingFiles.size(); ++i) {
            const QString &relFile = missingFiles.at(i);
            if (cancelCallback && cancelCallback()) {
                resultMessage = QStringLiteral("依赖修复已取消");
                return false;
            }

            // 去除 ./ 前缀，仅保留相对文件名用于服务端查找
            QString cleanedFile = relFile;
            while (cleanedFile.startsWith(QStringLiteral("./")))
                cleanedFile = cleanedFile.mid(2);
            while (cleanedFile.startsWith(QStringLiteral(".\\")))
                cleanedFile = cleanedFile.mid(2);

            // 构建下载 URL：depsBaseUrl?file=<percent-encoded-cleanedFile>
            QUrl fileUrl(online.depsBaseUrl);
            const QString query = QStringLiteral("file=%1")
                                      .arg(QString::fromUtf8(QUrl::toPercentEncoding(cleanedFile)));
            fileUrl.setQuery(query);

            const QString absPath = requiredFileAbsolutePath(appInstallDir, relFile);
            QDir().mkpath(QFileInfo(absPath).absolutePath());

            if (statusCallback) {
                statusCallback(QStringLiteral("下载依赖 [%1/%2]: %3")
                                   .arg(i + 1).arg(missingFiles.size()).arg(relFile));
            }

            QString dlError;
            const bool ok = downloadFileWithResume(
                fileUrl, absPath, dlError, timeoutMs,
                [&](qint64 recv, qint64 total) {
                    if (progressCallback && total > 0) {
                        progressCallback(recv, total);
                    }
                },
                StatusCallback(),
                cancelCallback);

            if (ok) {
                ++downloaded;
            } else {
                failedFiles.append(relFile);
                // 判断是否为 404（文件在服务端依赖目录中不存在）
                const bool is404 = dlError.contains(QStringLiteral("404"))
                                   || dlError.contains(QStringLiteral("不存在"));
                if (is404) {
                    notFoundFiles.append(relFile);
                    if (statusCallback) {
                        statusCallback(QStringLiteral("服务端依赖目录中未找到文件: %1").arg(relFile));
                    }
                } else {
                    if (statusCallback) {
                        statusCallback(QStringLiteral("下载失败: %1 (%2)").arg(relFile, dlError));
                    }
                }
            }

            if (installProgressCallback) {
                installProgressCallback(5 + (i + 1) * 90 / missingFiles.size());
            }
        }

        if (failedFiles.isEmpty()) {
            if (installProgressCallback) {
                installProgressCallback(100);
            }
            resultMessage = QStringLiteral("依赖文件修复完成（逐文件下载 %1 个）").arg(downloaded);
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return true;
        }

        // 部分文件下载失败，提示后回退到完整包下载
        if (statusCallback) {
            if (!notFoundFiles.isEmpty()) {
                statusCallback(QStringLiteral("以下 %1 个文件在服务端依赖目录中不存在：\n%2")
                                   .arg(notFoundFiles.size())
                                   .arg(notFoundFiles.join(QStringLiteral("\n"))));
            }
            statusCallback(QStringLiteral("共 %1 个文件下载失败，回退到完整包下载...")
                               .arg(failedFiles.size()));
        }
    }

    // 检查服务端是否配置了完整包
    if (!online.fullPackageUrl.isValid()) {
        resultMessage = QStringLiteral("依赖文件不完整且服务器未提供完整软件包下载，无法自动修复。\n缺失文件：%1")
                            .arg(missingFiles.join(QStringLiteral("\n")));
        return false;
    }

    // 下载完整包
    if (statusCallback) {
        statusCallback(QStringLiteral("正在下载完整软件包以修复缺失文件..."));
    }

    const QString fullPkgPath = cachedFullPackagePath(app, online);

    if (forceRedownload) {
        QFile::remove(fullPkgPath);
        QFile::remove(fullPkgPath + QStringLiteral(".part"));
        if (statusCallback) {
            statusCallback(QStringLiteral("已删除完整包缓存，准备重新下载: %1")
                               .arg(QDir::toNativeSeparators(fullPkgPath)));
        }
    }

    if (QFileInfo::exists(fullPkgPath)) {
        if (statusCallback) {
            statusCallback(QStringLiteral("检测到已下载完整包，直接复用: %1")
                               .arg(QDir::toNativeSeparators(fullPkgPath)));
        }
    } else {
        QString downloadError;
        if (!downloadFileWithResume(online.fullPackageUrl,
                                    fullPkgPath,
                                    downloadError,
                                    timeoutMs,
                                    progressCallback,
                                    statusCallback,
                                    cancelCallback)) {
            resultMessage = QStringLiteral("完整包下载失败：%1").arg(downloadError);
            return false;
        }
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("完整包下载完成，正在解压..."));
    }
    if (installProgressCallback) {
        installProgressCallback(10);
    }

    // 确定解压目标目录：优先使用服务端配置的安装子目录
    QString targetDir = online.subDir.isEmpty()
                          ? appAbsoluteDir(app)
                          : resolvePath(online.subDir);
    QDir().mkpath(targetDir);

    // 解压完整包到目标目录
    QTemporaryDir extractTempDir;
    if (!extractTempDir.isValid()) {
        resultMessage = QStringLiteral("无法创建临时目录用于解压完整包");
        return false;
    }
    const QString extractedDir = QDir(extractTempDir.path()).absoluteFilePath(QStringLiteral("extracted"));
    QDir().mkpath(extractedDir);

    QString extractError;
    const bool extractOk = extractZipFile(
        fullPkgPath, extractedDir, extractError,
        [&](int pct) {
            if (installProgressCallback) {
                installProgressCallback(10 + (pct * 60) / 100);
            }
            if (statusCallback && (pct % 20 == 0 || pct == 100)) {
                statusCallback(QStringLiteral("正在解压完整包...%1%").arg(pct));
            }
        },
        statusCallback);
    if (!extractOk) {
        resultMessage = QStringLiteral("完整包解压失败：%1").arg(extractError);
        return false;
    }

    // 剥离 ZIP 中常见的唯一顶层目录（如 AppName_v1.0/）
    const QString effectiveExtractedDir = unwrapSingleTopLevelDir(extractedDir);
    if (effectiveExtractedDir != extractedDir && statusCallback) {
        statusCallback(QStringLiteral("检测到压缩包有顶层目录，已自动剥离: %1")
                           .arg(QDir(extractedDir).relativeFilePath(effectiveExtractedDir)));
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("解压完成，正在复制文件到目标目录..."));
    }
    if (installProgressCallback) {
        installProgressCallback(75);
    }

    // 将解压出的文件复制到目标目录（仅覆盖缺失文件和已有文件）
    int copiedCount = 0;
    std::function<bool(const QString &, const QString &)> copyRecursive;
    copyRecursive = [&](const QString &srcDir, const QString &dstDir) -> bool {
        QDir src(srcDir);
        const auto entries = src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries) {
            const QString dstPath = QDir(dstDir).absoluteFilePath(fi.fileName());
            if (fi.isDir()) {
                QDir().mkpath(dstPath);
                if (!copyRecursive(fi.absoluteFilePath(), dstPath)) {
                    return false;
                }
            } else {
                QDir().mkpath(QFileInfo(dstPath).absolutePath());
                if (QFile::exists(dstPath)) {
                    QFile::remove(dstPath);
                }
                if (!QFile::copy(fi.absoluteFilePath(), dstPath)) {
                    if (statusCallback) {
                        statusCallback(QStringLiteral("复制文件失败：%1").arg(QDir::toNativeSeparators(dstPath)));
                    }
                    // 非致命，继续尝试其他文件
                } else {
                    ++copiedCount;
                }
            }
        }
        return true;
    };

    copyRecursive(effectiveExtractedDir, targetDir);

    if (installProgressCallback) {
        installProgressCallback(95);
    }

    // 再次校验依赖文件
    QStringList stillMissing;
    for (const QString &relFile : online.requiredFiles) {
        if (!QFileInfo::exists(requiredFileAbsolutePath(appInstallDir, relFile))) {
            stillMissing.append(relFile);
        }
    }

    if (!stillMissing.isEmpty()) {
        resultMessage = QStringLiteral("完整包安装后仍有 %1 个文件缺失：%2")
                            .arg(stillMissing.size())
                            .arg(stillMissing.join(QStringLiteral("\n")));
        return false;
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("依赖文件修复完成，共复制 %1 个文件").arg(copiedCount));
    }
    if (installProgressCallback) {
        installProgressCallback(100);
    }

    resultMessage = QStringLiteral("依赖文件已修复完成");
    return true;
}

bool AppManagerService::upgradeApp(const AppConfig &app,
                                   const OnlineAppInfo &online,
                                   QString &resultMessage,
                                   int timeoutMs,
                                   const DownloadProgressCallback &progressCallback,
                                   const StatusCallback &statusCallback,
                                   const InstallProgressCallback &installProgressCallback,
                                   const CancelCallback &cancelCallback,
                                   bool forceRedownload)
{
    if (!online.requestSuccess) {
        resultMessage = QStringLiteral("无法升级，在线信息不可用: %1").arg(online.errorMessage);
        return false;
    }

    const QString currentVersion = appCurrentVersion(app);
    if (currentVersion != QStringLiteral("未知")
        && compareVersions(currentVersion, online.latestVersion) >= 0) {
        resultMessage = QStringLiteral("当前已是最新版本，无需升级");
        return true;
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("开始下载升级包..."));
    }

    const QString packageFilePath = cachedUpgradePackagePath(app, online);

    if (forceRedownload) {
        QFile::remove(packageFilePath);
        QFile::remove(packageFilePath + QStringLiteral(".part"));
        if (statusCallback) {
            statusCallback(QStringLiteral("已删除升级包缓存，准备重新下载: %1")
                               .arg(QDir::toNativeSeparators(packageFilePath)));
        }
    }

    if (QFileInfo::exists(packageFilePath)) {
        if (statusCallback) {
            statusCallback(QStringLiteral("检测到已下载升级包，直接复用: %1")
                               .arg(QDir::toNativeSeparators(packageFilePath)));
        }
    } else {
        QString downloadError;
        if (!downloadFileWithResume(online.downloadUrl,
                                    packageFilePath,
                                    downloadError,
                                    timeoutMs,
                                    progressCallback,
                                    statusCallback,
                                    cancelCallback)) {
            resultMessage = QStringLiteral("升级失败：下载中断或下载失败（支持断点续传）：%1").arg(downloadError);
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }
    }

    if (statusCallback) {
        const qint64 fileSize = QFileInfo(packageFilePath).size();
        statusCallback(QStringLiteral("下载完成，文件: %1，大小: %2 字节")
                           .arg(QDir::toNativeSeparators(packageFilePath))
                           .arg(fileSize));
        statusCallback(QStringLiteral("正在校验升级包..."));
    }

    if (!online.sha256.isEmpty()) {
        QFile file(packageFilePath);
        if (!file.open(QIODevice::ReadOnly)) {
            resultMessage = QStringLiteral("升级失败：无法读取已下载升级包: %1")
                                .arg(QDir::toNativeSeparators(packageFilePath));
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            if (cancelCallback && cancelCallback()) {
                resultMessage = QStringLiteral("升级已取消");
                if (statusCallback) {
                    statusCallback(resultMessage);
                }
                return false;
            }
            hash.addData(file.read(64 * 1024));
        }
        const QString digest = QString::fromLatin1(hash.result().toHex()).toLower();
        if (digest != online.sha256.toLower()) {
            resultMessage = QStringLiteral("升级失败：升级包校验失败（SHA256 不匹配）");
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }
    }

    // 下载和校验完成。若目标程序正在运行，先将其关闭再安装。
#ifdef Q_OS_WIN
    {
        const QString targetExe = appAbsoluteExePath(app);
        if (isAppProcessRunning(targetExe)) {
            if (statusCallback) {
                statusCallback(QStringLiteral("检测到程序正在运行，正在自动关闭..."));
            }
            if (!killAppProcess(targetExe)) {
                resultMessage = QStringLiteral("升级失败：目标程序正在运行且无法自动关闭，请手动关闭后重试");
                if (statusCallback) {
                    statusCallback(resultMessage);
                }
                return false;
            }
            if (statusCallback) {
                statusCallback(QStringLiteral("程序已关闭，继续安装..."));
            }
        }
    }
#endif

    if (online.packageType == QStringLiteral("zip")) {
        if (statusCallback) {
            statusCallback(QStringLiteral("开始执行 ZIP 解压安装..."));
        }
        return upgradeByZipExtract(app,
                                   online,
                                   packageFilePath,
                                   resultMessage,
                                   statusCallback,
                                   installProgressCallback);
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("开始替换 EXE 文件..."));
    }
    return upgradeByExeReplace(app,
                               online,
                               packageFilePath,
                               resultMessage,
                               statusCallback,
                               installProgressCallback);
}

bool AppManagerService::upgradeByExeReplace(const AppConfig &app,
                                            const OnlineAppInfo &online,
                                            const QString &packageFilePath,
                                            QString &resultMessage,
                                            const StatusCallback &statusCallback,
                                            const InstallProgressCallback &installProgressCallback) const
{
    if (installProgressCallback) {
        installProgressCallback(10);
    }
    const QString currentVersion = appCurrentVersion(app);

    const QString targetExePath = appAbsoluteExePath(app);
    const QFileInfo targetInfo(targetExePath);
    QDir().mkpath(targetInfo.absolutePath());

    const bool targetExistsBefore = targetInfo.exists();
    if (!targetExistsBefore && statusCallback) {
        statusCallback(QStringLiteral("目标可执行文件不存在，按新增方式安装: %1")
                           .arg(QDir::toNativeSeparators(targetExePath)));
    }

    const QString backupPath = targetExePath + QStringLiteral(".bak");
    const QString tempNewPath = targetExePath + QStringLiteral(".new");

    QFile::remove(tempNewPath);
    if (!QFile::copy(packageFilePath, tempNewPath)) {
        resultMessage = QStringLiteral("无法写入临时升级文件: %1")
                            .arg(QDir::toNativeSeparators(tempNewPath));
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }
    if (installProgressCallback) {
        installProgressCallback(35);
    }

    // 备份旧版本，失败时可用于手动回滚。
    QFile::remove(backupPath);
    if (targetExistsBefore && !QFile::copy(targetExePath, backupPath)) {
        QFile::remove(tempNewPath);
        resultMessage = QStringLiteral("备份旧版本失败，请确认文件权限和占用状态");
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }
    if (installProgressCallback) {
        installProgressCallback(60);
    }

    // Windows 下若进程仍在运行，删除会失败，因此这里能有效检测“是否占用”。
    if (targetExistsBefore && !QFile::remove(targetExePath)) {
        QFile::remove(tempNewPath);
        resultMessage = QStringLiteral("目标程序可能正在运行，请关闭后重试");
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }

    if (!QFile::rename(tempNewPath, targetExePath)) {
        QFile::copy(backupPath, targetExePath);
        QFile::remove(tempNewPath);
        resultMessage = QStringLiteral("替换新版本失败，已尝试回滚旧版本");
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }
    if (installProgressCallback) {
        installProgressCallback(85);
    }

    if (installProgressCallback) {
        installProgressCallback(100);
    }

    resultMessage = QStringLiteral("升级成功: %1 -> %2").arg(currentVersion, online.latestVersion);
    if (statusCallback) {
        statusCallback(QStringLiteral("EXE 升级完成"));
    }
    return true;
}

bool AppManagerService::upgradeByZipExtract(const AppConfig &app,
                                            const OnlineAppInfo &online,
                                            const QString &zipFilePath,
                                            QString &resultMessage,
                                            const StatusCallback &statusCallback,
                                            const InstallProgressCallback &installProgressCallback) const
{
#ifdef Q_OS_WIN
    if (installProgressCallback) {
        installProgressCallback(5);
    }
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        resultMessage = QStringLiteral("无法创建临时目录用于 ZIP 解压");
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }

    if (!QFileInfo::exists(zipFilePath)) {
        resultMessage = QStringLiteral("ZIP 升级失败：下载文件不存在: %1")
                            .arg(QDir::toNativeSeparators(zipFilePath));
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }

    // 目标目录：优先使用服务端配置的安装子目录
    QString targetDir = online.subDir.isEmpty()
                          ? appAbsoluteDir(app)
                          : resolvePath(online.subDir);
    QDir().mkpath(targetDir);

    const QString extractedDir = QDir(tempDir.path()).absoluteFilePath(QStringLiteral("unzipped"));
    QDir().mkpath(extractedDir);

    if (statusCallback) {
        statusCallback(QStringLiteral("ZIP 安装目标目录: %1")
                           .arg(QDir::toNativeSeparators(targetDir)));
        statusCallback(QStringLiteral("ZIP 临时解压目录: %1")
                           .arg(QDir::toNativeSeparators(extractedDir)));
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("正在解压 ZIP 包..."));
    }

    {
        qint64 lastOutputTick = 0;
        QString extractError;
        const bool extractOk = extractZipFile(
            zipFilePath, extractedDir, extractError,
            [&](int zipPercent) {
                const int mappedPercent = qBound(5, 5 + (zipPercent * 65) / 100, 70);
                if (installProgressCallback) {
                    installProgressCallback(mappedPercent);
                }
                if (statusCallback && (zipPercent - lastOutputTick >= 10 || zipPercent == 100)) {
                    statusCallback(QStringLiteral("正在解压 ZIP 包...%1%").arg(zipPercent));
                    lastOutputTick = zipPercent;
                }
            },
            statusCallback);
        if (!extractOk) {
            resultMessage = QStringLiteral("ZIP 解压失败: %1").arg(extractError);
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }
    }

    // 剥离 ZIP 中常见的唯一顶层目录（如 AppName_v1.0/）
    const QString effectiveExtractedDir = unwrapSingleTopLevelDir(extractedDir);
    if (effectiveExtractedDir != extractedDir && statusCallback) {
        statusCallback(QStringLiteral("检测到压缩包有顶层目录，已自动剥离: %1")
                           .arg(QDir(extractedDir).relativeFilePath(effectiveExtractedDir)));
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("解压完成，正在覆盖普通文件..."));
    }
    if (installProgressCallback) {
        installProgressCallback(75);
    }

    QString manifestError;
    const QStringList manifestFiles = loadZipReplaceFileList(manifestError);
    if (!manifestError.isEmpty()) {
        resultMessage = QStringLiteral("ZIP 升级失败（读取替换清单失败）: %1").arg(manifestError);
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }

    const bool useManifest = !manifestFiles.isEmpty();
    if (useManifest && statusCallback) {
        statusCallback(QStringLiteral("检测到ZIP替换清单，共 %1 项，按清单替换文件")
                           .arg(manifestFiles.size()));
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("步骤1/3：先补齐客户端缺失目录和文件..."));
    }
    int createdDirCount = 0;
    int copiedMissingFileCount = 0;
    QString syncMissingError;
    if (!syncMissingFilesAndDirs(
            effectiveExtractedDir,
            targetDir,
            createdDirCount,
            copiedMissingFileCount,
            syncMissingError,
            [&](int done, int total, const QString &relPath) {
                if (statusCallback && (done == 1 || done == total || done % 50 == 0)) {
                    statusCallback(QStringLiteral("补齐缺失进度: %1/%2，最近文件: %3")
                                       .arg(done)
                                       .arg(total)
                                       .arg(QDir::toNativeSeparators(relPath)));
                }
            })) {
        resultMessage = QStringLiteral("ZIP 升级失败（补齐缺失阶段）: %1").arg(syncMissingError);
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }
    if (statusCallback) {
        statusCallback(QStringLiteral("补齐缺失完成：新增目录 %1 个，新增文件 %2 个")
                           .arg(createdDirCount)
                           .arg(copiedMissingFileCount));
    }

    if (useManifest) {
        if (statusCallback) {
            statusCallback(QStringLiteral("步骤2/3：检查需要替换的清单文件是否完整..."));
        }
        QStringList missingFiles;
        for (const QString &relativePath : manifestFiles) {
            const QString srcFilePath = QDir(effectiveExtractedDir).absoluteFilePath(relativePath);
            if (!QFileInfo::exists(srcFilePath)) {
                missingFiles << relativePath;
                if (statusCallback) {
                    statusCallback(QStringLiteral("清单文件在ZIP中不存在: %1")
                                       .arg(QDir::toNativeSeparators(relativePath)));
                }
            }
        }

        if (!missingFiles.isEmpty()) {
            resultMessage = QStringLiteral("ZIP 升级失败：清单中以下文件在压缩包内缺失: %1")
                                .arg(missingFiles.join(QStringLiteral("，")));
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }

        if (statusCallback) {
            statusCallback(QStringLiteral("步骤3/3：开始按清单执行替换..."));
        }

        int done = 0;
        for (const QString &relativePath : manifestFiles) {
            const QString srcFilePath = QDir(effectiveExtractedDir).absoluteFilePath(relativePath);
            const QString dstFilePath = resolvePath(relativePath);

            QString replaceError;
            if (!replaceFileWithRetry(srcFilePath, dstFilePath, replaceError)) {
                resultMessage = QStringLiteral("ZIP 升级失败（按清单替换阶段）: %1").arg(replaceError);
                if (statusCallback) {
                    statusCallback(resultMessage);
                }
                return false;
            }

            ++done;
            if (statusCallback && (done == 1 || done == manifestFiles.size() || done % 20 == 0)) {
                statusCallback(QStringLiteral("清单替换进度: %1/%2，最近文件: %3")
                                   .arg(done)
                                   .arg(manifestFiles.size())
                                   .arg(QDir::toNativeSeparators(relativePath)));
            }
        }
    } else {
        if (statusCallback) {
            statusCallback(QStringLiteral("步骤2/3：未配置清单，跳过清单检查"));
            statusCallback(QStringLiteral("步骤3/3：执行默认覆盖（非EXE）..."));
        }
        QString copyError;
        if (!copyDirectoryNonExe(
                effectiveExtractedDir,
                targetDir,
                copyError,
                [&](int done, int total, const QString &relPath) {
                    if (statusCallback && (done == 1 || done == total || done % 20 == 0)) {
                        statusCallback(QStringLiteral("普通文件覆盖进度: %1/%2，最近文件: %3")
                                           .arg(done)
                                           .arg(total)
                                           .arg(QDir::toNativeSeparators(relPath)));
                    }
                })) {
            resultMessage = QStringLiteral("ZIP 升级失败（普通文件覆盖阶段）: %1").arg(copyError);
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }
    }

    int replacedExeCount = 0;
    if (online.zipReplaceExeRecursively) {
        if (statusCallback) {
            statusCallback(QStringLiteral("正在递归替换 EXE 文件..."));
        }
        if (installProgressCallback) {
            installProgressCallback(85);
        }
        QString replaceError;
        if (!replaceExeFilesRecursively(effectiveExtractedDir, targetDir, replacedExeCount, replaceError)) {
            resultMessage = QStringLiteral("ZIP 升级失败（EXE 替换阶段）: %1").arg(replaceError);
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("文件安装完成"));
    }
    if (installProgressCallback) {
        installProgressCallback(95);
    }

    if (online.zipReplaceExeRecursively) {
        resultMessage = QStringLiteral("ZIP 升级成功，已覆盖目录: %1，递归替换 EXE 数量: %2")
                            .arg(QDir::toNativeSeparators(targetDir))
                            .arg(replacedExeCount);
    } else {
        resultMessage = QStringLiteral("ZIP 升级成功，已覆盖目录: %1")
                            .arg(QDir::toNativeSeparators(targetDir));
    }
    if (statusCallback) {
        statusCallback(resultMessage);
    }
    if (installProgressCallback) {
        installProgressCallback(100);
    }
    return true;
#else
    Q_UNUSED(app);
    Q_UNUSED(online);
    Q_UNUSED(zipFilePath);
    resultMessage = QStringLiteral("当前平台暂未实现 ZIP 升级，请使用 exe 包升级");
    if (statusCallback) {
        statusCallback(resultMessage);
    }
    Q_UNUSED(installProgressCallback);
    return false;
#endif
}

bool AppManagerService::downloadFileWithResume(const QUrl &url,
                                               const QString &targetFilePath,
                                               QString &errorMessage,
                                               int timeoutMs,
                                               const DownloadProgressCallback &progressCallback,
                                               const StatusCallback &statusCallback,
                                               const CancelCallback &cancelCallback) const
{
    if (!url.isValid()) {
        errorMessage = QStringLiteral("无效下载 URL: %1").arg(url.toString());
        return false;
    }

    const QString partPath = targetFilePath + QStringLiteral(".part");
    QDir().mkpath(QFileInfo(partPath).absolutePath());

    if (statusCallback) {
        statusCallback(QStringLiteral("下载准备完成，目标文件: %1")
                           .arg(QDir::toNativeSeparators(targetFilePath)));
    }

    qint64 downloadedBytes = QFileInfo(partPath).exists() ? QFileInfo(partPath).size() : 0;
    qint64 expectedTotal = -1;
    const int maxRetry = 5;

    if (statusCallback && downloadedBytes > 0) {
        statusCallback(QStringLiteral("发现断点文件，准备续传，已下载 %1 字节")
                           .arg(downloadedBytes));
    }

    for (int attempt = 1; attempt <= maxRetry; ++attempt) {
        const bool useRange = downloadedBytes > 0;

        if (statusCallback) {
            statusCallback(QStringLiteral("开始下载尝试 %1/%2（%3）")
                               .arg(attempt)
                               .arg(maxRetry)
                               .arg(useRange ? QStringLiteral("断点续传") : QStringLiteral("全量下载")));
        }

        QNetworkRequest request(url);
        request.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setRawHeader("User-Agent", "AppManager/" APP_VERSION " (Windows; Qt/5.15)");
        if (useRange) {
            request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(downloadedBytes) + "-");
        }

        QNetworkReply *reply = m_networkManager.get(request);
        QEventLoop loop;
        QTimer timer;
        QTimer cancelPoll;
        timer.setSingleShot(true);
        cancelPoll.setSingleShot(false);

        bool canceled = false;
        // 先将数据缓冲到内存，待确认 HTTP 状态后再落盘，
        // 避免服务端对 Range 请求返回 200（全量）时错误追加到 .part 文件。
        QByteArray recvBuffer;
        bool headerChecked = false;
        bool serverNoResume = false; // 服务端对 Range 请求返回 200

        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
            const QByteArray chunk = reply->readAll();
            if (chunk.isEmpty()) {
                return;
            }
            // 首包到达后检查 HTTP 状态
            if (!headerChecked) {
                headerChecked = true;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (useRange && status == 200) {
                    // 服务端不支持 Range，返回了完整文件，标记后继续接收但最终丢弃续传数据
                    serverNoResume = true;
                }
            }
            recvBuffer.append(chunk);
        });

        if (progressCallback) {
            QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                             [&](qint64 received, qint64 total) {
                                 if (serverNoResume) {
                                     // 服务端返回全量，显示全量进度
                                     progressCallback(received, total > 0 ? total : -1);
                                     return;
                                 }
                                 qint64 shownTotal = total;
                                 if (useRange && total > 0) {
                                     shownTotal = downloadedBytes + total;
                                 } else if (expectedTotal > 0 && total <= 0) {
                                     shownTotal = expectedTotal;
                                 }
                                 progressCallback(downloadedBytes + received, shownTotal);
                             });
        }

        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
            if (reply->isRunning()) {
                reply->abort();
            }
            loop.quit();
        });

        if (cancelCallback) {
            QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
                if (!reply->isRunning()) {
                    return;
                }
                if (cancelCallback()) {
                    canceled = true;
                    reply->abort();
                    loop.quit();
                }
            });
            cancelPoll.start(80);
        }

        timer.start(timeoutMs);
        loop.exec();

        if (cancelPoll.isActive()) {
            cancelPoll.stop();
        }

        // 读取剩余数据
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            recvBuffer.append(tail);
        }

        const bool timeout = !timer.isActive();
        const auto netError = reply->error();
        const QString netErrorStr = reply->errorString();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        const QByteArray contentRange = reply->rawHeader("Content-Range");

        reply->deleteLater();

        if (canceled) {
            // 取消前将已接收数据写入 .part，供下次续传
            if (!recvBuffer.isEmpty() && !serverNoResume) {
                QFile outFile(partPath);
                if (outFile.open(useRange ? (QIODevice::WriteOnly | QIODevice::Append)
                                          : (QIODevice::WriteOnly | QIODevice::Truncate))) {
                    outFile.write(recvBuffer);
                    outFile.close();
                }
            }
            errorMessage = QStringLiteral("下载已取消");
            return false;
        }

        // 服务端对 Range 请求返回 200（不支持续传），切换全量重下
        if (serverNoResume) {
            if (statusCallback) {
                statusCallback(QStringLiteral("服务端不支持续传回包，切换为全量重下"));
            }
            // 将收到的全量数据直接写入 .part（Truncate 覆盖旧数据）
            QFile outFile(partPath);
            if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                outFile.write(recvBuffer);
                outFile.close();
            }
            if (httpStatus == 200 && contentLength > 0) {
                expectedTotal = contentLength;
            }
            downloadedBytes = QFileInfo(partPath).exists() ? QFileInfo(partPath).size() : 0;
            // 若全量数据已接收完整，直接跳到大小校验；否则重试
            if (netError != QNetworkReply::NoError || timeout) {
                // 数据未收完，下次从头开始（无法续传）
                if (attempt < maxRetry) {
                    continue;
                }
                errorMessage = QStringLiteral("服务端不支持断点续传且下载未完成");
                return false;
            }
            // 数据收完，跳到下方的大小校验和文件落盘
        } else {
            // 正常路径：将缓冲数据写入 .part
            {
                QFile outFile(partPath);
                const QIODevice::OpenMode openMode = useRange
                    ? (QIODevice::WriteOnly | QIODevice::Append)
                    : (QIODevice::WriteOnly | QIODevice::Truncate);
                if (!outFile.open(openMode)) {
                    errorMessage = QStringLiteral("无法写入临时下载文件: %1")
                                       .arg(QDir::toNativeSeparators(partPath));
                    return false;
                }
                outFile.write(recvBuffer);
                outFile.flush();
                outFile.close();
            }

            // 解析 Content-Range（不区分大小写）
            const QByteArray contentRangeLower = contentRange.toLower().trimmed();
            if (contentRangeLower.startsWith("bytes ")) {
                const int slashPos = contentRangeLower.lastIndexOf('/');
                if (slashPos > 0) {
                    const QByteArray totalPart = contentRangeLower.mid(slashPos + 1).trimmed();
                    bool ok = false;
                    const qlonglong parsedTotal = totalPart.toLongLong(&ok);
                    if (ok && parsedTotal > 0) {
                        // 检测服务端文件是否发生变化（expectedTotal 与新解析值不一致）
                        if (expectedTotal > 0 && parsedTotal != expectedTotal) {
                            if (statusCallback) {
                                statusCallback(QStringLiteral("服务端文件大小已变化（%1 -> %2），删除断点文件重新下载")
                                                   .arg(expectedTotal).arg(parsedTotal));
                            }
                            QFile::remove(partPath);
                            downloadedBytes = 0;
                            expectedTotal = parsedTotal;
                            continue;
                        }
                        expectedTotal = parsedTotal;
                    }
                }
            } else if (httpStatus == 200 && contentLength > 0) {
                expectedTotal = contentLength;
            }

            downloadedBytes = QFileInfo(partPath).exists() ? QFileInfo(partPath).size() : 0;

            // HTTP 416: Range Not Satisfiable —— .part 文件比服务器文件大（文件已更新），从头重下
            if (httpStatus == 416) {
                if (statusCallback) {
                    statusCallback(QStringLiteral("服务端返回 416（Range 无效），删除断点文件后重新全量下载"));
                }
                QFile::remove(partPath);
                downloadedBytes = 0;
                continue;
            }

            if (timeout || netError != QNetworkReply::NoError) {
                if (attempt < maxRetry && downloadedBytes > 0) {
                    if (statusCallback) {
                        statusCallback(QStringLiteral("下载中断，准备重试续传，当前已下载 %1 字节")
                                           .arg(downloadedBytes));
                    }
                    continue;
                }
                errorMessage = timeout
                                   ? QStringLiteral("下载超时，已下载 %1 字节").arg(downloadedBytes)
                                   : QStringLiteral("网络异常中断：%1（已下载 %2 字节）").arg(netErrorStr).arg(downloadedBytes);
                return false;
            }
        }

        // 大小校验
        if (expectedTotal > 0) {
            if (downloadedBytes < expectedTotal) {
                if (attempt < maxRetry) {
                    if (statusCallback) {
                        statusCallback(QStringLiteral("下载未完成，继续续传（%1/%2 字节）")
                                           .arg(downloadedBytes)
                                           .arg(expectedTotal));
                    }
                    continue;
                }
                errorMessage = QStringLiteral("下载未完成，期望 %1 字节，实际 %2 字节")
                                   .arg(expectedTotal)
                                   .arg(downloadedBytes);
                return false;
            }
            if (downloadedBytes > expectedTotal) {
                // 大小超出预期（可能是陈旧的 .part 文件），删除后重试
                if (statusCallback) {
                    statusCallback(QStringLiteral("文件大小异常（期望 %1 字节，实际 %2 字节），删除断点文件重新下载")
                                       .arg(expectedTotal).arg(downloadedBytes));
                }
                QFile::remove(partPath);
                downloadedBytes = 0;
                if (attempt < maxRetry) {
                    continue;
                }
                errorMessage = QStringLiteral("下载文件大小异常，期望 %1 字节，实际 %2 字节（已用尽重试次数）")
                                   .arg(expectedTotal)
                                   .arg(downloadedBytes);
                return false;
            }
        }

        QFile::remove(targetFilePath);
        if (!QFile::rename(partPath, targetFilePath)) {
            errorMessage = QStringLiteral("下载完成但无法生成目标文件: %1")
                               .arg(QDir::toNativeSeparators(targetFilePath));
            return false;
        }

        if (statusCallback) {
            statusCallback(QStringLiteral("下载文件写入完成: %1（%2 字节）")
                               .arg(QDir::toNativeSeparators(targetFilePath))
                               .arg(downloadedBytes));
        }

        if (progressCallback) {
            progressCallback(downloadedBytes, expectedTotal > 0 ? expectedTotal : downloadedBytes);
        }
        return true;
    }

    errorMessage = QStringLiteral("下载失败：已超过最大重试次数");
    return false;
}

// ============================================================================
//  服务器连接 & 清单 & 历史版本
// ============================================================================

QString AppManagerService::serverBaseUrl() const
{
    return m_serverBaseUrl;
}

void AppManagerService::setAuthToken(const QString &token)
{
    m_authToken = token;
}

bool AppManagerService::tryConnectServer(int timeoutMs)
{
    if (m_serverBaseUrl.isEmpty()) {
        return false;
    }

    QString errorMessage;
    QByteArray response = httpGet(QUrl(m_serverBaseUrl + QStringLiteral("/")),
                                  errorMessage, timeoutMs);
    return errorMessage.isEmpty();
}

QJsonArray AppManagerService::fetchAppCatalog(const QString &authToken,
                                              int timeoutMs,
                                              const CancelCallback &cancelCallback)
{
    if (m_serverBaseUrl.isEmpty()) {
        return {};
    }

    QString errorMessage;
    QByteArray response = httpGet(QUrl(m_serverBaseUrl + QStringLiteral("/catalog")),
                                  errorMessage, timeoutMs,
                                  DownloadProgressCallback(), cancelCallback,
                                  authToken.toUtf8());
    if (!errorMessage.isEmpty()) {
        return {};
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        return {};
    }

    QJsonArray normalized;
    const QJsonArray catalog = doc.array();
    for (const QJsonValue &value : catalog) {
        if (!value.isObject()) {
            normalized.append(value);
            continue;
        }

        QJsonObject item = value.toObject();

        const QString downloadUrlText = item.value(QStringLiteral("downloadUrl")).toString().trimmed();
        if (!downloadUrlText.isEmpty()) {
            item.insert(QStringLiteral("downloadUrl"),
                        rebaseToConfiguredServer(downloadUrlText,
                                                 QUrl(downloadUrlText),
                                                 m_serverBaseUrl)
                            .toString());
        }

        const QString metaUrlText = item.value(QStringLiteral("updateMetaUrl")).toString().trimmed();
        if (!metaUrlText.isEmpty()) {
            item.insert(QStringLiteral("updateMetaUrl"),
                        rebaseToConfiguredServer(metaUrlText,
                                                 QUrl(metaUrlText),
                                                 m_serverBaseUrl)
                            .toString());
        }

        normalized.append(item);
    }
    return normalized;
}

QJsonObject AppManagerService::fetchHistoryVersions(const QString &appId,
                                                    int timeoutMs,
                                                    const CancelCallback &cancelCallback)
{
    if (m_serverBaseUrl.isEmpty()) {
        return {};
    }

    QString errorMessage;
    QByteArray response = httpGet(QUrl(m_serverBaseUrl + QStringLiteral("/history/") + appId),
                                  errorMessage, timeoutMs,
                                  DownloadProgressCallback(), cancelCallback);
    if (!errorMessage.isEmpty()) {
        return {};
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }

    QJsonObject result = doc.object();
    const QJsonArray versions = result.value(QStringLiteral("versions")).toArray();
    if (!versions.isEmpty()) {
        QJsonArray normalizedVersions;
        for (const QJsonValue &v : versions) {
            if (!v.isObject()) {
                normalizedVersions.append(v);
                continue;
            }
            QJsonObject vo = v.toObject();
            const QString dlText = vo.value(QStringLiteral("downloadUrl")).toString().trimmed();
            if (!dlText.isEmpty()) {
                vo.insert(QStringLiteral("downloadUrl"),
                          rebaseToConfiguredServer(dlText,
                                                   QUrl(dlText),
                                                   m_serverBaseUrl)
                              .toString());
            }
            normalizedVersions.append(vo);
        }
        result.insert(QStringLiteral("versions"), normalizedVersions);
    }

    return result;
}

bool AppManagerService::downloadToFile(const QUrl &url,
                                       const QString &filePath,
                                       QString &errorMessage,
                                       int timeoutMs,
                                       const DownloadProgressCallback &progressCallback,
                                       const StatusCallback &statusCallback,
                                       const CancelCallback &cancelCallback)
{
    return downloadFileWithResume(url,
                                  filePath,
                                  errorMessage,
                                  timeoutMs,
                                  progressCallback,
                                  statusCallback,
                                  cancelCallback);
}

// ============================================================================
//  配置管理
// ============================================================================

void AppManagerService::addAppEntry(const AppConfig &app)
{
    m_apps.push_back(app);
}

bool AppManagerService::removeAppEntry(const QString &appId)
{
    for (int i = 0; i < m_apps.size(); ++i) {
        if (m_apps[i].id == appId) {
            m_apps.removeAt(i);
            return true;
        }
    }
    return false;
}

bool AppManagerService::saveConfig(QString &errorMessage)
{
    if (m_appListPath.isEmpty()) {
        errorMessage = QStringLiteral("无客户端应用列表路径");
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("apps"), buildAppsArray(m_apps));

    QFile file(m_appListPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorMessage = QStringLiteral("无法写入客户端应用列表: %1").arg(m_appListPath);
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

// ====================================================================
// AppManager 自身升级支持
// ====================================================================

QString AppManagerService::appManagerVersion() const
{
    const QString appExePath = QCoreApplication::applicationFilePath();
    const QString version = getFileVersion(appExePath);
    return version.isEmpty() ? QStringLiteral("1.0.0") : version;
}

OnlineAppInfo AppManagerService::checkAppManagerUpdate(int timeoutMs)
{
    OnlineAppInfo result;
    
    // 从服务器的 /updates/AppManager.json 获取版本信息
    const QUrl updateMetaUrl = QUrl(m_serverBaseUrl.trimmed() + QStringLiteral("/updates/AppManager.json"));
    
    QString errorMessage;
    const QByteArray responseData = httpGet(updateMetaUrl, errorMessage, timeoutMs);
    
    if (!errorMessage.isEmpty() || responseData.isEmpty()) {
        result.errorMessage = !errorMessage.isEmpty() 
            ? errorMessage 
            : QStringLiteral("AppManager 更新元数据为空");
        return result;
    }
    
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.errorMessage = QStringLiteral("AppManager 更新元数据 JSON 格式错误");
        return result;
    }
    
    const QJsonObject obj = doc.object();
    result.latestVersion = obj.value(QStringLiteral("latestVersion")).toString().trimmed();
    QString downloadUrlStr = obj.value(QStringLiteral("downloadUrl")).toString().trimmed();
    
    // 将下载URL调整为配置的服务器地址
    if (!downloadUrlStr.isEmpty()) {
        QUrl baseUrl(m_serverBaseUrl);
        QUrl rawUrl(downloadUrlStr);
        result.downloadUrl = rebaseToConfiguredServer(downloadUrlStr, rawUrl, m_serverBaseUrl);
    }
    
    if (result.latestVersion.isEmpty() || !result.downloadUrl.isValid()) {
        result.errorMessage = QStringLiteral("AppManager 更新元数据缺少 latestVersion 或 downloadUrl");
        return result;
    }

    result.changeLog = obj.value(QStringLiteral("changeLog")).toString().trimmed();
    
    result.requestSuccess = true;
    return result;
}

bool AppManagerService::upgradeAppManager(const OnlineAppInfo &online,
                                          QString &resultMessage,
                                          int timeoutMs,
                                          const DownloadProgressCallback &progressCallback,
                                          const StatusCallback &statusCallback)
{
    if (!online.downloadUrl.isValid()) {
        resultMessage = QStringLiteral("下载URL无效");
        return false;
    }
    
    // 获取可执行文件所在目录
    const QString appBinDir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
    const QString tempDir = QDir::tempPath();
    
    // 下载安装程序到临时目录
    const QString installerFileName = QStringLiteral("AppManagerSetup_%1.exe")
                                        .arg(online.latestVersion);
    const QString installerPath = QDir(tempDir).absoluteFilePath(installerFileName);
    
    if (statusCallback) {
        statusCallback(QStringLiteral("正在下载 AppManager 安装程序..."));
    }
    
    if (!downloadToFile(online.downloadUrl, installerPath, resultMessage, 
                        timeoutMs, progressCallback, statusCallback)) {
        return false;
    }
    
    if (statusCallback) {
        statusCallback(QStringLiteral("下载完成，正在启动安装程序..."));
    }
    
    // 启动安装程序
    // 使用 /SILENT /NORESTART 选项进行静默安装
    // 创建延时脚本让当前进程安全关闭
    QStringList arguments;
    arguments << QStringLiteral("/SILENT");
    arguments << QStringLiteral("/NORESTART");
    
    // 使用QProcess启动安装程序
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    
    if (!process.startDetached(installerPath, arguments)) {
        resultMessage = QStringLiteral("启动安装程序失败: %1").arg(installerPath);
        return false;
    }
    
    resultMessage = QStringLiteral("安装程序已启动，AppManager 将在完成安装后自动重启");
    return true;
}

// ============================================================
// 文档管理
// ============================================================

QVector<ClientDocEntry> AppManagerService::fetchDocCatalog(int timeoutMs,
                                                            const CancelCallback &cancelCallback)
{
    if (m_serverBaseUrl.isEmpty()) return {};

    const QString url = m_serverBaseUrl + QStringLiteral("/docs/catalog");
    QString err;
    const QByteArray response = httpGet(QUrl(url), err, timeoutMs, {}, cancelCallback,
                                        m_authToken.toUtf8());
    if (response.isEmpty()) return {};

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) return {};

    // 用客户端自己的 m_serverBaseUrl 重建 downloadUrl，
    // 避免服务端 effectiveBaseUrl 与客户端配置的服务器地址不一致（如服务端返回
    // http://127.0.0.1:PORT，而客户端需要通过公网域名访问）。
    QString base = m_serverBaseUrl;
    if (base.endsWith('/')) base.chop(1);

    QVector<ClientDocEntry> result;
    for (const QJsonValue &v : doc.array()) {
        if (v.isObject()) {
            ClientDocEntry e = ClientDocEntry::fromJson(v.toObject());
            // 根据 docId + fileName 重建下载 URL，无论服务端返回什么都以本地配置为准
            if (!e.docId.isEmpty() && !e.fileName.isEmpty())
                e.downloadUrl = base + QStringLiteral("/docs/download/") + e.docId + QStringLiteral("/") + e.fileName;
            result.append(e);
        }
    }
    return result;
}

QString AppManagerService::localDocCacheDir() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/docs");
}

QString AppManagerService::localDocFilePath(const ClientDocEntry &doc) const
{
    return QDir(localDocCacheDir()).absoluteFilePath(
        doc.docId + QStringLiteral("/") + doc.fileName);
}

bool AppManagerService::isDocDownloaded(const ClientDocEntry &doc) const
{
    return QFileInfo::exists(localDocFilePath(doc));
}

bool AppManagerService::isDocUpToDate(const ClientDocEntry &doc) const
{
    if (!isDocDownloaded(doc)) return false;
    if (doc.sha256.isEmpty())  return true;
    const QString local = localDocFilePath(doc);
    QFile f(local);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QString localSha = QString::fromLatin1(
        QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex());
    return localSha.compare(doc.sha256, Qt::CaseInsensitive) == 0;
}

bool AppManagerService::downloadDoc(const ClientDocEntry &doc,
                                    QString &errorMessage,
                                    int timeoutMs,
                                    const DownloadProgressCallback &progressCallback,
                                    const CancelCallback &cancelCallback)
{
    if (doc.downloadUrl.isEmpty()) {
        errorMessage = QStringLiteral("文档下载地址为空");
        return false;
    }
    const QString localPath = localDocFilePath(doc);
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    return downloadFileWithResume(QUrl(doc.downloadUrl), localPath,
                                  errorMessage, timeoutMs,
                                  progressCallback, {}, cancelCallback);
}
