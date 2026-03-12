# AppManager 自身隐藏实现说明

## 📋 功能需求

AppManager 是应用启动和管理工具，用于管理其他应用的启动、检测和升级。自身不应该在主页面显示应用图标，以避免混淆。

## ✅ 实现方案

在 `mainwindow.cpp` 中的 4 个关键位置添加了 AppManager 自身的过滤逻辑。

---

## 🔧 代码修改详情

### 1️⃣ **刷新应用图标时跳过 AppManager**

**位置**：`MainWindow::refreshAppIcons()` 函数（~第 478 行）

**修改内容**：
```cpp
// 1) 本地已有的应用（跳过 AppManager 本身）
for (const AppConfig &app : apps) {
    // 跳过 AppManager 自身，不在主页面显示
    if (app.id.toLower() == QStringLiteral("appmanager")
        || app.name.toLower() == QStringLiteral("appmanager")) {
        continue;
    }
    
    // ... 继续加载其他应用的图标
}
```

**说明**：
- 检查应用的 `appId` 和 `appName` 是否为 "AppManager"（不区分大小写）
- 如果是 AppManager，则跳过不添加到列表中
- 其他应用正常显示

---

### 2️⃣ **防止 AppManager 被启动**

**位置**：`MainWindow::launchAppById()` 函数（~第 540 行）

**修改内容**：
```cpp
bool MainWindow::launchAppById(const QString &appId)
{
    const AppConfig app = m_appById.value(appId);
    if (app.id.isEmpty()) {
        return false;
    }

    // 不允许启动 AppManager 本身
    if (app.id.toLower() == QStringLiteral("appmanager")
        || app.name.toLower() == QStringLiteral("appmanager")) {
        logToFile(QStringLiteral("[%1] 该应用不可从本启动台启动，请使用菜单中的更新功能")
                      .arg(app.name));
        return false;
    }
    
    // ... 继续正常启动流程
}
```

**说明**：
- 即使 AppManager 被添加到配置中，也无法通过启动台启动
- 用户会看到日志提示："该应用不可从本启动台启动，请使用菜单中的更新功能"
- AppManager 自身的更新仅通过菜单 → 帮助 → 检查 AppManager 更新 来处理

---

### 3️⃣ **从远程应用列表中过滤 AppManager**

**位置**：`MainWindow::fetchRemoteCatalog()` 函数（~第 705 行）

**修改内容**：
```cpp
void MainWindow::fetchRemoteCatalog()
{
    // ... 获取服务器上的应用清单
    
    QJsonArray catalog = m_service.fetchAppCatalog();
    m_remoteCatalog.clear();
    for (const QJsonValue &v : catalog) {
        QJsonObject item = v.toObject();
        QString catalogAppId = item.value(QStringLiteral("appId")).toString();
        
        // 跳过 AppManager 本身，不在远程列表中显示
        if (catalogAppId.toLower() == QStringLiteral("appmanager")) {
            continue;
        }
        
        if (!m_appById.contains(catalogAppId)) {
            m_remoteCatalog.insert(catalogAppId, item);
        }
    }
    // ...
}
```

**说明**：
- 服务器返回的应用清单中即使包含 AppManager，也会被过滤掉
- 不会在"可下载"列表中显示 AppManager

---

### 4️⃣ **在检查更新时排除 AppManager**

**位置**：`MainWindow::onCheckUpdates()` 函数（~第 650 行）

**修改内容**：
```cpp
void MainWindow::onCheckUpdates()
{
    QVector<AppConfig> apps = m_service.apps();
    
    // 排除 AppManager，只检查其他应用的更新
    QVector<AppConfig> filteredApps;
    for (const AppConfig &app : apps) {
        if (app.id.toLower() != QStringLiteral("appmanager")
            && app.name.toLower() != QStringLiteral("appmanager")) {
            filteredApps.append(app);
        }
    }
    
    if (filteredApps.isEmpty()) {
        logToFile(QStringLiteral("没有可更新的应用"));
        return;
    }
    
    startUpdateWorkflow(filteredApps);
}
```

**说明**：
- "在线检测更新" 按钮仅检查其他应用的更新，不包括 AppManager
- AppManager 的更新检测单独通过菜单进行

---

## 🎯 行为变化总结

| 场景 | 修改前 | 修改后 |
|------|--------|--------|
| 主页面图标列表 | 显示 AppManager | ✅ 隐藏 AppManager |
| 点击应用启动 | 可启动 AppManager | ✅ 阻止，显示提示 |
| "在线检测更新" | 检查所有应用含 AppManager | ✅ 仅检查其他应用 |
| 服务器应用清单 | 显示 AppManager | ✅ 过滤掉 AppManager |
| 菜单→检查 AppManager 更新 | 正常运行 | ✅ 正常运行（独立功能） |

---

## 📝 配置建议

### 情况 1：不在 apps.json 中配置 AppManager（推荐）

此时 AppManager 完全不相关，所有修改都无影响。

```json
{
    "serverBaseUrl": "...",
    "apps": [
        { "id": "cantest", ... },
        { "id": "bctester", ... }
        // AppManager 未配置
    ]
}
```

### 情况 2：在 apps.json 中配置 AppManager

如果需要通过 apps.json 配置 AppManager（比如将来自动化管理），添加以下条目：

```json
{
    "serverBaseUrl": "...",
    "apps": [
        {
            "id": "appmanager",
            "name": "AppManager",
            "exe": "./AppManager.exe",
            "updateMetaUrl": "http://your-server/updates/appmanager.json"
        },
        { "id": "cantest", ... },
        { "id": "bctester", ... }
    ]
}
```

**结果**：
- ✅ AppManager 仍不在主页面显示
- ✅ 用户无法从启动台启动 AppManager
- ✅ "在线检测更新" 仅检查其他应用
- ✅ 菜单→检查 AppManager 更新 独立工作

---

## 🔍 验证方式

### 验证 1：图标不显示
1. 启动 AppManager
2. 查看主页面应用列表
3. 确认不存在 "AppManager" 图标

### 验证 2：无法启动
1. 如果 AppManager 在 apps.json 中，修改其 id 为 "test_appmanager"（绕过过滤）
2. 重启应用
3. 确认可以看到图标但无法启动
4. 改回 id 为 "appmanager"，确认图标消失

### 验证 3：更新检测独立
1. 点击 "在线检测更新" 按钮
2. 检查更新窗口中不包含 AppManager
3. 点击菜单→帮助→检查 AppManager 更新
4. 该功能独立运行，不受影响

---

## 🎨 代码风格说明

所有修改遵循现有代码风格：
- ✅ 使用 `QStringLiteral()` 宏定义字符串
- ✅ 使用 `toLower()` 进行不区分大小写的比较
- ✅ 保持日志记录的一致性
- ✅ 保持缩进和注释风格

---

## 📦 编译状态

- **编译结果**：✅ 成功，无任何警告和错误
- **编译路径**：`build/Desktop_Qt_5_15_2_MinGW_32_bit-Release/`
- **输出文件**：`release/AppManager.exe`

---

## ✨ 总结

通过在 4 个关键函数中添加 AppManager 自身的过滤逻辑，完整实现了需求：

1. **隐藏图标**：AppManager 自身不在主页面显示
2. **防止启动**：即使误操作也无法启动 AppManager
3. **独立更新**：AppManager 的更新通过独立菜单处理
4. **代码质量**：修改简洁，无副作用，易于维护

所有修改都向后兼容，不影响现有功能。
