#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "versionutils.h"
#include "updatedialog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFileIconProvider>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressDialog>
#include <QAbstractItemView>
#include <QStyledItemDelegate>
#include <QSettings>
#include <QTimer>

namespace {
constexpr int kProgressDialogWidth = 540;
constexpr int kProgressBarWidth = 420;
constexpr int kProgressBarHeight = 18;
constexpr qint64 kStartupAppCheckCooldownSec = 16 * 60 * 60; // 16 小时

void setupUnifiedProgressDialog(QProgressDialog &dlg, const QString &labelText, bool indeterminate)
{
    dlg.setLabelText(labelText);
    dlg.setWindowModality(Qt::NonModal);
    dlg.setCancelButton(nullptr);
    dlg.setAutoClose(true);
    dlg.setAutoReset(true);
    dlg.setMinimumDuration(0);
    dlg.setMinimumWidth(kProgressDialogWidth);
    dlg.setStyleSheet(QStringLiteral(
        "QProgressDialog { background: #f8fafc; }"
        "QLabel { color: #1f2937; min-width: %1px; }"
        "QProgressBar { min-width: %2px; max-width: %2px; min-height: %3px; border: 1px solid #cbd5e1; border-radius: 5px; background: #ffffff; text-align: center; }"
        "QProgressBar::chunk { background: #2563eb; border-radius: 4px; }")
            .arg(kProgressBarWidth)
            .arg(kProgressBarWidth)
            .arg(kProgressBarHeight));

    if (indeterminate) {
        dlg.setRange(0, 0);
    } else {
        dlg.setRange(0, 100);
        dlg.setValue(0);
    }
}
}

#ifdef Q_OS_WIN
#include <Windows.h>
#include <TlHelp32.h>

struct BringToFrontData {
    DWORD pid;
    HWND  hwnd;
};

static BOOL CALLBACK enumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    auto *d = reinterpret_cast<BringToFrontData *>(lParam);
    DWORD wndPid = 0;
    GetWindowThreadProcessId(hwnd, &wndPid);
    if (wndPid == d->pid && IsWindowVisible(hwnd)) {
        d->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

static QString compactStatusText(const QString &text, int maxLen = 120)
{
    QString oneLine = text;
    oneLine.replace('\n', ' ');
    oneLine = oneLine.simplified();
    if (oneLine.size() <= maxLen) {
        return oneLine;
    }
    const int half = (maxLen - 5) / 2;
    return oneLine.left(half) + QStringLiteral(" ... ") + oneLine.right(half);
}

static QString normalizeWindowsPath(const QString &path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(path)).toLower();
}

static QString processImagePathByPid(DWORD pid)
{
    HANDLE modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (modSnap == INVALID_HANDLE_VALUE) {
        return {};
    }

    QString result;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(modSnap, &me)) {
        result = QString::fromWCharArray(me.szExePath);
    }

    CloseHandle(modSnap);
    return result;
}

