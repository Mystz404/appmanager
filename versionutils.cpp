#include "versionutils.h"

#include <QStringList>

#ifdef Q_OS_WIN
#include <Windows.h>
#pragma comment(lib, "version.lib")
#endif

int compareVersions(const QString &left, const QString &right)
{
    const QStringList leftParts = left.split('.', Qt::SkipEmptyParts);
    const QStringList rightParts = right.split('.', Qt::SkipEmptyParts);

    const int maxCount = qMax(leftParts.size(), rightParts.size());
    for (int i = 0; i < maxCount; ++i) {
        const int l = (i < leftParts.size()) ? leftParts.at(i).toInt() : 0;
        const int r = (i < rightParts.size()) ? rightParts.at(i).toInt() : 0;

        if (l < r) {
            return -1;
        }
        if (l > r) {
            return 1;
        }
    }

    return 0;
}

QString getFileVersion(const QString &exePath)
{
#ifdef Q_OS_WIN
    const std::wstring wPath = exePath.toStdWString();
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(wPath.c_str(), &dummy);
    if (size == 0) {
        return {};
    }

    QByteArray buffer(static_cast<int>(size), 0);
    if (!GetFileVersionInfoW(wPath.c_str(), 0, size,
                             reinterpret_cast<LPVOID>(buffer.data()))) {
        return {};
    }

    VS_FIXEDFILEINFO *fileInfo = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(reinterpret_cast<LPCVOID>(buffer.data()),
                        L"\\", reinterpret_cast<LPVOID *>(&fileInfo), &len)) {
        return {};
    }
    if (len == 0 || fileInfo == nullptr) {
        return {};
    }

    return QStringLiteral("%1.%2.%3.%4")
        .arg(HIWORD(fileInfo->dwFileVersionMS))
        .arg(LOWORD(fileInfo->dwFileVersionMS))
        .arg(HIWORD(fileInfo->dwFileVersionLS))
        .arg(LOWORD(fileInfo->dwFileVersionLS));
#else
    Q_UNUSED(exePath)
    return {};
#endif
}
