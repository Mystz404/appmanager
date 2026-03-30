#ifndef DOCCLIENTTYPES_H
#define DOCCLIENTTYPES_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * @brief 客户端文档条目，与服务端 DocEntry 对应。
 *
 * 序列化格式与服务端 buildCatalog() 返回的 JSON 数组元素一致：
 * {
 *   "docId":       "bcmu_user_guide",
 *   "title":       "BCMU 用户手册",
 *   "version":     "1.0.0",
 *   "categories":  ["用户手册", "安装指南"],
 *   "description": "...",
 *   "fileName":    "bcmu_user_guide.pdf",
 *   "sha256":      "abcdef...",
 *   "keywords":    ["说明书"],
 *   "sortOrder":   10,
 *   "downloadUrl": "http://..."
 * }
 */
struct ClientDocEntry {
    QString     docId;
    QString     title;
    QString     version;
    QStringList categories;
    QString     description;
    QString     fileName;
    QString     sha256;
    QStringList keywords;
    int         sortOrder  = 0;
    QString     downloadUrl;

    static ClientDocEntry fromJson(const QJsonObject &obj)
    {
        ClientDocEntry e;
        e.docId       = obj.value(QStringLiteral("docId")).toString();
        e.title       = obj.value(QStringLiteral("title")).toString();
        e.version     = obj.value(QStringLiteral("version")).toString();
        e.description = obj.value(QStringLiteral("description")).toString();
        e.fileName    = obj.value(QStringLiteral("fileName")).toString();
        e.sha256      = obj.value(QStringLiteral("sha256")).toString();
        e.sortOrder   = obj.value(QStringLiteral("sortOrder")).toInt(0);
        e.downloadUrl = obj.value(QStringLiteral("downloadUrl")).toString();

        for (const QJsonValue &v : obj.value(QStringLiteral("categories")).toArray())
            e.categories.append(v.toString());
        for (const QJsonValue &v : obj.value(QStringLiteral("keywords")).toArray())
            e.keywords.append(v.toString());
        return e;
    }
};

#endif // DOCCLIENTTYPES_H