static DWORD findRunningProcessIdByPath(const QString &targetExePath)
{
    const QString targetNorm = normalizeWindowsPath(QFileInfo(targetExePath).absoluteFilePath());
    const QString targetExeName = QFileInfo(targetNorm).fileName();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD foundPid = 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString runningExeName = QString::fromWCharArray(pe.szExeFile);
            if (runningExeName.compare(targetExeName, Qt::CaseInsensitive) != 0) {
                continue;
            }

            const QString runningPath = normalizeWindowsPath(processImagePathByPid(pe.th32ProcessID));
            if (!runningPath.isEmpty() && runningPath == targetNorm) {
                foundPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return foundPid;
}
#endif

// ============================================================
// 自定义列表项代理（卡片布局）
// ============================================================
class AppItemDelegate : public QStyledItemDelegate
{
public:
    explicit AppItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(140, 195);
    }

    // 返回 … 按鈕在视口坐标系中的区域
    static QRect moreButtonRect(const QRect &itemRect)
    {
        return QRect(itemRect.right() - 36, itemRect.top() + 6, 32, 18);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect r = option.rect.adjusted(2, 2, -2, -2);
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered  = (option.state & QStyle::State_MouseOver) != 0;

        // 卡片背景
        QColor bgColor     = selected ? QColor(219, 234, 254)
                                      : (hovered ? QColor(239, 246, 255) : Qt::white);
        QColor borderColor = selected ? QColor(59, 130, 246)
                                      : (hovered ? QColor(147, 197, 253) : QColor(226, 232, 240));
        painter->setPen(QPen(borderColor, 1));
        painter->setBrush(bgColor);
        painter->drawRoundedRect(r, 10, 10);

        // 图标
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const int iconSize = 64;
        const QRect iconRect(r.left() + (r.width() - iconSize) / 2, r.top() + 14, iconSize, iconSize);
        if (!icon.isNull()) {
            icon.paint(painter, iconRect);
        }

        // 应用名称
        const QString name    = index.data(Qt::DisplayRole).toString();
        const QString version = index.data(Qt::UserRole + 2).toString();
        const QString status  = index.data(Qt::UserRole + 3).toString();
        const bool hasUpdate  = index.data(Qt::UserRole + 4).toBool();

        {
            QFont nf;
            nf.setPixelSize(14);
            painter->setFont(nf);
            painter->setPen(selected ? QColor(30, 58, 138) : QColor(31, 41, 55));
            const QRect nameRect(r.left() + 4, r.top() + 84, r.width() - 8, 44);
            painter->drawText(nameRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, name);
        }

        // 版本号（小字）
        if (!version.isEmpty()) {
            QFont vf;
            vf.setPixelSize(12);
            painter->setFont(vf);
            painter->setPen(QColor(107, 114, 128));
            const QRect vRect(r.left() + 4, r.top() + 130, r.width() - 8, 18);
            painter->drawText(vRect, Qt::AlignHCenter | Qt::AlignVCenter, version);
        }

        // 状态徽章（右下角）
        if (!status.isEmpty()) {
            QColor badgeFg, badgeBg;
            if (hasUpdate) {
                badgeFg = QColor(146, 64, 14);
                badgeBg = QColor(254, 243, 199);
            } else if (status == QStringLiteral("下载")) {
                badgeFg = QColor(30, 64, 175);
                badgeBg = QColor(219, 234, 254);
            } else {
                // 就绪状态不显示徽章
                goto draw_more_btn;
            }
            {
                QFont bf;
                bf.setPixelSize(11);
                painter->setFont(bf);
                QFontMetrics fm(bf);
                const int badgeW = fm.horizontalAdvance(status) + 10;
                const int badgeH = 16;
                const QRect badgeRect(r.right() - badgeW - 3, r.bottom() - badgeH - 3, badgeW, badgeH);
                painter->setPen(Qt::NoPen);
                painter->setBrush(badgeBg);
                painter->drawRoundedRect(badgeRect, 7, 7);
                painter->setPen(badgeFg);
                painter->drawText(badgeRect, Qt::AlignCenter, status);
            }
        }

        draw_more_btn:
        // … 按鈕（右上角）
        {
            const QRect mbr = moreButtonRect(option.rect);
            painter->setPen(Qt::NoPen);
            if (hovered || selected) {
                painter->setBrush(QColor(186, 199, 214));
            } else {
                painter->setBrush(QColor(220, 228, 238));
            }
            painter->drawRoundedRect(mbr, 5, 5);
            QFont mf;
            mf.setPixelSize(8);
            mf.setBold(true);
            painter->setFont(mf);
            painter->setPen(QColor(255, 255, 255));
            painter->drawText(mbr, Qt::AlignCenter, QStringLiteral("···"));
        }

        painter->restore();
    }
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setWindowTitle(QStringLiteral("APP 管理器"));
    resize(1000, 680);

    setupUiWidgets();
    applySimpleStyle();
    connectSignals();

    // 配置文件固定为程序根目录下 apps.json，无需手动选择。
    m_configPath = QCoreApplication::applicationDirPath() + QStringLiteral("/apps.json");
    loadConfig();

    // 初始化状态栏提示
    if (statusBar()) {
        statusBar()->showMessage(QStringLiteral("正在连接服务器..."));
    }

    // 延迟检测服务器连接状态
    QTimer::singleShot(500, this, &MainWindow::checkServerConnection);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setupUiWidgets()
{
    QWidget *root = ui->centralwidget;
    auto *mainLayout = new QVBoxLayout(root);
    mainLayout->setContentsMargins(18, 14, 18, 14);
    mainLayout->setSpacing(12);

    m_titleLabel = new QLabel(QStringLiteral("应用启动台"), root);
    m_titleLabel->setObjectName(QStringLiteral("titleLabel"));

    auto *buttonLayout = new QHBoxLayout();
    m_checkUpdatesButton = new QPushButton(QStringLiteral("检测应用更新"), root);

    buttonLayout->addWidget(m_checkUpdatesButton);
    buttonLayout->addStretch();

    m_appList = new QListWidget(root);
    m_appList->setViewMode(QListView::IconMode);
    m_appList->setIconSize(QSize(72, 72));
    m_appList->setResizeMode(QListView::Adjust);
    m_appList->setMovement(QListView::Static);
    m_appList->setGridSize(QSize(152, 208));
    m_appList->setSpacing(6);
    m_appList->setWordWrap(true);
    m_appList->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *hintLabel = new QLabel(QStringLiteral("点击图标启动应用"), root);
    hintLabel->setObjectName(QStringLiteral("hintLabel"));

    // 日志面板
    auto *logLabel = new QLabel(QStringLiteral("操作日志"), root);
    logLabel->setObjectName(QStringLiteral("logLabel"));

    m_logView = new QPlainTextEdit(root);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(3000);
    m_logView->setPlaceholderText(QStringLiteral("暂无日志"));
    m_logView->setObjectName(QStringLiteral("logView"));

    mainLayout->addWidget(m_titleLabel);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(hintLabel);
    mainLayout->addWidget(m_appList, 3);
    mainLayout->addWidget(logLabel);
    mainLayout->addWidget(m_logView, 1);

    // 自定义代理 + 禁用右键菜单（改用 ··· 按鈕）
    m_appList->setItemDelegate(new AppItemDelegate(m_appList));
    m_appList->setContextMenuPolicy(Qt::NoContextMenu);
    m_appList->viewport()->installEventFilter(this);

    // ==================== 创建菜单栏 ====================
    QMenuBar *menuBar = new QMenuBar(this);
    this->setMenuBar(menuBar);

    QMenu *helpMenu = menuBar->addMenu(QStringLiteral("帮助(&H)"));
    QAction *checkUpdateAction = helpMenu->addAction(QStringLiteral("检查 AppManager 更新(&U)"));
    checkUpdateAction->setToolTip(QStringLiteral("检查 AppManager 是否有新版本"));
    connect(checkUpdateAction, &QAction::triggered, this, &MainWindow::onCheckAppManagerUpdate);

    helpMenu->addSeparator();

    QAction *aboutAction = helpMenu->addAction(QStringLiteral("关于 AppManager(&A)"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutAppManager);
}

void MainWindow::connectSignals()
{
    connect(m_checkUpdatesButton, &QPushButton::clicked, this, &MainWindow::onCheckUpdates);
    connect(m_appList, &QListWidget::itemClicked, this, &MainWindow::onAppIconClicked);
}

void MainWindow::applySimpleStyle()
{
    qApp->setStyleSheet(
        QStringLiteral(
            "QWidget { background: #f5f7fb; color: #1f2937; }"
            "QLabel#titleLabel { font-size: 24px; font-weight: 700; }"
            "QLabel#hintLabel { color: #6b7280; font-size: 13px; }"
            "QPushButton {"
            "  background: #2563eb; color: white; border: none; border-radius: 8px;"
            "  padding: 8px 16px; font-size: 14px; font-weight: 600;"
            "}"
            "QPushButton:hover { background: #1d4ed8; }"
            "QPushButton:pressed { background: #1e40af; }"
            "QListWidget {"
            "  background: #f1f5f9; border: 1px solid #dbe2ea; border-radius: 10px;"
            "  padding: 6px;"
            "}"
            "QListWidget::item {"
            "  border-radius: 10px; padding: 4px 2px;"
            "  border: 1px solid #e2e8f0;"
            "  background: #ffffff;"
            "}"
            "QListWidget::item:hover {"
            "  background: #eff6ff;"
            "  border: 1px solid #93c5fd;"
            "}"
            "QListWidget::item:selected { background: #dbeafe; color: #1e3a8a; border: 1px solid #3b82f6; }"
            "QLabel#logLabel { font-size: 14px; font-weight: 600; color: #374151; }"
            "QPlainTextEdit#logView {"
            "  background: #1e293b; color: #a3e635;"
            "  font-family: Consolas, 'Courier New', monospace; font-size: 12px;"
            "  border: 1px solid #334155; border-radius: 6px;"
            "}"));
}

QString MainWindow::logFilePath() const
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/appmanager.log"));
}

void MainWindow::logToFile(const QString &message)
{
    const QFileInfo fileInfo(logFilePath());
    QDir().mkpath(fileInfo.absolutePath());

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream out(&file);
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    out << QStringLiteral("[%1] %2\n").arg(ts, message);
    file.close();

    appendLog(QStringLiteral("[%1] %2").arg(ts, message));
}

void MainWindow::appendLog(const QString &message)
{
    if (!m_logView) {
        return;
    }
    m_logView->appendPlainText(message);
}

void MainWindow::loadConfig()
{
    QString errorMessage;
    if (!m_service.loadConfig(m_configPath, errorMessage)) {
        logToFile(QStringLiteral("配置加载失败: %1").arg(errorMessage));
        return;
    }

    m_onlineCache.clear();
    m_appById.clear();
    for (const AppConfig &app : m_service.apps()) {
        m_appById.insert(app.id, app);
    }

    validateLocalApps();

    refreshAppIcons();
    logToFile(QStringLiteral("配置加载成功，共 %1 个应用，根目录: %2")
                  .arg(m_service.apps().size())
                  .arg(QDir::toNativeSeparators(m_service.appsRoot())));
}

void MainWindow::validateLocalApps()
{
    QStringList toRemove;
    for (auto it = m_appById.constBegin(); it != m_appById.constEnd(); ++it) {
        const AppConfig &app = it.value();
        const QString exePath = m_service.appAbsoluteExePath(app);
        if (!QFileInfo::exists(exePath)) {
            toRemove.append(app.id);
            logToFile(QStringLiteral("[%1] 本地文件不存在，已从配置移除: %2")
                          .arg(app.name, QDir::toNativeSeparators(exePath)));
        }
    }

    if (toRemove.isEmpty()) {
        return;
    }

    for (const QString &id : toRemove) {
        m_service.removeAppEntry(id);
        m_appById.remove(id);
        m_onlineCache.remove(id);
    }

    QString err;
    if (!m_service.saveConfig(err)) {
        logToFile(QStringLiteral("清理不存在的应用后保存配置失败: %1").arg(err));
    }

    // 有应用被移除，重新获取服务器清单
    fetchRemoteCatalog();

    refreshAppIcons();
}

void MainWindow::refreshAppIcons()
{
    const QVector<AppConfig> apps = m_service.apps();
    m_appList->clear();
    QFileIconProvider provider;

    // 1) 本地已有的应用（跳过 AppManager 本身）
    for (const AppConfig &app : apps) {
        // 跳过 AppManager 自身，不在主页面显示
        if (app.id.toLower() == QStringLiteral("appmanager")
            || app.name.toLower() == QStringLiteral("appmanager")) {
            continue;
        }

        const QString exePath = m_service.appAbsoluteExePath(app);
        QIcon icon = provider.icon(QFileInfo(exePath));
        if (icon.isNull()) {
            icon = style()->standardIcon(QStyle::SP_DesktopIcon);
        }

        const QString currentVersion = m_service.appCurrentVersion(app);
        const OnlineAppInfo online = m_onlineCache.value(app.id);
        const bool hasUpdate = online.requestSuccess
                               && compareVersions(currentVersion, online.latestVersion) < 0;
        const QString status = hasUpdate ? QStringLiteral("可升级") : QString();

        auto *item = new QListWidgetItem(icon, app.name);
        item->setData(Qt::UserRole,     app.id);
        item->setData(Qt::UserRole + 1, false);                      // isRemote
        item->setData(Qt::UserRole + 2, QStringLiteral("V%1").arg(currentVersion)); // version
        item->setData(Qt::UserRole + 3, status);                     // status badge
        item->setData(Qt::UserRole + 4, hasUpdate);                  // hasUpdate
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(120, 160));
        if (hasUpdate) {
            item->setToolTip(QStringLiteral("当前: V%1，最新: V%2")
                                 .arg(currentVersion, online.latestVersion));
        } else {
            item->setToolTip(QDir::toNativeSeparators(exePath));
        }
        m_appList->addItem(item);
    }

    // 2) 服务器上本地不存在的应用
    for (auto it = m_remoteCatalog.constBegin(); it != m_remoteCatalog.constEnd(); ++it) {
        const QJsonObject &info = it.value();
        QIcon icon = createDownloadIcon(96);

        const QString appName = info.value(QStringLiteral("appName")).toString();
        const QString version = info.value(QStringLiteral("latestVersion")).toString();

        auto *item = new QListWidgetItem(icon, appName);
        item->setData(Qt::UserRole,     it.key());
        item->setData(Qt::UserRole + 1, true);                              // isRemote
        item->setData(Qt::UserRole + 2, QStringLiteral("V%1").arg(version)); // version
        item->setData(Qt::UserRole + 3, QStringLiteral("下载"));              // status badge
        item->setData(Qt::UserRole + 4, false);                             // hasUpdate
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(120, 160));
        item->setToolTip(QStringLiteral("点击下载该应用"));
        m_appList->addItem(item);
    }
}

