#include "docbrowserpage.h"
#include "refreshbuttonutils.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QMovie>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

// ============================================================
// 文档卡片代理（仿 AppItemDelegate 风格）
// ============================================================
class DocItemDelegate : public QStyledItemDelegate
{
public:
    explicit DocItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    struct DocTypeColor {
        QColor bg;      // 卡片背景（浅色）
        QColor border;  // 描边（略深）
        QColor hoverBg; // 悬停背景
        QColor selBg;   // 选中背景
        QColor selBorder;// 选中描边
        QColor iconBg;  // 图标背景
        QColor iconLine; // 图标线条
    };

    static DocTypeColor colorForExt(const QString &ext)
    {
        const QString e = ext.toLower();
        if (e == QStringLiteral("pdf"))
            return { QColor(254,242,242), QColor(252,165,165), QColor(254,226,226),
                     QColor(254,202,202), QColor(239,68,68),
                     QColor(254,226,226), QColor(239,68,68) };
        if (e == QStringLiteral("doc") || e == QStringLiteral("docx"))
            return { QColor(239,246,255), QColor(147,197,253), QColor(219,234,254),
                     QColor(191,219,254), QColor(59,130,246),
                     QColor(219,234,254), QColor(59,130,246) };
        if (e == QStringLiteral("xls") || e == QStringLiteral("xlsx"))
            return { QColor(236,253,245), QColor(134,239,172), QColor(220,252,231),
                     QColor(187,247,208), QColor(34,197,94),
                     QColor(220,252,231), QColor(34,197,94) };
        if (e == QStringLiteral("ppt") || e == QStringLiteral("pptx"))
            return { QColor(255,247,237), QColor(253,186,116), QColor(254,243,199),
                     QColor(253,230,138), QColor(245,158,11),
                     QColor(254,243,199), QColor(245,158,11) };
        if (e == QStringLiteral("txt") || e == QStringLiteral("md"))
            return { QColor(249,250,251), QColor(209,213,219), QColor(243,244,246),
                     QColor(229,231,235), QColor(107,114,128),
                     QColor(243,244,246), QColor(107,114,128) };
        if (e == QStringLiteral("zip") || e == QStringLiteral("rar") || e == QStringLiteral("7z"))
            return { QColor(250,245,255), QColor(196,181,253), QColor(243,232,255),
                     QColor(233,213,255), QColor(139,92,246),
                     QColor(243,232,255), QColor(139,92,246) };
        // 默认：青色系
        return { QColor(236,254,255), QColor(165,243,252), QColor(207,250,254),
                 QColor(165,243,252), QColor(6,182,212),
                 QColor(207,250,254), QColor(6,182,212) };
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(220, 90);
    }

