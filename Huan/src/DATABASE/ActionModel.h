#pragma once

#include <QString>

// 动作库里的一个可复用动作（不绑定场景/槽位）。
struct LibraryAction {
    int id = -1;
    QString name;
    int type = -1;
    QString payload;
    int usageCount = 0; // 被执行过的次数，只对 id > 0 的动作累加
};

// 轮盘槽位上的动作。
struct Action {
    // 对应 actions_lib.id。只有"引用动作库"的动作才有值：
    // 轮盘槽位 / 动作页格子 -> 真实 id；流水线子步骤、旧表兜底 -> -1（不参与统计）
    int id = -1;
    int slot = -1;   // 槽位 (0-7)，-1 表示不绑定槽位
    QString name;    // 显示名字
    int type = -1;   // 0 按键，1 程序/链接，2 剪贴板转换，3 外部脚本，4 流水线，5 输入文本
    QString payload; // 参数
};

// 中键/侧键直接绑定的动作。
struct MouseBinding {
    QString button;  // "middle" / "x1" / "x2"
    QString name;
    int type = -1;
    QString payload;
};

// 鼠标按键的稳定字符串标识，供数据库和 UI 共用。
enum class MouseButton {
    None = 0,
    Right,
    Middle,
    X1,
    X2,
};

inline QString mouseButtonKey(MouseButton button) {
    switch (button) {
    case MouseButton::Right: return QStringLiteral("right");
    case MouseButton::Middle: return QStringLiteral("middle");
    case MouseButton::X1: return QStringLiteral("x1");
    case MouseButton::X2: return QStringLiteral("x2");
    default: return QStringLiteral("none");
    }
}

inline MouseButton mouseButtonFromKey(const QString& key) {
    const QString normalized = key.trimmed().toLower();
    if (normalized == QLatin1String("right")) return MouseButton::Right;
    if (normalized == QLatin1String("middle")) return MouseButton::Middle;
    if (normalized == QLatin1String("x1")) return MouseButton::X1;
    if (normalized == QLatin1String("x2")) return MouseButton::X2;
    return MouseButton::None;
}

inline QString mouseButtonDisplayName(MouseButton button) {
    switch (button) {
    case MouseButton::Right: return QStringLiteral("右键");
    case MouseButton::Middle: return QStringLiteral("中键");
    case MouseButton::X1: return QStringLiteral("侧键1 (X1)");
    case MouseButton::X2: return QStringLiteral("侧键2 (X2)");
    default: return QStringLiteral("无");
    }
}
