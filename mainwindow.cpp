#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "versionutils.h"
#include "docbrowserpage.h"
#include "logindialog.h"
#include "refreshbuttonutils.h"

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
#include <QDialog>
#include <QDesktopServices>
#include <QFileDialog>
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
#include <QProgressBar>
#include <QProgressDialog>
#include <QAbstractItemView>
#include <QStyledItemDelegate>
#include <QSettings>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QGridLayout>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QStackedWidget>
#include <QToolButton>
#include <QStyle>
#include <QMovie>

namespace {
constexpr int kProgressDialogWidth = 540;
constexpr int kProgressBarWidth = 420;
constexpr int kProgressBarHeight = 18;
constexpr qint64 kStartupAppCheckCooldownSec = 16 * 60 * 60; // 16 小时
constexpr int kPopupScrollTextThreshold = 180;
constexpr int kPopupScrollLineThreshold = 6;
constexpr int kPopupFixedWidth = 620;
constexpr int kPopupScrollMinHeight = 220;
constexpr int kMissingDepsDialogWidth = 760;
constexpr int kMissingDepsDialogHeight = 500;

bool shouldUseScrollablePopup(const QString &text)
{
    if (text.size() >= kPopupScrollTextThreshold) {
        return true;
    }
    return text.count(QLatin1Char('\n')) + 1 >= kPopupScrollLineThreshold;
}

QMessageBox::StandardButton showAdaptiveMessageBox(
    QWidget *parent,
    QMessageBox::Icon icon,
    const QString &title,
    const QString &text,
    QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton,
    const QHash<QMessageBox::StandardButton, QString> &buttonTexts = {})
{
    QMessageBox box(parent);
    box.setIcon(icon);
    box.setWindowTitle(title);
    box.setStandardButtons(buttons);

    if (defaultButton != QMessageBox::NoButton) {
        box.setDefaultButton(defaultButton);
    }

    for (auto it = buttonTexts.constBegin(); it != buttonTexts.constEnd(); ++it) {
        if (QAbstractButton *btn = box.button(it.key())) {
            btn->setText(it.value());
        }
    }

    if (!shouldUseScrollablePopup(text)) {
        box.setText(text);
        return static_cast<QMessageBox::StandardButton>(box.exec());
    }

    box.setText(QStringLiteral("详细内容如下："));
    box.setFixedWidth(kPopupFixedWidth);

    auto *textEdit = new QPlainTextEdit(&box);
    textEdit->setReadOnly(true);
    textEdit->setPlainText(text);
    textEdit->setMinimumHeight(kPopupScrollMinHeight);

    if (QGridLayout *grid = qobject_cast<QGridLayout *>(box.layout())) {
        const int row = grid->rowCount();
        const int colCount = qMax(1, grid->columnCount());
        grid->addWidget(textEdit, row, 0, 1, colCount);
    }

    return static_cast<QMessageBox::StandardButton>(box.exec());
}

QMessageBox::StandardButton showMissingDepsDialog(
    QWidget *parent,
    const QString &appName,
    const QStringList &missingDeps)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("依赖文件缺失"));
    dlg.setModal(true);
    dlg.resize(kMissingDepsDialogWidth, kMissingDepsDialogHeight);
    dlg.setMinimumSize(kMissingDepsDialogWidth, kMissingDepsDialogHeight);

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("[%1] 检测到依赖文件缺失").arg(appName), &dlg);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; color: #1f2937;"));
    layout->addWidget(title);

    auto *desc = new QLabel(QStringLiteral("以下文件缺失，应用可能无法正常运行。\n是否立即下载修复？"), &dlg);
    desc->setAlignment(Qt::AlignCenter);
    desc->setWordWrap(true);
    desc->setStyleSheet(QStringLiteral("font-size: 13px; color: #374151;"));
    layout->addWidget(desc);

    auto *missingView = new QPlainTextEdit(&dlg);
    missingView->setReadOnly(true);
    missingView->setPlainText(missingDeps.join(QStringLiteral("\n")));
    missingView->setMinimumHeight(300);
    missingView->setStyleSheet(QStringLiteral(
        "QPlainTextEdit {"
        "  border: 1px solid #cbd5e1;"
        "  border-radius: 8px;"
        "  background: #ffffff;"
        "  padding: 10px;"
        "  font-family: Consolas, 'Courier New';"
        "  font-size: 12px;"
        "}"));
    layout->addWidget(missingView, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, &dlg);
    if (QPushButton *yesBtn = buttons->button(QDialogButtonBox::Yes)) {
        yesBtn->setText(QStringLiteral("下载修复"));
        yesBtn->setDefault(true);
    }
    if (QPushButton *noBtn = buttons->button(QDialogButtonBox::No)) {
        noBtn->setText(QStringLiteral("继续启动"));
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    return dlg.exec() == QDialog::Accepted ? QMessageBox::Yes : QMessageBox::No;
}

