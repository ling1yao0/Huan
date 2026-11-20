#include "WheelWindow.h"

#include "ActionEngine.h"
#include "ActionRepository.h"
#include "AppConfig.h"

#include <QCursor>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include <QGuiApplication>
#include <QScreen>
#include <QToolTip>

namespace {
constexpr int kSectorCount = 8;
constexpr int kSectorDegrees = 360 / kSectorCount;
// 轮盘整体旋转偏移：让最内环扇区的中心对准正上方（而非分割线在正上方）
constexpr double kRotationOffset = 22.5;
// 隐藏环：外圈之外再留出的宽度，槽位 56~63
constexpr int kHiddenRingWidth = 60;
constexpr int kHiddenRingSlotBase = 56;
constexpr int kSlotCount = 64;
} // namespace

WheelWindow::WheelWindow() {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                   Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    // 轮盘只是视觉层：点击穿透到下面的窗口，鼠标选择由全局钩子处理。
    setAttribute(Qt::WA_TransparentForMouseEvents);
    resize(m_wheelSize, m_wheelSize);

    for (int i = 0; i < kSectorCount; ++i) {
        m_slotNames.append(QString());
    }

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, [this] {
        if (!isVisible()) return;
        if (m_armed) {
            // 鼠标触发键还按着：自动隐藏后把它的抬起吞掉。
            m_swallowTriggerUp = true;
        }
        hideWheel();
        m_armed = false;
        m_givenUp = false;
        m_hotkeyActive = false;
    });

    m_longPressTimer = new QTimer(this);
    m_longPressTimer->setSingleShot(true);
    connect(m_longPressTimer, &QTimer::timeout, this, &WheelWindow::handleLongPress);

    reloadConfig();
    refreshFromDatabase();

    // 预热原生窗口：避免第一次呼出时在低级钩子里创建窗口造成卡顿，
    // 甚至触发 Windows 的低级钩子超时保护。
    move(-10000, -10000);
    show();
    hide();
}

void WheelWindow::refreshFromDatabase() {
    loadSlotsForScene(m_scene);
}

void WheelWindow::setEditorWindow(HWND hwnd) {
    m_editorHwnd = hwnd;
}

void WheelWindow::showPreview() {
    m_scene = QStringLiteral("global");
    loadSlotsForScene(m_scene);
    m_slotNames[1] = QStringLiteral("一个很长的动作名字");
    m_previousHwnd = GetForegroundWindow();
    showWheelAt(QCursor::pos(), false);
    m_mouseLocal = QPoint(width() / 2 + 120, height() / 2 - 30);
    m_hoveredSector = hitTestSector(m_mouseLocal);
    update();
}

void WheelWindow::reloadConfig() {
    AppConfig& config = AppConfig::instance();

    m_triggerButton = mouseButtonFromKey(config.mouseTrigger());
    m_dragThreshold = config.dragThreshold();
    m_longPressMs = config.longPressMs();
    m_hideMs = config.wheelHideMs();

    const QString hotkeyText = config.keyboardTrigger();
    if (hotkeyText != m_hotkeyText) {
        m_hotkeyText = hotkeyText;
        m_hotkey = hotkeyText.isEmpty() ? KeyChord() : Win32Utils::parseKeySequence(hotkeyText);
        m_hotkeyDown = false;
        m_hotkeyActive = false;

        if (!hotkeyText.isEmpty() && !m_hotkey.valid) {
            qWarning().noquote() << "全局热键无效:" << hotkeyText << "原因:" << m_hotkey.error;
        }
    }

    const QString pageHotkeyText = config.actionPageHotkey();
    if (pageHotkeyText != m_pageHotkeyText) {
        m_pageHotkeyText = pageHotkeyText;
        m_pageHotkey = pageHotkeyText.isEmpty() ? KeyChord() : Win32Utils::parseKeySequence(pageHotkeyText);
    }

    // 呼出设置界面
    const QString settingsHotkeyText = config.settingsHotkey();
    if (settingsHotkeyText != m_settingsHotkeyText) {
        m_settingsHotkeyText = settingsHotkeyText;
        m_settingsHotkey =
            settingsHotkeyText.isEmpty() ? KeyChord() : Win32Utils::parseKeySequence(settingsHotkeyText);
    }

    // 强制退出
    const QString forceQuitHotkeyText = config.forceQuitHotkey();
    if (forceQuitHotkeyText != m_forceQuitHotkeyText) {
        m_forceQuitHotkeyText = forceQuitHotkeyText;
        m_forceQuitHotkey =
            forceQuitHotkeyText.isEmpty() ? KeyChord() : Win32Utils::parseKeySequence(forceQuitHotkeyText);
    }

    m_mouseBindings.clear();
    for (const MouseBinding& binding : ActionRepository::getMouseBindings()) {
        if (binding.type < 0 || binding.button.isEmpty()) continue;
        m_mouseBindings.insert(binding.button.trimmed().toLower(), binding);
    }

    // === 新增：读取 UI 配置 ===
    m_wheelSize = config.intValue(QStringLiteral("ui.wheel.size"), 400, 200, 800);
    m_innerRadius = config.intValue(QStringLiteral("ui.wheel.inner_radius"), 50, 30, 200);
    
    m_bgColor = QColor(config.value(QStringLiteral("ui.wheel.bg_color"), "#FEFEFE"));
    m_hoverColor = QColor(config.value(QStringLiteral("ui.wheel.hover_color"), "#ECECEC"));
    m_lineColor = QColor(config.value(QStringLiteral("ui.wheel.line_color"), "#ECECEC"));
    m_textColor = QColor(config.value(QStringLiteral("ui.wheel.text_color"), "#333333"));
    m_textHoverColor = QColor(config.value(QStringLiteral("ui.wheel.text_hover_color"), "#000000"));
    m_fontSizeBase = config.intValue(QStringLiteral("ui.wheel.font_size"), 10, 6, 20);
    // 外圈隐藏动作开关（全局默认值，具体场景值在 loadSlotsForScene 中覆盖）
    m_enableHiddenRing = config.intValue(QStringLiteral("ui.wheel.enable_hidden_ring"), 0) == 1;
    updateWheelGeometry();
}

