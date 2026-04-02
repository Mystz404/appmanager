#ifndef DOCBROWSERPAGE_H
#define DOCBROWSERPAGE_H

#include "appmanagerservice.h"
#include "docclienttypes.h"

#include <QHash>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QScrollArea>
#include <QToolButton>
#include <QWidget>

class QLabel;
class QMovie;
class QPushButton;

class DocBrowserPage : public QWidget
{
    Q_OBJECT

public:
    explicit DocBrowserPage(AppManagerService *service, QWidget *parent = nullptr);

    void refresh();

signals:
    void logMessage(const QString &msg);

private slots:
    void onDocItemClicked(QListWidgetItem *item);
    void onMoreButtonClicked(const QString &docId, const QPoint &globalPos);
    void onSearchTextChanged(const QString &text);

private:
    void buildUi();
    void rebuildCategoryFilter();
    void refreshDocGrid();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

    void openDoc(const ClientDocEntry &doc);
    void downloadAndOpen(const ClientDocEntry &doc);
    void openDocLocation(const ClientDocEntry &doc);
    void openDocWith(const ClientDocEntry &doc);
    void openDefaultAppsSettings();
    void removeDocLocal(const ClientDocEntry &doc);

    AppManagerService       *m_service    = nullptr;
    QVector<ClientDocEntry>  m_catalog;
    QString                  m_activeCategory;   // "" = 全部
    QString                  m_searchText;       // 模糊搜索关键词
    bool                     m_showDownloadedOnly = false; // 仅显示已下载

    QScrollArea             *m_catScroll  = nullptr;
    QWidget                 *m_catBar     = nullptr;
    QListWidget             *m_docList    = nullptr;
    QLabel                  *m_lblStatus          = nullptr;
    QPushButton             *m_btnRefresh         = nullptr;
    QMovie                  *m_refreshMovie       = nullptr;
    QLineEdit               *m_searchBox          = nullptr;
    QProgressBar            *m_docDownloadProgress = nullptr;

    QHash<QString, QToolButton *> m_catButtons;
    QToolButton                  *m_btnAll        = nullptr;
    QToolButton                  *m_btnDownloaded = nullptr;
};

#endif // DOCBROWSERPAGE_H
