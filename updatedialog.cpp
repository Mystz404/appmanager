#include "updatedialog.h"
#include "versionutils.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QMessageBox>
#include <QProcess>
#include <QVBoxLayout>

namespace {
constexpr int kUnifiedProgressBarWidth = 420;
constexpr int kUnifiedProgressBarHeight = 18;

void applyUnifiedProgressBarStyle(QProgressBar *bar)
{
    if (bar == nullptr) {
        return;
    }

    bar->setMinimumWidth(kUnifiedProgressBarWidth);
    bar->setMaximumWidth(kUnifiedProgressBarWidth);
    bar->setMinimumHeight(kUnifiedProgressBarHeight);
    bar->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 1px solid #cbd5e1; border-radius: 5px; background: #ffffff; text-align: center; }"
        "QProgressBar::chunk { background: #2563eb; border-radius: 4px; }"));
}
}

#ifdef Q_OS_WIN
#include <Windows.h>
#include <TlHelp32.h>
#endif

// ============================================================
// 构造 / 生命周期
// ============================================================

UpdateDialog::UpdateDialog(AppManagerService *service,
                           const QVector<AppConfig> &apps,
                           QWidget *parent)
    : QDialog(parent)
    , m_service(service)
    , m_apps(apps)
{
    setWindowTitle(QStringLiteral("在线升级"));
    setMinimumSize(620, 480);
    resize(720, 560);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    buildUi();
}

void UpdateDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!m_phaseStarted) {
        m_phaseStarted = true;
        // 延迟启动检测阶段，确保窗口已完整绘制。
        QTimer::singleShot(0, this, &UpdateDialog::runCheckPhase);
    }
}

void UpdateDialog::reject()
{
    m_canceled = true;
    QDialog::reject();
}

// ============================================================
// UI 构建
// ============================================================

void UpdateDialog::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    m_stack = new QStackedWidget(this);
    rootLayout->addWidget(m_stack);

    m_stack->addWidget(createCheckPage());   // PageCheck   = 0
    m_stack->addWidget(createSelectPage());  // PageSelect  = 1
    m_stack->addWidget(createUpgradePage()); // PageUpgrade = 2
    m_stack->addWidget(createResultPage());  // PageResult  = 3

    m_stack->setCurrentIndex(PageCheck);
}

QWidget *UpdateDialog::createCheckPage()
{
    auto *page = new QWidget;
    auto *ly = new QVBoxLayout(page);
    ly->setContentsMargins(24, 28, 24, 20);

    auto *heading = new QLabel(QStringLiteral("正在检测在线版本..."), page);
    heading->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    ly->addWidget(heading);
    ly->addSpacing(10);

    m_checkLabel = new QLabel(QStringLiteral("准备连接服务器..."), page);
    ly->addWidget(m_checkLabel);

    m_checkProgress = new QProgressBar(page);
    m_checkProgress->setRange(0, qMax(m_apps.size(), 1));
    m_checkProgress->setValue(0);
    m_checkProgress->setTextVisible(true);
    applyUnifiedProgressBarStyle(m_checkProgress);
    ly->addWidget(m_checkProgress);

    ly->addStretch();

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *cancelBtn = new QPushButton(QStringLiteral("取消"), page);
    btnRow->addWidget(cancelBtn);
    ly->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        if (m_canceled) {
            return;
        }
        m_canceled = true;
        m_checkLabel->setText(QStringLiteral("正在取消检测，请稍候..."));
        emitLog(QStringLiteral("用户取消在线检测，正在中止当前请求"));
    });

    return page;
}