void setupUnifiedProgressDialog(QProgressDialog &dlg,
                                const QString &labelText,
                                bool indeterminate,
                                bool cancellable = false)
{
    dlg.setLabelText(labelText);
    dlg.setWindowModality(Qt::NonModal);
    if (cancellable) {
        dlg.setCancelButtonText(QStringLiteral("取消"));
    } else {
        dlg.setCancelButton(nullptr);
    }
    dlg.setAutoClose(true);
    dlg.setAutoReset(true);
    dlg.setMinimumDuration(0);
    dlg.setFixedWidth(kProgressDialogWidth);
    dlg.setStyleSheet(QStringLiteral(
        "QProgressDialog { background: #f8fafc; }"
        "QLabel { color: #1f2937; }"
        "QProgressBar { min-height: %1px; border: 1px solid #cbd5e1; border-radius: 5px; background: #ffffff; text-align: center; }"
        "QProgressBar::chunk { background: #2563eb; border-radius: 4px; }")
            .arg(kProgressBarHeight));

    if (QLabel *label = dlg.findChild<QLabel *>()) {
        label->setWordWrap(true);
    }

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

static QString composeLaunchAndUpdateHint(const QString &updateHint)
{
    return QStringLiteral("点击图标启动应用 | %1").arg(updateHint);
}

static QString makeLocalAppId(const QString &exePath)
{
    const QString base = QFileInfo(exePath).completeBaseName().trimmed();
    const QString normalized = base.toLower();
    QString id;
    id.reserve(normalized.size() + 24);
    for (QChar ch : normalized) {
        if (ch.isLetterOrNumber()) {
            id.append(ch);
        } else {
            id.append(QLatin1Char('_'));
        }
    }
    while (id.contains(QStringLiteral("__"))) {
        id.replace(QStringLiteral("__"), QStringLiteral("_"));
    }
    id = id.trimmed();
    if (id.startsWith(QLatin1Char('_'))) {
        id.remove(0, 1);
    }
    if (id.endsWith(QLatin1Char('_'))) {
        id.chop(1);
    }
    if (id.isEmpty()) {
        id = QStringLiteral("local_app");
    }
    return QStringLiteral("local_%1_%2")
        .arg(id)
        .arg(QDateTime::currentMSecsSinceEpoch());
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

        if (hasUpdate) {
            const QRect ringRect = iconRect.adjusted(-4, -4, 4, 4);
            painter->setPen(QPen(QColor(96, 165, 250), 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(ringRect, 14, 14);

            const QRect flagRect(iconRect.right() - 12, iconRect.top() - 4, 20, 20);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(59, 130, 246));
            painter->drawEllipse(flagRect);

            QFont uf;
            uf.setPixelSize(10);
            uf.setBold(true);
            painter->setFont(uf);
            painter->setPen(Qt::white);
            painter->drawText(flagRect, Qt::AlignCenter, QStringLiteral("升"));
        }

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
                badgeFg = QColor(6, 95, 70);
                badgeBg = QColor(209, 250, 229);
            } else if (status == QStringLiteral("下载")) {
                badgeFg = QColor(30, 64, 175);
                badgeBg = QColor(219, 234, 254);
            } else if (status == QStringLiteral("排队")) {
                badgeFg = QColor(146, 64, 14);
                badgeBg = QColor(254, 243, 199);
            } else if (status == QStringLiteral("下载中")) {
                badgeFg = QColor(30, 64, 175);
                badgeBg = QColor(191, 219, 254);
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

        // 本地应用徽章（左上角）
        if (index.data(Qt::UserRole + 5).toBool()) {
            const QString localTag = QStringLiteral("本地");
            QFont lf;
            lf.setPixelSize(11);
            painter->setFont(lf);
            QFontMetrics lfm(lf);
            const int tagW = lfm.horizontalAdvance(localTag) + 10;
            const int tagH = 16;
            const QRect tagRect(r.left() + 3, r.top() + 3, tagW, tagH);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(254, 243, 199));   // 淡橙背景
            painter->drawRoundedRect(tagRect, 7, 7);
            painter->setPen(QColor(146, 64, 14));          // 深橙文字
            painter->drawText(tagRect, Qt::AlignCenter, localTag);
        }

        // 历史版本徽章（本地角标下方或单独左上角）
        if (index.data(Qt::UserRole + 6).toBool()) {
            const QString histTag = QStringLiteral("历史");
            QFont hf;
            hf.setPixelSize(11);
            painter->setFont(hf);
            QFontMetrics hfm(hf);
            const int tagW = hfm.horizontalAdvance(histTag) + 10;
            const int tagH = 16;
            // 本地和历史可能同时存在，历史徽章紧贴本地徽章下方
            const int topOffset = index.data(Qt::UserRole + 5).toBool() ? 22 : 3;
            const QRect tagRect(r.left() + 3, r.top() + topOffset, tagW, tagH);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(224, 231, 255));   // 淡紫背景
            painter->drawRoundedRect(tagRect, 7, 7);
            painter->setPen(QColor(67, 56, 202));        // 深紫文字
            painter->drawText(tagRect, Qt::AlignCenter, histTag);
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

    setWindowTitle(QStringLiteral("APP 管理器 v%1").arg(QString::fromLatin1(APP_VERSION)));
    resize(1000, 680);

    setupUiWidgets();
    applySimpleStyle();
    connectSignals();

    // 基础配置固定为程序根目录下 apps.json；客户端应用列表单独持久化到 client_apps.json。
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
    auto *mainLayout = new QHBoxLayout(root);
    mainLayout->setContentsMargins(18, 14, 18, 14);
    mainLayout->setSpacing(12);

    // 左侧图标导航栏
    auto *navWidget = new QWidget(root);
    navWidget->setObjectName(QStringLiteral("navPanel"));
    navWidget->setFixedWidth(112);
    auto *navLayout = new QVBoxLayout(navWidget);
    navLayout->setContentsMargins(8, 8, 8, 8);
    navLayout->setSpacing(10);

    m_navLaunchButton = new QToolButton(navWidget);
    m_navLaunchButton->setObjectName(QStringLiteral("navLaunchButton"));
    m_navLaunchButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_navLaunchButton->setIcon(QIcon(QStringLiteral(":/resources/laucher.ico")));
    m_navLaunchButton->setIconSize(QSize(38, 38));
    m_navLaunchButton->setText(QStringLiteral("启动台"));
    m_navLaunchButton->setToolTip(QStringLiteral("启动台"));
    m_navLaunchButton->setCheckable(true);
    m_navLaunchButton->setAutoExclusive(true);

    m_navCommunityButton = new QToolButton(navWidget);
    m_navCommunityButton->setObjectName(QStringLiteral("navCommunityButton"));
    m_navCommunityButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_navCommunityButton->setIcon(QIcon(QStringLiteral(":/resources/community.ico")));
    m_navCommunityButton->setIconSize(QSize(38, 38));
    m_navCommunityButton->setText(QStringLiteral("社区"));
    m_navCommunityButton->setToolTip(QStringLiteral("社区"));
    m_navCommunityButton->setCheckable(true);
    m_navCommunityButton->setAutoExclusive(true);

    m_navHelpButton = new QToolButton(navWidget);
    m_navHelpButton->setObjectName(QStringLiteral("navHelpButton"));
    m_navHelpButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_navHelpButton->setIcon(QIcon(QStringLiteral(":/resources/document.ico")));
    m_navHelpButton->setIconSize(QSize(38, 38));
    m_navHelpButton->setText(QStringLiteral("学习"));
    m_navHelpButton->setToolTip(QStringLiteral("学习"));
    m_navHelpButton->setCheckable(true);
    m_navHelpButton->setAutoExclusive(true);

    m_navLogButton = new QToolButton(navWidget);
    m_navLogButton->setObjectName(QStringLiteral("navLogButton"));
    m_navLogButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_navLogButton->setIcon(QIcon(QStringLiteral(":/resources/log.ico")));
    m_navLogButton->setIconSize(QSize(38, 38));
    m_navLogButton->setText(QStringLiteral("日志"));
    m_navLogButton->setToolTip(QStringLiteral("查看日志"));
    m_navLogButton->setCheckable(true);
    m_navLogButton->setAutoExclusive(true);

    navLayout->addWidget(m_navLaunchButton);
    navLayout->addWidget(m_navCommunityButton);
    navLayout->addWidget(m_navHelpButton);
    navLayout->addWidget(m_navLogButton);
    navLayout->addStretch();

    // 登录按钮（固定在导航底部）
    m_loginButton = new QToolButton(navWidget);
    m_loginButton->setObjectName(QStringLiteral("loginButton"));
    m_loginButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_loginButton->setIcon(QIcon(QStringLiteral(":/resources/login.ico")));
    m_loginButton->setIconSize(QSize(24, 24));
    m_loginButton->setText(QStringLiteral("登录"));
    m_loginButton->setToolTip(QStringLiteral("点击登录"));
    m_loginButton->setCheckable(false);
    navLayout->addWidget(m_loginButton);

    // 右侧区域（标题 + 页面堆栈）
    auto *rightPanel = new QWidget(root);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);

    m_titleLabel = new QLabel(QStringLiteral("应用启动台"), rightPanel);
    m_titleLabel->setObjectName(QStringLiteral("titleLabel"));

    // 右侧页面堆栈
    m_mainStack = new QStackedWidget(rightPanel);

    // 页面1：应用启动台
    auto *launchPage = new QWidget(m_mainStack);
    auto *launchLayout = new QVBoxLayout(launchPage);
    launchLayout->setContentsMargins(0, 0, 0, 0);
    launchLayout->setSpacing(10);

    auto *toolbarPanel = new QWidget(launchPage);
    toolbarPanel->setStyleSheet(QStringLiteral("QWidget { background: white; border-radius: 8px; }"));
    auto *toolbarPanelLayout = new QVBoxLayout(toolbarPanel);
    toolbarPanelLayout->setContentsMargins(12, 8, 12, 8);
    toolbarPanelLayout->setSpacing(6);

    auto *toolbarTopLayout = new QHBoxLayout();
    toolbarTopLayout->setContentsMargins(0, 0, 0, 0);
    toolbarTopLayout->setSpacing(10);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);

    m_addLocalAppButton = new QPushButton(QStringLiteral("+ 添加本地应用"), toolbarPanel);
    m_addLocalAppButton->setObjectName(QStringLiteral("addLocalAppButton"));
    m_launchSearchBox = new QLineEdit(toolbarPanel);
    m_launchSearchBox->setPlaceholderText(QStringLiteral("搜索应用名称…"));
    m_launchSearchBox->setFixedHeight(30);
    m_launchSearchBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_launchSearchBox->setMinimumWidth(140);
    m_launchSearchBox->setMaximumWidth(260);
    m_launchSearchBox->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  border: 1px solid #e2e8f0;"
        "  border-radius: 15px;"
        "  padding: 0 12px;"
        "  background: #f8fafc;"
        "  color: #374151;"
        "  font-size: 13px;"
        "}"
        "QLineEdit:focus {"
        "  border-color: #93c5fd;"
        "  background: white;"
        "}"));

    m_checkUpdatesButton = new QPushButton(toolbarPanel);
    setButtonIconWithoutDisabledTint(m_checkUpdatesButton, QStringLiteral(":/resources/refresh.ico"));
    m_checkUpdatesButton->setIconSize(QSize(30, 30));
    m_checkUpdatesButton->setFixedSize(34, 34);
    m_checkUpdatesButton->setToolTip(QStringLiteral("\u68c0\u6d4b\u5e94\u7528\u66f4\u65b0"));
    m_checkUpdatesButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #eff6ff;"
        "  border: 1px solid #bfdbfe;"
        "  border-radius: 17px;"
        "}"
        "QPushButton:hover { background: #dbeafe; border-color: #93c5fd; }"
        "QPushButton:pressed { background: #bfdbfe; }"
        "QPushButton:disabled { background: #f8fafc; border-color: #e2e8f0; }"));
    m_checkUpdatesButton->setCursor(Qt::PointingHandCursor);
    m_checkUpdatesButton->setObjectName(QStringLiteral("checkUpdatesButton"));
    buttonLayout->addStretch();

    // 刷新动画
    m_refreshMovie = new QMovie(QStringLiteral(":/resources/array.gif"), QByteArray(), this);
    m_refreshMovie->setScaledSize(QSize(28, 28));
    connect(m_refreshMovie, &QMovie::frameChanged, this, [this](int) {
        setButtonIconWithoutDisabledTint(m_checkUpdatesButton, m_refreshMovie->currentPixmap());
    });

    m_updateAllButton = new QPushButton(QStringLiteral("全部更新"), toolbarPanel);
    m_updateAllButton->setVisible(false);
    m_updateAllButton->setFixedHeight(34);
    m_updateAllButton->setCursor(Qt::PointingHandCursor);
    m_updateAllButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #ecfdf5;"
        "  color: #166534;"
        "  border: 1px solid #86efac;"
        "  border-radius: 17px;"
        "  padding: 0 14px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: #dcfce7; border-color: #4ade80; }"
        "QPushButton:pressed { background: #bbf7d0; }"
        "QPushButton:disabled { color: #94a3b8; background: #f8fafc; border-color: #e2e8f0; }"));
    buttonLayout->addWidget(m_updateAllButton);
    buttonLayout->addWidget(m_checkUpdatesButton);

    m_updateCheckProgress = new QProgressBar(toolbarPanel);
    m_updateCheckProgress->setTextVisible(false);
    m_updateCheckProgress->setFixedHeight(4);
    m_updateCheckProgress->setRange(0, 1);
    m_updateCheckProgress->setValue(0);
    {
        QSizePolicy progressPolicy = m_updateCheckProgress->sizePolicy();
        progressPolicy.setRetainSizeWhenHidden(true);
        m_updateCheckProgress->setSizePolicy(progressPolicy);
    }
    m_updateCheckProgress->setVisible(false);
    m_updateCheckProgress->setStyleSheet(QStringLiteral(
        "QProgressBar {"
        "  border: none;"
        "  border-radius: 2px;"
        "  background: #dbeafe;"
        "}"
        "QProgressBar::chunk {"
        "  border-radius: 2px;"
        "  background: #60a5fa;"
        "}"));

    m_updateCheckHintLabel = new QLabel(
        composeLaunchAndUpdateHint(QStringLiteral("点击左侧按钮可检测应用在线更新")),
        toolbarPanel);
    m_updateCheckHintLabel->setStyleSheet(QStringLiteral("color: #64748b; font-size: 12px;"));
    m_updateCheckHintLabel->setWordWrap(true);

    toolbarTopLayout->addWidget(m_addLocalAppButton, 0, Qt::AlignVCenter);
    toolbarTopLayout->addWidget(m_launchSearchBox, 1, Qt::AlignVCenter);
    toolbarTopLayout->addLayout(buttonLayout, 0);

    toolbarPanelLayout->addLayout(toolbarTopLayout);
    toolbarPanelLayout->addWidget(m_updateCheckProgress);
    toolbarPanelLayout->addWidget(m_updateCheckHintLabel);

    m_appList = new QListWidget(launchPage);
    m_appList->setViewMode(QListView::IconMode);
    m_appList->setIconSize(QSize(72, 72));
    m_appList->setResizeMode(QListView::Adjust);
    m_appList->setMovement(QListView::Static);
    m_appList->setGridSize(QSize(152, 208));
    m_appList->setSpacing(6);
    m_appList->setWordWrap(true);
    m_appList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_appList->setMinimumHeight(460);

    launchLayout->addWidget(toolbarPanel);
    launchLayout->addWidget(m_appList, 1);

    // 页面2：社区占位
    auto *communityPage = new QWidget(m_mainStack);
    auto *communityLayout = new QVBoxLayout(communityPage);
    communityLayout->setContentsMargins(24, 24, 24, 24);
    communityLayout->setSpacing(10);
    auto *communityTitle = new QLabel(QStringLiteral("社区"), communityPage);
    communityTitle->setObjectName(QStringLiteral("placeholderTitle"));
    auto *communityHint = new QLabel(QStringLiteral("社区功能建设中，后续可接入公告、活动、讨论与资源分享。"), communityPage);
    communityHint->setObjectName(QStringLiteral("placeholderHint"));
    communityHint->setWordWrap(true);
    communityLayout->addWidget(communityTitle);
    communityLayout->addWidget(communityHint);
    communityLayout->addStretch();

    // 页面3：帮助（文档浏览器）
    m_docBrowserPage = new DocBrowserPage(&m_service, m_mainStack);
    connect(m_docBrowserPage, &DocBrowserPage::logMessage,
            this, &MainWindow::logToFile);

    // 页面4：日志
    auto *logPage = new QWidget(m_mainStack);
    auto *logLayout = new QVBoxLayout(logPage);
    logLayout->setContentsMargins(12, 12, 12, 12);
    logLayout->setSpacing(8);
    auto *logTitle = new QLabel(QStringLiteral("日志信息"), logPage);
    logTitle->setObjectName(QStringLiteral("placeholderTitle"));
    auto *logHint = new QLabel(QStringLiteral("记录客户端运行过程中的关键操作与异常信息。"), logPage);
    logHint->setObjectName(QStringLiteral("placeholderHint"));
    logHint->setWordWrap(true);
    m_logView = new QPlainTextEdit(logPage);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);
    m_logView->setPlaceholderText(QStringLiteral("暂无日志"));
    m_logView->setObjectName(QStringLiteral("logView"));
    logLayout->addWidget(logTitle);
    logLayout->addWidget(logHint);
    logLayout->addWidget(m_logView, 1);

    m_mainStack->addWidget(launchPage);         // index 0
    m_mainStack->addWidget(communityPage);      // index 1
    m_mainStack->addWidget(m_docBrowserPage);   // index 2
    m_mainStack->addWidget(logPage);            // index 3

    rightLayout->addWidget(m_titleLabel);
    rightLayout->addWidget(m_mainStack, 1);

    mainLayout->addWidget(navWidget);
    mainLayout->addWidget(rightPanel, 1);

    m_navLaunchButton->setChecked(true);
    m_mainStack->setCurrentIndex(0);

    connect(m_navLaunchButton, &QToolButton::clicked, this, [this]() {
        m_mainStack->setCurrentIndex(0);
        m_titleLabel->setText(QStringLiteral("应用启动台"));
    });
    connect(m_navCommunityButton, &QToolButton::clicked, this, [this]() {
        m_mainStack->setCurrentIndex(1);
        m_titleLabel->setText(QStringLiteral("社区"));
    });
    connect(m_navHelpButton, &QToolButton::clicked, this, [this]() {
        m_mainStack->setCurrentIndex(2);
        m_titleLabel->setText(QStringLiteral("学习"));
        // 首次进入时自动拉取文档目录
        static bool s_docLoaded = false;
        if (!s_docLoaded) {
            s_docLoaded = true;
            m_docBrowserPage->refresh();
        }
    });
    connect(m_navLogButton, &QToolButton::clicked, this, [this]() {
        m_mainStack->setCurrentIndex(3);
        m_titleLabel->setText(QStringLiteral("日志"));
    });
    connect(m_loginButton, &QToolButton::clicked, this, &MainWindow::onLoginClicked);

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
    connect(m_addLocalAppButton, &QPushButton::clicked, this, &MainWindow::onAddLocalApp);
    connect(m_checkUpdatesButton, &QPushButton::clicked, this, &MainWindow::onCheckUpdates);
    connect(m_updateAllButton, &QPushButton::clicked, this, &MainWindow::onUpdateAllApps);
    connect(m_appList, &QListWidget::itemClicked, this, &MainWindow::onAppIconClicked);
    connect(m_launchSearchBox, &QLineEdit::textChanged, this, &MainWindow::filterAppList);
}

