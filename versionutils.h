#ifndef VERSIONUTILS_H
#define VERSIONUTILS_H

#include <QString>

/**
 * @brief 比较两个语义版本号（例如 1.2.10 与 1.3.0）。
 * @param left 左侧版本。
 * @param right 右侧版本。
 * @return <0 表示 left < right；0 表示相等；>0 表示 left > right。
 *
 * 规则：按 '.' 分段，逐段按整数比较，不足段按 0 补齐。
 */
int compareVersions(const QString &left, const QString &right);

/**
 * @brief 从 Windows 可执行文件读取文件版本号。
 * @param exePath 可执行文件绝对路径。
 * @return 格式如 "1.2.3.4"，读取失败时返回空字符串。
 */
QString getFileVersion(const QString &exePath);

#endif // VERSIONUTILS_H
