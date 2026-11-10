#pragma once

#include <QMap>
#include <QString>

// 运行时配置缓存。所有取值都在 reload() 时从 settings 表读入。
class AppConfig {
public:
    static AppConfig& instance();

    void reload();

    QString mouseTrigger() const;   // right / middle / x1 / x2 / none
    QString keyboardTrigger() const; // 例如 Ctrl+Alt+Space，空表示不启用
    int dragThreshold() const;      // 像素
    int longPressMs() const;        // 长按放弃拦截的阈值
    int wheelHideMs() const;        // 轮盘自动隐藏时间
    int scriptTimeoutMs() const;    // 外部脚本超时

    QString value(const QString& key, const QString& defaultValue = QString()) const;

    // 最小 / 最大值为可选范围约束：只传 (key, defaultValue) 时不做范围限制
    int intValue(const QString& key, int defaultValue,
                 int minimum = -2147483647, int maximum = 2147483647) const;

    QString actionPageHotkey() const; // 动作页呼出快捷键
    QString settingsHotkey() const;   // 设置界面呼出快捷键
    QString forceQuitHotkey() const;  // 强制退出快捷键


private:
    AppConfig() = default;
    QMap<QString, QString> m_values;
};