bool MainWindow::launchAppById(const QString &appId)
{
    const AppConfig app = m_appById.value(appId);
    if (app.id.isEmpty()) {
        return false;
    }

    // 不允许启动 AppManager 本身
    if (app.id.toLower() == QStringLiteral("appmanager")
        || app.name.toLower() == QStringLiteral("appmanager")) {
        logToFile(QStringLiteral("[%1] 该应用不可从本启动台启动，请使用菜单中的更新功能").arg(app.name));
        return false;
    }

    QStringList missing;
    if (!m_service.checkRequiredFiles(app, missing)) {
        logToFile(QStringLiteral("[%1] 启动失败，缺失文件: %2").arg(app.name, missing.join(QStringLiteral(", "))));
        return false;
    }

    const QString exePath = m_service.appAbsoluteExePath(app);

#ifdef Q_OS_WIN
    // 检查“同完整路径 EXE”是否已在运行，避免仅按文件名导致误判
    const DWORD targetPid = findRunningProcessIdByPath(exePath);

    if (targetPid != 0) {
        // 已运行，枚举窗口并调到前台
        BringToFrontData data = { targetPid, nullptr };
        EnumWindows(enumWindowsCallback, reinterpret_cast<LPARAM>(&data));

        if (data.hwnd) {
            if (IsIconic(data.hwnd)) {
                ShowWindow(data.hwnd, SW_RESTORE);
            }
            SetForegroundWindow(data.hwnd);
            logToFile(QStringLiteral("[%1] 已在运行，已将窗口调到前台").arg(app.name));
            return true;
        }

        // 同路径进程存在但未找到可见窗口，尝试再次拉起
        const bool restarted = QProcess::startDetached(exePath, {});
        logToFile(QStringLiteral("[%1] %2")
                      .arg(app.name,
                           restarted ? QStringLiteral("检测到同路径进程但无可见窗口，已尝试重新启动")
                                     : QStringLiteral("检测到同路径进程且无可见窗口，重新启动失败")));
        return restarted;
    }
#endif

    const bool started = QProcess::startDetached(exePath, {});
    logToFile(QStringLiteral("[%1] %2").arg(app.name, started ? QStringLiteral("启动成功") : QStringLiteral("启动失败")));
    return started;
}

