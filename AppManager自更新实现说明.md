# AppManager 自身升级功能实现说明

## 概述

AppManager 现已支持自身升级（自更新）功能，可以从服务器检查新版本并自动下载安装。

## 架构设计

### 1. 版本检查流程

```
用户点击"帮助" → "检查AppManager更新"
         ↓
checkAppManagerUpdate() 从服务器获取 /updates/AppManager.json
         ↓
对比版本号 (当前版本 vs 最新版本)
         ↓
如果有新版本 → 显示更新提示对话框
         ↓
用户确认升级 → downloadToFile() 下载安装程序
         ↓
完成后启动 ISCC 安装程序 → 自动关闭当前应用
         ↓
安装程序完成安装 → AppManager 自动重启
```

### 2. 核心方法

#### AppManagerService 层

```cpp
// 获取 AppManager 当前版本
QString appManagerVersion() const;

// 检查服务器是否有新版本
OnlineAppInfo checkAppManagerUpdate(int timeoutMs = 10000);

// 下载并启动升级安装程序
bool upgradeAppManager(const OnlineAppInfo &online,
                       QString &resultMessage,
                       int timeoutMs = 30000,
                       const DownloadProgressCallback &progressCallback = nullptr,
                       const StatusCallback &statusCallback = nullptr);
```

#### MainWindow 层

```cpp
// UI 槽函数
void onCheckAppManagerUpdate();   // 检查更新逻辑
void onAboutAppManager();          // 关于对话框
```

## 服务器配置

### 1. 元数据文件结构

服务器需要提供 `/updates/AppManager.json` 文件：

```json
{
  "latestVersion": "2.0.1",
  "downloadUrl": "http://your-server/releases/AppManagerSetup_2.0.1.exe"
}
```

### 2. 配置示例

假设服务器基址为：`http://192.168.1.100:8080`

则元数据 URL 为：`http://192.168.1.100:8080/updates/AppManager.json`

在 `apps.json` 中配置：

```json
{
  "serverBaseUrl": "http://192.168.1.100:8080",
  "apps": [...]
}
```

### 3. 安装程序管理

- **安装程序文件**：`AppManagerSetup_{version}.exe`
  - 由 Inno Setup 编译生成
  - 位置：`installer_output/AppManagerSetup.exe`
  - 复制到服务器的 `releases/` 目录

- **版本号**：在 `installer/AppManager.iss` 中定义
  - `build_installer.ps1 -Version 2.0.1` 自动更新版本号

## 用户交互流程

### 检查更新

1. 用户点击菜单栏：**帮助(H)** → **检查 AppManager 更新(U)**
2. 显示进度对话框：`正在检查 AppManager 新版本...`
3. 服务器查询完成后（中间可能有网络延迟）：
   - **情况 A**：已是最新版 → 信息框提示 `AppManager 已是最新版本 (v1.0.0)`
   - **情况 B**：发现新版本 → 弹出询问对话框

### 升级对话框

```
┌─────────────────────────────────────────┐
│   发现新版本                             │
├─────────────────────────────────────────┤
│  发现 AppManager 新版本 v2.0.1          │
│                                         │
│  当前版本: v1.0.0                      │
│                                         │
│  是否立即升级?                          │
├─────────────────────────────────────────┤
│  [是(Y)]              [否(N)]           │
└─────────────────────────────────────────┘
```

### 下载和安装

1. 用户选择"是" → 显示下载进度对话框
2. 下载完成 → 启动安装程序
3. 显示提示：`安装程序已启动，AppManager 将在完成安装后自动重启`
4. **2秒延迟** → 主应用自动关闭
5. Inno Setup 开始静默安装（`/SILENT /NORESTART`）
6. 安装完成 → AppManager 自动重启

## 关于菜单

用户可点击：**帮助(H)** → **关于 AppManager(A)**

显示关于对话框，包含当前版本号和更新提示。

## 版本号获取原理

AppManager 通过 Windows API 从 exe 文件资源段中读取版本号：

```cpp
QString getFileVersion(const QString &exePath);
// 返回格式: "1.0.0.0" (4 段版本号)
```

**必要条件**：编译发布时需要在 `.rc` 资源文件中设置版本信息，或使用 Qt 资源系统。

### 如何设置 exe 版本信息

1. 在 Qt 项目文件中添加：
   ```pro
   VERSION = 2.0.1.0
   QMAKE_TARGET_COPYRIGHT = Copyright 2024-2025
   QMAKE_TARGET_PRODUCT = AppManager
   ```

2. 重新编译：`qmake && mingw32-make`