void MainWindow::onAddLocalApp()
{
    const QString exePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择本地应用"),
        QDir::homePath(),
        QStringLiteral("可执行文件 (*.exe);;所有文件 (*.*)"));
    if (exePath.trimmed().isEmpty()) {
        return;
    }

    const QString normalized = QDir::cleanPath(exePath);
    if (!QFileInfo::exists(normalized)) {
        showAdaptiveMessageBox(this,
                               QMessageBox::Warning,
                               QStringLiteral("添加失败"),
                               QStringLiteral("所选文件不存在：\n%1").arg(QDir::toNativeSeparators(normalized)),
                               QMessageBox::Ok,
                               QMessageBox::Ok);
        return;
    }

    for (const AppConfig &existing : m_service.apps()) {
        const QString existingPath = QDir::cleanPath(m_service.appAbsoluteExePath(existing));
        if (existingPath.compare(normalized, Qt::CaseInsensitive) == 0) {
            showAdaptiveMessageBox(this,
                                   QMessageBox::Information,
                                   QStringLiteral("已存在"),
                                   QStringLiteral("该应用已添加到启动台：\n%1").arg(existing.name),
                                   QMessageBox::Ok,
                                   QMessageBox::Ok);
            return;
        }
    }

    AppConfig app;
    app.id = makeLocalAppId(normalized);
    app.name = QFileInfo(normalized).completeBaseName().trimmed();
    if (app.name.isEmpty()) {
        app.name = QFileInfo(normalized).fileName();
    }
    app.exeRelativePath = normalized;
    app.isLocalApp = true;

    m_service.addAppEntry(app);
    m_appById.insert(app.id, app);
    m_onlineCache.remove(app.id);

    QString err;
    if (!m_service.saveConfig(err)) {
        m_service.removeAppEntry(app.id);
        m_appById.remove(app.id);
        showAdaptiveMessageBox(this,
                               QMessageBox::Warning,
                               QStringLiteral("添加失败"),
                               QStringLiteral("保存配置失败：%1").arg(err),
                               QMessageBox::Ok,
                               QMessageBox::Ok);
        return;
    }

    logToFile(QStringLiteral("已添加本地应用: %1 (%2)")
                  .arg(app.name, QDir::toNativeSeparators(normalized)));
    refreshAppIcons();
}

