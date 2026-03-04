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
#endif

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
        statusBar()->showMessage(QStringLiteral("未检测服务器连接状态"));
    }
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
    m_checkRequiredButton = new QPushButton(QStringLiteral("检测必需文件"), root);
    m_checkUpdatesButton = new QPushButton(QStringLiteral("在线检测更新"), root);

    buttonLayout->addWidget(m_checkRequiredButton);
    buttonLayout->addWidget(m_checkUpdatesButton);
    buttonLayout->addStretch();

    m_appList = new QListWidget(root);
    m_appList->setViewMode(QListView::IconMode);
    m_appList->setIconSize(QSize(96, 96));
    m_appList->setResizeMode(QListView::Adjust);
    m_appList->setMovement(QListView::Static);
    m_appList->setGridSize(QSize(160, 170));
    m_appList->setSpacing(12);
    m_appList->setWordWrap(true);
    m_appList->setUniformItemSizes(true);

    auto *hintLabel = new QLabel(QStringLiteral("点击下方应用图标即可快捷启动"), root);
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
}

void MainWindow::connectSignals()
{
    connect(m_checkRequiredButton, &QPushButton::clicked, this, &MainWindow::onCheckRequiredFiles);
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
            "  background: white; border: 1px solid #dbe2ea; border-radius: 10px;"
            "}"
            "QListWidget::item {"
            "  border-radius: 8px; padding: 8px;"
            "}"
            "QListWidget::item:selected { background: #dbeafe; color: #1e3a8a; }"
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
    refreshAppIcons();
    logToFile(QStringLiteral("配置加载成功，共 %1 个应用，根目录: %2")
                  .arg(m_service.apps().size())
                  .arg(QDir::toNativeSeparators(m_service.appsRoot())));
}

void MainWindow::refreshAppIcons()
{
    const QVector<AppConfig> apps = m_service.apps();
    m_appList->clear();
    QFileIconProvider provider;

    for (const AppConfig &app : apps) {
        const QString exePath = m_service.appAbsoluteExePath(app);
        QIcon icon = provider.icon(QFileInfo(exePath));
        if (icon.isNull()) {
            icon = style()->standardIcon(QStyle::SP_DesktopIcon);
        }

        const QString currentVersion = m_service.appCurrentVersion(app);
        const OnlineAppInfo online = m_onlineCache.value(app.id);
        QString status = QStringLiteral("就绪");
        if (online.requestSuccess && compareVersions(currentVersion, online.latestVersion) < 0) {
            status = QStringLiteral("可升级到 %1").arg(online.latestVersion);
        }

        auto *item = new QListWidgetItem(icon,
                                         QStringLiteral("%1\nV%2\n%3")
                                             .arg(app.name, currentVersion, status));
        item->setData(Qt::UserRole, app.id);
        item->setTextAlignment(Qt::AlignHCenter);
        item->setToolTip(QDir::toNativeSeparators(exePath));
        m_appList->addItem(item);
    }
}

bool MainWindow::launchAppById(const QString &appId)
{
    const AppConfig app = m_appById.value(appId);
    if (app.id.isEmpty()) {
        return false;
    }

    QStringList missing;
    if (!m_service.checkRequiredFiles(app, missing)) {
        logToFile(QStringLiteral("[%1] 启动失败，缺失文件: %2").arg(app.name, missing.join(QStringLiteral(", "))));
        return false;
    }

    const QString exePath = m_service.appAbsoluteExePath(app);

#ifdef Q_OS_WIN
    // 检查该 EXE 是否已在运行，若已运行则将其窗口调到前台
    const QString exeName = QFileInfo(exePath).fileName();
    DWORD targetPid = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (exeName.compare(QString::fromWCharArray(pe.szExeFile), Qt::CaseInsensitive) == 0) {
                    targetPid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

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
        // 进程存在但未找到可见窗口，记录日志后不重复启动
        logToFile(QStringLiteral("[%1] 已在运行（未找到可见窗口）").arg(app.name));
        return true;
    }
#endif

    const bool started = QProcess::startDetached(exePath, {});
    logToFile(QStringLiteral("[%1] %2").arg(app.name, started ? QStringLiteral("启动成功") : QStringLiteral("启动失败")));
    return started;
}

void MainWindow::onCheckRequiredFiles()
{
    const QVector<AppConfig> apps = m_service.apps();
    if (apps.isEmpty()) {
        return;
    }

    int brokenCount = 0;
    for (const AppConfig &app : apps) {
        QStringList missing;
        const bool ok = m_service.checkRequiredFiles(app, missing);
        if (!ok) {
            ++brokenCount;
            logToFile(QStringLiteral("[%1] 缺失文件: %2").arg(app.name, missing.join(QStringLiteral(", "))));
        }
    }

    if (brokenCount == 0) {
        logToFile(QStringLiteral("所有应用的必需文件检测通过"));
    } else {
        logToFile(QStringLiteral("必需文件检测完成，异常应用数: %1").arg(brokenCount));
    }

    refreshAppIcons();
}

void MainWindow::onCheckUpdates()
{
    const QVector<AppConfig> apps = m_service.apps();
    if (apps.isEmpty()) {
        return;
    }

    UpdateDialog dlg(&m_service, apps, this);
    connect(&dlg, &UpdateDialog::logMessage, this, &MainWindow::logToFile);
    dlg.exec();

    // 同步在线缓存用于图标刷新
    m_onlineCache = dlg.onlineCache();

    // 状态栏反映服务器连接状态
    const int total = apps.size();
    const int success = dlg.serverSuccessCount();
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
}

void MainWindow::onAppIconClicked(QListWidgetItem *item)
{
    if (item == nullptr) {
        return;
    }

    const QString appId = item->data(Qt::UserRole).toString();
    launchAppById(appId);
}
