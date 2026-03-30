#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

/**
 * @brief 用户登录对话框。
 *
 * 向服务端 POST /login，成功后返回 token / username / role。
 * 使用方式：
 *   LoginDialog dlg(serverBaseUrl, this);
 *   if (dlg.exec() == QDialog::Accepted) {
 *       QString token = dlg.token();
 *       QString user  = dlg.username();
 *   }
 */
class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(const QString &serverBaseUrl, QWidget *parent = nullptr);

    QString token()    const { return m_token; }
    QString username() const { return m_username; }
    QString role()     const { return m_role; }

private:
    void buildUi();
    void onLogin();
    void recordFailure(const QString &errorText);
    void startCooldown();
    void onCooldownTick();

    static constexpr int kMaxFailures    = 5;
    static constexpr int kCooldownSecs   = 30;

    QString m_serverBaseUrl;
    QString m_token;
    QString m_username;
    QString m_role;

    int     m_failureCount    = 0;
    int     m_cooldownLeft    = 0;
    bool    m_requestInFlight = false;
    QTimer *m_cooldownTimer   = nullptr;

    QLineEdit  *m_edUsername = nullptr;
    QLineEdit  *m_edPassword = nullptr;
    QLabel     *m_lblError   = nullptr;
    QPushButton *m_btnLogin  = nullptr;
};

#endif // LOGINDIALOG_H