    // 右上角 "···" 按钮区域（改为卡片右侧居中或靠上）
    static QRect moreButtonRect(const QRect &itemRect)
    {
        return QRect(itemRect.right() - 32, itemRect.top() + (itemRect.height() - 24) / 2, 24, 24);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect r = option.rect.adjusted(2, 2, -2, -2);
        const bool hovered  = (option.state & QStyle::State_MouseOver) != 0;
        const bool selected = (option.state & QStyle::State_Selected) != 0;

        // 根据文档类型确定卡片颜色
        const QString docExt = index.data(Qt::UserRole + 13).toString();
        const DocTypeColor tc = colorForExt(docExt);

        const QColor bg = selected ? tc.selBg
                        : (hovered ? tc.hoverBg : tc.bg);
        const QColor border = selected ? tc.selBorder
                             : (hovered ? tc.border : tc.border.lighter(120));
        painter->setPen(QPen(border, 1.5));
        painter->setBrush(bg);
        painter->drawRoundedRect(r, 8, 8);

        // 新版小文档图标 (左侧)
        const int iconSize = 36;
        const QRect iconRect(r.left() + 12, r.top() + (r.height() - iconSize) / 2, iconSize, iconSize);

        const QString badge = index.data(Qt::UserRole + 12).toString();

        if (badge == QStringLiteral("下载")) {
            // 未下载状态：显示下载图标（使用类型对应色调）
            painter->setPen(Qt::NoPen);
            painter->setBrush(tc.iconBg);
            painter->drawRoundedRect(iconRect, 6, 6);
            
            painter->setPen(QPen(tc.iconLine, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            int cx = iconRect.center().x();
            int cy = iconRect.center().y();
            // 向下箭头
            painter->drawLine(cx, cy - 6, cx, cy + 4);
            painter->drawLine(cx - 5, cy - 1, cx, cy + 4);
            painter->drawLine(cx + 5, cy - 1, cx, cy + 4);
            // 底部横线
            painter->drawLine(cx - 6, cy + 8, cx + 6, cy + 8);
        } else {
            // 已下载或有更新：显示文档图标（使用类型对应色调）
            QRect docR = iconRect.adjusted(3, 2, -3, -2);
            painter->setPen(QPen(tc.border, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(tc.iconBg);
            
            QPainterPath path;
            path.moveTo(docR.left(), docR.top());
            path.lineTo(docR.right() - 8, docR.top());
            path.lineTo(docR.right(), docR.top() + 8);
            path.lineTo(docR.right(), docR.bottom());
            path.lineTo(docR.left(), docR.bottom());
            path.closeSubpath();
            painter->drawPath(path);
            
            painter->drawLine(docR.right() - 8, docR.top(), docR.right() - 8, docR.top() + 8);
            painter->drawLine(docR.right() - 8, docR.top() + 8, docR.right(), docR.top() + 8);
            
            painter->setPen(QPen(tc.iconLine, 1.5, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(docR.left() + 6, docR.top() + 14, docR.right() - 6, docR.top() + 14);
            painter->drawLine(docR.left() + 6, docR.top() + 20, docR.right() - 6, docR.top() + 20);
            painter->drawLine(docR.left() + 6, docR.top() + 26, docR.right() - 10, docR.top() + 26);
        }

        // 标题 (完全显示，部分居中对齐，多行处理)
        const QString title = index.data(Qt::DisplayRole).toString();
        const QRect nameRect(r.left() + 58, r.top() + 8, r.width() - 92, 40);
        {
            QFont nf;
            nf.setPixelSize(13);
            nf.setBold(true);
            painter->setFont(nf);
            painter->setPen(selected ? QColor(30, 58, 138) : QColor(31, 41, 55));
            painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWrapAnywhere, title);
        }

        // 文档类型标签
        if (!docExt.isEmpty()) {
            const QString extLabel = docExt.toUpper();
            QFont ef;
            ef.setPixelSize(10);
            ef.setBold(true);
            painter->setFont(ef);
            QFontMetrics efm(ef);
            const int tw = efm.horizontalAdvance(extLabel) + 8;
            const QRect extRect(r.left() + 58, r.bottom() - 34, tw, 14);
            painter->setPen(Qt::NoPen);
            painter->setBrush(tc.border);
            painter->drawRoundedRect(extRect, 3, 3);
            painter->setPen(Qt::white);
            painter->drawText(extRect, Qt::AlignCenter, extLabel);
        }

        // 版本信息
        const QString version = index.data(Qt::UserRole + 11).toString();
        const QRect vRect(r.left() + 58, r.bottom() - 20, r.width() - 96, 16);
        if (!version.isEmpty()) {
            QFont vf;
            vf.setPixelSize(11);
            painter->setFont(vf);
            painter->setPen(QColor(107, 114, 128));
            painter->drawText(vRect, Qt::AlignLeft | Qt::AlignVCenter, version);
        }

        // 右下角角标：下载 / 有更新
        if (badge == QStringLiteral("下载") || badge == QStringLiteral("有更新")) {
            const bool isDownload = (badge == QStringLiteral("下载"));
            const QColor badgeBg  = isDownload ? QColor(219, 234, 254) : QColor(254, 226, 226);
            const QColor badgeFg  = isDownload ? QColor(29,  78, 216)  : QColor(185, 28,  28);

            QFont bf;
            bf.setPixelSize(11);
            bf.setBold(true);
            painter->setFont(bf);
            QFontMetrics fm(bf);
            const int bw = fm.horizontalAdvance(badge) + 10;
            const int bh = 17;
            const QRect bRect(r.right() - bw - 4, r.bottom() - bh - 4, bw, bh);
            painter->setPen(Qt::NoPen);
            painter->setBrush(badgeBg);
            painter->drawRoundedRect(bRect, 8, 8);
            painter->setPen(badgeFg);
            painter->drawText(bRect, Qt::AlignCenter, badge);
        }

        // ··· 更多按钮（右侧居中）
        {
            const QRect mbr = moreButtonRect(option.rect);
            painter->setPen(Qt::NoPen);
            painter->setBrush((hovered || selected) ? QColor(226, 232, 240) : QColor(241, 245, 249));
            painter->drawRoundedRect(mbr, 6, 6);
            
            painter->setPen(QPen(QColor(100, 116, 139), 2, Qt::SolidLine, Qt::RoundCap));
            int cx = mbr.center().x();
            int cy = mbr.center().y();
            painter->drawPoint(cx - 4, cy);
            painter->drawPoint(cx, cy);
            painter->drawPoint(cx + 4, cy);
        }

        painter->restore();
    }
};

// ============================================================
// DocBrowserPage
// ============================================================
DocBrowserPage::DocBrowserPage(AppManagerService *service, QWidget *parent)
    : QWidget(parent)
    , m_service(service)
{
    buildUi();
}

void DocBrowserPage::buildUi()
{
    setStyleSheet(QStringLiteral(
        "DocBrowserPage { background: #f8fafc; }"));

    auto *vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(16, 12, 16, 12);
    vlay->setSpacing(10);

    // ---- 顶部：提示 + 状态 + 搜索框 + 刷新 + 下载进度条 ----
    auto *topBar = new QWidget(this);
    topBar->setStyleSheet(QStringLiteral(
        "QWidget { background: white; border-radius: 8px; }"));
    auto *topBarVLay = new QVBoxLayout(topBar);
    topBarVLay->setContentsMargins(12, 8, 12, 8);
    topBarVLay->setSpacing(6);

    auto *topLay = new QHBoxLayout();
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(10);

    // 固定提示标签
    auto *lblHint = new QLabel(QStringLiteral("\u70b9\u51fb\u56fe\u6807\u6253\u5f00\u6587\u6863"), topBar);
    lblHint->setStyleSheet(QStringLiteral(
        "color: #6b7280; font-size: 12px; background: transparent;"));

    // 竖分隔线
    auto *separator = new QFrame(topBar);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setFixedWidth(1);
    separator->setFixedHeight(14);
    separator->setStyleSheet(QStringLiteral("background: #e2e8f0; border: none;"));

    // 状态标签（动态更新）
    m_lblStatus = new QLabel(QStringLiteral("\u52a0\u8f7d\u6587\u6863\u5217\u8868\u4e2d\u2026"), topBar);
    m_lblStatus->setStyleSheet(QStringLiteral(
        "color: #6b7280; font-size: 12px; background: transparent;"));

    // 搜索框
    m_searchBox = new QLineEdit(topBar);
    m_searchBox->setPlaceholderText(QStringLiteral("\u641c\u7d22\u6587\u6863\u540d\u79f0\u2026"));
    m_searchBox->setMinimumWidth(180);
    m_searchBox->setMaximumWidth(300);
    m_searchBox->setFixedHeight(30);
    m_searchBox->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 15px;"
        "  padding: 0 12px 0 12px;"
        "  font-size: 12px;"
        "  background: #f8fafc;"
        "  color: #374151;"
        "}"
        "QLineEdit:focus {"
        "  border-color: #93c5fd;"
        "  background: white;"
        "}"));

    // 刷新按钮：使用 refresh.ico 图标
    m_btnRefresh = new QPushButton(topBar);
    setButtonIconWithoutDisabledTint(m_btnRefresh, QStringLiteral(":/resources/refresh.ico"));
    m_btnRefresh->setIconSize(QSize(30, 30));
    m_btnRefresh->setFixedSize(34, 34);
    m_btnRefresh->setToolTip(QStringLiteral("\u5237\u65b0\u6587\u6863\u76ee\u5f55"));
    m_btnRefresh->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #eff6ff;"
        "  border: 1px solid #bfdbfe;"
        "  border-radius: 17px;"
        "}"
        "QPushButton:hover { background: #dbeafe; border-color: #93c5fd; }"
        "QPushButton:pressed { background: #bfdbfe; }"
        "QPushButton:disabled { background: #f8fafc; border-color: #e2e8f0; }"));
    m_btnRefresh->setCursor(Qt::PointingHandCursor);

    m_refreshMovie = new QMovie(QStringLiteral(":/resources/array.gif"), QByteArray(), this);
    m_refreshMovie->setScaledSize(QSize(28, 28));
    connect(m_refreshMovie, &QMovie::frameChanged, this, [this](int) {
        setButtonIconWithoutDisabledTint(m_btnRefresh, m_refreshMovie->currentPixmap());
    });

    topLay->addWidget(lblHint);
    topLay->addWidget(separator);
    topLay->addWidget(m_lblStatus, 1);
    topLay->addWidget(m_searchBox);
    topLay->addWidget(m_btnRefresh);

    // 下载进度条（平时隐藏，下载时显示）
    m_docDownloadProgress = new QProgressBar(topBar);
    m_docDownloadProgress->setTextVisible(false);
    m_docDownloadProgress->setFixedHeight(4);
    m_docDownloadProgress->setRange(0, 100);
    m_docDownloadProgress->setValue(0);
    {
        QSizePolicy pp = m_docDownloadProgress->sizePolicy();
        pp.setRetainSizeWhenHidden(true);
        m_docDownloadProgress->setSizePolicy(pp);
    }
    m_docDownloadProgress->setVisible(false);
    m_docDownloadProgress->setStyleSheet(QStringLiteral(
        "QProgressBar {"
        "  border: none; border-radius: 2px; background: #d1fae5;"
        "}"
        "QProgressBar::chunk {"
        "  border-radius: 2px; background: #34d399;"
        "}"));

    topBarVLay->addLayout(topLay);
    topBarVLay->addWidget(m_docDownloadProgress);
    vlay->addWidget(topBar);

    // ---- 分类过滤栏 ----
    m_catScroll = new QScrollArea(this);
    m_catScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_catScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_catScroll->setWidgetResizable(true);
    m_catScroll->setFixedHeight(48);
    m_catScroll->setFrameShape(QFrame::NoFrame);
    m_catScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; }"
        "QScrollBar:horizontal { height: 4px; background: #f1f5f9; }"
        "QScrollBar::handle:horizontal { background: #cbd5e1; border-radius: 2px; }"));

    m_catBar = new QWidget;
    m_catBar->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *catLay = new QHBoxLayout(m_catBar);
    catLay->setContentsMargins(0, 4, 0, 4);
    catLay->setSpacing(6);

    // "全部" 按钮
    m_btnAll = new QToolButton(m_catBar);
    m_btnAll->setText(QStringLiteral("  全部  "));
    m_btnAll->setCheckable(true);
    m_btnAll->setChecked(true);
    m_btnAll->setCursor(Qt::PointingHandCursor);
    m_btnAll->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  background: #f1f5f9;"
        "  color: #475569;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 4px 14px;"
        "  font-size: 13px;"
        "  font-weight: normal;"
        "}"
        "QToolButton:hover { background: #e2e8f0; }"
        "QToolButton:checked {"
        "  background: #dbeafe;"
        "  color: #1d4ed8;"
        "  font-weight: bold;"
        "}"));
    catLay->addWidget(m_btnAll);

    // "已下载" 过滤按鈕
    m_btnDownloaded = new QToolButton(m_catBar);
    m_btnDownloaded->setText(QStringLiteral("  已下载  "));
    m_btnDownloaded->setCheckable(true);
    m_btnDownloaded->setCursor(Qt::PointingHandCursor);
    m_btnDownloaded->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  background: #f1f5f9;"
        "  color: #475569;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 4px 14px;"
        "  font-size: 13px;"
        "  font-weight: normal;"
        "}"
        "QToolButton:hover { background: #dcfce7; color: #15803d; }"
        "QToolButton:checked {"
        "  background: #dcfce7;"
        "  color: #15803d;"
        "  font-weight: bold;"
        "}"));
    catLay->addWidget(m_btnDownloaded);
    catLay->addStretch();

    m_catScroll->setWidget(m_catBar);
    vlay->addWidget(m_catScroll);

    // ---- 文档网格 ----
    m_docList = new QListWidget(this);
    m_docList->setViewMode(QListView::IconMode);
    m_docList->setResizeMode(QListView::Adjust);
    m_docList->setMovement(QListView::Static);
    m_docList->setGridSize(QSize(230, 96));
    m_docList->setSpacing(6);
    m_docList->setWordWrap(true);
    m_docList->setContextMenuPolicy(Qt::NoContextMenu);
    m_docList->setItemDelegate(new DocItemDelegate(m_docList));
    m_docList->viewport()->installEventFilter(this);
    m_docList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { border-radius: 10px; }"
        "QScrollBar:vertical { width: 6px; background: #f1f5f9; }"
        "QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 3px; }"));
    vlay->addWidget(m_docList, 1);

    // 信号连接
    connect(m_btnRefresh, &QPushButton::clicked, this, &DocBrowserPage::refresh);
    connect(m_btnAll, &QToolButton::clicked, this, [this]() {
        m_activeCategory.clear();
        m_showDownloadedOnly = false;
        m_searchBox->clear();
        m_btnAll->setChecked(true);
        m_btnDownloaded->setChecked(false);
        for (auto it = m_catButtons.constBegin(); it != m_catButtons.constEnd(); ++it) {
            it.value()->setChecked(false);
        }
        refreshDocGrid();
    });
    connect(m_btnDownloaded, &QToolButton::clicked, this, [this]() {
        m_activeCategory.clear();
        m_showDownloadedOnly = true;
        m_searchBox->clear();
        m_btnAll->setChecked(false);
        m_btnDownloaded->setChecked(true);
        for (auto it = m_catButtons.constBegin(); it != m_catButtons.constEnd(); ++it) {
            it.value()->setChecked(false);
        }
        refreshDocGrid();
    });
    connect(m_searchBox, &QLineEdit::textChanged,
            this, &DocBrowserPage::onSearchTextChanged);
    connect(m_docList, &QListWidget::itemClicked,
            this, &DocBrowserPage::onDocItemClicked);
}