void WheelWindow::updateWheelGeometry() {
    // 开启隐藏环时为外圈多预留 60px 的绘制空间，轮盘半径保持不变
    const int targetSize = m_wheelSize + (m_enableHiddenRing ? 2 * kHiddenRingWidth : 0);
    if (width() != targetSize) {
        resize(targetSize, targetSize);
    }
}

int WheelWindow::wheelOuterRadius() const {
    const int pad = m_enableHiddenRing ? kHiddenRingWidth : 0;
    return width() / 2 - 10 - pad;
}

void WheelWindow::loadSlotsForScene(const QString& scene) {
    m_rings = ActionRepository::getSceneRings(scene);
    m_slotNames.clear();
    // 56 个普通槽位 + 8 个隐藏环虚拟槽位（56~63）
    for (int i = 0; i < kSlotCount; ++i) {
        m_slotNames.append(QString());
    }

    for (const Action& action : ActionRepository::getAllActions(scene)) {
        if (action.slot < 0 || action.slot >= kSlotCount) continue;
        if (action.type < 0) continue; 
        m_slotNames[action.slot] = action.name;
    }

    // 场景专属的隐藏环开关：-1 表示继承全局配置
    const int sceneHidden = ActionRepository::getSceneHiddenRing(scene);
    if (sceneHidden == -1) {
        m_enableHiddenRing = AppConfig::instance().intValue(QStringLiteral("ui.wheel.enable_hidden_ring"), 0) == 1;
    } else {
        m_enableHiddenRing = (sceneHidden == 1);
    }
    updateWheelGeometry();
}

