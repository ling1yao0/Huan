#pragma once

#include <QColor>
#include <QHash>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <functional>

#include "ActionModel.h"
#include "Win32Utils.h" // KeyChord / windows.h

class WheelWindow : public QWidget {
    Q_OBJECT // <--- 修复：必须加这一行，否则信号不能用
signals:
    void triggerActionPage();
    void triggerSettingsWindow(); // 新增：呼出设置界面
    void triggerForceQuit();      // 新增：强制退出

public:
    WheelWindow();

    // 重新读取所有动作名字（用于热重载）。
    void refreshFromDatabase();

    // 重新读取触发方式、热键、鼠标绑定等设置。
    void reloadConfig();

    // 配置窗口打开时，不在它上面拦截鼠标/键盘。
    void setEditorWindow(HWND hwnd);

    // 开发预览：直接把轮盘显示在鼠标位置（不经过钩子）。
    void showPreview();

    // 全局鼠标钩子回调。返回 true 表示吞掉当前事件。
    bool handleGlobalMouse(WPARAM wParam, MSLLHOOKSTRUCT* mouse);

    // 全局键盘钩子回调。返回 true 表示吞掉当前事件。
    bool handleGlobalKey(WPARAM wParam, KBDLLHOOKSTRUCT* keyboard);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void loadSlotsForScene(const QString& scene);
    void showWheelAt(const QPoint& logicalCenter, bool fromKeyboard);
    void hideWheel();
    void updateHoverAt(const QPoint& logicalPos);
    int hitTestSector(const QPoint& localPos) const;
    int wheelOuterRadius() const; // 轮盘最外圈半径（隐藏环开启时会为外圈预留 60px）
    void updateWheelGeometry();   // 按当前隐藏环开关调整窗口尺寸

    void armTrigger(const POINT& point);
    void handleTriggerRelease();
    void handleLongPress();

    void startActionForSector(int sector);
    void runActionAfterFocus(HWND target, const std::function<void()>& action);
    void replayButton(MouseButton button, bool downOnly);

    bool handleBindingDown(MouseButton button);
    bool handleBindingUp(MouseButton button);

    void resetInteraction();
    void cancelWheel();

    bool hotkeyMatches(WORD vk, const KeyChord& chord) const;
    bool modifierDown(WORD vk) const;

    static MouseButton buttonForMessage(WPARAM wParam, const MSLLHOOKSTRUCT* mouse);
    static bool isButtonDown(WPARAM wParam);
    static bool isButtonUp(WPARAM wParam);

    // 轮盘鼠标触发状态
    bool m_armed = false;
    bool m_givenUp = false;
    bool m_swallowTriggerUp = false;
    bool m_keyboardMode = false;
    int m_hoveredSector = -1;
    int m_rings = 1; // 当前场景轮盘环数（1/2/3）
    QPoint m_anchor;      // 轮盘中心的逻辑屏幕坐标
    QPoint m_mouseLocal;  // 鼠标相对窗口的坐标
    HWND m_previousHwnd = nullptr;
    HWND m_editorHwnd = nullptr;
    QString m_scene = QStringLiteral("global");
    QStringList m_slotNames; // 固定 8 项，索引就是槽位
    MouseButton m_triggerButton = MouseButton::Right;

    QTimer* m_hideTimer = nullptr;
    QTimer* m_longPressTimer = nullptr;

    // 全局热键状态
    KeyChord m_hotkey;
    QString m_hotkeyText;
    QSet<WORD> m_downKeys;
    bool m_hotkeyActive = false; // 轮盘由热键打开
    bool m_hotkeyDown = false;   // 热键主键正被按住（用于吞掉抬起）

    // 中键 / 侧键直接绑定
    QHash<QString, MouseBinding> m_mouseBindings;
    bool m_bindingDown = false;
    QString m_bindingKey;
    HWND m_bindingHwnd = nullptr;
    QString m_bindingScene;

    // 配置缓存
    int m_dragThreshold = 3;
    int m_longPressMs = 220;
    int m_hideMs = 3000;

    // UI 缓存
    int m_wheelSize = 400;
    int m_innerRadius = 50;
    QColor m_bgColor;
    QColor m_hoverColor;
    QColor m_lineColor;
    QColor m_textColor;
    QColor m_textHoverColor;
    int m_fontSizeBase = 10;
    bool m_enableHiddenRing = false; // 外圈隐藏动作（槽位 56~63）

    // 动作页呼出
    KeyChord m_pageHotkey;
    QString m_pageHotkeyText;

    // 设置界面 / 强制退出
    KeyChord m_settingsHotkey;
    QString m_settingsHotkeyText;
    KeyChord m_forceQuitHotkey;
    QString m_forceQuitHotkeyText;
};