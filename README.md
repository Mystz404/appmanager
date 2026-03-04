# AppManager（多应用管理工具）

这是一个基于 Qt Widgets 的多应用管理器，适用于“同目录下多个 APP”的统一运维场景。

## 已实现功能

1. **多应用配置化管理**
   - 从 `apps.json` 读取应用定义。
   - 支持统一根目录 `appsRoot`，每个应用使用相对路径配置。
   - 支持统一版本仓文件 `versionStoreFile`，通过每个应用的 `versionKey` 区分版本。

2. **必要文件检测**
   - 检查每个应用的可执行文件、自定义必需文件，以及统一版本仓文件是否存在。
   - 检测结果在表格和日志中显示。

3. **在线检测升级**
   - 按应用 `updateMetaUrl` 请求在线元数据。
   - 比较本地版本与远端版本（语义化版本比较，如 `1.2.10` > `1.2.9`）。

4. **批量升级能力**
   - 支持“升级选中”与“升级全部”。
   - 升级流程：下载 → SHA256 校验（可选）→ 备份旧版本 → 替换 EXE → 写入新版本号。

5. **快捷启动与目录打开**
   - 可快速启动选中的应用。
   - 支持打开应用目录用于人工排障。

6. **管理界面**
   - 提供配置路径、操作按钮、应用状态表格、日志窗口。

## 配置文件格式（apps.json）

```json
{
  "appsRoot": ".",
   "versionStoreFile": "versions.json",
  "apps": [
    {
      "id": "demo_app_a",
      "name": "演示应用A",
      "exe": "managed_apps/AppA/AppA.exe",
         "versionKey": "demo_app_a",
      "requiredFiles": [
        "managed_apps/AppA/config.ini",
        "managed_apps/AppA/data/startup.dat"
      ],
      "updateMetaUrl": "https://example.com/updates/appa.json"
    }
  ]
}
```

- `appsRoot`：应用根目录；可写绝对路径，也可写相对 `apps.json` 的路径。
- `versionStoreFile`：统一版本仓 JSON 文件路径（相对 `appsRoot`，默认 `versions.json`）。
- `exe`：应用主程序路径（相对 `appsRoot`）。
- `versionKey`：在统一版本仓中的字段名；若不填默认使用 `id`。
- `requiredFiles`：启动前必须存在的文件列表。
- `updateMetaUrl`：在线元数据地址。

统一版本仓示例（`versions.json`）：

```json
{
   "demo_app_a": "1.2.3",
   "demo_app_b": "2.0.1"
}
```

## 在线元数据格式

每个应用的 `updateMetaUrl` 需要返回 JSON：

```json
{
  "latestVersion": "1.3.0",
  "downloadUrl": "https://example.com/download/AppA.exe",
   "sha256": "可选，升级包二进制的sha256十六进制小写",
   "packageType": "exe 或 zip，可选，默认 exe",
   "installRelativeDir": "仅 zip 时可选，解压覆盖目标目录（相对 appsRoot）"
}
```

- `packageType=exe`：按“替换可执行文件”升级；
- `packageType=zip`：下载 ZIP 后自动解压覆盖到目标目录（Windows 使用 PowerShell `Expand-Archive`）。

## 使用流程

1. 准备并维护 `apps.json`。
2. 准备统一版本仓文件（默认 `versions.json`），并为每个应用提供 `versionKey`。
3. 启动本工具后点击“在线检测更新”。
4. 根据结果点击“升级选中”或“升级全部”。
5. 点击“快捷启动选中”验证应用启动。

## 说明

- 升级为“替换 EXE”策略，适合单文件可执行程序；
- 若目标 EXE 被占用（仍在运行），会提示关闭后重试；
- 失败时保留 `.bak` 备份文件，便于回滚。