bool WheelWindow::handleGlobalMouse(WPARAM wParam, MSLLHOOKSTRUCT* mouse) {
    if (!mouse) return false;
    if (m_editorHwnd && IsWindowVisible(m_editorHwnd) &&
        GetForegroundWindow() == m_editorHwnd) {
        return false;
    }

    // 键盘热键打开的轮盘：鼠标只用来移动高亮和左键选择。
    if (isVisible() && m_keyboardMode) {
        if (wParam == WM_MOUSEMOVE) {
            updateHoverAt(Win32Utils::physicalToLogical(mouse->pt));
            m_hideTimer->start(m_hideMs);
            return false;
        }
        if (wParam == WM_LBUTTONDOWN) {
            const QPoint local = Win32Utils::physicalToLogical(mouse->pt) - pos();
            const int sector = hitTestSector(local);
            if (sector >= 0) {
                startActionForSector(sector);
            } else {
                cancelWheel();
            }
            return true;
        }
        if (wParam == WM_LBUTTONUP) return true;
    }

    if (wParam == WM_MOUSEMOVE) {
        if (m_armed && !m_givenUp) {
            const QPoint logical = Win32Utils::physicalToLogical(mouse->pt);
            const int dx = logical.x() - m_anchor.x();
            const int dy = logical.y() - m_anchor.y();

            if (dx * dx + dy * dy > m_dragThreshold * m_dragThreshold) {
                // 越阈即认定要呼出轮盘，长按判定立刻作废。
                // showWheelAt() 内部本来就 stop 了一次，但那只在"首次呼出"分支执行；
                // 这里前置一刀，以后无论再加什么呼出路径都不会漏。
                m_longPressTimer->stop();
                if (!isVisible()) {
                    showWheelAt(m_anchor, false);
                }
                updateHoverAt(logical);
                m_hideTimer->start(m_hideMs);
            }
        } else if (isVisible() && !m_keyboardMode) {
            updateHoverAt(Win32Utils::physicalToLogical(mouse->pt));
            m_hideTimer->start(m_hideMs);
        }
        return false;
    }

    const MouseButton button = buttonForMessage(wParam, mouse);
    if (button == MouseButton::None) return false;

    const bool down = isButtonDown(wParam);
    const bool up = isButtonUp(wParam);

    if (button == m_triggerButton) {
        if (down) {
            if (m_armed) return true;
            if (isVisible()) {
                hideWheel();
                resetInteraction();
            }
            armTrigger(mouse->pt);
            return true;
        }

        if (up) {
            if (m_swallowTriggerUp) {
                m_swallowTriggerUp = false;
                m_armed = false;
                return true;
            }
            if (m_givenUp) {
                // 长按已经补发过按下事件，这次抬起交给系统。
                m_givenUp = false;
                m_armed = false;
                return false;
            }
            if (!m_armed) return false;
            handleTriggerRelease();
            return true;
        }
    }

    // 中键 / 侧键直接绑定动作（与轮盘触发键互斥）。
    if (down && handleBindingDown(button)) return true;
    if (up && handleBindingUp(button)) return true;

    return false;
}

bool WheelWindow::handleGlobalKey(WPARAM wParam, KBDLLHOOKSTRUCT* keyboard) {
    if (!keyboard) return false;
    if (m_editorHwnd && IsWindowVisible(m_editorHwnd) &&
        GetForegroundWindow() == m_editorHwnd) {
        return false;
    }

    const WORD vk = static_cast<WORD>(keyboard->vkCode);
    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!down && !up) return false;

    if (down) {
        if (vk == VK_ESCAPE && isVisible()) {
            cancelWheel();
            return true;
        }

        m_downKeys.insert(vk);

        // 强制退出优先级最高（配置界面也可能卡住，必须能随时脱身）
        if (m_forceQuitHotkey.valid && hotkeyMatches(vk, m_forceQuitHotkey)) {
            emit triggerForceQuit();
            return true;
        }

        if (m_settingsHotkey.valid && hotkeyMatches(vk, m_settingsHotkey)) {
            emit triggerSettingsWindow();
            return true;
        }

        if (m_pageHotkey.valid && hotkeyMatches(vk, m_pageHotkey)) {
            emit triggerActionPage();
            return true; // 吞掉按键
        }

        if (m_hotkey.valid && !m_hotkeyDown && hotkeyMatches(vk, m_hotkey)) {
            m_hotkeyDown = true;
            m_hotkeyActive = true;
            m_keyboardMode = true;
            m_previousHwnd = GetForegroundWindow();
            m_scene = Win32Utils::getForegroundProcessName().toLower();;
            qDebug() << "【调试】热键呼出轮盘，抓到的当前场景名:" << m_scene; // 加上这一行
            loadSlotsForScene(m_scene);

            showWheelAt(QCursor::pos(), true);
            return true;
        }

        // 修改这里：如果用户设置了多键热键，按住主键不放触发自动重复时拦截
        if (m_hotkeyDown && m_hotkey.valid) {
            for (const auto& k : m_hotkey.keys) {
                if (vk == k.vk) return true;
            }
        }
    } else {
        // 修改这里
        bool isMainKeyUp = false;
        if (m_hotkeyDown && m_hotkey.valid) {
            for (const auto& k : m_hotkey.keys) {
                if (vk == k.vk) isMainKeyUp = true;
            }
        }
        
        if (m_hotkeyDown && isMainKeyUp) {
            const bool wasActive = m_hotkeyActive;
            const int sector = m_hoveredSector;

            m_hotkeyDown = false;
            m_hotkeyActive = false;
            m_downKeys.remove(vk);

            if (wasActive && isVisible()) {
                if (sector >= 0) {
                    startActionForSector(sector);
                } else {
                    cancelWheel();
                }
            } else {
                hideWheel();
            }
            return true;
        }
        m_downKeys.remove(vk);
    }

    return false;
}