QWidget *UpdateDialog::createSelectPage()
{
    auto *page = new QWidget;
    auto *ly = new QVBoxLayout(page);
    ly->setContentsMargins(24, 20, 24, 16);

    auto *info = new QLabel(QStringLiteral("以下应用有新版本可用，请勾选需要升级的项目："), page);
    info->setWordWrap(true);
    info->setStyleSheet(QStringLiteral("font-size: 14px;"));
    ly->addWidget(info);
    ly->addSpacing(4);

    m_selectList = new QListWidget(page);
    m_selectList->setSelectionMode(QAbstractItemView::NoSelection);
    ly->addWidget(m_selectList, 1);

    auto *pickRow = new QHBoxLayout;
    auto *allBtn = new QPushButton(QStringLiteral("全选"), page);
    auto *noneBtn = new QPushButton(QStringLiteral("全不选"), page);
    pickRow->addWidget(allBtn);
    pickRow->addWidget(noneBtn);
    pickRow->addStretch();
    ly->addLayout(pickRow);

    auto *actRow = new QHBoxLayout;
    actRow->addStretch();
    auto *cancelBtn = new QPushButton(QStringLiteral("取消"), page);
    auto *startBtn = new QPushButton(QStringLiteral("开始升级"), page);
    startBtn->setStyleSheet(QStringLiteral("font-weight: bold;"));
    actRow->addWidget(cancelBtn);
    actRow->addSpacing(8);
    actRow->addWidget(startBtn);
    ly->addLayout(actRow);

    // 全选 / 全不选
    connect(allBtn, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < m_selectList->count(); ++i)
            m_selectList->item(i)->setCheckState(Qt::Checked);
    });
    connect(noneBtn, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < m_selectList->count(); ++i)
            m_selectList->item(i)->setCheckState(Qt::Unchecked);
    });

    // 取消 → 关闭对话框
    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        emitLog(QStringLiteral("用户取消执行升级"));
        reject();
    });

    // 开始升级 → 收集勾选项，进入升级阶段
    connect(startBtn, &QPushButton::clicked, this, [this]() {
        QList<int> selected;
        for (int i = 0; i < m_selectList->count(); ++i) {
            auto *item = m_selectList->item(i);
            if (item->checkState() == Qt::Checked) {
                selected.push_back(item->data(Qt::UserRole).toInt());
            }
        }
        if (selected.isEmpty()) {
            emitLog(QStringLiteral("用户未选择任何可更新应用"));
            showResultPhase(QStringLiteral("提示"),
                           QStringLiteral("未选择任何应用，已取消升级。"));
            return;
        }
        m_totalToUpgrade = selected.size();
        runUpgradePhase(selected);
    });

    return page;
}

QWidget *UpdateDialog::createUpgradePage()
{
    auto *page = new QWidget;
    auto *ly = new QVBoxLayout(page);
    ly->setContentsMargins(24, 28, 24, 20);

    auto *heading = new QLabel(QStringLiteral("正在执行升级..."), page);
    heading->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    ly->addWidget(heading);
    ly->addSpacing(10);

    m_upgradeLabel = new QLabel(QStringLiteral("准备中..."), page);
    m_upgradeLabel->setWordWrap(true);
    ly->addWidget(m_upgradeLabel);

    m_upgradeProgress = new QProgressBar(page);
    m_upgradeProgress->setRange(0, 100);
    m_upgradeProgress->setValue(0);
    m_upgradeProgress->setTextVisible(true);
    applyUnifiedProgressBarStyle(m_upgradeProgress);
    ly->addWidget(m_upgradeProgress);

    ly->addStretch();

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *cancelBtn = new QPushButton(QStringLiteral("取消"), page);
    btnRow->addWidget(cancelBtn);
    ly->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        if (m_canceled) {
            return;
        }
        m_canceled = true;
        m_upgradeLabel->setText(QStringLiteral("正在取消升级，请稍候..."));
        emitLog(QStringLiteral("用户取消升级，正在中止当前任务"));
    });

    return page;
}

QWidget *UpdateDialog::createResultPage()
{
    auto *page = new QWidget;
    auto *ly = new QVBoxLayout(page);
    ly->setContentsMargins(24, 24, 24, 16);

    m_resultTitle = new QLabel(page);
    m_resultTitle->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    ly->addWidget(m_resultTitle);
    ly->addSpacing(4);

    m_resultDetail = new QLabel(page);
    m_resultDetail->setWordWrap(true);
    ly->addWidget(m_resultDetail);

    ly->addStretch();

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), page);
    btnRow->addWidget(closeBtn);
    ly->addLayout(btnRow);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    return page;
}

// ============================================================
// 阶段 1：在线检测
// ============================================================

