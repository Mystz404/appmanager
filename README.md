# AppManager

> 基于 Qt 5 Widgets 的多应用统一管理工具，适用于同一现场部署多个上位机程序、需要集中分发/升级/维护的场景。

---

## 目录

- [功能概览](#功能概览)
- [技术栈与构建信息](#技术栈与构建信息)
- [目录结构](#目录结构)
- [核心模块说明](#核心模块说明)
  - [apptypes.h — 数据模型](#apptypesh--数据模型)
  - [appmanagerservice — 服务层](#appmanagerservice--服务层)
  - [versionutils — 版本工具](#versionutils--版本工具)
  - [MainWindow — 主界面](#mainwindow--主界面)
  - [UpdateDialog — 升级向导对话框](#updatedialog--升级向导对话框)
  - [LoginDialog — 登录认证](#logindialog--登录认证)
  - [DocBrowserPage — 文档浏览](#docbrowserpage--文档浏览)
  - [refreshbuttonutils.h — UI 工具](#refreshbuttonutilsh--ui-工具)
- [配置文件详解](#配置文件详解)
  - [apps.json — 客户端主配置](#appsjson--客户端主配置)
  - [client_apps.json — 持久化应用清单](#client_appsjson--持久化应用清单)
  - [zip_replace_manifest.json — ZIP 升级替换清单](#zip_replace_manifestjson--zip-升级替换清单)
- [升级包类型与流程](#升级包类型与流程)
  - [EXE 包升级](#exe-包升级)
  - [ZIP 包升级](#zip-包升级)
  - [依赖文件修复](#依赖文件修复)
  - [断点续传与下载缓存](#断点续传与下载缓存)
- [服务端元数据格式](#服务端元数据格式)
- [AppManager 自身升级](#appmanager-自身升级)
- [安装包制作](#安装包制作)
- [已知限制与注意事项](#已知限制与注意事项)

---

## 功能概览

| 功能 | 说明 |
|---|---|
| 多应用配置化管理 | 从 `apps.json` 读取应用定义，支持统一根目录管理 |
| 在线版本检测 | 向服务端拉取各应用升级元数据，语义化版本比较 |
| 向导式批量升级 | 检测 → 选择 → 升级 → 结果 四阶段向导，支持取消 |
| EXE 直接替换升级 | 备份旧版本 → 替换 EXE → SHA256 校验，失败可回滚 |
| ZIP 解压覆盖升级 | 支持顶层目录自动剥离，按清单替换或全目录覆盖 |
| 依赖完整性修复 | 缺失少量文件时逐文件下载，缺失多时下载完整包解压 |
| 断点续传 | 下载中断后保留 `.part` 文件，下次自动续传 |
| 下载缓存复用 | 下载成功后缓存到 `download_cache/`，失败重试无需重下 |
| 进程管理 | 升级前自动关闭目标程序，升级后可重新启动 |
| 快捷启动 | 一键启动/重启受管应用，支持多开控制 |
| 登录认证 | 对接服务端 `/login` 接口，五次失败后 30 秒冷却 |
| 文档浏览下载 | 浏览服务端文档目录，支持分类过滤、搜索、本地缓存 |
| AppManager 自更新 | 启动时静默检测自身新版本，后台下载后提示用户 |
| 日志记录 | 实时界面日志 + 本地文件日志，时间戳格式化 |
| 安装包打包 | 提供 Inno Setup 脚本，支持一键打包 `AppManagerSetup_x.x.x.x.exe` |

---

## 技术栈与构建信息

| 项目 | 内容 |
|---|---|
| 框架 | Qt 5.15.2（MinGW 8.1 32-bit） |
| 语言 | C++17 |
| 构建工具 | qmake + mingw32-make |
| Qt 模块 | core, gui, widgets, network |
| 平台 | Windows（发布版本为 Release 32-bit） |
| 当前版本 | 1.0.3.20 |
| 安装器工具 | Inno Setup 6 |

### 构建步骤

```bat
:: 进入构建目录
cd build\Desktop_Qt_5_15_2_MinGW_32_bit-Release

:: 生成 Makefile（首次或 .pro 修改后需执行）
set PATH=C:\Qt\Tools\mingw810_32\bin;C:\Qt\5.15.2\mingw81_32\bin;%PATH%
qmake ..\..\AppManager.pro -spec win32-g++ "CONFIG+=release"

:: 编译
mingw32-make -j8
```

编译产物输出至 `build\Desktop_Qt_5_15_2_MinGW_32_bit-Release\AppManager\AppManager.exe`。

---

## 目录结构

```
AppManager/
├── AppManager.pro              # qmake 项目文件
├── main.cpp                    # 程序入口
├── apptypes.h                  # 核心数据结构（AppConfig / OnlineAppInfo）
├── docclienttypes.h            # 文档客户端数据结构（ClientDocEntry）
├── appmanagerservice.h/.cpp    # 服务层：配置加载、下载、升级、依赖修复
├── versionutils.h/.cpp         # 版本号比较 + Windows EXE 版本读取
├── mainwindow.h/.cpp/.ui       # 主窗口 UI 与交互逻辑
├── updatedialog.h/.cpp         # 向导式升级对话框
├── logindialog.h/.cpp          # 登录对话框
├── docbrowserpage.h/.cpp       # 文档浏览页面
├── refreshbuttonutils.h        # 刷新按钮动画工具（header-only）
├── apps.json                   # 客户端主配置（appsRoot + 应用列表 + serverBaseUrl）
├── zip_replace_manifest.json   # ZIP 升级文件替换清单（示例）
├── versions.json               # 版本号存储文件（示例）
├── resources/                  # 图标等嵌入资源
├── installer/
│   └── AppManager.iss          # Inno Setup 打包脚本
├── installer_output/           # 安装包输出目录
├── dist/                       # 发布文件暂存目录
└── build/                      # 构建中间产物
```

---

## 核心模块说明

### apptypes.h — 数据模型

定义两个核心数据结构，是整个应用的数据契约。

#### `AppConfig`（应用静态配置）

| 字段 | 类型 | 来源 | 说明 |
|---|---|---|---|
| `id` | `QString` | apps.json | 唯一标识符，用作哈希键/缓存键 |
| `name` | `QString` | apps.json | 显示名称（可含中文） |
| `exeRelativePath` | `QString` | apps.json | 主 EXE 相对 appsRoot 的路径 |
| `requiredRelativeFiles` | `QStringList` | apps.json | 必需文件列表，用于完整性检查 |
| `updateMetaUrl` | `QUrl` | 由 serverBaseUrl 自动生成 | 升级元数据接口地址 |
| `isLocalApp` | `bool` | apps.json / 用户添加 | 本地手动添加，不参与在线升级检测 |
| `isHistoryVersion` | `bool` | 下载历史版本时标记 | 历史版本副本，不参与在线检测 |
| `allowMultiInstance` | `bool` | apps.json | 是否允许多开 |

#### `OnlineAppInfo`（在线版本信息）

| 字段 | 类型 | 说明 |
|---|---|---|
| `requestSuccess` | `bool` | 网络请求与 JSON 解析是否成功 |
| `latestVersion` | `QString` | 服务端声明的最新版本号 |
| `downloadUrl` | `QUrl` | 升级包下载地址 |
| `sha256` | `QString` | 升级包 SHA256（可选，为空则跳过校验） |
| `packageType` | `QString` | `"exe"` 或 `"zip"` |
| `subDir` | `QString` | 客户端安装子目录（相对 appsRoot） |
| `zipReplaceExeRecursively` | `bool` | ZIP 升级时是否递归替换所有 EXE |
| `requiredFiles` | `QStringList` | 服务端声明的依赖文件列表 |
| `fullPackageUrl` | `QUrl` | 完整包下载地址（依赖修复用） |
| `depsBaseUrl` | `QUrl` | 单文件依赖下载基址 |
| `changeLog` | `QString` | 当前版本更新说明 |

---

### appmanagerservice — 服务层

`AppManagerService`（继承 `QObject`）是所有业务逻辑的核心，主界面和升级对话框均通过它操作。

#### 配置管理

- `loadConfig(configPath)` — 读取 `apps.json`，解析 `appsRoot`、`serverBaseUrl`、应用列表，自动生成 `updateMetaUrl`
- 优先读取 `client_apps.json`（持久化清单），不存在时从 `apps.json` 迁移
- `saveConfig()` — 将当前应用列表写回 `client_apps.json`
- `addAppEntry()` / `removeAppEntry()` — 动态增删应用条目

#### 本地检查

- `checkRequiredFiles(app, missingFiles)` — 检查 EXE 及 requiredFiles 是否全部存在
- `missingRequiredDeps(app)` — 返回仅 requiredFiles 中缺失的文件列表
- `appCurrentVersion(app)` — 调用 `getFileVersion()` 读取 EXE 的 Windows 文件版本

#### 网络通信

- `httpGet(url, ...)` — 通用 HTTP GET，支持超时、进度回调、取消回调
- `checkOnlineInfo(app, ...)` — 请求升级元数据 JSON，返回 `OnlineAppInfo`
- `tryConnectServer()` — 连通性探测
- `fetchAppCatalog()` — 从服务端拉取受管应用清单（支持 `Authorization` 头）
- `fetchHistoryVersions(appId)` — 获取某应用的历史版本列表

#### 下载

- `downloadFileWithResume(url, targetFilePath, ...)` — 断点续传下载（`.part` 文件机制），支持进度/状态/取消回调，最多重试 5 次

#### 升级执行

- `upgradeApp(app, online, ...)` — 完整升级流程入口（下载 → 校验 → 关闭进程 → 安装），支持 `forceRedownload`
- `upgradeByExeReplace(...)` — EXE 包安装：临时文件写入 → 备份旧版本 → 原子替换
- `upgradeByZipExtract(...)` — ZIP 包安装：PowerShell 解压 → 自动剥离顶层目录 → 按清单或全目录覆盖 → 替换 EXE

#### 依赖修复

- `checkAndFixDependencies(app, online, ...)` — 完整依赖修复流程，支持 `forceRedownload`
  - ≤6 个缺失文件 且 `depsBaseUrl` 有效：逐文件下载
  - 否则：下载 `fullPackageUrl` 完整包并解压

#### 下载缓存（内部辅助函数）

- `persistentDownloadCacheDir()` — 返回 `<appDir>/download_cache/`，不存在则自动创建
- `cachedUpgradePackagePath(app, online)` — `download_cache/upgrades/<id>_<version>.<ext>`
- `cachedFullPackagePath(app, online)` — `download_cache/full_packages/<id>_<version>_full.zip`

#### 解压引擎（`extractZipFile()`）

- 调用系统 PowerShell（优先 Sysnative → System32 → PATH 三级回退）
- 每个文件独立 `try/catch`，单文件失败跳过，不影响整体解压
- 同时读取 stdout/stderr，防止管道缓冲区满导致死锁
- 路径中单引号自动转义（`'` → `''`），防止 PowerShell 脚本崩溃
- 支持解压超时（默认 10 分钟），防止进程挂死
- `unwrapSingleTopLevelDir()` — 自动剥离 ZIP 内唯一顶层目录

#### 回调类型别名

```cpp
using DownloadProgressCallback = std::function<void(qint64 receivedBytes, qint64 totalBytes)>;
using StatusCallback           = std::function<void(const QString &status)>;
using InstallProgressCallback  = std::function<void(int percent)>;
using CancelCallback           = std::function<bool()>;
```

---

### versionutils — 版本工具

| 函数 | 说明 |
|---|---|
| `compareVersions(left, right)` | 语义化版本比较，按 `.` 分段逐整数比较，不足段补 0；返回 <0 / 0 / >0 |
| `getFileVersion(exePath)` | 调用 Windows `VerQueryValueW` 读取 EXE 的文件版本资源，返回 `"1.2.3.4"` 格式字符串 |

---

### MainWindow — 主界面

主窗口为 `QMainWindow`，包含：

- **应用列表**：显示所有受管应用的名称、版本、状态、操作按钮
- **工具栏**：刷新、检查更新、升级全部等全局操作
- **日志面板**：实时输出操作日志，同步写入本地日志文件
- **文档浏览标签页**：嵌入 `DocBrowserPage`
- **用户面板**：登录/注销、显示用户名和角色
- **服务器连接状态**：启动后自动探测服务器可达性
- **AppManager 自更新**：启动后静默检测，后台下载新版本后弹出提示

关键私有方法：

| 方法 | 说明 |
|---|---|
| `loadConfig()` | 读取配置、校验本地文件、刷新列表 |
| `validateLocalApps()` | 检查所有应用的 EXE 及必需文件是否存在 |
| `startUpdateWorkflow(apps)` | 启动 `UpdateDialog` 执行升级流程 |
| `fetchRemoteCatalog()` | 从服务端拉取最新应用清单并合并到本地 |
| `trySilentAppManagerAutoUpdate()` | 静默自检 AppManager 新版本 |
| `tryStartupAppsUpdateCheck()` | 启动时后台批量检查所有应用是否有新版本 |
| `refreshUpdateActions()` | 根据检测结果刷新升级按钮和徽标 |
| `checkServerConnection()` | 探测服务器连通性，更新状态指示器 |

---

### UpdateDialog — 升级向导对话框

将完整升级流程封装为四个阶段（`QStackedWidget` 切换）：

```
[检测页 PageCheck]
    ↓ 依次请求各应用元数据，比较版本
[选择页 PageSelect]
    ↓ 用户勾选要升级的应用
[升级页 PageUpgrade]
    ↓ 依次执行 checkAndFixDependencies + upgradeApp
[结果页 PageResult]
    → 显示成功/失败统计和日志
```

**失败重试机制**（对话框内 `while(true)` 循环）：

- 依赖修复失败时弹出 4 按钮选择框：重试（复用缓存）/ 删除重下 / 继续升级 / 跳过
- 升级失败时弹出 3 按钮选择框：重试（复用缓存）/ 删除重下 / 跳过

升级完成后自动重启升级前被自动关闭的应用程序。

---

### LoginDialog — 登录认证

- 向服务端 `POST /login` 发送用户名/密码
- 成功后暴露 `token()`、`username()`、`role()` 供主窗口使用
- 内置防暴力破解：连续失败 5 次后进入 30 秒登录冷却期
- token 由主窗口传入 `AppManagerService::setAuthToken()`，后续文档/目录请求自动携带 `Authorization: Bearer <token>`

---

### DocBrowserPage — 文档浏览

- 调用 `AppManagerService::fetchAppCatalog()` 拉取服务端文档目录（`ClientDocEntry` 列表）
- 支持按分类过滤、关键词搜索（标题 + 描述 + keywords 模糊匹配）
- 文档条目支持：在线打开、下载后打开、在文件夹中显示、用其他程序打开、删除本地缓存
- 本地已下载文档图标高亮区分
- 分页加载（"加载更多"按钮）

---

### refreshbuttonutils.h — UI 工具

Header-only 工具，提供三个内联函数，解决 Qt 按钮禁用时图标变灰的问题，并管理刷新动画：

- `setButtonIconWithoutDisabledTint()` — 强制使所有状态（包括 Disabled）显示原始图标
- `setRefreshButtonBusyState()` — 一键切换按钮忙/闲状态（动画 GIF ↔ 静态图标）

---

## 配置文件详解

### apps.json — 客户端主配置

```json
{
  "serverBaseUrl": "http://your-server:6655",
  "appsRoot": ".",
  "apps": [
    {
      "id":        "myapp",
      "name":      "我的应用",
      "exe":       "MyApp/MyApp.exe",
      "requiredFiles": [
        "MyApp/config.ini",
        "MyApp/data/startup.dat"
      ]
    }
  ]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `serverBaseUrl` | 是 | 服务端根地址，用于自动生成 `updateMetaUrl` 和下载地址 |
| `appsRoot` | 否 | 所有应用的根目录，相对路径基于 `apps.json` 所在目录；默认为当前目录 |
| `apps[].id` | 是 | 应用唯一 ID，建议小写+下划线 |
| `apps[].name` | 是 | 界面显示名称 |
| `apps[].exe` | 是 | 主 EXE 相对 `appsRoot` 的路径 |
| `apps[].requiredFiles` | 否 | 必需文件列表（相对 `appsRoot`） |
| `apps[].isLocalApp` | 否 | `true` 表示本地手动添加，不参与在线升级 |
| `apps[].allowMultiInstance` | 否 | `true` 允许多开 |

> `updateMetaUrl` 无需手动填写，程序启动时自动以 `serverBaseUrl/updates/<id>.json` 生成。

### client_apps.json — 持久化应用清单

首次启动后自动从 `apps.json` 迁移生成，后续所有对应用列表的增删改均写入此文件。格式与 `apps.json` 中 `apps` 数组完全一致。

### zip_replace_manifest.json — ZIP 升级替换清单

```json
{
  "description": "ZIP升级替换清单（相对 appsRoot），可动态增删 files 项。",
  "files": [
    "managed_apps/MyApp.exe",
    "managed_apps/MyApp/settings.json"
  ]
}
```

此文件存在且 `files` 非空时，ZIP 升级将**仅替换清单中列出的文件**，而不是全目录覆盖。适用于只更新部分文件的增量 ZIP 包。文件不存在则执行默认策略：覆盖所有非 EXE 文件 + 递归替换 EXE。

---

## 升级包类型与流程

### EXE 包升级

适用于 `packageType: "exe"` 的单可执行文件程序：

```
下载升级包（断点续传，保存至 download_cache/upgrades/）
    ↓
SHA256 校验（若 sha256 字段非空）
    ↓
关闭目标程序（若正在运行，调用 TerminateProcess）
    ↓
复制升级包到 <目标路径>.new 临时文件
    ↓
备份旧版本为 <目标路径>.bak
    ↓
删除旧版本，重命名 .new → 目标路径
    （失败则从 .bak 回滚）
    ↓
升级完成
```

### ZIP 包升级

适用于 `packageType: "zip"` 的多文件程序包：

```
下载 ZIP 升级包（断点续传，保存至 download_cache/upgrades/）
    ↓
SHA256 校验（若 sha256 字段非空）
    ↓
关闭目标程序（若正在运行）
    ↓
PowerShell 解压 ZIP 到临时目录
    ↓
自动剥离 ZIP 内唯一顶层目录（若解压根目录下恰好只有1个子目录）
    ↓
步骤1：补齐目标目录缺失的文件和目录（syncMissingFilesAndDirs）
    ↓
步骤2：按 zip_replace_manifest.json 或默认策略覆盖文件
         有清单：仅替换清单中的文件
         无清单：覆盖所有非 EXE 文件（copyDirectoryNonExe）
    ↓
步骤3：递归替换所有 EXE 文件（若 zipReplaceExeRecursively=true）
    ↓
升级完成
```

### 依赖文件修复

升级前检测到 `requiredFiles` 中有文件缺失时，在升级阶段自动触发：

```
缺失文件数 ≤ 6 且 depsBaseUrl 有效
    → 逐文件从 depsBaseUrl?file=<path> 下载
    → 部分失败时回退到完整包策略

缺失文件数 > 6 或逐文件部分失败
    → 下载 fullPackageUrl 完整包（download_cache/full_packages/）
    → PowerShell 解压到临时目录
    → 自动剥离顶层目录
    → 复制所有文件到目标目录
    → 再次校验依赖文件
```

### 断点续传与下载缓存

- 每个下载任务先写 `<目标文件>.part`，完成后重命名为目标文件名
- 再次下载同一 URL 时，若 `.part` 存在则自动携带 `Range: bytes=<已下载字节数>-` 请求头续传
- 升级包和完整包成功下载后保存在 `download_cache/` 目录，下次重试直接复用，无需重新下载
- 升级失败时弹出对话框，用户可选择"重试（复用缓存）"或"删除已下载文件并重下"
- `forceRedownload=true` 时删除缓存文件和 `.part` 文件后从零重新下载

---

## 服务端元数据格式

AppManager 从 `serverBaseUrl/updates/<appId>.json` 拉取升级元数据，格式如下：

```json
{
  "latestVersion":  "2.1.0",
  "downloadUrl":    "http://your-server:6655/download/myapp_2.1.0.zip",
  "sha256":         "abc123...",
  "packageType":    "zip",
  "subDir":         "MyApp",
  "changeLog":      "修复了若干问题，新增XXX功能",
  "zipReplaceExeRecursively": true,
  "requiredFiles": [
    "MyApp/config.ini",
    "MyApp/data/"
  ],
  "fullPackageUrl": "http://your-server:6655/download/myapp_full.zip",
  "depsBaseUrl":    "http://your-server:6655/deps/myapp"
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `latestVersion` | 是 | 服务端最新版本号 |
| `downloadUrl` | 是 | 升级包下载地址 |
| `sha256` | 否 | 升级包 SHA256 哈希，为空则跳过校验 |
| `packageType` | 否 | `"exe"`（默认）或 `"zip"` |
| `subDir` | 否 | 客户端安装子目录，为空表示安装到 `appsRoot` 根目录 |
| `changeLog` | 否 | 版本更新说明，显示在升级确认对话框 |
| `zipReplaceExeRecursively` | 否 | ZIP 升级时是否递归替换 EXE，默认 `true` |
| `requiredFiles` | 否 | 依赖文件列表，升级前完整性检查 |
| `fullPackageUrl` | 否 | 完整包地址，依赖修复用 |
| `depsBaseUrl` | 否 | 单文件依赖下载基址（请求格式：`depsBaseUrl?file=<percent-encoded-path>`） |

---

## AppManager 自身升级

AppManager 支持通过同一套升级机制更新自身：

1. 启动时调用 `trySilentAppManagerAutoUpdate()`，在后台线程静默检测新版本
2. 检测到新版本后弹出提示框（显示版本号和 changeLog），用户确认后开始下载
3. 下载完成后提示用户关闭 AppManager，由新版本替换旧文件后重新启动
4. 服务端需要为 AppManager 提供对应的元数据文件（`/updates/appmanager.json`）

---

## 安装包制作

使用 `installer/AppManager.iss`（Inno Setup 6）打包：

1. 编译 Release，执行 `windeployqt AppManager.exe` 部署所有 Qt 运行时依赖
2. 确认 `AppManager.iss` 中 `MySourceDir` 指向 `build/.../AppManager` 输出目录
3. 用 Inno Setup Compiler 打开并编译 `AppManager.iss`
4. 安装包输出至 `installer_output/AppManagerSetup_<version>.exe`

安装包特性：

- 默认安装路径：`%APPDATA%\AppManager`
- 需要管理员权限（`PrivilegesRequired=admin`）
- 支持 64 位系统（`ArchitecturesInstallIn64BitMode=x64compatible`）
- 包含开始菜单快捷方式
- 支持静默升级安装（同一 AppId 自动覆盖旧版本）

---

## 已知限制与注意事项

- **仅支持 Windows**：ZIP 解压依赖系统 PowerShell，进程检测/终止依赖 Windows API（`TlHelp32.h`），非 Windows 平台仅有编译桩，无实际功能
- **PowerShell 要求**：需要 Windows PowerShell 5.1 及以上（Windows 7 SP1+ 默认自带）；程序依次尝试 `Sysnative`、`System32`、`PATH` 三个位置查找 `powershell.exe`
- **32-bit 进程适配**：以 MinGW 32-bit 编译，`Sysnative` 虚拟目录用于 32-bit 进程访问 64-bit PowerShell，避免 WOW64 文件系统重定向问题
- **ZIP 路径长度**：Windows 默认路径限制为 260 字符，解压路径过长可能导致失败；建议将 AppManager 安装在较短路径下
- **SHA256 校验**：若服务端元数据未提供 `sha256` 字段，则跳过文件完整性校验
- **下载缓存清理**：`download_cache/` 目录不会自动清理旧版本缓存文件，建议在升级完成后或更换服务器时人工清理
- **多应用并发升级**：当前升级流程为顺序执行（逐应用依次升级），不支持并发升级