void WheelWindow::armTrigger(const POINT& point) {
    m_armed = true;
    m_givenUp = false;
    m_swallowTriggerUp = false;
    m_keyboardMode = false;
    m_hoveredSector = -1;
    m_anchor = Win32Utils::physicalToLogical(point);
    m_previousHwnd = GetForegroundWindow();
    m_scene = Win32Utils::getForegroundProcessName().toLower();;
    qDebug() << "【调试】鼠标呼出轮盘，抓到的当前场景名:" << m_scene; // 加上这一行
    loadSlotsForScene(m_scene);
    m_longPressTimer->start(m_longPressMs);
}

void WheelWindow::handleTriggerRelease() {
    m_armed = false;
    m_longPressTimer->stop();

    if (isVisible()) {
        const int sector = m_hoveredSector;
        if (sector >= 0) {
            startActionForSector(sector);
        } else {
            cancelWheel();
        }
        return;
    }

    // 短按：把这次点击原样还给系统（兼容右键菜单、中键滚动、侧键前进后退）。
    replayButton(m_triggerButton, false);
    resetInteraction();
}

void WheelWindow::handleLongPress() {
    if (!m_armed || isVisible()) return;

    // 超过长按阈值仍没有移动：补发按下事件，放弃拦截这次点击。
    replayButton(m_triggerButton, true);
    m_givenUp = true;
    m_armed = false;
}

void WheelWindow::showWheelAt(const QPoint& logicalCenter, bool fromKeyboard) {
    if (!isVisible()) {
        m_anchor = logicalCenter;
        m_keyboardMode = fromKeyboard;

        QPoint topLeft = logicalCenter - QPoint(width() / 2, height() / 2);

        // 把轮盘窗口限制在鼠标所在屏幕的可用区域内，
        // 避免触发点靠近屏幕边缘时轮盘一半跑到屏幕外。
        if (QScreen* screen = QGuiApplication::screenAt(logicalCenter)) {
            const QRect available = screen->availableGeometry();
            // QRect::right()/bottom() 是闭区间，所以要 +1；
            // qMax 兜底：屏幕可用宽度比窗口还小时，至少保证左/上不越界。
            const int maxX = qMax(available.left(), available.right() - width() + 1);
            const int maxY = qMax(available.top(), available.bottom() - height() + 1);
            topLeft.setX(qBound(available.left(), topLeft.x(), maxX));
            topLeft.setY(qBound(available.top(), topLeft.y(), maxY));
        }

        move(topLeft);

        // 窗口被 clamp 后，鼠标不一定在窗口中心；
        // 用实际窗口位置重新算鼠标相对坐标，让红线一开始就指向鼠标，
        // 同时立即高亮正确扇区（鼠标在窗口外时 hitTestSector 返回 -1）。
        m_mouseLocal = logicalCenter - topLeft;
        m_hoveredSector = hitTestSector(m_mouseLocal);

        show();
        raise();

        m_hideTimer->start(m_hideMs);
        m_longPressTimer->stop();
        update();
    } else {
        m_keyboardMode = fromKeyboard;
        updateHoverAt(logicalCenter);
    }
}

void WheelWindow::hideWheel() {
    if (isVisible()) hide();
    m_hoveredSector = -1;
    m_keyboardMode = false;
    m_hideTimer->stop();
    update();
}

void WheelWindow::updateHoverAt(const QPoint& logicalPos) {
    m_mouseLocal = logicalPos - pos();
    m_hoveredSector = hitTestSector(m_mouseLocal);
    update();
}

int WheelWindow::hitTestSector(const QPoint& localPos) const {
    const int cx = width() / 2;
    const int cy = height() / 2;
    const int outerRadius = wheelOuterRadius();
    const int innerRadius = m_innerRadius;
    
    // 使用浮点数计算厚度，避免截断误差
    qreal ringThickness = static_cast<qreal>(outerRadius - innerRadius) / m_rings;
    const int dx = localPos.x() - cx;
    const int dy = localPos.y() - cy;
    double dist = std::sqrt(dx * dx + dy * dy);

    // 隐藏环：只要超出真实外圈，无论多远都命中（飞出屏幕边缘也能触发）
    if (m_enableHiddenRing && dist >= outerRadius) {
        const int hiddenSectorCount = 8;
        const qreal hiddenDegrees = 360.0 / hiddenSectorCount;
        double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx)) * 180.0 / M_PI;
        angle += 90.0 + kRotationOffset;
        if (angle < 0.0) angle += 360.0;
        if (angle >= 360.0) angle -= 360.0;
        const int sector = static_cast<int>(angle / hiddenDegrees);
        return kHiddenRingSlotBase + sector;
    }

    if (dist <= innerRadius || dist >= outerRadius) return -1;
    int ring = static_cast<int>((dist - innerRadius) / ringThickness);
    if (ring >= m_rings) ring = m_rings - 1;
    int sectorCount = 8 << ring;
    qreal degreesPerSector = 360.0 / sectorCount;
    
    double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx)) * 180.0 / M_PI;
    // 整体旋转 22.5 度：让扇区中心对准正上方
    angle += 90.0 + kRotationOffset;
    if (angle < 0.0) angle += 360.0;
    if (angle >= 360.0) angle -= 360.0;
    int sector = static_cast<int>(angle / degreesPerSector);
    int baseSlot = (ring == 0) ? 0 : (ring == 1 ? 8 : 24);
    
    return baseSlot + sector;
}

