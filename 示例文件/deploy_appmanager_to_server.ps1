# AppManager 部署脚本
# 用途：将 AppManager 及其安装程序部署到更新服务器
# 使用方法：.\deploy_appmanager_to_server.ps1 -ServerIp 192.168.1.100 -Port 8080 -Version 2.0.0

param(
    [string]$ServerIp = "192.168.1.100",
    [int]$Port = 8080,
    [string]$Version = "2.0.0",
    [string]$NetworkPath = "\\$ServerIp\AppManager",
    [string]$HttpBaseUrl = "http://$($ServerIp):$Port"
)

$ErrorActionPreference = "Stop"

# ================================================================
# 步骤 1: 验证源文件
# ================================================================

Write-Host "[1/5] 验证源文件..."

$installerExe = "D:\WorkSpace\QtProjects\AppManager\installer_output\AppManagerSetup.exe"
if (-not (Test-Path $installerExe)) {
    throw "找不到安装程序: $installerExe`n请先运行 build_installer.ps1 生成安装包"
}

$appManagerExe = "D:\WorkSpace\QtProjects\AppManager\build\Desktop_Qt_5_15_2_MinGW_32_bit-Release\release\AppManager.exe"
if (-not (Test-Path $appManagerExe)) {
    throw "找不到主程序: $appManagerExe"
}

Write-Host "✓ 源文件验证成功"

# ================================================================
# 步骤 2: 创建目标目录
# ================================================================

Write-Host "[2/5] 创建目标目录..."

$releasesDir = Join-Path $NetworkPath "releases"
$updatesDir = Join-Path $NetworkPath "updates"

if (-not (Test-Path $releasesDir)) {
    New-Item -ItemType Directory -Path $releasesDir -Force | Out-Null
}

if (-not (Test-Path $updatesDir)) {
    New-Item -ItemType Directory -Path $updatesDir -Force | Out-Null
}

Write-Host "✓ 目标目录已创建或已存在"

# ================================================================
# 步骤 3: 复制安装程序
# ================================================================

Write-Host "[3/5] 复制安装程序..."

$targetInstallerName = "AppManagerSetup_${Version}.exe"
$targetInstallerPath = Join-Path $releasesDir $targetInstallerName

Copy-Item -Path $installerExe -Destination $targetInstallerPath -Force
Write-Host "✓ 安装程序已复制: $targetInstallerPath"

# ================================================================
# 步骤 4: 生成元数据文件
# ================================================================

Write-Host "[4/5] 生成元数据文件..."

$metadataPath = Join-Path $updatesDir "AppManager.json"

# 计算文件 SHA256（可选）
$fileHash = (Get-FileHash -Path $installerExe -Algorithm SHA256).Hash
Write-Host "安装程序 SHA256: $fileHash"

$downloadUrl = "$HttpBaseUrl/releases/$targetInstallerName"

$metadata = @{
    latestVersion = $Version
    downloadUrl = $downloadUrl
    releaseNotes = "AppManager 版本 $Version`n`n请点击'帮助' -> '检查AppManager更新'进行升级"
    minSupportedVersion = "1.0.0"
    forceUpdate = $false
    sha256 = $fileHash
    changelogUrl = "$HttpBaseUrl/docs/CHANGELOG.md"
} | ConvertTo-Json -Depth 10

Set-Content -Path $metadataPath -Value $metadata -Encoding UTF8
Write-Host "✓ 元数据文件已生成: $metadataPath"
Write-Host ""
Write-Host "元数据预览:"
Write-Host $metadata

# ================================================================
# 步骤 5: 验证部署
# ================================================================

Write-Host ""
Write-Host "[5/5] 验证部署..."

if ((Test-Path $targetInstallerPath) -and (Test-Path $metadataPath)) {
    Write-Host "✓ 部署成功!"
    Write-Host ""
    Write-Host "部署信息:"
    Write-Host "├─ 服务器地址: $NetworkPath (映射到 $HttpBaseUrl)"
    Write-Host "├─ 版本号: $Version"
    Write-Host "├─ 安装程序: $targetInstallerName"
    Write-Host "├─ 下载URL: $downloadUrl"
    Write-Host "└─ 元数据URL: $HttpBaseUrl/updates/AppManager.json"
    Write-Host ""
    Write-Host "客户端配置:"
    Write-Host "在 apps.json 中配置: `"serverBaseUrl`": `"$HttpBaseUrl`""
    Write-Host ""
} else {
    throw "部署验证失败！"
}

# ================================================================
# 额外信息
# ================================================================

Write-Host "下一步:"
Write-Host "1. 确保服务器的 /updates/AppManager.json 和 /releases/AppManagerSetup_*.exe 可通过 HTTP 访问"
Write-Host "2. 在客户端 apps.json 中配置 serverBaseUrl 指向: $HttpBaseUrl"
Write-Host "3. 客户端可通过'帮助 -> 检查AppManager更新'获取最新版本"
Write-Host ""