void UpdateDialog::runCheckPhase()
{
    m_canceled = false;
    m_checkProgress->setMaximum(m_apps.size());

    for (int i = 0; i < m_apps.size(); ++i) {
        if (m_canceled) {
            break;
        }

        const AppConfig &app = m_apps.at(i);
        m_checkLabel->setText(QStringLiteral("正在检测 [%1/%2] %3")
                                  .arg(i + 1)
                                  .arg(m_apps.size())
                                  .arg(app.name));
        QApplication::processEvents();

        const OnlineAppInfo online = m_service->checkOnlineInfo(
            app,
            10000,
            [this]() { return m_canceled; });
        if (m_canceled) {
            m_checkProgress->setValue(i + 1);
            QApplication::processEvents();
            break;
        }
        m_onlineCache.insert(app.id, online);

        if (!online.requestSuccess) {
            emitLog(QStringLiteral("[%1] 在线检测失败: %2").arg(app.name, online.errorMessage));
            ++m_serverFailCount;
        } else {
            ++m_serverSuccessCount;
            const QString currentVersion = m_service->appCurrentVersion(app);
            const bool hasUpdate = compareVersions(currentVersion, online.latestVersion) < 0;
            if (hasUpdate) {
                m_updatableIndexes.push_back(i);
                emitLog(QStringLiteral("[%1] 检测到新版本: %2 -> %3")
                            .arg(app.name, currentVersion, online.latestVersion));
            } else {
                emitLog(QStringLiteral("[%1] 已是最新版本: %2").arg(app.name, currentVersion));
            }
        }

        m_checkProgress->setValue(i + 1);
        QApplication::processEvents();
    }

    if (m_canceled) {
        emitLog(QStringLiteral("检测升级操作被取消"));
        showResultPhase(
            QStringLiteral("检测已取消"),
            QStringLiteral("已取消在线检测。\n如需继续，请重新检测。"));
        return;
    }

    emitLog(QStringLiteral("在线检测完成，可升级应用数: %1 / %2")
                .arg(m_updatableIndexes.size())
                .arg(m_apps.size()));

    if (m_updatableIndexes.isEmpty()) {
        showResultPhase(QStringLiteral("检测完成"),
                       QStringLiteral("所有应用已是最新版本（共检测 %1 个应用）。")
                           .arg(m_apps.size()));
        return;
    }

    showSelectPhase();
}

// ============================================================
// 阶段 2：用户选择
// ============================================================

void UpdateDialog::showSelectPhase()
{
    m_selectList->clear();

    for (int idx : m_updatableIndexes) {
        const AppConfig &app = m_apps.at(idx);
        const QString currentVersion = m_service->appCurrentVersion(app);
        const OnlineAppInfo &online = m_onlineCache.value(app.id);

        auto *item = new QListWidgetItem(
            QStringLiteral("%1    %2 -> %3").arg(app.name, currentVersion, online.latestVersion),
            m_selectList);
        item->setData(Qt::UserRole, idx);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }

    m_stack->setCurrentIndex(PageSelect);
}

// ============================================================
// 阶段 3：执行升级
// ============================================================