void WheelWindow::startActionForSector(int sector) {
    const HWND target = m_previousHwnd;
    const QString scene = m_scene;

    hideWheel();
    resetInteraction();

    if (sector < 0) return;

    runActionAfterFocus(target, [sector, scene] {
        ActionEngine::execute(sector, scene);
    });
}

void WheelWindow::runActionAfterFocus(HWND target, const std::function<void()>& action) {
    if (target) {
        Win32Utils::forceForegroundWindow(target);
    }

    // 给目标窗口一点时间重新获得键盘焦点，再注入按键。
    QTimer::singleShot(150, this, [this, action, target] {
        if (target && GetForegroundWindow() != target) {
            // 系统第一次常常只是“表面答应”，这里补抢一次。
            Win32Utils::forceForegroundWindow(target);
        }
        if (!target || GetForegroundWindow() == target) {
            action();
            return;
        }

        // 还是没抢到。这里不用 Sleep / msleep 干等 —— 那会把主线程和全局钩子一起卡住，
        // 等于把上一个卡键问题请回来。交给下一轮事件循环再确认一次。
        QTimer::singleShot(50, this, [this, action, target] {
            if (GetForegroundWindow() != target) {
                Win32Utils::forceForegroundWindow(target);
            }
            if (GetForegroundWindow() == target) {
                action();
                return;
            }

            // 熔断：目标窗口彻底不交出前台焦点（卡死，或被系统拒绝）。
            // 宁可这次动作不执行，也不能把 Alt+F4 这类系统键发到桌面上 ——
            // 那会直接弹出“关闭 Windows”的黑屏界面。
            qWarning() << "【熔断】焦点抢夺失败，目标不是前台窗口，已放弃执行本次动作";
            QToolTip::showText(QCursor::pos(),
                               QStringLiteral("目标窗口未获得焦点，已取消本次动作"));
        });
    });
}

void WheelWindow::replayButton(MouseButton button, bool downOnly) {
    INPUT inputs[2] = {};
    int count = 0;

    auto append = [&](bool down) {
        INPUT input = {};
        input.type = INPUT_MOUSE;

        switch (button) {
        case MouseButton::Right:
            input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case MouseButton::Middle:
            input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        case MouseButton::X1:
            input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON1;
            break;
        case MouseButton::X2:
            input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON2;
            break;
        default:
            return;
        }

        inputs[count++] = input;
    };

    append(true);
    if (!downOnly) append(false);

    if (count == 0) return;

    // 长按放弃拦截：只补按下，抬起用系统的真实事件，间隔天然存在。
    if (downOnly) {
        SendInput(1, inputs, sizeof(INPUT));
        return;
    }

    // 短按透传：按下立刻补发，抬起延后 25ms。
    // 把 down / up 塞进同一次 SendInput 的话，目标程序收到的是"0 毫秒点击"——
    // Electron 系（VS Code）会弹出右键菜单后立刻关掉，看起来就是菜单"闪一下"；
    // 原生程序（Keil）反应钝，0ms 点击也能把菜单留住，所以只有 VS Code 出问题。
    SendInput(1, inputs, sizeof(INPUT));
    const MouseButton pending = button;
    QTimer::singleShot(25, this, [pending]() {
        INPUT up = {};
        up.type = INPUT_MOUSE;
        switch (pending) {
        case MouseButton::Right: up.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
        case MouseButton::Middle: up.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
        case MouseButton::X1:
            up.mi.dwFlags = MOUSEEVENTF_XUP;
            up.mi.mouseData = XBUTTON1;
            break;
        case MouseButton::X2:
            up.mi.dwFlags = MOUSEEVENTF_XUP;
            up.mi.mouseData = XBUTTON2;
            break;
        default:
            return;
        }
        SendInput(1, &up, sizeof(INPUT));
    });
}

