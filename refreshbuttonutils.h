#ifndef REFRESHBUTTONUTILS_H
#define REFRESHBUTTONUTILS_H

#include <QIcon>
#include <QMovie>
#include <QPixmap>
#include <QPushButton>

inline void setButtonIconWithoutDisabledTint(QPushButton *button, const QPixmap &pixmap)
{
    if (button == nullptr || pixmap.isNull()) {
        return;
    }

    QIcon icon;
    icon.addPixmap(pixmap, QIcon::Normal, QIcon::Off);
    icon.addPixmap(pixmap, QIcon::Disabled, QIcon::Off);
    icon.addPixmap(pixmap, QIcon::Active, QIcon::Off);
    icon.addPixmap(pixmap, QIcon::Selected, QIcon::Off);
    button->setIcon(icon);
}

inline void setButtonIconWithoutDisabledTint(QPushButton *button, const QString &resourcePath)
{
    setButtonIconWithoutDisabledTint(button, QPixmap(resourcePath));
}

inline void setRefreshButtonBusyState(QPushButton *button,
                                      QMovie *movie,
                                      bool busy,
                                      const QString &idleIconPath = QStringLiteral(":/resources/refresh.ico"))
{
    if (button == nullptr) {
        return;
    }

    button->setEnabled(!busy);
    if (movie == nullptr) {
        if (!busy) {
            setButtonIconWithoutDisabledTint(button, idleIconPath);
        }
        return;
    }

    if (busy) {
        movie->start();
        return;
    }

    movie->stop();
    setButtonIconWithoutDisabledTint(button, idleIconPath);
}

#endif // REFRESHBUTTONUTILS_H