void MainWindow::applySimpleStyle()
{
    qApp->setStyleSheet(
        QStringLiteral(
            "QWidget { background: #f5f7fb; color: #1f2937; }"
            "QLabel#titleLabel { font-size: 24px; font-weight: 700; }"
            "QWidget#navPanel { background: #e8eef7; border: 1px solid #d1dae8; border-radius: 10px; }"
            "QToolButton#navLaunchButton, QToolButton#navCommunityButton, QToolButton#navHelpButton, QToolButton#navLogButton {"
            "  min-width: 90px; min-height: 92px; border-radius: 12px;"
            "  font-size: 13px; font-weight: 600;"
            "  padding: 5px 2px 4px 2px;"
            "  border: 1px solid transparent; background: transparent;"
            "}"
            "QToolButton#navLaunchButton:hover, QToolButton#navCommunityButton:hover, QToolButton#navHelpButton:hover, QToolButton#navLogButton:hover {"
            "  background: #dbeafe; border-color: #93c5fd;"
            "}"
            "QToolButton#navLaunchButton:checked, QToolButton#navCommunityButton:checked, QToolButton#navHelpButton:checked {"
            "  background: #dbeafe; border-color: #60a5fa;"
            "  color: #1e3a8a;"
            "}"
            "QToolButton#navLogButton:checked {"
            "  background: #dbeafe; border-color: #60a5fa;"
            "  color: #1e3a8a;"
            "}"
            "QToolButton#loginButton {"
            "  min-width: 80px; min-height: 56px; max-height: 60px; border-radius: 10px;"
            "  font-size: 12px; font-weight: 600;"
            "  padding: 4px 6px;"
            "  border: 1px solid #86efac; background: #dcfce7; color: #166534;"
            "}"
            "QToolButton#loginButton:hover {"
            "  background: #bbf7d0; border-color: #4ade80;"
            "}"
            "QToolButton#loginButton:pressed {"
            "  background: #86efac; border-color: #22c55e;"
            "}"
            "QLabel#placeholderTitle { font-size: 22px; font-weight: 700; color: #0f172a; }"
            "QLabel#placeholderHint { font-size: 14px; color: #475569; }"
            "QPushButton {"
            "  background: #eef2ff; color: #1e3a8a; border: 1px solid #c7d2fe; border-radius: 8px;"
            "  padding: 8px 16px; font-size: 14px; font-weight: 600;"
            "}"
            "QPushButton:hover { background: #e0e7ff; border-color: #a5b4fc; }"
            "QPushButton:pressed { background: #c7d2fe; border-color: #818cf8; }"
            "QPushButton#addLocalAppButton {"
            "  background: transparent; color: #2563eb; border: 1px solid #93c5fd;"
            "}"
            "QPushButton#addLocalAppButton:hover {"
            "  background: transparent; border-color: #60a5fa; color: #1d4ed8;"
            "}"
            "QPushButton#addLocalAppButton:pressed {"
            "  background: transparent; border-color: #3b82f6; color: #1e40af;"
            "}"
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

    // 从注册表恢复登录状态
    QSettings authSettings(QStringLiteral("Software\\AppManager\\AppManager"), QSettings::NativeFormat);
    m_authToken = authSettings.value(QStringLiteral("authToken")).toString();
    m_authUser  = authSettings.value(QStringLiteral("authUser")).toString();
    if (!m_authToken.isEmpty() && m_loginButton) {
        m_loginButton->setIcon(QIcon(QStringLiteral(":/resources/logout.ico")));
        m_loginButton->setText(m_authUser.isEmpty() ? QStringLiteral("已登录") : m_authUser);
        m_loginButton->setToolTip(QStringLiteral("已登录为 %1，点击注销").arg(m_authUser));
    }
}

void MainWindow::validateLocalApps()
{
    QStringList toRemove;
    for (auto it = m_appById.constBegin(); it != m_appById.constEnd(); ++it) {
        const AppConfig &app = it.value();
        if (app.id.compare(QStringLiteral("appmanager"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        const QString exePath = m_service.appAbsoluteExePath(app);
        if (!QFileInfo::exists(exePath)) {
            toRemove.append(app.id);
            logToFile(QStringLiteral("[%1] 启动文件不存在，已从列表与配置移除: %2")
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
        const bool hasUpdate = !app.isLocalApp
                       && online.requestSuccess
                               && compareVersions(currentVersion, online.latestVersion) < 0;
        const QString status = hasUpdate ? QStringLiteral("可升级") : QString();

        auto *item = new QListWidgetItem(icon, app.name);
        item->setData(Qt::UserRole,     app.id);
        item->setData(Qt::UserRole + 1, false);                      // isRemote
        item->setData(Qt::UserRole + 2, QStringLiteral("V%1").arg(currentVersion)); // version
        item->setData(Qt::UserRole + 3, status);                     // status badge
        item->setData(Qt::UserRole + 4, hasUpdate);                  // hasUpdate
        item->setData(Qt::UserRole + 5, app.isLocalApp && !app.isHistoryVersion); // isLocalApp badge
        item->setData(Qt::UserRole + 6, app.isHistoryVersion);         // isHistoryVersion
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

        // 已在下载队列中的应用显示排队标记
        const QString statusBadge = m_downloadQueuedSet.contains(it.key())
                                        ? QStringLiteral("排队")
                                        : QStringLiteral("下载");

        auto *item = new QListWidgetItem(icon, appName);
        item->setData(Qt::UserRole,     it.key());
        item->setData(Qt::UserRole + 1, true);                              // isRemote
        item->setData(Qt::UserRole + 2, QStringLiteral("V%1").arg(version)); // version
        item->setData(Qt::UserRole + 3, statusBadge);                       // status badge
        item->setData(Qt::UserRole + 4, false);                             // hasUpdate
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(120, 160));
        item->setToolTip(QStringLiteral("点击下载该应用"));
        m_appList->addItem(item);
    }

    refreshUpdateActions();
    // 重新应用当前搜索过滤
    if (m_launchSearchBox && !m_launchSearchBox->text().isEmpty()) {
        filterAppList(m_launchSearchBox->text());
    }
}

void MainWindow::filterAppList(const QString &text)
{
    const QString keyword = text.trimmed().toLower();
    for (int i = 0; i < m_appList->count(); ++i) {
        QListWidgetItem *it = m_appList->item(i);
        const bool visible = keyword.isEmpty()
                             || it->text().toLower().contains(keyword);
        it->setHidden(!visible);
    }
}

QVector<AppConfig> MainWindow::collectUpdatableApps() const
{
    QVector<AppConfig> updatableApps;
    for (const AppConfig &app : m_service.apps()) {
        if (app.isLocalApp
            || app.id.compare(QStringLiteral("appmanager"), Qt::CaseInsensitive) == 0
            || app.name.compare(QStringLiteral("appmanager"), Qt::CaseInsensitive) == 0) {
            continue;
        }

        const OnlineAppInfo online = m_onlineCache.value(app.id);
        if (!online.requestSuccess) {
            continue;
        }

        const QString currentVersion = m_service.appCurrentVersion(app);
        if (compareVersions(currentVersion, online.latestVersion) < 0) {
            updatableApps.push_back(app);
        }
    }
    return updatableApps;
}

void MainWindow::refreshUpdateActions()
{
    if (m_updateAllButton == nullptr || m_updateCheckHintLabel == nullptr) {
        return;
    }

    const QVector<AppConfig> updatableApps = collectUpdatableApps();
    const bool hasUpdates = !updatableApps.isEmpty();
    m_updateAllButton->setVisible(hasUpdates);
    m_updateAllButton->setEnabled(!m_isCheckingUpdates);
    if (hasUpdates && !m_isCheckingUpdates) {
        m_updateAllButton->setText(QStringLiteral("全部更新 (%1)").arg(updatableApps.size()));
        m_updateCheckHintLabel->setText(
            composeLaunchAndUpdateHint(
                QStringLiteral("已检测到 %1 个应用可升级，可直接在右侧执行全部更新。")
                .arg(updatableApps.size())));
    } else if (!m_isCheckingUpdates) {
        m_updateCheckHintLabel->setText(
            composeLaunchAndUpdateHint(QStringLiteral("点击右侧按钮可检测应用在线更新")));
    }
}

void MainWindow::setInlineUpdateCheckState(bool checking,
                                           int current,
                                           int total,
                                           const QString &message)
{
    m_isCheckingUpdates = checking;

    if (m_checkUpdatesButton != nullptr) {
        setRefreshButtonBusyState(m_checkUpdatesButton, m_refreshMovie, checking);
    }
    if (m_updateAllButton != nullptr) {
        m_updateAllButton->setEnabled(!checking);
    }
    if (m_updateCheckProgress != nullptr) {
        m_updateCheckProgress->setVisible(checking);
        m_updateCheckProgress->setRange(0, qMax(total, 1));
        m_updateCheckProgress->setValue(qBound(0, current, qMax(total, 1)));
    }
    if (m_updateCheckHintLabel != nullptr && !message.isEmpty()) {
        m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(message));
    }
}

// ============================================================
// 实际执行进程启动（不做前置检查，由调用方保证 EXE 存在）
// ============================================================
void MainWindow::doActualLaunch(const AppConfig &app, bool forceNewWindow)
{
    const QString exePath = m_service.appAbsoluteExePath(app);

    if (forceNewWindow) {
        const bool started = QProcess::startDetached(exePath, {});
        logToFile(QStringLiteral("[%1] %2")
                      .arg(app.name,
                           started ? QStringLiteral("已在新窗口打开")
                                   : QStringLiteral("新窗口启动失败")));
        return;
    }

#ifdef Q_OS_WIN
    const DWORD targetPid = findRunningProcessIdByPath(exePath);

    if (targetPid != 0) {
        BringToFrontData data = { targetPid, nullptr };
        EnumWindows(enumWindowsCallback, reinterpret_cast<LPARAM>(&data));

        if (data.hwnd) {
            if (IsIconic(data.hwnd)) {
                ShowWindow(data.hwnd, SW_RESTORE);
            }
            SetForegroundWindow(data.hwnd);
            logToFile(QStringLiteral("[%1] 已在运行，已将窗口调到前台").arg(app.name));
            return;
        }

        const bool restarted = QProcess::startDetached(exePath, {});
        logToFile(QStringLiteral("[%1] %2")
                      .arg(app.name,
                           restarted ? QStringLiteral("检测到同路径进程但无可见窗口，已尝试重新启动")
                                     : QStringLiteral("检测到同路径进程且无可见窗口，重新启动失败")));
        return;
    }
#endif

    const bool started = QProcess::startDetached(exePath, {});
    logToFile(QStringLiteral("[%1] %2").arg(app.name, started ? QStringLiteral("启动成功") : QStringLiteral("启动失败")));
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

    const bool isHistoryApp = app.isHistoryVersion;
    const bool isPureLocalApp = app.isLocalApp && !isHistoryApp;

    const QString exePath = m_service.appAbsoluteExePath(app);

    // ---- Step 1: 主程序 EXE 存在性检查 ----
    if (!QFileInfo::exists(exePath)) {
        if (isPureLocalApp) {
            showAdaptiveMessageBox(this,
                                   QMessageBox::Warning,
                                   QStringLiteral("启动失败"),
                                   QStringLiteral("本地应用文件不存在：\n%1")
                                       .arg(QDir::toNativeSeparators(exePath)),
                                   QMessageBox::Ok,
                                   QMessageBox::Ok);
            logToFile(QStringLiteral("[%1] 启动失败：本地应用文件不存在: %2")
                          .arg(app.name, QDir::toNativeSeparators(exePath)));
            return false;
        }

        if (isHistoryApp) {
            showAdaptiveMessageBox(this,
                                   QMessageBox::Warning,
                                   QStringLiteral("启动失败"),
                                   QStringLiteral("历史版本文件不存在：\n%1")
                                       .arg(QDir::toNativeSeparators(exePath)),
                                   QMessageBox::Ok,
                                   QMessageBox::Ok);
            logToFile(QStringLiteral("[%1] 启动失败：历史版本文件不存在: %2")
                          .arg(app.name, QDir::toNativeSeparators(exePath)));
            return false;
        }

        // 普通远端应用：EXE 缺失时自动下载恢复
        logToFile(QStringLiteral("[%1] 主程序文件缺失，正在自动下载恢复...").arg(app.name));

        // 确保在线信息已缓存
        if (!m_onlineCache.value(appId).requestSuccess) {
            const OnlineAppInfo freshInfo = m_service.checkOnlineInfo(app);
            m_onlineCache.insert(appId, freshInfo);
        }
        startUpdateWorkflow({ app });

        const AppConfig updatedApp = m_appById.value(appId);
        if (!updatedApp.id.isEmpty() && QFileInfo::exists(m_service.appAbsoluteExePath(updatedApp))) {
            doActualLaunch(updatedApp);
        } else {
            logToFile(QStringLiteral("[%1] 自动下载完成，但主程序仍未找到，请联系管理员").arg(
                updatedApp.id.isEmpty() ? appId : updatedApp.name));
        }
        return true;
    }

#ifdef Q_OS_WIN
    // 应用已在运行时，点击图标只负责拉起已有窗口，不重复做依赖检查。
    if (findRunningProcessIdByPath(exePath) != 0) {
        doActualLaunch(app);
        return true;
    }
#endif

    // 纯本地应用：只检查 EXE 是否存在，直接启动
    if (isPureLocalApp) {
        doActualLaunch(app);
        return true;
    }

    // ---- Step 2: EXE 存在，检查依赖文件完整性 ----
    // 历史版本应用：不检查更新，但按“主应用最新版本”的依赖清单检查依赖。
    AppConfig depSourceApp = app;
    QString depSourceId = app.id;

    if (isHistoryApp) {
        QString baseAppId;
        const int markerPos = app.name.lastIndexOf(QStringLiteral("(v"));
        if (markerPos >= 0 && app.name.endsWith(QLatin1Char(')'))) {
            const QString histVersion = app.name.mid(markerPos + 2, app.name.size() - markerPos - 3).trimmed();
            const QString suffixToken = histVersion;
            if (!suffixToken.isEmpty()) {
                const QString idSuffix = QStringLiteral("_") + QString(suffixToken).replace('.', '_');
                if (app.id.endsWith(idSuffix)) {
                    baseAppId = app.id.left(app.id.size() - idSuffix.size());
                }
            }
        }
        if (baseAppId.isEmpty()) {
            const int lastUnderscore = app.id.lastIndexOf(QLatin1Char('_'));
            if (lastUnderscore > 0) {
                baseAppId = app.id.left(lastUnderscore);
            }
        }

        const AppConfig baseApp = m_appById.value(baseAppId);
        if (!baseApp.id.isEmpty() && !baseApp.isHistoryVersion) {
            depSourceApp = baseApp;
            depSourceId = baseApp.id;
        } else {
            logToFile(QStringLiteral("[%1] 未能定位对应主应用，历史版本依赖将仅按本地配置检查")
                          .arg(app.name));
        }
    }

    OnlineAppInfo online = m_onlineCache.value(depSourceId);
    if (!online.requestSuccess) {
        online = m_service.checkOnlineInfo(depSourceApp, 15000);
        if (online.requestSuccess) {
            m_onlineCache.insert(depSourceId, online);
        }
    }

    QStringList requiredFiles = depSourceApp.requiredRelativeFiles;
    for (const QString &rf : online.requiredFiles) {
        if (!rf.trimmed().isEmpty() && !requiredFiles.contains(rf)) {
            requiredFiles.push_back(rf);
        }
    }

    QStringList missingDeps;
    const QString appInstallDir = m_service.appAbsoluteDir(app);
    for (const QString &rf : requiredFiles) {
        const QString cleaned = QDir::cleanPath(rf);
        const QFileInfo pathInfo(cleaned);

        bool exists = false;
        if (pathInfo.isAbsolute()) {
            exists = QFileInfo::exists(cleaned);
        } else {
            // 兼容两类配置：相对 app 安装目录 或 相对 appsRoot。
            const QString inAppDir = QDir(appInstallDir).absoluteFilePath(cleaned);
            const QString inAppsRoot = QDir(m_service.appsRoot()).absoluteFilePath(cleaned);
            exists = QFileInfo::exists(inAppDir) || QFileInfo::exists(inAppsRoot);
        }

        if (!exists) {
            missingDeps.push_back(rf);
        }
    }

    if (!missingDeps.isEmpty()) {
        logToFile(QStringLiteral("[%1] 检测到 %2 个依赖文件缺失: %3")
                      .arg(app.name)
                      .arg(missingDeps.size())
                      .arg(missingDeps.join(QStringLiteral(", "))));

        const int reply = static_cast<int>(showMissingDepsDialog(this, app.name, missingDeps));

        if (reply == QMessageBox::Yes) {
            if (!online.requestSuccess || online.requiredFiles.isEmpty()) {
                logToFile(QStringLiteral("[%1] 无法下载依赖：在线依赖清单不可用，继续尝试启动").arg(app.name));
                doActualLaunch(app);
                return true;
            }

            QProgressDialog depProgress(QStringLiteral("[%1] 正在修复依赖...").arg(app.name), QString(), 0, 100, this);
            setupUnifiedProgressDialog(depProgress,
                                       QStringLiteral("[%1] 正在修复依赖...").arg(app.name),
                                       false,
                                       true);
            depProgress.show();
            QApplication::processEvents();

            QString depResult;
            const bool repaired = m_service.checkAndFixDependencies(
                app,
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
                    depProgress.setLabelText(compactStatusText(QStringLiteral("[%1] %2").arg(app.name, status), 120));
                    QApplication::processEvents();
                },
                [&](int installPct) {
                    depProgress.setValue(40 + (qBound(0, installPct, 100) * 60) / 100);
                    QApplication::processEvents();
                },
                [&depProgress]() {
                    return depProgress.wasCanceled();
                });

            if (repaired) {
                depProgress.setValue(100);
            }
            const bool depCanceled = depProgress.wasCanceled();
            depProgress.close();

            if (repaired) {
                logToFile(QStringLiteral("[%1] 依赖修复完成，正在启动应用").arg(app.name));
            } else {
                if (depCanceled) {
                    showAdaptiveMessageBox(this,
                                           QMessageBox::Information,
                                           QStringLiteral("已取消"),
                                           QStringLiteral("已取消步骤：依赖修复。\n\n接下来：将继续尝试启动应用；若启动异常，可再次执行依赖修复。"),
                                           QMessageBox::Ok,
                                           QMessageBox::Ok);
                }
                logToFile(QStringLiteral("[%1] %2: %3")
                              .arg(app.name,
                                   depCanceled ? QStringLiteral("依赖修复已取消") : QStringLiteral("依赖修复失败"),
                                   depResult.trimmed().isEmpty() ? QStringLiteral("未知错误") : depResult.trimmed()));
            }

            doActualLaunch(app);
            return true;
        }

        // 用户选择跳过修复，直接启动
        logToFile(QStringLiteral("[%1] 用户跳过依赖修复，尝试直接启动...").arg(app.name));
    }

    // ---- Step 3: 正常启动 ----
    doActualLaunch(app);
    return true;
}

void MainWindow::startUpdateWorkflow(const QVector<AppConfig> &apps)
{
    if (apps.isEmpty() || m_isCheckingUpdates) {
        return;
    }

    const int total = apps.size();
    int successCount = 0;
    int failCount = 0;

    m_isCheckingUpdates = true;
    if (m_checkUpdatesButton) { setRefreshButtonBusyState(m_checkUpdatesButton, m_refreshMovie, true); }
    if (m_updateAllButton)    m_updateAllButton->setEnabled(false);
    if (m_updateCheckProgress) {
        m_updateCheckProgress->setVisible(true);
        m_updateCheckProgress->setRange(0, total * 100);
        m_updateCheckProgress->setValue(0);
    }
    if (m_updateCheckHintLabel) {
        m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(
            QStringLiteral("正在准备升级 %1 个应用...").arg(total)));
    }
    QApplication::processEvents();

    for (int i = 0; i < total; ++i) {
        const AppConfig &app   = apps.at(i);
        const OnlineAppInfo online = m_onlineCache.value(app.id);
        const int baseProgress = i * 100;

        if (m_updateCheckProgress) m_updateCheckProgress->setValue(baseProgress);
        if (m_updateCheckHintLabel) {
            m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(
                QStringLiteral("正在升级 [%1/%2] %3 ...").arg(i + 1).arg(total).arg(app.name)));
        }
        QApplication::processEvents();

        QString resultMsg;
        const bool ok = m_service.upgradeApp(
            app, online, resultMsg, 180000,
            [this, baseProgress](qint64 received, qint64 totalBytes) {
                if (m_updateCheckProgress && totalBytes > 0) {
                    const int pct = static_cast<int>(received * 70 / totalBytes);
                    m_updateCheckProgress->setValue(baseProgress + pct);
                }
                QApplication::processEvents();
            },
            [this, i, total, appName = app.name](const QString &status) {
                if (m_updateCheckHintLabel) {
                    m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(
                        QStringLiteral("[%1/%2] %3: %4")
                            .arg(i + 1).arg(total).arg(appName, status)));
                }
                logToFile(QStringLiteral("[%1] %2").arg(appName, status));
                QApplication::processEvents();
            },
            [this, baseProgress](int installPct) {
                if (m_updateCheckProgress) {
                    m_updateCheckProgress->setValue(baseProgress + 70 + installPct * 30 / 100);
                }
                QApplication::processEvents();
            },
            []() -> bool { return false; });

        logToFile(ok
            ? QStringLiteral("[%1] 升级成功：%2").arg(app.name, resultMsg)
            : QStringLiteral("[%1] 升级失败：%2").arg(app.name, resultMsg));
        ok ? ++successCount : ++failCount;

        if (m_updateCheckProgress) m_updateCheckProgress->setValue(baseProgress + 100);
        QApplication::processEvents();
    }

    fetchRemoteCatalog();
    refreshAppIcons();

    const QString resultText = (failCount == 0)
        ? QStringLiteral("升级完成：%1 个应用全部成功。").arg(successCount)
        : (successCount > 0
            ? QStringLiteral("升级完成：%1 个成功，%2 个失败。").arg(successCount).arg(failCount)
            : QStringLiteral("升级失败：全部 %1 个应用均未成功。").arg(total));

    m_isCheckingUpdates = false;
    if (m_checkUpdatesButton) { setRefreshButtonBusyState(m_checkUpdatesButton, m_refreshMovie, false); }
    if (m_updateAllButton)    m_updateAllButton->setEnabled(true);
    if (m_updateCheckProgress) m_updateCheckProgress->setVisible(false);
    if (m_updateCheckHintLabel) {
        m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(resultText));
    }

    refreshUpdateActions();
    if (statusBar()) statusBar()->showMessage(resultText, 8000);
}