bool WheelWindow::handleBindingDown(MouseButton button) {
    const QString key = mouseButtonKey(button);
    const auto it = m_mouseBindings.constFind(key);
    if (it == m_mouseBindings.constEnd() || it->type < 0) return false;

    m_bindingDown = true;
    m_bindingKey = key;
    m_bindingHwnd = GetForegroundWindow();
    m_bindingScene = Win32Utils::getForegroundProcessName().toLower();
    return true;
}

bool WheelWindow::handleBindingUp(MouseButton button) {
    const QString key = mouseButtonKey(button);
    if (!m_bindingDown || m_bindingKey != key) return false;

    m_bindingDown = false;

    const auto it = m_mouseBindings.constFind(key);
    if (it == m_mouseBindings.constEnd() || it->type < 0) return true;

    Action action;
    action.slot = -1;
    action.name = it->name;
    action.type = it->type;
    action.payload = it->payload;

    const HWND target = m_bindingHwnd;
    const QString scene = m_bindingScene;
    runActionAfterFocus(target, [action, scene] {
        ActionEngine::executeAction(action, scene);
    });
    return true;
}

void WheelWindow::resetInteraction() {
    m_armed = false;
    m_givenUp = false;
    m_hotkeyActive = false;
    m_keyboardMode = false;
    m_hoveredSector = -1;
    m_hideTimer->stop();
    m_longPressTimer->stop();
    update();
}

void WheelWindow::cancelWheel() {
    if (m_armed) {
        // 触发键还按着：轮盘取消后要把它的抬起也吞掉，
        // 否则目标程序会收到一个没有对应按下的抬起事件。
        m_swallowTriggerUp = true;
    }

    hideWheel();
    m_armed = false;
    m_givenUp = false;
    m_hotkeyActive = false;
    m_longPressTimer->stop();
    update();
}

bool WheelWindow::hotkeyMatches(WORD vk, const KeyChord& chord) const {
    if (!chord.valid || vk != chord.key.vk) return false;

    for (const KeyInput& modifier : chord.modifiers) {
        if (!modifierDown(modifier.vk)) return false;
    }
    return true;
}

bool WheelWindow::modifierDown(WORD vk) const {
    switch (vk) {
    case VK_CONTROL:
        return Win32Utils::isKeyDown(VK_CONTROL) || Win32Utils::isKeyDown(VK_LCONTROL) ||
               Win32Utils::isKeyDown(VK_RCONTROL) || m_downKeys.contains(VK_CONTROL) ||
               m_downKeys.contains(VK_LCONTROL) || m_downKeys.contains(VK_RCONTROL);
    case VK_SHIFT:
        return Win32Utils::isKeyDown(VK_SHIFT) || Win32Utils::isKeyDown(VK_LSHIFT) ||
               Win32Utils::isKeyDown(VK_RSHIFT) || m_downKeys.contains(VK_SHIFT) ||
               m_downKeys.contains(VK_LSHIFT) || m_downKeys.contains(VK_RSHIFT);
    case VK_MENU:
        return Win32Utils::isKeyDown(VK_MENU) || Win32Utils::isKeyDown(VK_LMENU) ||
               Win32Utils::isKeyDown(VK_RMENU) || m_downKeys.contains(VK_MENU) ||
               m_downKeys.contains(VK_LMENU) || m_downKeys.contains(VK_RMENU);
    case VK_LWIN:
        return Win32Utils::isKeyDown(VK_LWIN) || Win32Utils::isKeyDown(VK_RWIN) ||
               m_downKeys.contains(VK_LWIN) || m_downKeys.contains(VK_RWIN);
    case VK_RWIN:
        return Win32Utils::isKeyDown(VK_RWIN) || m_downKeys.contains(VK_RWIN);
    case VK_RCONTROL:
        return Win32Utils::isKeyDown(VK_RCONTROL) || m_downKeys.contains(VK_RCONTROL);
    case VK_RMENU:
        return Win32Utils::isKeyDown(VK_RMENU) || m_downKeys.contains(VK_RMENU);
    default:
        return Win32Utils::isKeyDown(vk) || m_downKeys.contains(vk);
    }
}

MouseButton WheelWindow::buttonForMessage(WPARAM wParam, const MSLLHOOKSTRUCT* mouse) {
    switch (wParam) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return MouseButton::Right;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return MouseButton::Middle;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        if (!mouse) return MouseButton::None;
        const WORD xButton = HIWORD(mouse->mouseData);
        if (xButton == XBUTTON1) return MouseButton::X1;
        if (xButton == XBUTTON2) return MouseButton::X2;
        return MouseButton::None;
    }
    default:
        return MouseButton::None;
    }
}

