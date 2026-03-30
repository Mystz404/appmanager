#include "logindialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include <QVBoxLayout>

// 跨对话框实例保持冒冷状态
namespace {
    int    g_failureCount    = 0;
    qint64 g_cooldownUntilMs = 0;  // 0 = 无冒冷
}

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

LoginDialog::LoginDialog(const QString &serverBaseUrl, QWidget *parent)
    : QDialog(parent)
    , m_serverBaseUrl(serverBaseUrl)
{
    setWindowTitle(QStringLiteral("登录"));
    setFixedWidth(360);
    buildUi();

    m_cooldownTimer = new QTimer(this);
    m_cooldownTimer->setInterval(1000);
    connect(m_cooldownTimer, &QTimer::timeout, this, &LoginDialog::onCooldownTick);

    // 如果冒冷期未结束，恢复冒冷状态
    m_failureCount = g_failureCount;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (g_cooldownUntilMs > now) {
        m_cooldownLeft = static_cast<int>((g_cooldownUntilMs - now + 999) / 1000);
        m_btnLogin->setEnabled(false);
        m_edUsername->setEnabled(false);
        m_edPassword->setEnabled(false);
        m_lblError->setText(QStringLiteral("多次登录失败，请 %1 秒后重试").arg(m_cooldownLeft));
        m_cooldownTimer->start();
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void LoginDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("登录到 AppManager 服务"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: bold; color: #1e293b;"));
    root->addWidget(title);

    auto *form = new QFormLayout;
    form->setSpacing(10);
    m_edUsername = new QLineEdit(this);
    m_edUsername->setPlaceholderText(QStringLiteral("用户名"));
    m_edPassword = new QLineEdit(this);
    m_edPassword->setEchoMode(QLineEdit::Password);
    m_edPassword->setPlaceholderText(QStringLiteral("密码"));
    form->addRow(QStringLiteral("用户名："), m_edUsername);
    form->addRow(QStringLiteral("密　码："), m_edPassword);
    root->addLayout(form);

    m_lblError = new QLabel(this);
    m_lblError->setStyleSheet(QStringLiteral("color: #ef4444; font-size: 12px;"));
    m_lblError->setWordWrap(true);
    m_lblError->setMinimumHeight(20);  // 始终占位，避免弹出时控件位移
    root->addWidget(m_lblError);

    auto *btnBox = new QHBoxLayout;
    m_btnLogin = new QPushButton(QStringLiteral("登 录"));
    m_btnLogin->setDefault(true);
    m_btnLogin->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2563eb; color: white; border-radius: 4px;"
        " padding: 7px 24px; font-size: 13px; }"
        "QPushButton:hover { background: #1d4ed8; }"
        "QPushButton:disabled { background: #94a3b8; }"));
    auto *btnCancel = new QPushButton(QStringLiteral("取 消"));
    btnCancel->setAutoDefault(false);  // 防止 Enter 在冒冷期间触发取消
    btnCancel->setStyleSheet(QStringLiteral(
        "QPushButton { background: #f1f5f9; color: #334155; border-radius: 4px;"
        " padding: 7px 24px; font-size: 13px; }"
        "QPushButton:hover { background: #e2e8f0; }"));
    btnBox->addStretch();
    btnBox->addWidget(m_btnLogin);
    btnBox->addWidget(btnCancel);
    root->addLayout(btnBox);

    connect(m_btnLogin,  &QPushButton::clicked, this, &LoginDialog::onLogin);
    connect(btnCancel,   &QPushButton::clicked, this, &QDialog::reject);
    // 不再连接 returnPressed：m_btnLogin 已 setDefault(true)，对话框内任意地方按 Enter 就会触发按钒，再连 returnPressed 会导致发送两次请求
}

// ---------------------------------------------------------------------------
// 登录逻辑
// ---------------------------------------------------------------------------

void LoginDialog::recordFailure(const QString &errorText)
{
    m_lblError->setText(errorText);
    ++m_failureCount;
    g_failureCount = m_failureCount;  // 同步到跨实例状态
    if (m_failureCount >= kMaxFailures) {
        startCooldown();
    }
}

