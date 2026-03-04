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
#include <QRegularExpression>

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

bool replaceSingleExeWithBackup(const QString &srcExePath,
                                const QString &dstExePath,
                                QString &errorMessage)
{
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

    return true;
}

}

AppManagerService::AppManagerService(QObject *parent)
    : QObject(parent)
{
}

bool AppManagerService::loadConfig(const QString &configPath, QString &errorMessage)
{
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
    const QString rootFromConfig = root.value(QStringLiteral("appsRoot")).toString().trimmed();
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

    const QJsonArray appsArray = root.value(QStringLiteral("apps")).toArray();
    if (appsArray.isEmpty()) {
        errorMessage = QStringLiteral("配置中未定义任何应用（apps 为空）");
        return false;
    }

    QVector<AppConfig> parsedApps;
    parsedApps.reserve(appsArray.size());

    for (int i = 0; i < appsArray.size(); ++i) {
        const QJsonObject obj = appsArray.at(i).toObject();
        AppConfig app;
        app.id = obj.value(QStringLiteral("id")).toString().trimmed();
        app.name = obj.value(QStringLiteral("name")).toString().trimmed();
        app.exeRelativePath = obj.value(QStringLiteral("exe")).toString().trimmed();
        app.updateMetaUrl = QUrl(obj.value(QStringLiteral("updateMetaUrl")).toString().trimmed());

        const QJsonArray reqArray = obj.value(QStringLiteral("requiredFiles")).toArray();
        for (const QJsonValue &value : reqArray) {
            const QString relativeFile = value.toString().trimmed();
            if (!relativeFile.isEmpty()) {
                app.requiredRelativeFiles.append(relativeFile);
            }
        }

        if (app.id.isEmpty() || app.name.isEmpty() || app.exeRelativePath.isEmpty()
            || !app.updateMetaUrl.isValid()) {
            errorMessage = QStringLiteral("第 %1 个应用配置不完整或 updateMetaUrl 无效").arg(i + 1);
            return false;
        }

        parsedApps.push_back(app);
    }

    m_apps = parsedApps;
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

    for (const QString &relativeFile : app.requiredRelativeFiles) {
        if (!QFileInfo::exists(resolvePath(relativeFile))) {
            missingFiles.push_back(relativeFile);
        }
    }

    return missingFiles.isEmpty();
}