3. 验证版本（PowerShell）：
   ```powershell
   (Get-Item "AppManager.exe").VersionInfo.FileVersion
   ```

## 网络和超时处理

- **检查更新超时**：`10 秒` (可配置)
- **下载超时**：`30 秒` (可配置)
- **网络错误处理**：显示错误信息，允许用户重试
- **URL 重定基**：自动将服务器返回的 URL 重定向至 `serverBaseUrl` 配置的主机

## 故障排查

### 问题 1：检查更新后无响应

**原因**：可能是网络连接问题或服务器未配置

**解决方案**：
1. 检查 `apps.json` 中的 `serverBaseUrl` 是否正确
2. 确认服务器 `/updates/AppManager.json` 文件存在且格式正确
3. 检查客户端是否能访问服务器

### 问题 2：下载失败

**可能原因**：
- 安装程序 URL 不可达
- 文件下载中断
- 磁盘空间不足

**解决方案**：
1. 验证 `downloadUrl` 指向的文件确实存在
2. 检查 `/releases/` 目录权限
3. 清理临时文件夹 (`%TEMP%`)

### 问题 3：安装程序启动失败

**可能原因**：
- 下载的 exe 文件损坏
- 权限不足
- 文件名格式错误

**解决方案**：
1. 手动下载并测试安装程序
2. 运行权限检查：以管理员身份运行 AppManager
3. 检查临时目录是否包含 `AppManagerSetup_*.exe`

### 问题 4：安装后应用未重启

**原因**：正常行为 - 需要用户手动启动或通过安装程序的重启选项

**改进方案**：
可修改 `upgradeAppManager()` 在安装程序启动前设置一个注册表项指向新 exe 位置，然后在应用启动时检查是否需要更新快捷方式。

## 高级功能扩展

### 1. 灰度更新

在 `AppManager.json` 中添加：
```json
{
  "latestVersion": "2.0.1",
  "downloadUrl": "...",
  "minVersion": "1.5.0",
  "releaseNotes": "修复 xxx 问题，新增 yyy 功能"
}
```

### 2. 强制更新

添加 `forceUpdate` 标志：
```json
{
  "latestVersion": "2.0.1",
  "downloadUrl": "...",
  "forceUpdate": true
}
```

修改 UI 逻辑，强制更新时禁用"否"按钮。

### 3. 增量更新

使用 Delta 更新库（如 bsdiff）减少下载大小：
```json
{
  "latestVersion": "2.0.1",
  "downloadUrl": "...",
  "deltaUrl": "...",
  "deltaFromVersion": "2.0.0"
}
```

### 4. 签名验证

在 `upgradeAppManager()` 中添加 SHA256 校验：
```cpp
const QString sha256 = online.sha256;
// 或从 AppManager.json 读取：
// "sha256": "abc123..."
```

## 测试清单

- [ ] 检查更新（无新版本）
- [ ] 检查更新（有新版本）
- [ ] 下载安装程序
- [ ] 启动安装程序
- [ ] 应用自动关闭
- [ ] 安装程序安装完成
- [ ] 应用重启成功
- [ ] 关于对话框显示正确版本号
- [ ] 网络超时处理
- [ ] 下载中断重试
- [ ] 权限不足错误处理
- [ ] 多语言支持（中文）

## API 参考

### AppManagerService::appManagerVersion()

```cpp
QString version = m_service.appManagerVersion();
// 返回: "1.0.0" 或 "1.0.0.0"
// 失败时返回: "1.0.0" (默认值)
```

### AppManagerService::checkAppManagerUpdate()

```cpp
OnlineAppInfo info = m_service.checkAppManagerUpdate(10000);
// 返回的 OnlineAppInfo 包含：
// - requestSuccess: 是否成功获取信息
// - errorMessage: 错误信息（失败时有值）
// - latestVersion: 最新版本号
// - downloadUrl: 下载链接（已重定基）
```

### AppManagerService::upgradeAppManager()

```cpp
QString msg;
bool ok = m_service.upgradeAppManager(
    onlineInfo,
    msg,
    30000,  // 超时
    [](qint64 rx, qint64 total) {
        // 下载进度回调
    },
    [](const QString &status) {
        // 状态文本回调
    }
);
```

## 总结

AppManager 自更新功能提供了：

✅ 自动版本检查  
✅ 用户友好的升级流程  
✅ 无缝安装和重启  
✅ 完整的错误处理和日志  
✅ 可配置的超时和回调  
✅ URL 重定基支持（多服务器环境）  

通过配置服务器的 `/updates/AppManager.json` 和 `Inno Setup` 安装程序，
用户可以方便地获取最新版本。