void MainWindow::startUpdateWorkflow(const QVector<AppConfig> &apps)
{
    if (apps.isEmpty()) {
        return;
    }

    auto *dlg = new UpdateDialog(&m_service, apps, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setModal(false);
    dlg->setWindowModality(Qt::NonModal);
    connect(dlg, &UpdateDialog::logMessage, this, &MainWindow::logToFile);

    connect(dlg, &QDialog::finished, this, [this, dlg, apps](int) {
        const QHash<QString, OnlineAppInfo> cache = dlg->onlineCache();
        for (auto it = cache.constBegin(); it != cache.constEnd(); ++it) {
            m_onlineCache.insert(it.key(), it.value());
        }

        const int total = apps.size();
        const int success = dlg->serverSuccessCount();
        if (statusBar()) {
            if (success == total) {
                statusBar()->showMessage(QStringLiteral("已成功连接至服务器，全部应用在线检测通过"), 8000);
            } else if (success > 0) {
                statusBar()->showMessage(QStringLiteral("部分应用连接服务器成功，%1 个失败").arg(total - success), 8000);
            } else {
                statusBar()->showMessage(QStringLiteral("无法连接服务器，全部应用检测失败"), 8000);
            }
        }

        refreshAppIcons();
        fetchRemoteCatalog();
        refreshAppIcons();
    });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

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

void MainWindow::onAppIconClicked(QListWidgetItem *item)
{
    if (item == nullptr) {
        return;
    }

    const QString appId = item->data(Qt::UserRole).toString();
    const bool isRemote = item->data(Qt::UserRole + 1).toBool();

    if (isRemote) {
        onDownloadRemoteApp(appId);
    } else {
        launchAppById(appId);
    }
}

// ============================================================
// 服务器连接检测
// ============================================================

void MainWindow::checkServerConnection()
{
    if (m_service.serverBaseUrl().isEmpty()) {
        if (statusBar()) {
            statusBar()->showMessage(QStringLiteral("未配置服务器地址"));
        }
        return;
    }

    m_serverConnected = m_service.tryConnectServer(3000);
    if (statusBar()) {
        if (m_serverConnected) {
            statusBar()->showMessage(QStringLiteral("已连接服务器"));
        } else {
            statusBar()->showMessage(QStringLiteral("无法连接服务器"));
        }
    }

    // 连接成功后获取应用清单
    if (m_serverConnected) {
        // 启动时静默检测 AppManager 自更新（仅执行一次）
        if (!m_startupAutoUpdateChecked) {
            QTimer::singleShot(0, this, &MainWindow::trySilentAppManagerAutoUpdate);
        }
        // 启动时低频检测其他应用更新（串行抽样，降低并发和请求量）
        if (!m_startupAppsUpdateCheckScheduled) {
            m_startupAppsUpdateCheckScheduled = true;
            QTimer::singleShot(1200, this, &MainWindow::tryStartupAppsUpdateCheck);
        }
        fetchRemoteCatalog();
        refreshAppIcons();
    }
}

void MainWindow::trySilentAppManagerAutoUpdate()
{
    if (m_startupAutoUpdateChecked || m_startupAutoUpdateRunning) {
        return;
    }
    if (!m_serverConnected) {
        return;
    }

    m_startupAutoUpdateChecked = true;
    m_startupAutoUpdateRunning = true;

    appendLog(QStringLiteral("启动时静默检测 AppManager 更新..."));

    const OnlineAppInfo online = m_service.checkAppManagerUpdate();
    if (!online.requestSuccess) {
        appendLog(QStringLiteral("启动静默检测失败: %1").arg(online.errorMessage));
        m_startupAutoUpdateRunning = false;
        return;
    }

    const QString currentVersion = m_service.appManagerVersion();
    const QString latestVersion = online.latestVersion;
    if (compareVersions(currentVersion, latestVersion) >= 0) {
        appendLog(QStringLiteral("启动静默检测完成：AppManager 已是最新版本(v%1)").arg(currentVersion));
        m_startupAutoUpdateRunning = false;
        return;
    }

    appendLog(QStringLiteral("检测到 AppManager 新版本: v%1 -> v%2，开始静默升级")
                  .arg(currentVersion, latestVersion));
    if (statusBar()) {
        statusBar()->showMessage(QStringLiteral("检测到 AppManager 新版本，正在后台升级..."));
    }

    QString result;
    const bool ok = m_service.upgradeAppManager(
        online,
        result,
        30000,
        [&](qint64 received, qint64 total) {
            Q_UNUSED(received)
            Q_UNUSED(total)
            QApplication::processEvents();
        },
        [&](const QString &status) {
            if (statusBar()) {
                statusBar()->showMessage(compactStatusText(QStringLiteral("[AppManager] %1").arg(status), 120));
            }
            QApplication::processEvents();
        });

    m_startupAutoUpdateRunning = false;

    if (!ok) {
        appendLog(QStringLiteral("AppManager 启动静默升级失败: %1").arg(result));
        if (statusBar()) {
            statusBar()->showMessage(QStringLiteral("AppManager 静默升级失败"), 5000);
        }
        return;
    }

    appendLog(QStringLiteral("AppManager 启动静默升级已启动: %1").arg(result));
    if (statusBar()) {
        statusBar()->showMessage(QStringLiteral("AppManager 正在升级，即将重启..."));
    }

    // 给用户与日志系统留出短暂缓冲，随后退出由安装程序接管。
    QTimer::singleShot(1500, this, [this]() {
        QApplication::quit();
    });
}

void MainWindow::tryStartupAppsUpdateCheck()
{
    if (!m_serverConnected) {
        return;
    }

    const QString org = QCoreApplication::organizationName().isEmpty()
                            ? QStringLiteral("AppManager")
                            : QCoreApplication::organizationName();
    const QString appName = QCoreApplication::applicationName().isEmpty()
                                ? QStringLiteral("AppManager")
                                : QCoreApplication::applicationName();
    QSettings settings(org, appName);
    settings.beginGroup(QStringLiteral("startup_other_app_update_check"));

    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    const qint64 lastCheckSec = settings.value(QStringLiteral("lastCheckEpoch"), 0).toLongLong();
    if (lastCheckSec > 0 && (nowSec - lastCheckSec) < kStartupAppCheckCooldownSec) {
        const qint64 remainMin = (kStartupAppCheckCooldownSec - (nowSec - lastCheckSec) + 59) / 60;
        appendLog(QStringLiteral("启动更新检测已跳过（冷却中，约 %1 分钟后可再次检测）").arg(remainMin));
        settings.endGroup();
        return;
    }

    QVector<AppConfig> candidates;
    for (const AppConfig &app : m_service.apps()) {
        if (app.id.compare(QStringLiteral("appmanager"), Qt::CaseInsensitive) == 0
            || app.name.compare(QStringLiteral("appmanager"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        if (!app.updateMetaUrl.isValid() || app.updateMetaUrl.isEmpty()) {
            continue;
        }
        candidates.push_back(app);
    }

    if (candidates.isEmpty()) {
        appendLog(QStringLiteral("启动更新检测：无可检测应用"));
        settings.setValue(QStringLiteral("lastCheckEpoch"), nowSec);
        settings.endGroup();
        return;
    }

    int cursor = settings.value(QStringLiteral("cursor"), 0).toInt();
    if (cursor < 0) {
        cursor = 0;
    }
    const int total = candidates.size();
    const int batch = total;

    int checkedCount = 0;
    int updateCount = 0;
    int failCount = 0;

    appendLog(QStringLiteral("启动更新检测：本次串行检测 %1 个应用").arg(total));

    for (int i = 0; i < batch; ++i) {
        const int idx = (cursor + i) % total;
        const AppConfig &app = candidates.at(idx);

        const OnlineAppInfo online = m_service.checkOnlineInfo(app);
        ++checkedCount;

        if (!online.requestSuccess) {
            ++failCount;
            appendLog(QStringLiteral("[%1] 启动检测失败: %2").arg(app.name, online.errorMessage));
            continue;
        }

        m_onlineCache.insert(app.id, online);
        const QString currentVersion = m_service.appCurrentVersion(app);
        if (compareVersions(currentVersion, online.latestVersion) < 0) {
            ++updateCount;
            appendLog(QStringLiteral("[%1] 启动检测发现更新: %2 -> %3")
                          .arg(app.name, currentVersion, online.latestVersion));
        }
    }

    settings.setValue(QStringLiteral("lastCheckEpoch"), nowSec);
    settings.setValue(QStringLiteral("cursor"), (cursor + checkedCount) % qMax(total, 1));
    settings.endGroup();

    if (statusBar()) {
        statusBar()->showMessage(
            QStringLiteral("启动检测完成：抽样%1个，发现更新%2个，失败%3个")
                .arg(checkedCount)
                .arg(updateCount)
                .arg(failCount),
            7000);
    }

    refreshAppIcons();
}

void MainWindow::fetchRemoteCatalog()
{
    if (!m_serverConnected) {
        m_serverConnected = m_service.tryConnectServer(3000);
        if (!m_serverConnected) {
            return;
        }
    }

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
    if (!m_remoteCatalog.isEmpty()) {
        logToFile(QStringLiteral("有 %1 个应用未安装").arg(m_remoteCatalog.size()));
    }
}

// ============================================================
// 视口事件过滤：拦截 ··· 按钮点击
// ============================================================

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_appList->viewport() && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QModelIndex idx = m_appList->indexAt(me->pos());
            if (idx.isValid()) {
                const QRect itemRect = m_appList->visualRect(idx);
                if (AppItemDelegate::moreButtonRect(itemRect).contains(me->pos())) {
                    const QString appId = idx.data(Qt::UserRole).toString();
                    showAppContextMenu(appId, m_appList->viewport()->mapToGlobal(me->pos()));
                    return true; // 消耗事件，阻止 itemClicked 触发
                }
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ============================================================
// 应用上下文菜单
// ============================================================

void MainWindow::showAppContextMenu(const QString &appId, const QPoint &globalPos)
{
    const bool isRemote = !m_appById.contains(appId);

    QMenu menu(this);

    if (isRemote) {
        menu.addAction(QStringLiteral("下载应用"), this, [this, appId]() {
            onDownloadRemoteApp(appId);
        });
    } else {
        menu.addAction(QStringLiteral("打开应用"), this, [this, appId]() {
            launchAppById(appId);
        });
        menu.addAction(QStringLiteral("打开文件位置"), this, [this, appId]() {
            onOpenAppLocation(appId);
        });

        // 有可用升级时显示升级选项
        const OnlineAppInfo online = m_onlineCache.value(appId);
        const QString currentVersion = m_service.appCurrentVersion(m_appById.value(appId));
        if (online.requestSuccess && compareVersions(currentVersion, online.latestVersion) < 0) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("升级到 V%1").arg(online.latestVersion), this, [this, appId]() {
                const QVector<AppConfig> singleApp = { m_appById.value(appId) };
                startUpdateWorkflow(singleApp);
            });
        }

        menu.addSeparator();

        menu.addAction(QStringLiteral("下载历史版本"), this, [this, appId]() {
            onDownloadHistoryVersion(appId);
        });

        menu.addSeparator();

        menu.addAction(QStringLiteral("删除应用"), this, [this, appId]() {
            onDeleteApp(appId);
        });
    }

    menu.exec(globalPos);
}

// ============================================================
// 打开文件位置
// ============================================================

void MainWindow::onOpenAppLocation(const QString &appId)
{
    const AppConfig app = m_appById.value(appId);
    if (app.id.isEmpty()) {
        return;
    }

    const QString exePath = m_service.appAbsoluteExePath(app);
    const QString appDir = m_service.appAbsoluteDir(app);

#ifdef Q_OS_WIN
    bool opened = false;
    if (QFileInfo::exists(exePath)) {
        opened = QProcess::startDetached(
            QStringLiteral("explorer.exe"),
            { QStringLiteral("/select,"), QDir::toNativeSeparators(exePath) });
    }
    if (!opened) {
        opened = QProcess::startDetached(
            QStringLiteral("explorer.exe"),
            { QDir::toNativeSeparators(appDir) });
    }
#else
    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(appDir));
#endif

    if (!opened) {
        logToFile(QStringLiteral("[%1] 打开文件位置失败: %2")
                      .arg(app.name, QDir::toNativeSeparators(appDir)));
    }
}

// ============================================================
// 删除应用
// ============================================================

void MainWindow::onDeleteApp(const QString &appId)
{
    const AppConfig app = m_appById.value(appId);
    if (app.id.isEmpty()) {
        return;
    }

    const QString exePath = m_service.appAbsoluteExePath(app);
    const QString appDir  = m_service.appAbsoluteDir(app);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(QStringLiteral("确认删除"));
    msgBox.setText(QStringLiteral("确定要删除应用「%1」吗？\n\n将关闭正在运行的进程并删除磁盘文件。").arg(app.name));
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    msgBox.button(QMessageBox::Ok)->setText(QStringLiteral("确定"));
    msgBox.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    msgBox.setDefaultButton(QMessageBox::Cancel);

    if (msgBox.exec() != QMessageBox::Ok) {
        return;
    }

    const bool deleteFiles = true;

    // 1) 关闭正在运行的进程
#ifdef Q_OS_WIN
    const QString exeName = QFileInfo(exePath).fileName();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (exeName.compare(QString::fromWCharArray(pe.szExeFile), Qt::CaseInsensitive) == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        WaitForSingleObject(hProc, 3000);
                        CloseHandle(hProc);
                        logToFile(QStringLiteral("[%1] 已关闭运行中的进程").arg(app.name));
                    }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
#endif

    // 2) 删除磁盘文件
    if (deleteFiles) {
        // 仅删除当前应用 EXE，避免共享目录下误删其他应用文件。
        if (QFileInfo::exists(exePath)) {
            if (QFile::remove(exePath)) {
                logToFile(QStringLiteral("[%1] 已删除可执行文件: %2")
                              .arg(app.name, QDir::toNativeSeparators(exePath)));
            } else {
                logToFile(QStringLiteral("[%1] 可执行文件删除失败: %2")
                              .arg(app.name, QDir::toNativeSeparators(exePath)));
            }
        }

        // 仅在目录未被其他应用共享且已为空时，尝试清理空目录。
        const QString appsRoot = QDir::cleanPath(m_service.appsRoot());
        const QString cleanDir = QDir::cleanPath(appDir);
        bool usedByOtherApps = false;
        for (auto it = m_appById.constBegin(); it != m_appById.constEnd(); ++it) {
            if (it.key() == appId) {
                continue;
            }
            const QString otherDir = QDir::cleanPath(m_service.appAbsoluteDir(it.value()));
            if (otherDir == cleanDir) {
                usedByOtherApps = true;
                break;
            }
        }

        if (!usedByOtherApps
            && !cleanDir.isEmpty()
            && cleanDir != appsRoot
            && QDir(cleanDir).exists()) {
            QDir dir(cleanDir);
            if (dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
                QString currentDir = cleanDir;
                while (!currentDir.isEmpty() && currentDir != appsRoot) {
                    QDir d(currentDir);
                    if (!d.exists() || !d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
                        break;
                    }
                    const QString parentDir = QFileInfo(currentDir).absolutePath();
                    QDir().rmdir(currentDir);
                    currentDir = parentDir;
                }
            }
        }
    }

    // 3) 从配置中移除
    m_service.removeAppEntry(appId);
    m_appById.remove(appId);
    m_onlineCache.remove(appId);

    QString err;
    if (m_service.saveConfig(err)) {
        logToFile(QStringLiteral("已删除应用: %1").arg(app.name));
        validateLocalApps();
        fetchRemoteCatalog();
        refreshAppIcons();
    } else {
        logToFile(QStringLiteral("删除应用后保存配置失败: %1").arg(err));
    }
}

// ============================================================
// 下载历史版本
// ============================================================

void MainWindow::onDownloadHistoryVersion(const QString &appId)
{
    if (!m_serverConnected) {
        // 尝试重连
        m_serverConnected = m_service.tryConnectServer(3000);
        if (!m_serverConnected) {
            logToFile(QStringLiteral("无法获取历史版本：未连接服务器"));
            return;
        }
    }

    const AppConfig app = m_appById.value(appId);
    if (app.id.isEmpty()) {
        return;
    }

    // 获取历史版本列表
    logToFile(QStringLiteral("[%1] 正在获取历史版本列表...").arg(app.name));
    QProgressDialog fetchProgress(QStringLiteral("正在获取历史版本列表..."), QString(), 0, 0, this);
    setupUnifiedProgressDialog(fetchProgress, QStringLiteral("正在获取历史版本列表..."), true);
    fetchProgress.show();
    QApplication::processEvents();

    QJsonObject histMeta = m_service.fetchHistoryVersions(appId);
    fetchProgress.close();
    QJsonArray versions = histMeta.value(QStringLiteral("versions")).toArray();

    if (versions.isEmpty()) {
        logToFile(QStringLiteral("[%1] 没有可用的历史版本").arg(app.name));
        return;
    }

    // 显示选择对话框
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("选择历史版本 - %1").arg(app.name));
    dlg.setMinimumSize(420, 300);

    auto *layout = new QVBoxLayout(&dlg);
    auto *label = new QLabel(QStringLiteral("选择要下载的历史版本:"), &dlg);
    layout->addWidget(label);

    auto *list = new QListWidget(&dlg);
    for (const QJsonValue &v : versions) {
        QJsonObject vo = v.toObject();
        QString text = QStringLiteral("v%1  -  %2")
            .arg(vo.value(QStringLiteral("version")).toString(),
                 vo.value(QStringLiteral("fileName")).toString());
        auto *listItem = new QListWidgetItem(text, list);
        listItem->setData(Qt::UserRole, v);
    }
    layout->addWidget(list);

    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btnBox);

    if (dlg.exec() != QDialog::Accepted || !list->currentItem()) {
        return;
    }

    // 获取选择的版本信息
    QJsonObject selected = list->currentItem()->data(Qt::UserRole).toJsonObject();
    QString version = selected.value(QStringLiteral("version")).toString();
    QUrl downloadUrl(selected.value(QStringLiteral("downloadUrl")).toString());

    // 确定保存路径：与原应用同目录
    QString appDir = m_service.appAbsoluteDir(app);
    QDir().mkpath(appDir);

    // 构造版本化文件名：原始EXE基名_版本号.exe
    QFileInfo origExeInfo(app.exeRelativePath);
    QString histExeName = origExeInfo.completeBaseName() + QStringLiteral("_")
                        + version + QStringLiteral(".") + origExeInfo.suffix();
    QString targetPath = QDir(appDir).absoluteFilePath(histExeName);

    // 下载
    logToFile(QStringLiteral("[%1] 正在下载历史版本 v%2...").arg(app.name, version));
    QProgressDialog dlProgress(QStringLiteral("[%1] 正在下载历史版本 v%2...").arg(app.name, version),
                               QString(),
                               0,
                               100,
                               this);
    setupUnifiedProgressDialog(dlProgress,
                               QStringLiteral("[%1] 正在下载历史版本 v%2...").arg(app.name, version),
                               false);
    dlProgress.show();
    QApplication::processEvents();

    QString err;
    if (!m_service.downloadToFile(
            downloadUrl,
            targetPath,
            err,
            30000,
            [&](qint64 received, qint64 total) {
                if (total > 0) {
                    const int p = qBound(0, static_cast<int>((received * 100) / total), 100);
                    dlProgress.setValue(p);
                }
                QApplication::processEvents();
            },
            [&](const QString &status) {
                dlProgress.setLabelText(compactStatusText(QStringLiteral("[%1] %2").arg(app.name, status), 120));
                QApplication::processEvents();
            })) {
        dlProgress.close();
        logToFile(QStringLiteral("[%1] 下载失败: %2").arg(app.name, err));
        return;
    }
    dlProgress.setValue(100);
    dlProgress.close();

    // 添加为新应用
    AppConfig newApp;
    newApp.id = appId + QStringLiteral("_") + QString(version).replace('.', '_');
    newApp.name = app.name + QStringLiteral("_") + version;
    newApp.exeRelativePath = QDir(m_service.appsRoot()).relativeFilePath(targetPath);
    // 历史版本不设置 updateMetaUrl（无需在线更新）

    m_service.addAppEntry(newApp);
    m_appById.insert(newApp.id, newApp);

    if (m_service.saveConfig(err)) {
        logToFile(QStringLiteral("[%1] 历史版本 v%2 已添加为新应用: %3").arg(app.name, version, newApp.name));
        refreshAppIcons();
    } else {
        logToFile(QStringLiteral("保存配置失败: %1").arg(err));
    }
}

// ============================================================
// 下载远程应用
// ============================================================

void MainWindow::onDownloadRemoteApp(const QString &appId)
{
    if (!m_remoteCatalog.contains(appId)) {
        logToFile(QStringLiteral("未找到远程应用信息: %1").arg(appId));
        return;
    }

    QJsonObject appInfo = m_remoteCatalog.value(appId);
    QString appName = appInfo.value(QStringLiteral("appName")).toString();
    QString pkgFile = appInfo.value(QStringLiteral("packageFileName")).toString();
    QUrl downloadUrl(appInfo.value(QStringLiteral("downloadUrl")).toString());
    QString subDir  = appInfo.value(QStringLiteral("subDir")).toString().trimmed();

    // 保存路径：根据服务端配置的 subDir 决定存放位置
    QString targetDir = m_service.appsRoot();
    QString exeRel = pkgFile;
    if (!subDir.isEmpty()) {
        targetDir = QDir(m_service.appsRoot()).absoluteFilePath(subDir);
        QDir().mkpath(targetDir);
        exeRel = subDir + QStringLiteral("/") + pkgFile;
    }
    QString targetPath = QDir(targetDir).absoluteFilePath(pkgFile);

    logToFile(QStringLiteral("正在下载: %1...").arg(appName));
    QProgressDialog dlProgress(QStringLiteral("[%1] 正在下载应用...").arg(appName), QString(), 0, 100, this);
    setupUnifiedProgressDialog(dlProgress,
                               QStringLiteral("[%1] 正在下载应用...").arg(appName),
                               false);
    dlProgress.show();
    QApplication::processEvents();

    QString err;
    if (!m_service.downloadToFile(
            downloadUrl,
            targetPath,
            err,
            30000,
            [&](qint64 received, qint64 total) {
                if (total > 0) {
                    const int p = qBound(0, static_cast<int>((received * 100) / total), 100);
                    dlProgress.setValue(p);
                }
                QApplication::processEvents();
            },
            [&](const QString &status) {
                dlProgress.setLabelText(compactStatusText(QStringLiteral("[%1] %2").arg(appName, status), 120));
                QApplication::processEvents();
            })) {
        dlProgress.close();
        logToFile(QStringLiteral("[%1] 下载失败: %2").arg(appName, err));
        return;
    }
    dlProgress.setValue(100);
    dlProgress.close();

    // 添加到配置
    AppConfig newApp;
    newApp.id = appId;
    newApp.name = appName;
    newApp.exeRelativePath = exeRel;
    {
        QUrl baseUrl(m_service.serverBaseUrl().trimmed());
        QString path = baseUrl.path();
        if (!path.endsWith('/')) {
            path += '/';
        }
        path += QStringLiteral("updates/") + appId + QStringLiteral(".json");
        baseUrl.setPath(path);
        newApp.updateMetaUrl = baseUrl;
    }

    m_service.addAppEntry(newApp);
    m_appById.insert(newApp.id, newApp);
    m_remoteCatalog.remove(appId);

    if (m_service.saveConfig(err)) {
        logToFile(QStringLiteral("已下载并添加应用: %1").arg(appName));
    } else {
        logToFile(QStringLiteral("保存配置失败: %1").arg(err));
    }

    // 下载完成后立即进行依赖文件检测
    QProgressDialog checkProgress(QStringLiteral("[%1] 正在检测依赖信息...").arg(appName), QString(), 0, 0, this);
    setupUnifiedProgressDialog(checkProgress,
                               QStringLiteral("[%1] 正在检测依赖信息...").arg(appName),
                               true);
    checkProgress.show();
    QApplication::processEvents();

    OnlineAppInfo online = m_service.checkOnlineInfo(newApp);
    checkProgress.close();

    if (online.requestSuccess && !online.requiredFiles.isEmpty()) {
        logToFile(QStringLiteral("[%1] 正在检测依赖文件完整性...").arg(appName));
        QProgressDialog depProgress(QStringLiteral("[%1] 正在修复依赖...").arg(appName), QString(), 0, 100, this);
        setupUnifiedProgressDialog(depProgress,
                       QStringLiteral("[%1] 正在修复依赖...").arg(appName),
                       false);
        depProgress.show();
        QApplication::processEvents();

        QString depResult;
        if (!m_service.checkAndFixDependencies(
                newApp,
                online,
                depResult,
                180000,
                [&](qint64 received, qint64 total) {
                    if (total > 0) {
                        const int p = qBound(0, static_cast<int>((received * 100) / total), 100);
                        depProgress.setValue((p * 40) / 100);
                    }
                    QApplication::processEvents();
                },
                [&](const QString &status) {
                    depProgress.setLabelText(compactStatusText(QStringLiteral("[%1] %2").arg(appName, status), 120));
                    QApplication::processEvents();
                },
                [&](int installPct) {
                    depProgress.setValue(40 + (qBound(0, installPct, 100) * 60) / 100);
                    QApplication::processEvents();
                })) {
            depProgress.close();
            const QString failMsg = depResult.trimmed().isEmpty()
                                        ? QStringLiteral("依赖修复失败")
                                        : QStringLiteral("依赖修复失败: %1").arg(depResult.trimmed());
            logToFile(QStringLiteral("[%1] %2").arg(appName, failMsg));
        } else {
            depProgress.setValue(100);
            depProgress.close();
            const QString okMsg = depResult.trimmed().isEmpty()
                                      ? QStringLiteral("依赖文件完整性检查通过")
                                      : depResult.trimmed();
            logToFile(QStringLiteral("[%1] %2").arg(appName, okMsg));
        }
    }

    refreshAppIcons();
}

QIcon MainWindow::createDownloadIcon(int size) const
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 圆形背景
    const qreal margin = size * 0.06;
    QRectF bgRect(margin, margin, size - margin * 2, size - margin * 2);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(219, 234, 254)); // #dbeafe
    painter.drawEllipse(bgRect);

    const qreal cx = size / 2.0;
    const qreal cy = size / 2.0;

    // 下载箭头
    QPen pen(QColor(37, 99, 235), size * 0.04, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);

    const qreal arrowTop = cy - size * 0.22;
    const qreal arrowBot = cy + size * 0.08;
    const qreal wingLen  = size * 0.12;

    // 竖线
    painter.drawLine(QPointF(cx, arrowTop), QPointF(cx, arrowBot));
    // 箭头两翼
    painter.drawLine(QPointF(cx, arrowBot), QPointF(cx - wingLen, arrowBot - wingLen));
    painter.drawLine(QPointF(cx, arrowBot), QPointF(cx + wingLen, arrowBot - wingLen));

    // 底部托盘
    const qreal trayLeft  = cx - size * 0.20;
    const qreal trayRight = cx + size * 0.20;
    const qreal trayTop   = cy + size * 0.16;
    const qreal trayBot   = cy + size * 0.24;

    QPainterPath tray;
    tray.moveTo(trayLeft,  trayTop);
    tray.lineTo(trayLeft,  trayBot);
    tray.lineTo(trayRight, trayBot);
    tray.lineTo(trayRight, trayTop);
    painter.drawPath(tray);

    painter.end();
    return QIcon(pixmap);
}