QByteArray AppManagerService::httpGet(const QUrl &url,
                                      QString &errorMessage,
                                      int timeoutMs,
                                      const DownloadProgressCallback &progressCallback) const
{
    if (!url.isValid()) {
        errorMessage = QStringLiteral("无效 URL: %1").arg(url.toString());
        return {};
    }

    QNetworkRequest request(url);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_networkManager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

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

    timer.start(timeoutMs);
    loop.exec();

    const bool timeout = !timer.isActive();
    const QNetworkReply::NetworkError netError = reply->error();
    const QByteArray response = reply->readAll();
    const QString netErrorStr = reply->errorString();
    reply->deleteLater();

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

OnlineAppInfo AppManagerService::checkOnlineInfo(const AppConfig &app, int timeoutMs)
{
    OnlineAppInfo info;

    QString errorMessage;
    const QByteArray body = httpGet(app.updateMetaUrl, errorMessage, timeoutMs);
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
    info.downloadUrl = QUrl(obj.value(QStringLiteral("downloadUrl")).toString().trimmed());
    info.sha256 = obj.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    info.packageType = obj.value(QStringLiteral("packageType")).toString().trimmed().toLower();
    info.installRelativeDir = obj.value(QStringLiteral("installRelativeDir")).toString().trimmed();
    const QString installModeText = obj.value(QStringLiteral("installMode")).toString().trimmed();
    info.installMode = installModeText.isEmpty() ? QStringLiteral("single") : installModeText;
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
        info.fullPackageUrl = QUrl(fullPkgStr);
    }

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

    if (info.installMode != QStringLiteral("single")
        && info.installMode != QStringLiteral("allApps")) {
        info.requestSuccess = false;
        info.errorMessage = QStringLiteral("installMode 仅支持 single 或 allApps");
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
                                                 const InstallProgressCallback &installProgressCallback)
{
    // 没有服务器指定的依赖文件列表，跳过检查
    if (online.requiredFiles.isEmpty()) {
        if (statusCallback) {
            statusCallback(QStringLiteral("服务器未配置依赖文件列表，跳过完整性检查"));
        }
        return true;
    }

    // 检查每个依赖文件是否存在
    QStringList missingFiles;
    for (const QString &relFile : online.requiredFiles) {
        const QString absPath = resolvePath(relFile);
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

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        resultMessage = QStringLiteral("无法创建临时目录用于下载完整包");
        return false;
    }

    const QString fullPkgPath = QDir(tempDir.path()).absoluteFilePath(QStringLiteral("full_package.zip"));

    QString downloadError;
    if (!downloadFileWithResume(online.fullPackageUrl,
                                fullPkgPath,
                                downloadError,
                                timeoutMs,
                                progressCallback,
                                statusCallback)) {
        resultMessage = QStringLiteral("完整包下载失败：%1").arg(downloadError);
        return false;
    }

    if (statusCallback) {
        statusCallback(QStringLiteral("完整包下载完成，正在解压..."));
    }
    if (installProgressCallback) {
        installProgressCallback(10);
    }

    // 确定解压目标目录
    QString targetDir;
    if (online.installMode == QStringLiteral("allApps")) {
        targetDir = appsRoot();
    } else if (online.installRelativeDir.trimmed().isEmpty()) {
        targetDir = appAbsoluteDir(app);
    } else {
        targetDir = resolvePath(online.installRelativeDir);
    }
    QDir().mkpath(targetDir);

    // 解压完整包到目标目录
    const QString extractedDir = QDir(tempDir.path()).absoluteFilePath(QStringLiteral("extracted"));
    QDir().mkpath(extractedDir);

#ifdef Q_OS_WIN
    QProcess process;
    process.setProgram(QStringLiteral("powershell"));
    process.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-Command"),
        QStringLiteral(
            "$zipPath='%1';"
            "$destPath='%2';"
            "Add-Type -AssemblyName System.IO.Compression.FileSystem;"
            "$zip=[System.IO.Compression.ZipFile]::OpenRead($zipPath);"
            "$entries=@($zip.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) });"
            "$total=[Math]::Max($entries.Count,1);"
            "$idx=0;"
            "foreach($e in $entries){"
            "  $out=[System.IO.Path]::Combine($destPath,$e.FullName);"
            "  $dir=[System.IO.Path]::GetDirectoryName($out);"
            "  if(-not [string]::IsNullOrEmpty($dir)){ [System.IO.Directory]::CreateDirectory($dir) | Out-Null };"
            "  [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e,$out,$true);"
            "  $idx=$idx+1;"
            "  $p=[int](($idx*100)/$total);"
            "  Write-Output (\"PROGRESS:{0}\" -f $p);"
            "}"
            "$zip.Dispose();")
            .arg(QDir::toNativeSeparators(fullPkgPath), QDir::toNativeSeparators(extractedDir))
    });
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(5000)) {
        resultMessage = QStringLiteral("完整包解压失败：无法启动解压进程");
        return false;
    }

    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        const QByteArray outChunk = process.readAllStandardOutput();
        if (!outChunk.isEmpty()) {
            const QList<QByteArray> lines = outChunk.split('\n');
            for (const QByteArray &line : lines) {
                QByteArray trimmed = line.trimmed();
                if (trimmed.startsWith("PROGRESS:")) {
                    bool ok = false;
                    const int pct = QString::fromLatin1(trimmed.mid(9)).toInt(&ok);
                    if (ok) {
                        if (installProgressCallback) {
                            installProgressCallback(10 + (pct * 60) / 100);
                        }
                        if (statusCallback && (pct % 20 == 0 || pct == 100)) {
                            statusCallback(QStringLiteral("正在解压完整包...%1%").arg(pct));
                        }
                    }
                }
            }
        }
        if (!process.waitForFinished(10)) {
            continue;
        }
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stdErr = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        resultMessage = QStringLiteral("完整包解压失败：%1").arg(stdErr.isEmpty() ? QStringLiteral("未知错误") : stdErr);
        return false;
    }
#else
    Q_UNUSED(fullPkgPath)
    resultMessage = QStringLiteral("当前平台暂不支持完整包解压");
    return false;