// ============================================================
// 刷新目录（从服务器拉取）
// ============================================================
void DocBrowserPage::refresh()
{
    m_lblStatus->setText(QStringLiteral("正在从服务器获取文档目录…"));
    setRefreshButtonBusyState(m_btnRefresh, m_refreshMovie, true);
    m_docDownloadProgress->setRange(0, 0);
    m_docDownloadProgress->setVisible(true);
    QApplication::processEvents();

    // 在 Qt 事件循环中异步执行（避免阰塞 UI），通过 singleShot + lambda 实现
    QTimer::singleShot(0, this, [this]() {
        m_catalog = m_service->fetchDocCatalog(15000);
        setRefreshButtonBusyState(m_btnRefresh, m_refreshMovie, false);
        m_docDownloadProgress->setVisible(false);

        if (m_catalog.isEmpty()) {
            m_lblStatus->setText(QStringLiteral("暂无文档，或服务器未连接。"));
        } else {
            m_lblStatus->setText(QStringLiteral("共 %1 个文档").arg(m_catalog.size()));
        }

        rebuildCategoryFilter();
        refreshDocGrid();
    });
}

// ============================================================
// 重建分类过滤栏
// ============================================================
void DocBrowserPage::rebuildCategoryFilter()
{
    // 收集所有分类（去重）
    QStringList allCats;
    for (const ClientDocEntry &e : m_catalog) {
        for (const QString &c : e.categories) {
            if (!allCats.contains(c)) allCats.append(c);
        }
    }
    allCats.sort();

    // 删除旧分类按钮
    for (auto *btn : m_catButtons) btn->deleteLater();
    m_catButtons.clear();

    auto *catLay = qobject_cast<QHBoxLayout *>(m_catBar->layout());
    // 移除 stretch，重加
    if (catLay->count() > 1) {
        QLayoutItem *stretchItem = catLay->takeAt(catLay->count() - 1);
        delete stretchItem;
    }

    for (const QString &cat : allCats) {
        auto *btn = new QToolButton(m_catBar);
        btn->setText(QStringLiteral("  %1  ").arg(cat));
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        
        // 统一清爽风格，浅色系
        btn->setStyleSheet(QStringLiteral(
            "QToolButton {"
            "  background: #f1f5f9;"
            "  color: #475569;"
            "  border: none;"
            "  border-radius: 12px;"
            "  padding: 4px 14px;"
            "  font-size: 13px;"
            "  font-weight: normal;"
            "}"
            "QToolButton:hover { background: #e2e8f0; }"
            "QToolButton:checked {"
            "  background: #dbeafe;"
            "  color: #1d4ed8;"
            "  font-weight: bold;"
            "}"));

        catLay->addWidget(btn);
        m_catButtons.insert(cat, btn);

        connect(btn, &QToolButton::clicked, this, [this, cat]() {
            m_activeCategory = cat;
            m_showDownloadedOnly = false;
            m_searchBox->clear();  // 切分类时清空搜索
            m_btnAll->setChecked(false);
            m_btnDownloaded->setChecked(false);
            for (auto it = m_catButtons.constBegin(); it != m_catButtons.constEnd(); ++it) {
                it.value()->setChecked(it.key() == cat);
            }
            refreshDocGrid();
        });
    }
    catLay->addStretch();

    m_btnAll->setChecked(m_activeCategory.isEmpty() && !m_showDownloadedOnly);
    m_btnDownloaded->setChecked(m_showDownloadedOnly);
}