bool WheelWindow::isButtonDown(WPARAM wParam) {
    return wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN;
}

bool WheelWindow::isButtonUp(WPARAM wParam) {
    return wParam == WM_RBUTTONUP || wParam == WM_MBUTTONUP || wParam == WM_XBUTTONUP;
}

void WheelWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const int cx = width() / 2;
    const int cy = height() / 2;
    const int outerRadius = wheelOuterRadius(); // 保留 10px 边距；开启隐藏环时已为外圈多留 60px
    const int innerRadius = m_innerRadius;
    // 使用 qreal 防止精度丢失
    qreal ringThickness = static_cast<qreal>(outerRadius - innerRadius) / m_rings;
    // 圆盘背景
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_bgColor); // 替换硬编码颜色
    painter.drawEllipse(QPoint(cx, cy), outerRadius, outerRadius);
    // 绘制悬停扇区高亮（普通环 0~55，隐藏环 56~63）
    if (m_hoveredSector >= 0) {
        qreal rIn = 0.0;
        qreal rOut = 0.0;
        int sector = 0;
        qreal degreesPerSector = 0.0;
        if (m_hoveredSector >= kHiddenRingSlotBase) {
            sector = m_hoveredSector - kHiddenRingSlotBase;
            degreesPerSector = 360.0 / kSectorCount;
            rIn = outerRadius;
            rOut = outerRadius + kHiddenRingWidth;
        } else {
            int ring = (m_hoveredSector < 8) ? 0 : (m_hoveredSector < 24 ? 1 : 2);
            int baseSlot = (ring == 0) ? 0 : (ring == 1 ? 8 : 24);
            sector = m_hoveredSector - baseSlot;
            degreesPerSector = 360.0 / (8 << ring);
            rIn = innerRadius + ring * ringThickness;
            rOut = rIn + ringThickness;
        }
        // 正确的扇环绘制路径
        QPainterPath path;
        QRectF rectOuter(cx - rOut, cy - rOut, rOut * 2, rOut * 2);
        QRectF rectInner(cx - rIn, cy - rIn, rIn * 2, rIn * 2);
        
        // 整体旋转 kRotationOffset：让扇区中心对准正上方
        qreal startAngle = 90.0 + kRotationOffset - sector * degreesPerSector;
        
        path.arcMoveTo(rectOuter, startAngle);
        path.arcTo(rectOuter, startAngle, -degreesPerSector);
        path.arcTo(rectInner, startAngle - degreesPerSector, degreesPerSector);
        path.closeSubpath();
        
        // 悬停高亮：普通环半透明，隐藏环用不透明色（看清选中的外圈动作）
        QColor hoverBg = m_hoverColor;
        hoverBg.setAlpha(m_hoveredSector >= kHiddenRingSlotBase ? 255 : 210);
        painter.setBrush(hoverBg);
        painter.drawPath(path);

        // 隐藏环悬停时额外显示动作名称
        if (m_hoveredSector >= kHiddenRingSlotBase && m_hoveredSector < kSlotCount) {
            const QString text =
                (m_hoveredSector < m_slotNames.size()) ? m_slotNames.at(m_hoveredSector) : QString();
            if (!text.isEmpty()) {
                const qreal textRadius = rIn + (rOut - rIn) / 2.0;
                const qreal angle = (sector * degreesPerSector - 90.0 - kRotationOffset +
                                     degreesPerSector / 2.0) * M_PI / 180.0;
                const int tx = cx + static_cast<int>(textRadius * std::cos(angle));
                const int ty = cy + static_cast<int>(textRadius * std::sin(angle));
                QFont font(QStringLiteral("Microsoft YaHei"));
                font.setPointSize(qMax(6, m_fontSizeBase - 2));
                font.setBold(true);
                painter.setFont(font);
                painter.setPen(QColor(QStringLiteral("#000000")));
                const int maxTextWidth =
                    static_cast<int>(2.0 * textRadius * std::sin(M_PI / kSectorCount)) - 6;
                QFontMetrics metrics(font);
                painter.drawText(QRect(tx - maxTextWidth / 2, ty - metrics.height() / 2,
                                       maxTextWidth, metrics.height()),
                                 Qt::AlignCenter, metrics.elidedText(text, Qt::ElideRight, maxTextWidth));
            }
        }
    }
    // 中心圆
    QPen linePen(m_lineColor); // 替换硬编码颜色
    linePen.setWidth(1);
    painter.setPen(linePen);                         // 关键：使用灰色线笔
    painter.setBrush(m_bgColor); // 替换硬编码颜色
    painter.drawEllipse(QPointF(cx, cy), innerRadius, innerRadius); // 画出带边框的中心圆
    // ================= 绘制外环分割线 =================
    painter.setBrush(Qt::NoBrush); // 取消填充，防止覆盖
    // 从内到外，给每一个环的边界画一个同心圆
    for (int ring = 0; ring < m_rings; ++ring) {
        qreal rOut = innerRadius + (ring + 1) * ringThickness;
        painter.drawEllipse(QPointF(cx, cy), rOut, rOut);
    }
    // =======================================================
    // 分割线与文字
    for (int ring = 0; ring < m_rings; ++ring) {
        int sectorCount = 8 << ring;
        qreal degreesPerSector = 360.0 / sectorCount; // 关键修复：用浮点数
        qreal rIn = innerRadius + ring * ringThickness;
        qreal rOut = rIn + ringThickness;
        qreal textRadius = rIn + ringThickness / 2.0;
        int baseSlot = (ring == 0) ? 0 : (ring == 1 ? 8 : 24);
        
        painter.setPen(linePen);
        for (int i = 0; i < sectorCount; ++i) {
            qreal angle = (i * degreesPerSector - 90.0 - kRotationOffset) * M_PI / 180.0;
            painter.drawLine(QPointF(cx + rIn * std::cos(angle), cy + rIn * std::sin(angle)),
                             QPointF(cx + rOut * std::cos(angle), cy + rOut * std::sin(angle)));
        }
        for (int i = 0; i < sectorCount; ++i) {
            int slot = baseSlot + i;
            if (slot >= kHiddenRingSlotBase) continue; // 隐藏环不画文字/分割内容
            QString text = (slot < m_slotNames.size()) ? m_slotNames.at(slot) : QString();
            if (text.isEmpty()) continue;
            qreal angle = (i * degreesPerSector - 90.0 - kRotationOffset + degreesPerSector / 2.0) * M_PI / 180.0;
            int tx = cx + static_cast<int>(textRadius * std::cos(angle));
            int ty = cy + static_cast<int>(textRadius * std::sin(angle));
            QFont font(QStringLiteral("Microsoft YaHei"));
            // 字号根据环数递减
            int pointSize = qMax(6, m_fontSizeBase - ring); 
            font.setPointSize(pointSize);
            
            int maxTextWidth = static_cast<int>(2.0 * textRadius * std::sin(M_PI / sectorCount)) - 6;
            QFontMetrics metrics(font);
            
            font.setBold(slot == m_hoveredSector);
            metrics = QFontMetrics(font);
            QString shown = metrics.elidedText(text, Qt::ElideRight, maxTextWidth);
            int textHeight = metrics.height();
            painter.setFont(font);
            
            // 替换硬编码文字颜色
            painter.setPen(slot == m_hoveredSector ? m_textHoverColor : m_textColor);
            
            painter.drawText(QRect(tx - maxTextWidth / 2, ty - textHeight / 2, maxTextWidth, textHeight),
                             Qt::AlignCenter, shown);
        }
    }
    // 外圈隐藏动作不绘制任何文字 / 分割线，只在悬停时给最外侧一圈淡高亮（见上方悬停分支）
    // 中心红线
    const int dx = m_mouseLocal.x() - cx;
    const int dy = m_mouseLocal.y() - cy;
    const double distance = std::sqrt(static_cast<double>(dx * dx + dy * dy));
    if (distance > innerRadius) {
        QPen redPen(QColor(QStringLiteral("#F87171")));
        redPen.setWidth(3);
        redPen.setCapStyle(Qt::RoundCap);
        painter.setPen(redPen);
        const double maxDistance = static_cast<double>(outerRadius) + (m_enableHiddenRing ? kHiddenRingWidth : 0);
        const double clamped = qMin(distance, maxDistance);
        const QPointF start(cx + dx * (innerRadius / distance), cy + dy * (innerRadius / distance));
        const QPointF end(cx + dx * (clamped / distance), cy + dy * (clamped / distance));
        painter.drawLine(start, end);
    }
    // 中心提示
    painter.setPen(QColor(QStringLiteral("#9CA3AF")));
    QFont hintFont(QStringLiteral("Microsoft YaHei"));
    hintFont.setPointSize(8);
    painter.setFont(hintFont);
    QString centerText = m_scene;
    if (centerText.length() > 10) centerText = centerText.left(8) + "...";
    painter.drawText(QRect(cx - 46, cy - 20, 92, 40), Qt::AlignCenter, centerText + "\n松开执行");
}