param(
    [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"

$projectRoot = "D:\WorkSpace\QtProjects\AppManager"
$buildReleaseDir = Join-Path $projectRoot "build\Desktop_Qt_5_15_2_MinGW_32_bit-Release\release"
$distDir = Join-Path $projectRoot "dist\AppManager"
$issFile = Join-Path $projectRoot "installer\AppManager.iss"

$qtBin = "C:\Qt\5.15.2\mingw81_32\bin"
$windeployqt = Join-Path $qtBin "windeployqt.exe"

# Try common Inno Setup install locations.
$isccCandidates = @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    throw "未找到 ISCC.exe，请先安装 Inno Setup 6。"
}

if (-not (Test-Path $windeployqt)) {
    throw "未找到 windeployqt: $windeployqt"
}

if (-not (Test-Path (Join-Path $buildReleaseDir "AppManager.exe"))) {
    throw "未找到 Release 可执行文件，请先编译 Release：$buildReleaseDir\AppManager.exe"
}

Write-Host "[1/5] 准备 dist 目录..."
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Get-ChildItem $distDir -Force | Remove-Item -Recurse -Force

Write-Host "[2/5] 复制主程序和配置..."
Copy-Item (Join-Path $buildReleaseDir "AppManager.exe") $distDir -Force
Copy-Item (Join-Path $projectRoot "apps.json") $distDir -Force

# Optional runtime files your app expects.
$optionalFiles = @("versions.json", "zip_replace_manifest.json")
foreach ($f in $optionalFiles) {
    $src = Join-Path $projectRoot $f
    if (Test-Path $src) {
        Copy-Item $src $distDir -Force
    }
}

Write-Host "[3/5] 执行 windeployqt..."
& $windeployqt --release --compiler-runtime --no-translations (Join-Path $distDir "AppManager.exe")

Write-Host "[4/5] 更新安装包版本号..."
$issText = Get-Content $issFile -Raw
$issText = [regex]::Replace($issText, '#define MyAppVersion ".*?"', "#define MyAppVersion `"$Version`"")
Set-Content -Path $issFile -Value $issText -Encoding UTF8

Write-Host "[5/5] 编译安装包..."
& $iscc $issFile

Write-Host "完成。安装包输出目录：$projectRoot\installer_output"
