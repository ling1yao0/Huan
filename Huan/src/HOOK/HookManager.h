#pragma once

#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class HookManager {
public:
    using MouseCallback = std::function<bool(WPARAM, MSLLHOOKSTRUCT*)>;
    using KeyboardCallback = std::function<bool(WPARAM, KBDLLHOOKSTRUCT*)>;

    HookManager() = default;
    ~HookManager();

    // 安装低级鼠标钩子（WH_MOUSE_LL）。
    bool install(MouseCallback callback);

    // 安装低级键盘钩子（WH_KEYBOARD_LL），用于全局热键。
    bool installKeyboard(KeyboardCallback callback);

    // 卸载全部钩子。
    void uninstall();

private:
    // 给 Windows 调用的静态函数没有 this，通过 s_instance 找到对象。
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    void uninstallMouse();
    void uninstallKeyboard();

    HHOOK m_hHook = nullptr;
    HHOOK m_hKeyboard = nullptr;

    MouseCallback m_callback;
    KeyboardCallback m_keyCallback;

    static HookManager* s_instance;
};