#endif

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

    copyRecursive(extractedDir, targetDir);

    if (installProgressCallback) {
        installProgressCallback(95);
    }

    // 再次校验依赖文件
    QStringList stillMissing;
    for (const QString &relFile : online.requiredFiles) {
        if (!QFileInfo::exists(resolvePath(relFile))) {
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
                                   const InstallProgressCallback &installProgressCallback)
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

    QTemporaryDir downloadDir;
    if (!downloadDir.isValid()) {
        resultMessage = QStringLiteral("升级失败：无法创建临时下载目录");
        return false;
    }

    QString packageExt = QStringLiteral(".bin");
    const QString pathPart = online.downloadUrl.path().toLower();
    if (pathPart.endsWith(QStringLiteral(".zip"))) {
        packageExt = QStringLiteral(".zip");
    } else if (pathPart.endsWith(QStringLiteral(".exe"))) {
        packageExt = QStringLiteral(".exe");
    }
    const QString packageFilePath = QDir(downloadDir.path()).absoluteFilePath(
        QStringLiteral("%1_%2%3").arg(app.id, online.latestVersion, packageExt));

    QString downloadError;
    if (!downloadFileWithResume(online.downloadUrl,
                                packageFilePath,
                                downloadError,
                                timeoutMs,
                                progressCallback,
                                statusCallback)) {
        resultMessage = QStringLiteral("升级失败：下载中断或下载失败（支持断点续传）：%1").arg(downloadError);
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
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

    if (online.packageType == QStringLiteral("zip")) {
        if (statusCallback) {
            statusCallback(QStringLiteral("开始执行 ZIP 解压安装，模式: %1")
                               .arg(online.installMode));
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

    QString targetDir;
    if (online.installMode == QStringLiteral("allApps")) {
        targetDir = appsRoot();
    } else if (online.installRelativeDir.trimmed().isEmpty()) {
        targetDir = appAbsoluteDir(app);
    } else {
        targetDir = resolvePath(online.installRelativeDir);
    }
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

    // 使用 PowerShell 原生 Expand-Archive 执行解压覆盖，避免引入额外第三方压缩库依赖。
    QProcess process;
    process.setProgram(QStringLiteral("powershell"));
    process.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-Command"),
        QStringLiteral(
            "$zipPath='%1';"
            "$destPath='%2';"
            "Add-Type -AssemblyName System.IO.Compression.FileSystem;"
            "$zip=[System.IO.Compression.ZipFile]::OpenRead($zipPath);"
            "$entries=@($zip.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) });"
            "$total=[Math]::Max($entries.Count,1);"
            "$idx=0;"
            "foreach($e in $entries){"
            "  $out=[System.IO.Path]::Combine($destPath,$e.FullName);"
            "  $dir=[System.IO.Path]::GetDirectoryName($out);"
            "  if(-not [string]::IsNullOrEmpty($dir)){ [System.IO.Directory]::CreateDirectory($dir) | Out-Null };"
            "  [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e,$out,$true);"
            "  $idx=$idx+1;"
            "  $p=[int](($idx*100)/$total);"
            "  Write-Output (\"PROGRESS:{0}\" -f $p);"
            "}"
            "$zip.Dispose();")
            .arg(QDir::toNativeSeparators(zipFilePath), QDir::toNativeSeparators(extractedDir))
    });
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(5000)) {
        resultMessage = QStringLiteral("ZIP 解压失败：无法启动解压进程");
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
    }

    QByteArray stdoutBuffer;
    qint64 lastOutputTick = 0;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        const QByteArray outChunk = process.readAllStandardOutput();
        if (!outChunk.isEmpty()) {
            stdoutBuffer.append(outChunk);
            while (true) {
                int eolPos = stdoutBuffer.indexOf('\n');
                if (eolPos < 0) {
                    break;
                }
                QByteArray line = stdoutBuffer.left(eolPos).trimmed();
                stdoutBuffer.remove(0, eolPos + 1);
                if (line.startsWith("PROGRESS:")) {
                    bool ok = false;
                    const int zipPercent = QString::fromLatin1(line.mid(9)).toInt(&ok);
                    if (ok) {
                        const int mappedPercent = qBound(5, 5 + (zipPercent * 65) / 100, 70);
                        if (installProgressCallback) {
                            installProgressCallback(mappedPercent);
                        }
                        if (statusCallback && (zipPercent - lastOutputTick >= 10 || zipPercent == 100)) {
                            statusCallback(QStringLiteral("正在解压 ZIP 包...%1%").arg(zipPercent));
                            lastOutputTick = zipPercent;
                        }
                    }
                }
            }
        }
        if (!process.waitForFinished(10)) {
            continue;
        }
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stdOut = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        const QString stdErr = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        resultMessage = QStringLiteral("ZIP 解压失败: %1")
                            .arg(stdErr.isEmpty() ? QStringLiteral("未知错误") : stdErr);
        if (!stdOut.isEmpty()) {
            resultMessage += QStringLiteral("；输出: %1").arg(stdOut);
        }
        if (statusCallback) {
            statusCallback(resultMessage);
        }
        return false;
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
            extractedDir,
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
            const QString srcFilePath = QDir(extractedDir).absoluteFilePath(relativePath);
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
            const QString srcFilePath = QDir(extractedDir).absoluteFilePath(relativePath);
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
                extractedDir,
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
    if (online.installMode == QStringLiteral("allApps")) {
        if (statusCallback) {
            statusCallback(QStringLiteral("全量模式：开始替换 apps.json 标识的全部应用 EXE..."));
        }
        QStringList missingExeApps;
        for (int i = 0; i < m_apps.size(); ++i) {
            const AppConfig &cfg = m_apps.at(i);
            const QString srcExePath = QDir(extractedDir).absoluteFilePath(cfg.exeRelativePath);
            const QString dstExePath = resolvePath(cfg.exeRelativePath);

            if (!QFileInfo::exists(srcExePath)) {
                missingExeApps << QStringLiteral("%1(%2)").arg(cfg.name, cfg.exeRelativePath);
                if (statusCallback) {
                    statusCallback(QStringLiteral("缺失 EXE，无法替换: %1")
                                       .arg(QDir::toNativeSeparators(srcExePath)));
                }
                continue;
            }

            if (statusCallback) {
                statusCallback(QStringLiteral("替换应用 EXE [%1/%2]: %3")
                                   .arg(i + 1)
                                   .arg(m_apps.size())
                                   .arg(cfg.name));
            }

            QString replaceError;
            if (!replaceSingleExeWithBackup(srcExePath, dstExePath, replaceError)) {
                resultMessage = QStringLiteral("ZIP 升级失败（全量 EXE 替换阶段）: %1").arg(replaceError);
                if (statusCallback) {
                    statusCallback(resultMessage);
                }
                return false;
            }
            ++replacedExeCount;
        }

        if (!missingExeApps.isEmpty()) {
            resultMessage = QStringLiteral("ZIP 升级失败：压缩包缺少以下应用 EXE：%1")
                                .arg(missingExeApps.join(QStringLiteral("，")));
            if (statusCallback) {
                statusCallback(resultMessage);
            }
            return false;
        }
    } else if (online.zipReplaceExeRecursively) {
        if (statusCallback) {
            statusCallback(QStringLiteral("正在递归替换 EXE 文件..."));
        }
        if (installProgressCallback) {
            installProgressCallback(85);
        }
        QString replaceError;
        if (!replaceExeFilesRecursively(extractedDir, targetDir, replacedExeCount, replaceError)) {
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

    if (online.installMode == QStringLiteral("allApps")) {
        resultMessage = QStringLiteral("ZIP 全量升级成功，已覆盖目录: %1，替换应用 EXE 数量: %2")
                            .arg(QDir::toNativeSeparators(targetDir))
                            .arg(replacedExeCount);
    } else if (online.zipReplaceExeRecursively) {
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
                                               const StatusCallback &statusCallback) const
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
        if (useRange) {
            request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(downloadedBytes) + "-");
        }

        QIODevice::OpenMode openMode = QIODevice::WriteOnly;
        if (useRange) {
            openMode |= QIODevice::Append;
        } else {
            openMode |= QIODevice::Truncate;
        }

        QFile outFile(partPath);
        if (!outFile.open(openMode)) {
            errorMessage = QStringLiteral("无法写入临时下载文件: %1")
                               .arg(QDir::toNativeSeparators(partPath));
            return false;
        }

        QNetworkReply *reply = m_networkManager.get(request);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);

        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
            const QByteArray chunk = reply->readAll();
            if (!chunk.isEmpty()) {
                outFile.write(chunk);
            }
        });

        if (progressCallback) {
            QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                             [&](qint64 received, qint64 total) {
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

        timer.start(timeoutMs);
        loop.exec();

        const bool timeout = !timer.isActive();
        const auto netError = reply->error();
        const QString netErrorStr = reply->errorString();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        const QByteArray contentRange = reply->rawHeader("Content-Range");

        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            outFile.write(tail);
        }

        outFile.flush();
        outFile.close();

        if (contentRange.startsWith("bytes ")) {
            const int slashPos = contentRange.lastIndexOf('/');
            if (slashPos > 0) {
                const QByteArray totalPart = contentRange.mid(slashPos + 1).trimmed();
                bool ok = false;
                const qlonglong parsedTotal = totalPart.toLongLong(&ok);
                if (ok && parsedTotal > 0) {
                    expectedTotal = parsedTotal;
                }
            }
        } else if (httpStatus == 200 && contentLength > 0) {
            expectedTotal = contentLength;
        }

        downloadedBytes = QFileInfo(partPath).exists() ? QFileInfo(partPath).size() : 0;

        reply->deleteLater();

        if (useRange && httpStatus == 200) {
            if (statusCallback) {
                statusCallback(QStringLiteral("服务端不支持续传回包，切换为全量重下"));
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
                errorMessage = QStringLiteral("下载文件大小异常，期望 %1 字节，实际 %2 字节")
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
            statusCallback(QStringLiteral("下载文件写入完成: %1")
                               .arg(QDir::toNativeSeparators(targetFilePath)));
        }

        if (progressCallback) {
            progressCallback(downloadedBytes, expectedTotal > 0 ? expectedTotal : downloadedBytes);
        }
        return true;
    }

    errorMessage = QStringLiteral("下载失败：已超过最大重试次数");
    return false;
}