// ============================================================
// 刷新文档网格
// ============================================================
void DocBrowserPage::refreshDocGrid()
{
    m_docList->clear();

    const bool isSearching = !m_searchText.trimmed().isEmpty();
    const QString kw = m_searchText.trimmed().toLower();

    for (const ClientDocEntry &doc : m_catalog) {
        // 搜索模式：模糊匹配标题、描述、关键词（按字符包含，不区分大小写）
        if (isSearching) {
            const QString titleL = doc.title.toLower();
            const QString descL  = doc.description.toLower();
            bool matched = titleL.contains(kw) || descL.contains(kw);
            if (!matched) {
                for (const QString &k : doc.keywords)
                    if (k.toLower().contains(kw)) { matched = true; break; }
            }
            if (!matched) continue;
        } else if (m_showDownloadedOnly) {
            // 已下载过滤模式
            if (!m_service->isDocDownloaded(doc)) continue;
        } else {
            // 分类过滤模式
            if (!m_activeCategory.isEmpty() && !doc.categories.contains(m_activeCategory))
                continue;
        }

        const bool downloaded = m_service->isDocDownloaded(doc);
        const bool upToDate   = downloaded && m_service->isDocUpToDate(doc);
        QString badge;
        if (!downloaded)      badge = QStringLiteral("下载");
        else if (!upToDate)   badge = QStringLiteral("有更新");
        else                  badge = QStringLiteral("已下载");

        auto *item = new QListWidgetItem(doc.title);
        item->setData(Qt::UserRole,      doc.docId);
        item->setData(Qt::UserRole + 10, doc.categories);
        item->setData(Qt::UserRole + 11, QStringLiteral("v%1").arg(doc.version));
        item->setData(Qt::UserRole + 12, badge);
        // 文件后缀名（用于卡片着色和类型标签）
        const int dotIdx = doc.fileName.lastIndexOf('.');
        item->setData(Qt::UserRole + 13, dotIdx >= 0 ? doc.fileName.mid(dotIdx + 1) : QString());
        item->setTextAlignment(Qt::AlignHCenter);
        item->setToolTip(doc.description.isEmpty() ? doc.title : doc.description);
        m_docList->addItem(item);
    }

    // 搜索模式下更新状态标签
    if (isSearching) {
        const int cnt = m_docList->count();
        m_lblStatus->setText(
            cnt > 0
            ? QStringLiteral("搜索 \u300c%1\u300d: 找到 %2 个文档").arg(m_searchText.trimmed()).arg(cnt)
            : QStringLiteral("搜索 \u300c%1\u300d: 未找到匹配文档").arg(m_searchText.trimmed()));
    } else if (!m_catalog.isEmpty()) {
        m_lblStatus->setText(QStringLiteral("共 %1 个文档").arg(m_catalog.size()));
    }
}