// ============================================================
// AppManager 自身升级
// ============================================================

void MainWindow::onCheckAppManagerUpdate()
{
    appendLog(QStringLiteral("正在检查 AppManager 新版本..."));
    
    QProgressDialog checkProgress(QStringLiteral("正在检查 AppManager 新版本..."), QString(), 0, 0, this);
    setupUnifiedProgressDialog(checkProgress, QStringLiteral("正在检查 AppManager 新版本..."), true);
    checkProgress.show();
    QApplication::processEvents();

    OnlineAppInfo online = m_service.checkAppManagerUpdate();
    checkProgress.close();

    if (!online.requestSuccess) {
        logToFile(QStringLiteral("[AppManager] 检查更新失败: %1").arg(online.errorMessage));
        QMessageBox::information(this, QStringLiteral("版本检查"), 
                                QStringLiteral("检查更新失败: %1").arg(online.errorMessage));
        return;
    }

    const QString currentVersion = m_service.appManagerVersion();
    const QString latestVersion = online.latestVersion;
    
    logToFile(QStringLiteral("[AppManager] 当前版本: %1，最新版本: %2").arg(currentVersion, latestVersion));

    if (compareVersions(currentVersion, latestVersion) >= 0) {
        QMessageBox::information(this, QStringLiteral("版本检查"),
                                QStringLiteral("AppManager 已是最新版本 (v%1)").arg(currentVersion));
        return;
    }

    // 提示用户升级
    QMessageBox::StandardButton ret = QMessageBox::question(
        this,
        QStringLiteral("发现新版本"),
        QStringLiteral("发现 AppManager 新版本 v%1\n\n当前版本: v%2\n\n是否立即升级?")
            .arg(latestVersion, currentVersion),
        QMessageBox::Yes | QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        appendLog(QStringLiteral("正在下载 AppManager v%1...").arg(latestVersion));
        
        QProgressDialog dlProgress(QStringLiteral("正在下载 AppManager..."), QString(), 0, 100, this);
        setupUnifiedProgressDialog(dlProgress, QStringLiteral("正在下载 AppManager..."), false);
        dlProgress.show();
        QApplication::processEvents();

        QString result;
        if (!m_service.upgradeAppManager(
                online,
                result,
                30000,
                [&](qint64 received, qint64 total) {
                    if (total > 0) {
                        const int p = qBound(0, static_cast<int>((received * 100) / total), 100);
                        dlProgress.setValue(p);
                    }
                    QApplication::processEvents();
                },
                [&](const QString &status) {
                    dlProgress.setLabelText(compactStatusText(status, 120));
                    QApplication::processEvents();
                })) {
            dlProgress.close();
            logToFile(QStringLiteral("[AppManager] 升级失败: %1").arg(result));
            QMessageBox::warning(this, QStringLiteral("升级失败"), result);
        } else {
            dlProgress.setValue(100);
            dlProgress.close();
            logToFile(QStringLiteral("[AppManager] %1").arg(result));
            QMessageBox::information(this, QStringLiteral("升级进行中"),
                                    QStringLiteral("[AppManager] %1").arg(result));
            
            // 2秒后关闭应用，让安装程序进行
            QTimer::singleShot(2000, this, [this]() {
                QApplication::quit();
            });
        }
    }
}

void MainWindow::onAboutAppManager()
{
    const QString appVersion = m_service.appManagerVersion();
    QMessageBox::about(this, QStringLiteral("关于 AppManager"),
                       QStringLiteral("AppManager v%1\n\n"
                                      "应用管理和升级工具\n\n"
                                      "© 2024-2025\n\n"
                                      "点击菜单中的'检查更新'可查询新版本")
                           .arg(appVersion));
}