void MainWindow::onCheckUpdates()
{
    if (m_isCheckingUpdates) {
        return;
    }

    QVector<AppConfig> apps = m_service.apps();

    // 排除 AppManager，只检查其他应用的更新
    QVector<AppConfig> filteredApps;
    for (const AppConfig &app : apps) {
        if (!app.isLocalApp
            && app.id.toLower() != QStringLiteral("appmanager")
            && app.name.toLower() != QStringLiteral("appmanager")) {
            filteredApps.append(app);
        }
    }

    if (filteredApps.isEmpty()) {
        logToFile(QStringLiteral("没有可更新的应用"));
        setInlineUpdateCheckState(false, 0, 1, QStringLiteral("当前没有可在线检测的应用。"));
        refreshUpdateActions();
        return;
    }

    if (!m_serverConnected) {
        m_serverConnected = m_service.tryConnectServer(3000);
    }
    if (!m_serverConnected) {
        setInlineUpdateCheckState(false, 0, filteredApps.size(), QStringLiteral("无法连接服务器，未执行在线检测。"));
        if (statusBar()) {
            statusBar()->showMessage(QStringLiteral("无法连接服务器，在线检测已取消"), 5000);
        }
        return;
    }

    int successCount = 0;
    int updateCount = 0;
    const int total = filteredApps.size();

    setInlineUpdateCheckState(true, 0, total, QStringLiteral("正在检测在线更新..."));
    if (statusBar()) {
        statusBar()->showMessage(QStringLiteral("正在检测在线更新..."));
    }
    QApplication::processEvents();

    for (int i = 0; i < total; ++i) {
        const AppConfig &app = filteredApps.at(i);
        setInlineUpdateCheckState(
            true,
            i,
            total,
            QStringLiteral("正在检测 [%1/%2] %3").arg(i + 1).arg(total, 0, 10).arg(app.name));
        QApplication::processEvents();

        const OnlineAppInfo online = m_service.checkOnlineInfo(app);
        m_onlineCache.insert(app.id, online);
        if (online.requestSuccess) {
            ++successCount;
            const QString currentVersion = m_service.appCurrentVersion(app);
            if (compareVersions(currentVersion, online.latestVersion) < 0) {
                ++updateCount;
                logToFile(QStringLiteral("[%1] 检测到可升级版本: %2 -> %3")
                              .arg(app.name, currentVersion, online.latestVersion));
            } else {
                logToFile(QStringLiteral("[%1] 已是最新版本: %2").arg(app.name, currentVersion));
            }
        } else {
            logToFile(QStringLiteral("[%1] 在线检测失败: %2").arg(app.name, online.errorMessage));
        }

        setInlineUpdateCheckState(true, i + 1, total, QString());
        refreshAppIcons();
        QApplication::processEvents();
    }

    fetchRemoteCatalog();
    refreshAppIcons();

    const QString resultText = updateCount > 0
        ? QStringLiteral("检测完成：发现 %1 个应用可升级。可点击右侧按钮执行全部更新。").arg(updateCount)
        : QStringLiteral("检测完成：所有应用均为最新版本。已成功检测 %1/%2 个应用。").arg(successCount).arg(total);
    setInlineUpdateCheckState(false, total, total, resultText);

    if (statusBar()) {
        statusBar()->showMessage(
            QStringLiteral("检测完成：成功 %1/%2，发现更新 %3")
                .arg(successCount)
                .arg(total)
                .arg(updateCount),
            8000);
    }
}