// ============================================================
// 搜索框文字变化
// ============================================================
void DocBrowserPage::onSearchTextChanged(const QString &text)
{
    m_searchText = text;
    const bool isSearching = !text.trimmed().isEmpty();

    // 搜索时取消分类选中激活态（分类按钮变灰暗，搜索优先）
    m_btnAll->setChecked(!isSearching);
    m_btnDownloaded->setChecked(false);
    for (auto it = m_catButtons.constBegin(); it != m_catButtons.constEnd(); ++it)
        it.value()->setChecked(false);

    refreshDocGrid();
}

// ============================================================
// 单击文档条目
// ============================================================
void DocBrowserPage::onDocItemClicked(QListWidgetItem *item)
{
    if (!item) return;
    const QString docId = item->data(Qt::UserRole).toString();
    const auto it = std::find_if(m_catalog.constBegin(), m_catalog.constEnd(),
                                  [&](const ClientDocEntry &e){ return e.docId == docId; });
    if (it == m_catalog.constEnd()) return;

    const ClientDocEntry &doc = *it;
    if (m_service->isDocDownloaded(doc)) {
        openDoc(doc);
    } else {
        downloadAndOpen(doc);
    }
}

// ============================================================
// 打开已下载的文档
// ============================================================
void DocBrowserPage::openDoc(const ClientDocEntry &doc)
{
    const QString path = m_service->localDocFilePath(doc);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this, QStringLiteral("打开失败"),
            QStringLiteral("无法打开文档：\n%1").arg(QDir::toNativeSeparators(path)));
    }
}