void UpdateDialog::runUpgradePhase(const QList<int> &selectedIndexes)
{
    m_stack->setCurrentIndex(PageUpgrade);
    m_upgradeProgress->setRange(0, selectedIndexes.size() * 100);
    m_upgradeProgress->setValue(0);
    m_canceled = false;
    m_upgradeSuccessCount = 0;
    m_closedApps.clear();
    bool wasCanceled = false;

    for (int i = 0; i < selectedIndexes.size(); ++i) {
        if (m_canceled) {
            emitLog(QStringLiteral("升级操作被用户取消，已处理 %1 / %2")
                        .arg(i)
                        .arg(selectedIndexes.size()));
            wasCanceled = true;
            break;
        }

        const AppConfig &app = m_apps.at(selectedIndexes.at(i));
        const OnlineAppInfo &online = m_onlineCache.value(app.id);
        const QString exePath = m_service->appAbsoluteExePath(app);

        // 检测目标程序是否正在运行
        if (isProcessRunning(exePath)) {
            const int ret = QMessageBox::question(
                this,
                QStringLiteral("程序正在运行"),
                QStringLiteral("%1 正在运行，需要关闭后才能升级。\n是否关闭程序并继续升级？").arg(app.name),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);
            if (ret == QMessageBox::Yes) {
                if (terminateProcess(exePath)) {
                    m_closedApps.append(exePath);
                    emitLog(QStringLiteral("[%1] 已自动关闭程序，准备升级").arg(app.name));
                } else {
                    emitLog(QStringLiteral("[%1] 无法关闭程序，跳过升级").arg(app.name));
                    m_upgradeProgress->setValue((i + 1) * 100);
                    continue;
                }
            } else {
                emitLog(QStringLiteral("[%1] 用户拒绝关闭程序，跳过升级").arg(app.name));
                m_upgradeProgress->setValue((i + 1) * 100);
                continue;
            }
        }

        // 检查依赖文件完整性（服务端指定的文件列表）
        if (!online.requiredFiles.isEmpty()) {
            m_upgradeLabel->setText(QStringLiteral("正在检查依赖文件 [%1/%2] %3")
                                        .arg(i + 1)
                                        .arg(selectedIndexes.size())
                                        .arg(app.name));
            QApplication::processEvents();

            QString depResult;
            const int baseProgress = i * 100;
            bool depOk = false;
            bool depForceRedownload = false;
            bool skipByDepFailure = false;
            while (true) {
                depOk = m_service->checkAndFixDependencies(
                    app, online, depResult, 180000,
                    // 下载进度
                    [&](qint64 received, qint64 total) {
                        if (total <= 0) return;
                        int pct = static_cast<int>((received * 100) / total);
                        pct = qBound(0, pct, 100);
                        m_upgradeProgress->setValue(baseProgress + (pct * 30) / 100);
                        QApplication::processEvents();
                    },
                    // 状态文本
                    [&](const QString &status) {
                        m_upgradeLabel->setText(QStringLiteral("依赖修复 [%1/%2] %3\n%4")
                                                    .arg(i + 1)
                                                    .arg(selectedIndexes.size())
                                                    .arg(app.name, compactStatus(status)));
                        emitLog(QStringLiteral("[%1] %2").arg(app.name, status));
                        QApplication::processEvents();
                    },
                    // 安装进度
                    [&](int installPct) {
                        m_upgradeProgress->setValue(baseProgress + (qBound(0, installPct, 100) * 30) / 100);
                        QApplication::processEvents();
                    },
                    // 取消检测
                    [this]() {
                        return m_canceled;
                    },
                    depForceRedownload);

                if (depOk || m_canceled) {
                    break;
                }

                emitLog(QStringLiteral("[%1] 依赖修复失败：%2").arg(app.name, depResult));

                QMessageBox box(this);
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(QStringLiteral("依赖修复失败"));
                box.setText(QStringLiteral("%1 依赖修复失败：\n%2").arg(app.name, depResult));
                auto *retryBtn = box.addButton(QStringLiteral("重试（使用已下载文件）"), QMessageBox::AcceptRole);
                auto *redownloadBtn = box.addButton(QStringLiteral("删除已下载文件并重下"), QMessageBox::DestructiveRole);
                auto *continueBtn = box.addButton(QStringLiteral("仍然继续升级"), QMessageBox::YesRole);
                auto *skipBtn = box.addButton(QStringLiteral("跳过该应用"), QMessageBox::RejectRole);
                box.setDefaultButton(qobject_cast<QPushButton *>(retryBtn));
                box.exec();

                if (box.clickedButton() == retryBtn) {
                    depForceRedownload = false;
                    continue;
                }
                if (box.clickedButton() == redownloadBtn) {
                    depForceRedownload = true;
                    continue;
                }
                if (box.clickedButton() == continueBtn) {
                    break;
                }

                skipByDepFailure = true;
                break;
            }

            if (!depOk) {
                if (m_canceled) {
                    emitLog(QStringLiteral("[%1] 依赖修复已取消").arg(app.name));
                    wasCanceled = true;
                    break;
                }
                if (skipByDepFailure) {
                    emitLog(QStringLiteral("[%1] 用户选择跳过（依赖修复失败）").arg(app.name));
                    m_upgradeProgress->setValue((i + 1) * 100);
                    continue;
                }
            }
        }

        m_upgradeLabel->setText(QStringLiteral("正在升级 [%1/%2] %3")
                                    .arg(i + 1)
                                    .arg(selectedIndexes.size())
                                    .arg(app.name));
        QApplication::processEvents();

        const int baseProgress = i * 100;
        QString result;
        bool ok = false;
        bool forceRedownload = false;
        while (true) {
            ok = m_service->upgradeApp(
                app, online, result, 180000,
                // 下载进度回调（整体 0–70%）
                [&](qint64 received, qint64 total) {
                    if (total <= 0) {
                        m_upgradeProgress->setValue(baseProgress);
                        return;
                    }
                    int pct = static_cast<int>((received * 100) / total);
                    pct = qBound(0, pct, 100);
                    m_upgradeProgress->setValue(baseProgress + (pct * 70) / 100);
                    QApplication::processEvents();
                },
                // 状态文本回调
                [&](const QString &status) {
                    const QString compact = compactStatus(status);
                    m_upgradeLabel->setText(QStringLiteral("正在升级 [%1/%2] %3\n%4")
                                                .arg(i + 1)
                                                .arg(selectedIndexes.size())
                                                .arg(app.name, compact));
                    emitLog(QStringLiteral("[%1] %2").arg(app.name, status));
                    QApplication::processEvents();
                },
                // 安装进度回调（整体 70–100%）
                [&](int installPct) {
                    const int p = qBound(0, installPct, 100);
                    m_upgradeProgress->setValue(baseProgress + 70 + (p * 30) / 100);
                    QApplication::processEvents();
                },
                [this]() {
                    return m_canceled;
                },
                forceRedownload);

            if (ok || m_canceled) {
                break;
            }

            emitLog(QStringLiteral("[%1] 升级失败：%2").arg(app.name, result));
            QMessageBox box(this);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle(QStringLiteral("升级失败"));
            box.setText(QStringLiteral("%1 升级失败：\n%2").arg(app.name, result));
            auto *retryBtn = box.addButton(QStringLiteral("重试（使用已下载文件）"), QMessageBox::AcceptRole);
            auto *redownloadBtn = box.addButton(QStringLiteral("删除已下载文件并重下"), QMessageBox::DestructiveRole);
            auto *skipBtn = box.addButton(QStringLiteral("跳过该应用"), QMessageBox::RejectRole);
            box.setDefaultButton(qobject_cast<QPushButton *>(retryBtn));
            box.exec();

            if (box.clickedButton() == retryBtn) {
                forceRedownload = false;
                continue;
            }
            if (box.clickedButton() == redownloadBtn) {
                forceRedownload = true;
                continue;
            }
            break;
        }

        m_upgradeProgress->setValue((i + 1) * 100);
        QApplication::processEvents();

        if (m_canceled) {
            emitLog(QStringLiteral("[%1] 升级已取消").arg(app.name));
            wasCanceled = true;
            break;
        }

        if (ok) {
            ++m_upgradeSuccessCount;
        }
        emitLog(QStringLiteral("[%1] %2").arg(app.name, result));
    }

    // 升级完成后重启先前自动关闭的程序
    relaunchClosedApps();

    // 展示结果
    const int total = selectedIndexes.size();
    const int failed = total - m_upgradeSuccessCount;

    if (wasCanceled) {
        showResultPhase(
            QStringLiteral("升级已中断"),
            QStringLiteral("升级被用户取消。\n成功: %1，总计: %2，未完成的任务已判定为失败。")
                .arg(m_upgradeSuccessCount)
                .arg(total));
    } else if (failed > 0) {
        showResultPhase(
            QStringLiteral("升级未全部成功"),
            QStringLiteral("成功: %1 / %2，失败: %3")
                .arg(m_upgradeSuccessCount)
                .arg(total)
                .arg(failed));
    } else {
        showResultPhase(
            QStringLiteral("升级完成"),
            QStringLiteral("全部升级成功！\n成功: %1 / %2")
                .arg(m_upgradeSuccessCount)
                .arg(total));
    }
}