void MainWindow::onUpdateAllApps()
{
    const QVector<AppConfig> updatableApps = collectUpdatableApps();
    if (updatableApps.isEmpty()) {
        refreshUpdateActions();
        return;
    }

    startUpdateWorkflow(updatableApps);
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
        if (app.isLocalApp
            || app.id.compare(QStringLiteral("appmanager"), Qt::CaseInsensitive) == 0
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

    QJsonArray catalog = m_service.fetchAppCatalog(m_authToken);
    m_remoteCatalog.clear();
    bool hasAllowMultiChange = false;
    for (const QJsonValue &v : catalog) {
        QJsonObject item = v.toObject();
        QString catalogAppId = item.value(QStringLiteral("appId")).toString();
        
        // 跳过 AppManager 本身，不在远程列表中显示
        if (catalogAppId.toLower() == QStringLiteral("appmanager")) {
            continue;
        }
        
        const bool allowMulti = item.value(QStringLiteral("allowMultiInstance")).toBool(false);
        if (m_appById.contains(catalogAppId)) {
            AppConfig app = m_appById.value(catalogAppId);
            if (app.allowMultiInstance != allowMulti) {
                app.allowMultiInstance = allowMulti;
                m_appById.insert(catalogAppId, app);

                m_service.removeAppEntry(catalogAppId);
                m_service.addAppEntry(app);
                hasAllowMultiChange = true;
            }
            continue;
        }

        m_remoteCatalog.insert(catalogAppId, item);
    }

    if (hasAllowMultiChange) {
        QString err;
        if (!m_service.saveConfig(err)) {
            logToFile(QStringLiteral("更新多开配置保存失败: %1").arg(err));
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
    const AppConfig app = m_appById.value(appId);
    const bool isLocalApp = !isRemote && app.isLocalApp && !app.isHistoryVersion;
    const bool isHistoryApp = !isRemote && app.isHistoryVersion;

    QMenu menu(this);

    if (isRemote) {
        menu.addAction(QStringLiteral("下载应用"), this, [this, appId]() {
            onDownloadRemoteApp(appId);
        });
    } else {
        menu.addAction(QStringLiteral("打开应用"), this, [this, appId]() {
            launchAppById(appId);
        });
        if (app.allowMultiInstance) {
            menu.addAction(QStringLiteral("在新窗口打开"), this, [this, appId]() {
                const AppConfig selected = m_appById.value(appId);
                if (selected.id.isEmpty()) {
                    return;
                }
                const QString exePath = m_service.appAbsoluteExePath(selected);
                if (!QFileInfo::exists(exePath)) {
                    showAdaptiveMessageBox(this,
                                           QMessageBox::Warning,
                                           QStringLiteral("启动失败"),
                                           QStringLiteral("应用文件不存在：\n%1")
                                               .arg(QDir::toNativeSeparators(exePath)),
                                           QMessageBox::Ok,
                                           QMessageBox::Ok);
                    return;
                }
                doActualLaunch(selected, true);
            });
        }
        menu.addAction(QStringLiteral("打开文件位置"), this, [this, appId]() {
            onOpenAppLocation(appId);
        });

        if (isLocalApp) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("移除应用"), this, [this, appId]() {
                onDeleteApp(appId);
            });
            menu.exec(globalPos);
            return;
        }

        if (isHistoryApp) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("删除应用"), this, [this, appId]() {
                onDeleteApp(appId);
            });
            menu.exec(globalPos);
            return;
        }

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

    if (app.isHistoryVersion) {
        if (showAdaptiveMessageBox(
                this,
                QMessageBox::Question,
                QStringLiteral("删除历史版本"),
                QStringLiteral("确定删除历史版本「%1」吗？\n\n将同时删除对应 EXE 文件。")
                    .arg(app.name),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Cancel,
                {
                    { QMessageBox::Ok, QStringLiteral("删除") },
                    { QMessageBox::Cancel, QStringLiteral("取消") },
                }) != QMessageBox::Ok) {
            return;
        }

        if (QFileInfo::exists(exePath)) {
            if (QFile::remove(exePath)) {
                logToFile(QStringLiteral("[%1] 已删除历史版本文件: %2")
                              .arg(app.name, QDir::toNativeSeparators(exePath)));
            } else {
                logToFile(QStringLiteral("[%1] 历史版本文件删除失败: %2")
                              .arg(app.name, QDir::toNativeSeparators(exePath)));
            }
        }

        m_service.removeAppEntry(appId);
        m_appById.remove(appId);
        m_onlineCache.remove(appId);

        QString err;
        if (m_service.saveConfig(err)) {
            logToFile(QStringLiteral("已删除历史版本应用: %1").arg(app.name));
            validateLocalApps();
            refreshAppIcons();
        } else {
            logToFile(QStringLiteral("删除历史版本后保存配置失败: %1").arg(err));
        }
        return;
    }

    if (app.isLocalApp) {
        if (showAdaptiveMessageBox(
                this,
                QMessageBox::Question,
                QStringLiteral("移除本地应用"),
                QStringLiteral("确定从客户端移除应用「%1」吗？\n\n仅移除应用链接，不会删除本地文件。").arg(app.name),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Cancel,
                {
                    { QMessageBox::Ok, QStringLiteral("移除") },
                    { QMessageBox::Cancel, QStringLiteral("取消") },
                }) != QMessageBox::Ok) {
            return;
        }

        m_service.removeAppEntry(appId);
        m_appById.remove(appId);
        m_onlineCache.remove(appId);

        QString err;
        if (m_service.saveConfig(err)) {
            logToFile(QStringLiteral("已移除本地应用链接: %1").arg(app.name));
            refreshAppIcons();
        } else {
            logToFile(QStringLiteral("移除本地应用后保存配置失败: %1").arg(err));
        }
        return;
    }

    if (showAdaptiveMessageBox(
            this,
            QMessageBox::Question,
            QStringLiteral("确认删除"),
            QStringLiteral("确定要删除应用「%1」吗？\n\n将关闭正在运行的进程并删除磁盘文件。").arg(app.name),
            QMessageBox::Ok | QMessageBox::Cancel,
            QMessageBox::Cancel,
            {
                { QMessageBox::Ok, QStringLiteral("确定") },
                { QMessageBox::Cancel, QStringLiteral("取消") },
            }) != QMessageBox::Ok) {
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
    setupUnifiedProgressDialog(fetchProgress, QStringLiteral("正在获取历史版本列表..."), true, true);
    fetchProgress.show();
    QApplication::processEvents();

    QJsonObject histMeta = m_service.fetchHistoryVersions(appId, 10000, [&fetchProgress]() {
        return fetchProgress.wasCanceled();
    });
    const bool fetchCanceled = fetchProgress.wasCanceled();
    fetchProgress.close();
    if (fetchCanceled) {
        logToFile(QStringLiteral("[%1] 获取历史版本已取消").arg(app.name));
        showAdaptiveMessageBox(this,
                               QMessageBox::Information,
                               QStringLiteral("已取消"),
                               QStringLiteral("已取消步骤：获取历史版本列表。\n\n接下来：可稍后重新点击“下载历史版本”继续。"),
                               QMessageBox::Ok,
                               QMessageBox::Ok);
        return;
    }
    QJsonArray versions = histMeta.value(QStringLiteral("versions")).toArray();

    if (versions.isEmpty()) {
        showAdaptiveMessageBox(this, QMessageBox::Information,
                               QStringLiteral("历史版本"),
                               QStringLiteral("应用 [%1] 暂无可用历史版本。\n。").arg(app.name),
                               QMessageBox::Ok, QMessageBox::Ok);
        logToFile(QStringLiteral("[%1] 没有可用的历史版本").arg(app.name));
        return;
    }

    // 预先收集已下载的历史版本 ID
    QSet<QString> downloadedIds;
    for (auto it = m_appById.constBegin(); it != m_appById.constEnd(); ++it) {
        if (it.value().isHistoryVersion) {
            downloadedIds.insert(it.key());
        }
    }

    // 显示历史版本选择对话框
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("历史版本 - %1").arg(app.name));
    dlg.setFixedSize(640, 420);

    auto *dlgLayout = new QVBoxLayout(&dlg);
    dlgLayout->setContentsMargins(18, 14, 18, 14);
    dlgLayout->setSpacing(10);

    auto *topLabel = new QLabel(
        QStringLiteral("<b>%1</b> 的历史版本列表（当前：v%2）")
            .arg(app.name)
            .arg(m_service.appCurrentVersion(app)), &dlg);
    dlgLayout->addWidget(topLabel);

    auto *table = new QTableWidget(versions.size(), 3, &dlg);
    table->setHorizontalHeaderLabels({
        QStringLiteral("版本号"),
        QStringLiteral("名称"),
        QStringLiteral("状态")
    });
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setDefaultSectionSize(28);
    table->setAlternatingRowColors(true);

    // 倒序展示（最新历史在最上方）
    for (int i = 0; i < versions.size(); ++i) {
        const int row = versions.size() - 1 - i;
        QJsonObject vo = versions.at(row).toObject();
        const QString ver  = vo.value(QStringLiteral("version")).toString();
        const QString histId = appId + QStringLiteral("_") + QString(ver).replace('.', '_');
        const bool alreadyDl = downloadedIds.contains(histId);

        table->setItem(i, 0, new QTableWidgetItem(QStringLiteral("v") + ver));
        table->setItem(i, 1, new QTableWidgetItem(app.name));
        auto *statusItem = new QTableWidgetItem(alreadyDl ? QStringLiteral("[V] 已下载") : QStringLiteral("未下载"));
        if (alreadyDl) {
            statusItem->setForeground(QColor(22, 163, 74));
        }
        table->setItem(i, 2, statusItem);
        table->item(i, 0)->setData(Qt::UserRole, versions.at(row));
        table->item(i, 0)->setData(Qt::UserRole + 1, alreadyDl);
    }
    table->resizeColumnsToContents();
    dlgLayout->addWidget(table);

    auto *hint = new QLabel(QStringLiteral("点击下载历史版本，多版本可共存。"), &dlg);
    hint->setStyleSheet(QStringLiteral("color: #6b7280; font-size: 12px;"));
    dlgLayout->addWidget(hint);

    auto *btnBox = new QDialogButtonBox(&dlg);
    auto *btnDl = btnBox->addButton(QStringLiteral("下载"), QDialogButtonBox::AcceptRole);
    btnDl->setEnabled(false);
    btnBox->addButton(QDialogButtonBox::Cancel);
    connect(table, &QTableWidget::itemSelectionChanged, [table, btnDl]() {
        const int row = table->currentRow();
        btnDl->setEnabled(row >= 0 && table->item(row, 0));
    });
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlgLayout->addWidget(btnBox);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const int selRow = table->currentRow();
    if (selRow < 0) {
        return;
    }
    // 获取选择的版本信息
    QJsonObject selected = table->item(selRow, 0)->data(Qt::UserRole).toJsonObject();
    QString version = selected.value(QStringLiteral("version")).toString();
    QUrl downloadUrl(selected.value(QStringLiteral("downloadUrl")).toString());
    const bool alreadyDownloaded = table->item(selRow, 0)->data(Qt::UserRole + 1).toBool();

    if (alreadyDownloaded) {
        showAdaptiveMessageBox(this, QMessageBox::Information,
                               QStringLiteral("当前版本已存在"),
                               QStringLiteral("%1 v%2 版本已存在。").arg(app.name, version),
                               QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

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
                               false,
                               true);
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
            },
            [&dlProgress]() {
                return dlProgress.wasCanceled();
            })) {
        const bool downloadCanceled = dlProgress.wasCanceled();
        dlProgress.close();
        if (downloadCanceled) {
            showAdaptiveMessageBox(this,
                                   QMessageBox::Information,
                                   QStringLiteral("已取消"),
                                   QStringLiteral("已取消步骤：下载历史版本文件。\n\n接下来：可重新选择该版本并再次下载。"),
                                   QMessageBox::Ok,
                                   QMessageBox::Ok);
        }
        logToFile(QStringLiteral("[%1] %2: %3")
                      .arg(app.name,
                           downloadCanceled ? QStringLiteral("下载已取消") : QStringLiteral("下载失败"),
                           err));
        return;
    }
    dlProgress.setValue(100);
    dlProgress.close();

    // 添加为历史版本应用（标记 isLocalApp + isHistoryVersion，不参与在线更新检测）
    AppConfig newApp;
    newApp.id = appId + QStringLiteral("_") + QString(version).replace('.', '_');
    newApp.name = QStringLiteral("%1 (v%2)").arg(app.name, version);
    newApp.exeRelativePath = QDir(m_service.appsRoot()).relativeFilePath(targetPath);
    newApp.isLocalApp = true;
    newApp.isHistoryVersion = true;
    newApp.allowMultiInstance = app.allowMultiInstance;

    // 若同版本已下载，直接提示
    if (m_appById.contains(newApp.id)) {
        showAdaptiveMessageBox(this, QMessageBox::Information,
                               QStringLiteral("已存在"),
                               QStringLiteral("%1 v%2 已在应用列表中。").arg(app.name, version),
                               QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    m_service.addAppEntry(newApp);
    m_appById.insert(newApp.id, newApp);

    if (m_service.saveConfig(err)) {
        logToFile(QStringLiteral("[%1] 历史版本 v%2 已添加到应用列表").arg(app.name, version));
        refreshAppIcons();
    } else {
        logToFile(QStringLiteral("保存配置失败: %1").arg(err));
    }
}

// ============================================================
// 下载远程应用
// ============================================================

void MainWindow::updateDownloadItemBadge(const QString &appId, const QString &badge)
{
    if (!m_appList) return;
    for (int i = 0; i < m_appList->count(); ++i) {
        QListWidgetItem *it = m_appList->item(i);
        if (it && it->data(Qt::UserRole).toString() == appId
                && it->data(Qt::UserRole + 1).toBool()) {
            it->setData(Qt::UserRole + 3, badge);
            break;
        }
    }
}

void MainWindow::onDownloadRemoteApp(const QString &appId)
{
    if (!m_remoteCatalog.contains(appId)) {
        logToFile(QStringLiteral("未找到远程应用信息: %1").arg(appId));
        return;
    }
    // 已在队列中（排队或下载中）则忽略重复点击
    if (m_downloadQueuedSet.contains(appId)) {
        return;
    }
    m_downloadQueuedSet.insert(appId);
    m_downloadQueue.enqueue(appId);

    updateDownloadItemBadge(appId, QStringLiteral("排队"));

    const int queued = m_downloadQueue.size();
    if (m_updateCheckHintLabel) {
        m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(
            queued == 1
                ? QStringLiteral("已加入下载队列，将自动开始下载。")
                : QStringLiteral("已加入下载队列，共 %1 个应用待下载。").arg(queued)));
    }

    processDownloadQueue();
}