// ============================================================
// 下载文档后再打开
// ============================================================
void DocBrowserPage::downloadAndOpen(const ClientDocEntry &doc)
{
    // --- 内联进度条：展示在顶部工具栏，无弹窗 ---
    m_docDownloadProgress->setRange(0, 0);  // 不确定进度，等待 Content-Length
    m_docDownloadProgress->setValue(0);
    m_docDownloadProgress->setVisible(true);
    m_lblStatus->setText(QStringLiteral("正在下载「%1」…").arg(doc.title));
    setRefreshButtonBusyState(m_btnRefresh, m_refreshMovie, true);
    QApplication::processEvents();

    AppManagerService::DownloadProgressCallback progress = [&](qint64 recv, qint64 total) {
        if (total > 0) {
            if (m_docDownloadProgress->maximum() == 0)
                m_docDownloadProgress->setRange(0, 100);
            m_docDownloadProgress->setValue(static_cast<int>(recv * 100 / total));
        } else {
            if (m_docDownloadProgress->maximum() != 0)
                m_docDownloadProgress->setRange(0, 0);
        }
        QApplication::processEvents();
    };

    AppManagerService::CancelCallback cancel = [&]() { return false; };

    QString err;
    const bool ok = m_service->downloadDoc(doc, err, 60000, progress, cancel);

    m_docDownloadProgress->setVisible(false);
    setRefreshButtonBusyState(m_btnRefresh, m_refreshMovie, false);

    if (!ok) {
        m_lblStatus->setText(QStringLiteral("下载失败：%1").arg(err));
        QMessageBox::warning(this, QStringLiteral("下载失败"),
            QStringLiteral("文档下载失败：%1").arg(err));
        emit logMessage(QStringLiteral("文档下载失败: %1 — %2").arg(doc.title, err));
        return;
    }

    m_lblStatus->setText(QStringLiteral("「%1」下载完成").arg(doc.title));
    emit logMessage(QStringLiteral("文档已下载: %1").arg(doc.title));
    refreshDocGrid();

    // 下载完成后自动打开
    openDoc(doc);
}

