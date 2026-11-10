#pragma once

#include <QPoint>
#include <QString>
#include <QVector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// 一个按键：虚拟键码 + 是否为扩展键（方向键、小键盘回车等）。
struct KeyInput {
    WORD vk = 0;
    bool extended = false;
};

// 解析后的组合键，例如 Ctrl+Shift+Esc。
struct KeyChord {
    QVector<KeyInput> modifiers; // 修饰键（Ctrl/Shift/Alt/Win）
    QVector<KeyInput> keys;      // 主键序列（支持同时按下多个，如 A+S+D）
    KeyInput key;                // 兼容旧代码：保留第一个主键
    bool valid = false;
    QString error;               // 解析失败原因
};

class Win32Utils {
public:
    // 解析任意组合键。支持：
    //   Ctrl / Control / RCtrl、Shift / RShift、Alt / RAlt、Win / Meta / RWin
    //   A-Z、0-9、F1-F24、方向键、Home/End/PgUp/PgDn、Insert/Delete
    //   小键盘、多媒体键、浏览器键、常见符号（+ - = ; ' / \ [ ] ` , .）
    // 注意：Ctrl+Alt+Del 是 Windows 安全注意序列，SendInput 无法模拟，会被判为无效。
    static KeyChord parseKeySequence(const QString& sequence);

    // 发送组合键。返回 false 时 error 中带原因。
    static bool sendKeySequence(const QString& sequence, QString* error = nullptr);

    // 输入任意 Unicode 文本（不受键盘布局/输入法影响）。
    static bool sendText(const QString& text, QString* error = nullptr);

    // 模拟单键，可附带 Ctrl。
    static void sendKey(WORD vk, bool withCtrl);

    // 剪贴板读写（目前只处理文本格式）。
    static QString getClipboardText();
    static bool setClipboardText(const QString& text);

    // 模拟 Ctrl+C 抓取当前选中文字，并尽量恢复原剪贴板。
    // 没有选中内容时返回空字符串。
    static QString captureSelectedText(int waitMs = 250);

    // 释放本程序注入过、但仍处于按下状态的修饰键，避免 Ctrl 卡死。
    static void clearModifiers();

    // 当前前台窗口的进程名，例如 "Code.exe"；取不到时返回 "global"。
    static QString getForegroundProcessName();

    // 绕过 Windows 前台窗口限制，把焦点还给指定窗口。
    static void forceForegroundWindow(HWND hwnd);

    // 把低级鼠标钩子给出的物理屏幕坐标换算成 Qt 逻辑坐标（多屏 + DPI）。
    static QPoint physicalToLogical(const POINT& pt);

    // 查询某个虚拟键当前是否按下。
    static bool isKeyDown(int vk);
};
