#include "HookManager.h"

HookManager* HookManager::s_instance = nullptr;

HookManager::~HookManager() {
    uninstall();
}

bool HookManager::install(MouseCallback callback) {
    m_callback = std::move(callback);
    s_instance = this;

    m_hHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
    return m_hHook != nullptr;
}

bool HookManager::installKeyboard(KeyboardCallback callback) {
    m_keyCallback = std::move(callback);
    s_instance = this;

    m_hKeyboard = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    return m_hKeyboard != nullptr;
}

void HookManager::uninstallMouse() {
    if (m_hHook) {
        UnhookWindowsHookEx(m_hHook);
        m_hHook = nullptr;
    }
    m_callback = nullptr;
}

void HookManager::uninstallKeyboard() {
    if (m_hKeyboard) {
        UnhookWindowsHookEx(m_hKeyboard);
        m_hKeyboard = nullptr;
    }
    m_keyCallback = nullptr;
}

void HookManager::uninstall() {
    uninstallMouse();
    uninstallKeyboard();
    s_instance = nullptr;
}

LRESULT CALLBACK HookManager::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance && s_instance->m_callback) {
        auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // 过滤本程序 SendInput 产生的注入事件，避免自己触发自己（死循环）。
        if (!(mouse->flags & LLMHF_INJECTED)) {
            if (s_instance->m_callback(wParam, mouse)) {
                return 1; // 吞掉事件，阻止原生右键菜单等
            }
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK HookManager::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance && s_instance->m_keyCallback) {
        auto* keyboard = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (!(keyboard->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED))) {
            if (s_instance->m_keyCallback(wParam, keyboard)) {
                return 1; // 吞掉热键，避免字符输入到目标窗口
            }
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
