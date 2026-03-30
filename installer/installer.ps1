<#
脚本名称: build_installer.ps1
用途: 一键打包 AppManager 安装程序（准备 dist -> 补齐 Qt 运行库 -> 调用 Inno Setup 编译）

使用示例:
  .\build_installer.ps1
  .\build_installer.ps1 -Version 1.2.3

输出结果:
  - 中间产物目录: dist\AppManager
  - 安装包输出目录: installer_output
#>

param(
    # 安装包版本号，会写回 AppManager.iss 中的 MyAppVersion
    # 建议与可执行文件版本保持一致，便于排查与升级管理
    [string]$Version = "1.0.3.0"
)

function Convert-ToFileVersion([string]$semanticVersion)
{
    # Inno Setup VersionInfoVersion 需要数字格式，补齐到 4 段。
    $parts = $semanticVersion.Split('.', [System.StringSplitOptions]::RemoveEmptyEntries)
    if ($parts.Count -eq 0) {
        return "0.0.0.0"
    }

    $normalized = @()
    foreach ($p in $parts) {
        $n = 0
        if (-not [int]::TryParse($p, [ref]$n)) {
            $n = 0
        }
        $normalized += [string]$n
    }

    while ($normalized.Count -lt 4) {
        $normalized += "0"
    }

    if ($normalized.Count -gt 4) {
        $normalized = $normalized[0..3]
    }

    return ($normalized -join '.')
}

# 发生任何错误立即终止脚本，避免出现“部分成功”的脏状态。
$ErrorActionPreference = "Stop"

# ==================== 路径配置 ====================
# 项目根目录（按当前仓库固定路径配置）
$projectRoot = "D:\WorkSpace\QtProjects\AppManager"

# Release 可执行文件目录（Qt 构建输出）
$buildReleaseDir = Join-Path $projectRoot "build\Desktop_Qt_5_15_2_MinGW_32_bit-Release\release"

# 打包暂存目录（供 Inno Setup 收集最终文件）
$distDir = Join-Path $projectRoot "dist\AppManager"

# Inno Setup 脚本文件路径
$issFile = Join-Path $projectRoot "installer\AppManager.iss"

# Qt 工具目录与 windeployqt 路径
$qtBin = "C:\Qt\5.15.2\mingw81_32\bin"
$windeployqt = Join-Path $qtBin "windeployqt.exe"

# ==================== 工具探测 ====================
# 尝试常见 Inno Setup 安装位置，取第一个存在的 ISCC.exe
$isccCandidates = @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

# 未找到 Inno Setup 编译器时直接报错
if (-not $iscc) {
    throw "未找到 ISCC.exe，请先安装 Inno Setup 6。"
}

# 未找到 windeployqt 时直接报错
if (-not (Test-Path $windeployqt)) {
    throw "未找到 windeployqt: $windeployqt"
}

# 未找到主程序时直接报错（说明还没有完成 Release 编译）
if (-not (Test-Path (Join-Path $buildReleaseDir "AppManager.exe"))) {
    throw "未找到 Release 可执行文件，请先编译 Release：$buildReleaseDir\AppManager.exe"
}

# ==================== [1/5] 准备 dist 目录 ====================
# -Force: 目录存在时不报错
Write-Host "[1/5] 准备 dist 目录..."
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

# 清空 dist 目录下旧内容，保证打包产物干净
Get-ChildItem $distDir -Force | Remove-Item -Recurse -Force

# ==================== [2/5] 复制主程序和配置 ====================
Write-Host "[2/5] 复制主程序和配置..."

# 复制主程序
Copy-Item (Join-Path $buildReleaseDir "AppManager.exe") $distDir -Force

# 复制核心配置文件（运行时依赖）
Copy-Item (Join-Path $projectRoot "apps.json") $distDir -Force

# 可选运行时文件：存在才复制，不存在则跳过
$optionalFiles = @("versions.json", "zip_replace_manifest.json")
foreach ($f in $optionalFiles) {
    $src = Join-Path $projectRoot $f
    if (Test-Path $src) {
        Copy-Item $src $distDir -Force
    }
}

# ==================== [3/5] 执行 windeployqt ====================
# 作用: 自动收集 Qt 运行库、插件、编译器运行时到 dist 目录
# --release: 按 Release 模式部署
# --compiler-runtime: 附带编译器运行时
# --no-translations: 不复制 Qt 翻译文件，减小体积
Write-Host "[3/5] 执行 windeployqt..."
& $windeployqt --release --compiler-runtime --no-translations (Join-Path $distDir "AppManager.exe")

# ==================== [4/5] 更新安装包版本号 ====================
Write-Host "[4/5] 更新安装包版本号..."

$fileVersion = Convert-ToFileVersion $Version

# 读取 .iss 全文并替换版本宏（只替换 MyAppVersion 这一行）
$issText = Get-Content $issFile -Raw
$issText = [regex]::Replace($issText, '#define MyAppVersion ".*?"', "#define MyAppVersion `"$Version`"")
$issText = [regex]::Replace($issText, '#define MyAppFileVersion ".*?"', "#define MyAppFileVersion `"$fileVersion`"")

# 以 UTF8 写回，确保中文注释和字符串不乱码
Set-Content -Path $issFile -Value $issText -Encoding UTF8

# ==================== [5/5] 编译安装包 ====================
# 调用 Inno Setup 编译器，根据 AppManager.iss 生成安装包 exe
Write-Host "[5/5] 编译安装包..."
& $iscc $issFile

# 完成提示
Write-Host "完成。安装包输出目录：$projectRoot\installer_output"