void MainWindow::processDownloadQueue()
{
    if (m_isCheckingUpdates || m_downloadQueue.isEmpty()) {
        return;
    }
    const QString appId = m_downloadQueue.dequeue();
    // 注意：m_downloadQueuedSet 中保留该 id 直到下载完成再移除，防止重复入队
    updateDownloadItemBadge(appId, QStringLiteral("下载中"));

    doDownloadRemoteApp(appId);

    // 下载完成（doDownloadRemoteApp 同步锁定），继续处理队列中下一个
    processDownloadQueue();
}

void MainWindow::doDownloadRemoteApp(const QString &appId)
{
    if (!m_remoteCatalog.contains(appId)) {
        logToFile(QStringLiteral("未找到远程应用信息: %1").arg(appId));
        m_downloadQueuedSet.remove(appId);
        return;
    }

    QJsonObject appInfo = m_remoteCatalog.value(appId);
    const QString appName = appInfo.value(QStringLiteral("appName")).toString();
    const QString pkgFile = appInfo.value(QStringLiteral("packageFileName")).toString();
    const QUrl downloadUrl(appInfo.value(QStringLiteral("downloadUrl")).toString());
    const QString subDir  = appInfo.value(QStringLiteral("subDir")).toString().trimmed();
    const bool allowMultiInstance = appInfo.value(QStringLiteral("allowMultiInstance")).toBool(false);

    // 保存路径：根据服务端配置的 subDir 决定存放位置
    QString targetDir = m_service.appsRoot();
    QString exeRel = pkgFile;
    if (!subDir.isEmpty()) {
        targetDir = QDir(m_service.appsRoot()).absoluteFilePath(subDir);
        QDir().mkpath(targetDir);
        exeRel = subDir + QStringLiteral("/") + pkgFile;
    }
    const QString targetPath = QDir(targetDir).absoluteFilePath(pkgFile);

    // 锁定 UI，显示内联进度
    m_isCheckingUpdates = true;
    if (m_checkUpdatesButton) { setRefreshButtonBusyState(m_checkUpdatesButton, m_refreshMovie, true); }
    if (m_updateAllButton)    m_updateAllButton->setEnabled(false);
    if (m_updateCheckProgress) {
        m_updateCheckProgress->setVisible(true);
        m_updateCheckProgress->setRange(0, 100);
        m_updateCheckProgress->setValue(0);
    }
    auto setHint = [this](const QString &msg) {
        if (m_updateCheckHintLabel)
            m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(msg));
    };
    auto restoreUi = [this, &setHint, appId](const QString &finalMsg) {
        m_isCheckingUpdates = false;
        m_downloadQueuedSet.remove(appId);   // 下载完成，从集合移除
        if (m_checkUpdatesButton) { setRefreshButtonBusyState(m_checkUpdatesButton, m_refreshMovie, false); }
        if (m_updateAllButton)    m_updateAllButton->setEnabled(true);
        if (m_updateCheckProgress) m_updateCheckProgress->setVisible(false);
        setHint(finalMsg);
        refreshUpdateActions();
    };

    setHint(QStringLiteral("正在下载 %1...").arg(appName));
    logToFile(QStringLiteral("正在下载: %1...").arg(appName));
    QApplication::processEvents();

    QString err;
    if (!m_service.downloadToFile(
            downloadUrl, targetPath, err, 30000,
            [this](qint64 received, qint64 total) {
                if (m_updateCheckProgress && total > 0) {
                    m_updateCheckProgress->setValue(
                        qBound(0, static_cast<int>(received * 65 / total), 65));
                }
                QApplication::processEvents();
            },
            [this, appName](const QString &status) {
                if (m_updateCheckHintLabel)
                    m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(
                        QStringLiteral("下载 %1: %2").arg(appName, status)));
                QApplication::processEvents();
            },
            []() -> bool { return false; })) {
        logToFile(QStringLiteral("[%1] 下载失败: %2").arg(appName, err));
        restoreUi(QStringLiteral("下载 %1 失败：%2").arg(appName, err));
        return;
    }

    if (m_updateCheckProgress) m_updateCheckProgress->setValue(65);
    setHint(QStringLiteral("下载完成，正在检测依赖信息..."));
    QApplication::processEvents();

    // 添加到配置
    AppConfig newApp;
    newApp.id = appId;
    newApp.name = appName;
    newApp.exeRelativePath = exeRel;
    newApp.allowMultiInstance = allowMultiInstance;
    {
        QUrl baseUrl(m_service.serverBaseUrl().trimmed());
        QString path = baseUrl.path();
        if (!path.endsWith('/')) path += '/';
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

    // 依赖检测
    const OnlineAppInfo online = m_service.checkOnlineInfo(newApp, 10000, []() { return false; });
    m_onlineCache.insert(newApp.id, online);
    if (m_updateCheckProgress) m_updateCheckProgress->setValue(72);
    QApplication::processEvents();

    if (online.requestSuccess && !online.requiredFiles.isEmpty()) {
        logToFile(QStringLiteral("[%1] 正在检测依赖文件完整性...").arg(appName));
        setHint(QStringLiteral("正在修复 %1 的依赖文件...").arg(appName));
        QApplication::processEvents();

        QString depResult;
        m_service.checkAndFixDependencies(
            newApp, online, depResult, 180000,
            [this](qint64 received, qint64 total) {
                if (m_updateCheckProgress && total > 0) {
                    m_updateCheckProgress->setValue(
                        72 + qBound(0, static_cast<int>(received * 20 / total), 20));
                }
                QApplication::processEvents();
            },
            [this, appName](const QString &status) {
                if (m_updateCheckHintLabel)
                    m_updateCheckHintLabel->setText(composeLaunchAndUpdateHint(
                        QStringLiteral("[%1] %2").arg(appName, status)));
                logToFile(QStringLiteral("[%1] %2").arg(appName, status));
                QApplication::processEvents();
            },
            [this](int installPct) {
                if (m_updateCheckProgress)
                    m_updateCheckProgress->setValue(92 + installPct * 8 / 100);
                QApplication::processEvents();
            },
            []() -> bool { return false; });
        logToFile(QStringLiteral("[%1] %2").arg(appName,
            depResult.trimmed().isEmpty() ? QStringLiteral("依赖处理完成") : depResult.trimmed()));
    }

    if (m_updateCheckProgress) m_updateCheckProgress->setValue(100);
    QApplication::processEvents();

    refreshAppIcons();
    restoreUi(QStringLiteral("%1 已下载安装完成。").arg(appName));
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
        showAdaptiveMessageBox(this,
                               QMessageBox::Information,
                               QStringLiteral("版本检查"),
                               QStringLiteral("检查更新失败: %1").arg(online.errorMessage),
                               QMessageBox::Ok,
                               QMessageBox::Ok);
        return;
    }

    const QString currentVersion = m_service.appManagerVersion();
    const QString latestVersion = online.latestVersion;
    
    logToFile(QStringLiteral("[AppManager] 当前版本: %1，最新版本: %2").arg(currentVersion, latestVersion));

    if (compareVersions(currentVersion, latestVersion) >= 0) {
        showAdaptiveMessageBox(this,
                               QMessageBox::Information,
                               QStringLiteral("版本检查"),
                               QStringLiteral("AppManager 已是最新版本 (v%1)").arg(currentVersion),
                               QMessageBox::Ok,
                               QMessageBox::Ok);
        return;
    }

    // 提示用户升级
    QMessageBox::StandardButton ret = showAdaptiveMessageBox(
        this,
        QMessageBox::Question,
        QStringLiteral("发现新版本"),
        QStringLiteral("发现 AppManager 新版本 v%1\n\n当前版本: v%2\n\n是否立即升级?")
            .arg(latestVersion, currentVersion),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);

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
            showAdaptiveMessageBox(this,
                                   QMessageBox::Warning,
                                   QStringLiteral("升级失败"),
                                   result,
                                   QMessageBox::Ok,
                                   QMessageBox::Ok);
        } else {
            dlProgress.setValue(100);
            dlProgress.close();
            logToFile(QStringLiteral("[AppManager] %1").arg(result));
            showAdaptiveMessageBox(this,
                                   QMessageBox::Information,
                                   QStringLiteral("升级进行中"),
                                   QStringLiteral("[AppManager] %1").arg(result),
                                   QMessageBox::Ok,
                                   QMessageBox::Ok);
            
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
void MainWindow::onLoginClicked()
{
    if (!m_authToken.isEmpty()) {
        // 已登录 → 注销
        onLogoutClicked();
        return;
    }

    // 未登录 → 弹出登录对话框
    LoginDialog dlg(m_service.serverBaseUrl(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    m_authToken = dlg.token();
    m_authUser  = dlg.username();

    QSettings settings(QStringLiteral("Software\\AppManager\\AppManager"), QSettings::NativeFormat);
    settings.setValue(QStringLiteral("authToken"), m_authToken);
    settings.setValue(QStringLiteral("authUser"),  m_authUser);

    if (m_loginButton) {
        m_loginButton->setIcon(QIcon(QStringLiteral(":/resources/logout.ico")));
        const QString displayName = m_authUser.isEmpty() ? QStringLiteral("已登录") : m_authUser;
        m_loginButton->setText(displayName);
        m_loginButton->setToolTip(QStringLiteral("已登录为 %1，点击注销").arg(m_authUser));
    }

    fetchRemoteCatalog();
    refreshAppIcons();
}

void MainWindow::onLogoutClicked()
{
    m_authToken.clear();
    m_authUser.clear();
    QSettings settings(QStringLiteral("Software\\AppManager\\AppManager"), QSettings::NativeFormat);
    settings.remove(QStringLiteral("authToken"));
    settings.remove(QStringLiteral("authUser"));
    if (m_loginButton) {
        m_loginButton->setIcon(QIcon(QStringLiteral(":/resources/login.ico")));
        m_loginButton->setText(QStringLiteral("登录"));
        m_loginButton->setToolTip(QStringLiteral("点击登录"));
    }
    fetchRemoteCatalog();
    refreshAppIcons();
}