void LoginDialog::startCooldown()
{
    m_cooldownLeft = kCooldownSecs;
    g_cooldownUntilMs = QDateTime::currentMSecsSinceEpoch() + qint64(kCooldownSecs) * 1000;
    m_btnLogin->setEnabled(false);
    m_edUsername->setEnabled(false);
    m_edPassword->setEnabled(false);
    m_lblError->setText(QStringLiteral("多次登录失败，请 %1 秒后重试").arg(m_cooldownLeft));
    m_cooldownTimer->start();
}

void LoginDialog::onCooldownTick()
{
    --m_cooldownLeft;
    if (m_cooldownLeft <= 0) {
        m_cooldownTimer->stop();
        m_failureCount = 0;
        g_failureCount = 0;
        g_cooldownUntilMs = 0;
        m_btnLogin->setEnabled(true);
        m_edUsername->setEnabled(true);
        m_edPassword->setEnabled(true);
        m_lblError->clear();
    } else {
        m_lblError->setText(QStringLiteral("多次登录失败，请 %1 秒后重试").arg(m_cooldownLeft));
    }
}

void LoginDialog::onLogin()
{
    if (m_cooldownTimer->isActive() || m_requestInFlight)
        return;

    const QString username = m_edUsername->text().trimmed();
    const QString password = m_edPassword->text();
    if (username.isEmpty() || password.isEmpty()) {
        m_lblError->setText(QStringLiteral("用户名和密码不能为空"));
        return;
    }

    m_requestInFlight = true;
    m_btnLogin->setEnabled(false);
    m_btnLogin->setText(QStringLiteral("登录中…"));
    m_lblError->clear();
    QApplication::processEvents();

    const QString url = m_serverBaseUrl.endsWith('/')
        ? m_serverBaseUrl + QStringLiteral("login")
        : m_serverBaseUrl + QStringLiteral("/login");

    QJsonObject bodyObj;
    bodyObj.insert(QStringLiteral("username"), username);
    bodyObj.insert(QStringLiteral("password"), password);
    const QByteArray bodyData = QJsonDocument(bodyObj).toJson(QJsonDocument::Compact);

    QNetworkAccessManager nam;
    const QUrl loginUrl(url);
    QNetworkRequest request(loginUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "AppManager/" APP_VERSION " (Windows; Qt/5.15)");

    QNetworkReply *reply = nam.post(request, bodyData);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(8000);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout,        &loop, &QEventLoop::quit);
    loop.exec();
    const bool timedOut = !timer.isActive(); // 在 stop() 之前检查，否则永远为 true
    timer.stop();
    const QNetworkReply::NetworkError netErr = reply->error();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray respData = reply->readAll();
    reply->deleteLater();

    m_requestInFlight = false;
    m_btnLogin->setEnabled(true);
    m_btnLogin->setText(QStringLiteral("登 录"));

    if (timedOut) {
        recordFailure(QStringLiteral("连接服务器超时，请检查网络后重试"));
        return;
    }
    if (netErr != QNetworkReply::NoError) {
        qWarning() << "[Login] Network error:" << netErr << "HTTP status:" << httpStatus;
        if (httpStatus == 401 || httpStatus == 403) {
            recordFailure(QStringLiteral("用户名或密码错误，请检查后重试"));
        } else {
            recordFailure(QStringLiteral("连接服务器失败，请检查网络后重试"));
        }
        return;
    }

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(respData, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[Login] Invalid server response:" << parseErr.errorString() << respData;
        recordFailure(QStringLiteral("服务器返回格式错误，请稍后重试"));
        return;
    }

    const QJsonObject resp = doc.object();
    if (resp.contains(QStringLiteral("error"))) {
        qWarning() << "[Login] Auth failed:" << resp.value(QStringLiteral("error")).toString();
        recordFailure(QStringLiteral("用户名或密码错误，请检查后重试"));
        return;
    }

    m_token    = resp.value(QStringLiteral("token")).toString();
    m_username = resp.value(QStringLiteral("username")).toString();
    m_role     = resp.value(QStringLiteral("role")).toString();

    if (m_token.isEmpty()) {
        qWarning() << "[Login] Server returned no token, response:" << respData;
        recordFailure(QStringLiteral("登录失败，请稍后重试"));
        return;
    }

    accept();
}