// ============================================================
// 阶段 4：结果展示
// ============================================================

void UpdateDialog::showResultPhase(const QString &title, const QString &detail)
{
    m_resultTitle->setText(title);
    m_resultDetail->setText(detail);

    m_stack->setCurrentIndex(PageResult);
}

// ============================================================
// 工具方法
// ============================================================

void UpdateDialog::emitLog(const QString &msg)
{
    m_logEntries.push_back(msg);
    emit logMessage(msg);
}

QString UpdateDialog::compactStatus(const QString &text, int maxLen)
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

// ============================================================
// 进程管理
// ============================================================

bool UpdateDialog::isProcessRunning(const QString &exePath) const
{
#ifdef Q_OS_WIN
    const QString exeName = QFileInfo(exePath).fileName();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (exeName.compare(QString::fromWCharArray(pe.szExeFile), Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    Q_UNUSED(exePath)
    return false;
#endif
}

bool UpdateDialog::terminateProcess(const QString &exePath)
{
#ifdef Q_OS_WIN
    const QString exeName = QFileInfo(exePath).fileName();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool allOk = true;
    bool anyFound = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (exeName.compare(QString::fromWCharArray(pe.szExeFile), Qt::CaseInsensitive) == 0) {
                anyFound = true;
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    if (TerminateProcess(hProc, 0)) {
                        // 等待进程完全退出，最多 5 秒
                        WaitForSingleObject(hProc, 5000);
                    } else {
                        allOk = false;
                    }
                    CloseHandle(hProc);
                } else {
                    allOk = false;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return anyFound && allOk;
#else
    Q_UNUSED(exePath)
    return false;
#endif
}

void UpdateDialog::relaunchClosedApps()
{
    if (m_closedApps.isEmpty()) {
        return;
    }
    for (const QString &exePath : qAsConst(m_closedApps)) {
        const bool ok = QProcess::startDetached(exePath, {});
        const QString name = QFileInfo(exePath).completeBaseName();
        if (ok) {
            emitLog(QStringLiteral("[%1] 升级完成，已自动重新启动").arg(name));
        } else {
            emitLog(QStringLiteral("[%1] 升级完成，自动重启失败").arg(name));
        }
    }
    m_closedApps.clear();
}