// ============================================================
// 更多菜单
// ============================================================
void DocBrowserPage::onMoreButtonClicked(const QString &docId, const QPoint &globalPos)
{
    const auto it = std::find_if(m_catalog.constBegin(), m_catalog.constEnd(),
                                  [&](const ClientDocEntry &e){ return e.docId == docId; });
    if (it == m_catalog.constEnd()) return;
    const ClientDocEntry &doc = *it;

    QMenu menu(this);

    const bool downloaded = m_service->isDocDownloaded(doc);

    if (downloaded) {
        QAction *actOpen = menu.addAction(QStringLiteral("打开文件位置"));
        connect(actOpen, &QAction::triggered, this, [this, doc]() { openDocLocation(doc); });

        QAction *actWith = menu.addAction(QStringLiteral("使用其他应用打开"));
        connect(actWith, &QAction::triggered, this, [this, doc]() { openDocWith(doc); });
    }

    QAction *actDefault = menu.addAction(QStringLiteral("设置默认应用…"));
    connect(actDefault, &QAction::triggered, this, &DocBrowserPage::openDefaultAppsSettings);

    if (downloaded) {
        menu.addSeparator();
        QAction *actRemove = menu.addAction(QStringLiteral("移除本地文档"));
        actRemove->setToolTip(QStringLiteral("删除本地缓存的文档文件，不影响服务器端文档"));
        connect(actRemove, &QAction::triggered, this, [this, doc]() { removeDocLocal(doc); });
    }

    menu.exec(globalPos);
}

