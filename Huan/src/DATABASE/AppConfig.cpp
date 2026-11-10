#include "AppConfig.h"

#include "ActionRepository.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStandardPaths>

AppConfig& AppConfig::instance() {
    static AppConfig config;
    return config;
}

void AppConfig::reload() {
    m_values = ActionRepository::settings();

    // 备份目录默认放在用户文档下，避免程序目录需要管理员权限；
    // 万一系统没配 Documents（writableLocation 返回空），退到程序目录，避免拼出 "/Huan_Backup" 这种根路径
    QString backupDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (backupDir.trimmed().isEmpty()) {
        backupDir = QCoreApplication::applicationDirPath();
    }
    backupDir += QStringLiteral("/Huan_Backup");

    // 数据库暂时不可用时也要保证有可用的默认值。
    const QMap<QString, QString> defaults = {
        {QStringLiteral("trigger.mouse"), QStringLiteral("right")},
        {QStringLiteral("trigger.keyboard"), QStringLiteral("")},
        {QStringLiteral("trigger.threshold"), QStringLiteral("3")},
        {QStringLiteral("trigger.longpress"), QStringLiteral("220")},
        {QStringLiteral("trigger.timeout"), QStringLiteral("3000")},
        {QStringLiteral("action.script_timeout"), QStringLiteral("8000")},
        {QStringLiteral("trigger.actionpage_keyboard"), QStringLiteral("Alt+Space")},
        // 新增：呼出设置界面 / 强制退出
        {QStringLiteral("trigger.settings_keyboard"), QStringLiteral("Ctrl+Alt+S")},
        {QStringLiteral("trigger.force_quit_keyboard"), QStringLiteral("Ctrl+Alt+Q")},
        
        // 动作页 UI 默认值
        {QStringLiteral("ui.actionpage.width"), QStringLiteral("500")},
        {QStringLiteral("ui.actionpage.height"), QStringLiteral("680")},
        {QStringLiteral("ui.actionpage.btn_width"), QStringLiteral("100")},
        {QStringLiteral("ui.actionpage.btn_height"), QStringLiteral("70")},
        {QStringLiteral("ui.actionpage.bg_color"), QStringLiteral("#F8F9FA")},
        {QStringLiteral("ui.actionpage.btn_bg"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("ui.actionpage.btn_hover_bg"), QStringLiteral("#eaf3ff")},
        {QStringLiteral("ui.actionpage.text_color"), QStringLiteral("#333333")},
        {QStringLiteral("ui.actionpage.radius"), QStringLiteral("6")},
        {QStringLiteral("ui.actionpage.h_spacing"), QStringLiteral("4")},
        {QStringLiteral("ui.actionpage.v_spacing"), QStringLiteral("4")},
        
        // 轮盘 UI 默认值
        {QStringLiteral("ui.wheel.size"), QStringLiteral("400")},
        {QStringLiteral("ui.wheel.inner_radius"), QStringLiteral("50")},
        {QStringLiteral("ui.wheel.bg_color"), QStringLiteral("#FEFEFE")},
        {QStringLiteral("ui.wheel.hover_color"), QStringLiteral("#ECECEC")},
        {QStringLiteral("ui.wheel.line_color"), QStringLiteral("#ECECEC")},
        {QStringLiteral("ui.wheel.text_color"), QStringLiteral("#333333")},
        {QStringLiteral("ui.wheel.text_hover_color"), QStringLiteral("#000000")},
        {QStringLiteral("ui.wheel.font_size"), QStringLiteral("10")}, // 一环的字体大小，外环按比例递减
        {QStringLiteral("ui.wheel.enable_hidden_ring"), QStringLiteral("0")}, // 轮盘外圈隐藏动作（超出外圈松开触发）

        // 数据备份：手动导出的默认目录，也是自动备份的写入位置
        {QStringLiteral("backup.path"), backupDir},
        // 自动备份开关：0 关闭 / 1 开启
        {QStringLiteral("backup.auto"), QStringLiteral("0")}
    };

    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        if (!m_values.contains(it.key())) {
            m_values.insert(it.key(), it.value());
        }
    }
}

QString AppConfig::value(const QString& key, const QString& defaultValue) const {
    return m_values.value(key, defaultValue);
}

QString AppConfig::mouseTrigger() const {
    return value(QStringLiteral("trigger.mouse"), QStringLiteral("right")).trimmed().toLower();
}

QString AppConfig::keyboardTrigger() const {
    return value(QStringLiteral("trigger.keyboard")).trimmed();
}

int AppConfig::intValue(const QString& key, int defaultValue, int minimum, int maximum) const {
    bool ok = false;
    const int parsed = value(key).toInt(&ok);
    if (!ok) return defaultValue;
    return qBound(minimum, parsed, maximum);
}

int AppConfig::dragThreshold() const {
    return intValue(QStringLiteral("trigger.threshold"), 3, 1, 50);
}

int AppConfig::longPressMs() const {
    return intValue(QStringLiteral("trigger.longpress"), 220, 50, 2000);
}

int AppConfig::wheelHideMs() const {
    return intValue(QStringLiteral("trigger.timeout"), 3000, 500, 30000);
}

int AppConfig::scriptTimeoutMs() const {
    return intValue(QStringLiteral("action.script_timeout"), 8000, 500, 120000);
}


QString AppConfig::actionPageHotkey() const {
    return value(QStringLiteral("trigger.actionpage_keyboard")).trimmed();
}

QString AppConfig::settingsHotkey() const {
    return value(QStringLiteral("trigger.settings_keyboard")).trimmed();
}

QString AppConfig::forceQuitHotkey() const {
    return value(QStringLiteral("trigger.force_quit_keyboard")).trimmed();
}