// ============================================================
// 打开文件位置
// ============================================================
void DocBrowserPage::openDocLocation(const ClientDocEntry &doc)
{
    const QString path = m_service->localDocFilePath(doc);
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("explorer"),
        { QStringLiteral("/select,"), QDir::toNativeSeparators(path) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

// ============================================================
// 使用其他应用打开（调出系统"打开方式"对话框）
// ============================================================
void DocBrowserPage::openDocWith(const ClientDocEntry &doc)
{
    const QString path = QDir::toNativeSeparators(m_service->localDocFilePath(doc));
#ifdef Q_OS_WIN
    // rundll32 OpenAs_RunDLL 弹出系统"打开方式"对话框
    QProcess::startDetached(QStringLiteral("rundll32.exe"),
        { QStringLiteral("shell32.dll,OpenAs_RunDLL"), path });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_service->localDocFilePath(doc)));
#endif
}

// ============================================================
// 打开系统"默认应用"设置（设置 PDF 默认程序）
// ============================================================
void DocBrowserPage::openDefaultAppsSettings()
{
#ifdef Q_OS_WIN
    // Windows 10/11 默认应用设置页
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")));
    // 若 ms-settings 不可用（Win8），则打开控制面板
    // QProcess::startDetached("control", {"defaultprograms"});
#endif
}

// ============================================================
// 移除本地文档
// ============================================================
void DocBrowserPage::removeDocLocal(const ClientDocEntry &doc)
{
    const QString path = m_service->localDocFilePath(doc);
    const int ret = QMessageBox::question(this,
        QStringLiteral("移除文档"),
        QStringLiteral("确定要删除本地缓存的「%1」吗？\n\n文件路径：%2\n\n此操作不影响服务器上的文档。")
            .arg(doc.title, QDir::toNativeSeparators(path)));
    if (ret != QMessageBox::Yes) return;

    if (QFile::remove(path)) {
        emit logMessage(QStringLiteral("已移除本地文档: %1").arg(doc.title));
        refreshDocGrid();
    } else {
        if (QFile::exists(path)) {
            QMessageBox::warning(this, QStringLiteral("删除失败"),
                QStringLiteral("无法删除文件，请先关闭「%1」再尝试。\n\n文件路径：%2")
                    .arg(doc.title, QDir::toNativeSeparators(path)));
        } else {
            QMessageBox::warning(this, QStringLiteral("删除失败"),
                QStringLiteral("无法删除文件：\n%1").arg(QDir::toNativeSeparators(path)));
        }
    }
}

// ============================================================
// 事件过滤器：拦截 ··· 按钮点击 & 双击打开
// ============================================================
bool DocBrowserPage::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_docList->viewport()) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                const QModelIndex idx = m_docList->indexAt(me->pos());
                if (!idx.isValid()) return false;

                QListWidgetItem *item = m_docList->item(idx.row());
                const QRect itemRect = m_docList->visualItemRect(item);

                if (DocItemDelegate::moreButtonRect(itemRect).contains(me->pos())) {
                    const QString docId = item->data(Qt::UserRole).toString();
                    onMoreButtonClicked(docId, me->globalPos());
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}
