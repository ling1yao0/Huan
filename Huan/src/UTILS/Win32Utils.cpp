#include "Win32Utils.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QScreen>
#include <QSet>
#include <QStringList>

#include <cwchar>

namespace {

// 记录本程序通过 SendInput 按下、但还没松开的修饰键。
// clearModifiers() 只释放这些键，避免把用户物理按住的键“虚拟松开”。
QSet<WORD> g_injectedModifiers;

bool openClipboardWithRetry(int tries = 6) {
    for (int i = 0; i < tries; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(10);
    }
    return false;
}

INPUT makeKeyInput(WORD vk, bool extended, bool keyUp) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    if (extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (keyUp) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    return in;
}

bool flushInput(const QVector<INPUT>& events, QString* error) {
    if (events.isEmpty()) return true;

    const UINT sent = SendInput(static_cast<UINT>(events.size()),
                                const_cast<INPUT*>(events.constData()),
                                sizeof(INPUT));
    if (sent == static_cast<UINT>(events.size())) return true;

    const DWORD code = GetLastError();
    if (error) {
        *error = QStringLiteral("SendInput 失败（Windows 错误码 %1，已发送 %2/%3 个事件）")
                     .arg(code)
                     .arg(sent)
                     .arg(events.size());
        if (code == ERROR_ACCESS_DENIED) {
            *error += QStringLiteral("。目标窗口可能以管理员权限运行，请右键以管理员身份运行本程序。");
        }
    }
    return false;
}

QString normalizedToken(QString token) {
    token = token.trimmed();
    token.remove(QLatin1Char(' '));
    token.remove(QLatin1Char('-'));
    token.remove(QLatin1Char('_'));
    return token.toUpper();
}

bool modifierToken(const QString& normalized, KeyInput& out) {
    if (normalized == QLatin1String("CTRL") || normalized == QLatin1String("CONTROL") ||
        normalized == QLatin1String("LCTRL")) {
        out = {VK_CONTROL, false};
        return true;
    }
    if (normalized == QLatin1String("RCTRL")) {
        out = {VK_RCONTROL, true};
        return true;
    }
    if (normalized == QLatin1String("SHIFT") || normalized == QLatin1String("LSHIFT")) {
        out = {VK_SHIFT, false};
        return true;
    }
    if (normalized == QLatin1String("RSHIFT")) {
        out = {VK_RSHIFT, true};
        return true;
    }
    if (normalized == QLatin1String("ALT") || normalized == QLatin1String("LALT")) {
        out = {VK_MENU, false};
        return true;
    }
    if (normalized == QLatin1String("RALT") || normalized == QLatin1String("ALTGR")) {
        out = {VK_RMENU, true};
        return true;
    }
    if (normalized == QLatin1String("WIN") || normalized == QLatin1String("META") ||
        normalized == QLatin1String("CMD") || normalized == QLatin1String("SUPER") ||
        normalized == QLatin1String("LWIN")) {
        out = {VK_LWIN, false};
        return true;
    }
    if (normalized == QLatin1String("RWIN")) {
        out = {VK_RWIN, true};
        return true;
    }
    return false;
}

const QHash<QString, KeyInput>& namedKeys() {
    static const QHash<QString, KeyInput> map = [] {
        QHash<QString, KeyInput> m;
        auto add = [&m](const char* name, WORD vk, bool extended = false) {
            m.insert(QString::fromLatin1(name), KeyInput{vk, extended});
        };

        add("BACKSPACE", VK_BACK);
        add("BKSP", VK_BACK);
        add("TAB", VK_TAB);
        add("ENTER", VK_RETURN);
        add("RETURN", VK_RETURN);
        add("PAUSE", VK_PAUSE);
        add("BREAK", VK_PAUSE);
        add("CAPSLOCK", VK_CAPITAL);
        add("CAPS", VK_CAPITAL);
        add("ESC", VK_ESCAPE);
        add("ESCAPE", VK_ESCAPE);
        add("SPACE", VK_SPACE);
        add("SPACEBAR", VK_SPACE);

        add("PGUP", VK_PRIOR, true);
        add("PAGEUP", VK_PRIOR, true);
        add("PGDN", VK_NEXT, true);
        add("PAGEDOWN", VK_NEXT, true);
        add("END", VK_END, true);
        add("HOME", VK_HOME, true);
        add("LEFT", VK_LEFT, true);
        add("LEFTARROW", VK_LEFT, true);
        add("UP", VK_UP, true);
        add("UPARROW", VK_UP, true);
        add("RIGHT", VK_RIGHT, true);
        add("RIGHTARROW", VK_RIGHT, true);
        add("DOWN", VK_DOWN, true);
        add("DOWNARROW", VK_DOWN, true);
        add("PRINTSCREEN", VK_SNAPSHOT, true);
        add("PRTSC", VK_SNAPSHOT, true);
        add("PRINT", VK_SNAPSHOT, true);
        add("SYSRQ", VK_SNAPSHOT, true);
        add("SNAPSHOT", VK_SNAPSHOT, true);
        add("INSERT", VK_INSERT, true);
        add("INS", VK_INSERT, true);
        add("DELETE", VK_DELETE, true);
        add("DEL", VK_DELETE, true);
        add("HELP", VK_HELP);
        add("APPS", VK_APPS, true);
        add("MENU", VK_APPS, true);
        add("CONTEXTMENU", VK_APPS, true);
        add("MENUKEY", VK_APPS, true);
        add("SLEEP", VK_SLEEP);

        add("NUMLOCK", VK_NUMLOCK, true);
        add("SCROLLLOCK", VK_SCROLL);
        add("SCROLL", VK_SCROLL);
        add("MULTIPLY", VK_MULTIPLY);
        add("NUMMULTIPLY", VK_MULTIPLY);
        add("ADD", VK_ADD);
        add("NUMADD", VK_ADD);
        add("NUMPLUS", VK_ADD);
        add("SUBTRACT", VK_SUBTRACT);
        add("NUMSUBTRACT", VK_SUBTRACT);
        add("NUMMINUS", VK_SUBTRACT);
        add("DECIMAL", VK_DECIMAL);
        add("NUMDECIMAL", VK_DECIMAL);
        add("DIVIDE", VK_DIVIDE, true);
        add("NUMDIVIDE", VK_DIVIDE, true);
        add("NUMENTER", VK_RETURN, true);
        add("NUMPADENTER", VK_RETURN, true);
        add("PADENTER", VK_RETURN, true);

        for (int i = 0; i <= 9; ++i) {
            const WORD vk = static_cast<WORD>(VK_NUMPAD0 + i);
            m.insert(QStringLiteral("NUMPAD%1").arg(i), KeyInput{vk, false});
            m.insert(QStringLiteral("NUM%1").arg(i), KeyInput{vk, false});
        }

        add("VOLUMEUP", VK_VOLUME_UP, true);
        add("VOLUMEDOWN", VK_VOLUME_DOWN, true);
        add("VOLUMEMUTE", VK_VOLUME_MUTE, true);
        add("MUTE", VK_VOLUME_MUTE, true);
        add("MEDIANEXT", VK_MEDIA_NEXT_TRACK, true);
        add("NEXTTRACK", VK_MEDIA_NEXT_TRACK, true);
        add("MEDIAPREV", VK_MEDIA_PREV_TRACK, true);
        add("PREVTRACK", VK_MEDIA_PREV_TRACK, true);
        add("MEDIAPLAY", VK_MEDIA_PLAY_PAUSE, true);
        add("PLAYPAUSE", VK_MEDIA_PLAY_PAUSE, true);
        add("MEDIAPLAYPAUSE", VK_MEDIA_PLAY_PAUSE, true);
        add("MEDIASTOP", VK_MEDIA_STOP, true);

        add("BROWSERBACK", VK_BROWSER_BACK, true);
        add("BROWSERFORWARD", VK_BROWSER_FORWARD, true);
        add("BROWSERREFRESH", VK_BROWSER_REFRESH, true);
        add("REFRESH", VK_BROWSER_REFRESH, true);
        add("BROWSERSTOP", VK_BROWSER_STOP, true);
        add("BROWSERSEARCH", VK_BROWSER_SEARCH, true);
        add("SEARCH", VK_BROWSER_SEARCH, true);
        add("BROWSERHOME", VK_BROWSER_HOME, true);
        add("BROWSERFAVORITES", VK_BROWSER_FAVORITES, true);
        add("FAVORITES", VK_BROWSER_FAVORITES, true);
        add("LAUNCHMAIL", VK_LAUNCH_MAIL, true);
        add("LAUNCHMEDIA", VK_LAUNCH_MEDIA_SELECT, true);
        add("MEDIASELECT", VK_LAUNCH_MEDIA_SELECT, true);
        add("LAUNCHAPP1", VK_LAUNCH_APP1, true);
        add("LAUNCHAPP2", VK_LAUNCH_APP2, true);

        add("BACKTICK", VK_OEM_3);
        add("GRAVE", VK_OEM_3);
        add("MINUS", VK_OEM_MINUS);
        add("EQUAL", VK_OEM_PLUS);
        add("EQUALS", VK_OEM_PLUS);
        add("LBRACKET", VK_OEM_4);
        add("LEFTBRACKET", VK_OEM_4);
        add("RBRACKET", VK_OEM_6);
        add("RIGHTBRACKET", VK_OEM_6);
        add("BACKSLASH", VK_OEM_5);
        add("SEMICOLON", VK_OEM_1);
        add("QUOTE", VK_OEM_7);
        add("APOSTROPHE", VK_OEM_7);
        add("COMMA", VK_OEM_COMMA);
        add("PERIOD", VK_OEM_PERIOD);
        add("DOT", VK_OEM_PERIOD);
        add("SLASH", VK_OEM_2);

        return m;
    }();
    return map;
}

void addModifier(QVector<KeyInput>& modifiers, const KeyInput& modifier) {
    for (const KeyInput& existing : modifiers) {
        if (existing.vk == modifier.vk) return;
    }
    modifiers.append(modifier);
}

bool addCharacterKey(QChar ch, KeyInput& out, QVector<KeyInput>& modifiers, QString& error) {
    const ushort code = ch.unicode();
    if (code < 32 || code > 126) {
        error = QStringLiteral("暂不支持字符“%1”。发送中文/任意文本请使用“输入文本”动作。").arg(ch);
        return false;
    }

    if (code >= 'a' && code <= 'z') {
        out = {static_cast<WORD>(code - ('a' - 'A')), false};
        return true;
    }
    if (code >= 'A' && code <= 'Z') {
        out = {static_cast<WORD>(code), false};
        return true;
    }
    if (code >= '0' && code <= '9') {
        out = {static_cast<WORD>(code), false};
        return true;
    }

    const SHORT scan = VkKeyScanW(static_cast<WCHAR>(code));
    if (scan == -1) {
        error = QStringLiteral("当前键盘布局无法输入字符“%1”。").arg(ch);
        return false;
    }

    out.vk = static_cast<WORD>(LOBYTE(scan));
    out.extended = false;

    const BYTE state = HIBYTE(scan);
    if (state & 1) addModifier(modifiers, {VK_SHIFT, false});
    if (state & 2) addModifier(modifiers, {VK_CONTROL, false});
    if (state & 4) addModifier(modifiers, {VK_MENU, false});
    return true;
}

} // namespace

KeyChord Win32Utils::parseKeySequence(const QString& sequence) {
    KeyChord chord;

    const QString text = sequence.trimmed();
    if (text.isEmpty()) {
        chord.error = QStringLiteral("按键序列为空");
        return chord;
    }

    // 按 '+' 切分，同时把末尾的 '+' 当作“加号键”，例如 Ctrl++。
    QStringList tokens;
    int start = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('+')) continue;

        const QString token = text.mid(start, i - start).trimmed();
        if (!token.isEmpty()) tokens.append(token);
        if (i == text.size() - 1) tokens.append(QStringLiteral("+"));

        start = i + 1;
    }
    if (start < text.size()) {
        const QString tail = text.mid(start).trimmed();
        if (!tail.isEmpty()) tokens.append(tail);
    }

    for (const QString& token : tokens) {
        const QString normalized = normalizedToken(token);

        KeyInput modifier;
        if (modifierToken(normalized, modifier)) {
            addModifier(chord.modifiers, modifier);
            continue;
        }

        // === 修改点：移除只能有一个主键的报错，改为追加 ===
        KeyInput newKey;
        // F1 - F24
        if (normalized.startsWith(QLatin1Char('F')) && normalized.size() <= 3) {
            bool ok = false;
            const int number = normalized.mid(1).toInt(&ok);
            if (ok && number >= 1 && number <= 24) {
                newKey = {static_cast<WORD>(VK_F1 + number - 1), false};
                chord.keys.append(newKey);
                continue;
            }
        }

        const auto it = namedKeys().constFind(normalized);
        if (!normalized.isEmpty() && it != namedKeys().constEnd()) {
            newKey = it.value();
            chord.keys.append(newKey);
            continue;
        }

        if (token == QLatin1String("+") || normalized == QLatin1String("PLUS")) {
            if (!addCharacterKey(QLatin1Char('+'), newKey, chord.modifiers, chord.error)) {
                return chord;
            }
            chord.keys.append(newKey);
            continue;
        }

        if (token.size() == 1) {
            if (!addCharacterKey(token.at(0), newKey, chord.modifiers, chord.error)) {
                return chord;
            }
            chord.keys.append(newKey);
            continue;
        }

        chord.error = QStringLiteral("未知按键“%1”。可用示例：Ctrl+Shift+Esc、Alt+F4、Win+Space、MediaPlayPause、Ctrl+Left").arg(token);
        return chord;
    }

    if (chord.keys.isEmpty()) { // 修改为检查列表
        chord.error = QStringLiteral("缺少主键，不能只发送修饰键");
        return chord;
    }
    // 顺便给旧的 key 赋个值，防止其他地方崩溃
    chord.key = chord.keys.first();

    auto hasModifier = [&chord](WORD vk) {
        for (const KeyInput& modifier : chord.modifiers) {
            if (modifier.vk == vk) return true;
        }
        return false;
    };

    // Ctrl+Alt+Del 由 Windows 安全机制接管，普通进程无法通过 SendInput 触发。
    bool hasDel = false;
    for (const auto& k : chord.keys) {
        if (k.vk == VK_DELETE) hasDel = true;
    }
    if (hasDel && hasModifier(VK_CONTROL) && hasModifier(VK_MENU)) {
        chord.error = QStringLiteral("Ctrl+Alt+Del 是 Windows 安全注意序列，任何普通程序都无法模拟");
        return chord;
    }

    chord.valid = true;
    return chord;
}

bool Win32Utils::sendKeySequence(const QString& sequence, QString* error) {
    const KeyChord chord = parseKeySequence(sequence);
    if (!chord.valid) {
        if (error) *error = chord.error;
        return false;
    }

    // 先按下修饰键，给目标程序留出识别组合键的时间。
    QVector<INPUT> modifiersDown;
    modifiersDown.reserve(chord.modifiers.size());
    for (const KeyInput& modifier : chord.modifiers) {
        modifiersDown.append(makeKeyInput(modifier.vk, modifier.extended, false));
        g_injectedModifiers.insert(modifier.vk);
    }

    QVector<INPUT> modifiersUp;
    modifiersUp.reserve(chord.modifiers.size());
    for (int i = chord.modifiers.size() - 1; i >= 0; --i) {
        modifiersUp.append(makeKeyInput(chord.modifiers[i].vk, chord.modifiers[i].extended, true));
    }

    if (!modifiersDown.isEmpty()) {
        if (!flushInput(modifiersDown, error)) {
            clearModifiers();
            return false;
        }
        Sleep(12);
    }

    // === 修改点：依次按下所有主键 ===
    for (const KeyInput& key : chord.keys) {
        const QVector<INPUT> mainDown{makeKeyInput(key.vk, key.extended, false)};
        if (!flushInput(mainDown, error)) {
            clearModifiers();
            return false;
        }
        Sleep(10); // 给每个按键一点间隔
    }
    Sleep(20);
    // === 修改点：依次松开所有主键（逆序） ===
    for (int i = chord.keys.size() - 1; i >= 0; --i) {
        const KeyInput& key = chord.keys[i];
        const QVector<INPUT> mainUp{makeKeyInput(key.vk, key.extended, true)};
        if (!flushInput(mainUp, error)) {
            clearModifiers();
            return false;
        }
        Sleep(10);
    }

    if (!chord.modifiers.isEmpty()) {
        Sleep(12);
        if (!flushInput(modifiersUp, error)) {
            clearModifiers();
            return false;
        }
        for (const KeyInput& modifier : chord.modifiers) {
            g_injectedModifiers.remove(modifier.vk);
        }
    }

    return true;
}

bool Win32Utils::sendText(const QString& text, QString* error) {
    if (text.isEmpty()) return true;

    QVector<INPUT> batch;
    batch.reserve(512);

    auto flush = [&batch, error]() -> bool {
        if (batch.isEmpty()) return true;
        const bool ok = flushInput(batch, error);
        batch.clear();
        return ok;
    };

    auto appendKey = [&batch](WORD vk, bool extended) {
        batch.append(makeKeyInput(vk, extended, false));
        batch.append(makeKeyInput(vk, extended, true));
    };

    for (QChar ch : text) {
        if (ch == QLatin1Char('\r')) continue;

        if (ch == QLatin1Char('\n')) {
            appendKey(VK_RETURN, false);
        } else if (ch == QLatin1Char('\t')) {
            appendKey(VK_TAB, false);
        } else {
            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wScan = ch.unicode();
            down.ki.dwFlags = KEYEVENTF_UNICODE;

            INPUT up = down;
            up.ki.dwFlags |= KEYEVENTF_KEYUP;

            batch.append(down);
            batch.append(up);
        }

        if (batch.size() >= 512 && !flush()) return false;
    }

    return flush();
}

void Win32Utils::sendKey(WORD vk, bool withCtrl) {
    if (withCtrl) {
        const QVector<INPUT> ctrlDown{makeKeyInput(VK_CONTROL, false, false)};
        QString error;
        if (!flushInput(ctrlDown, &error)) {
            qWarning().noquote() << error;
            return;
        }
        g_injectedModifiers.insert(VK_CONTROL);
        Sleep(10);
    }

    const QVector<INPUT> keyDown{makeKeyInput(vk, false, false)};
    QString error;
    if (!flushInput(keyDown, &error)) {
        qWarning().noquote() << error;
        clearModifiers();
        return;
    }
    Sleep(35);

    const QVector<INPUT> keyUp{makeKeyInput(vk, false, true)};
    flushInput(keyUp, &error);

    if (withCtrl) {
        Sleep(10);
        const QVector<INPUT> ctrlUp{makeKeyInput(VK_CONTROL, false, true)};
        flushInput(ctrlUp, &error);
        g_injectedModifiers.remove(VK_CONTROL);
    }
}

void Win32Utils::clearModifiers() {
    QVector<INPUT> releases;
    releases.reserve(g_injectedModifiers.size());
    for (WORD vk : g_injectedModifiers) {
        releases.append(makeKeyInput(vk, false, true));
    }

    const QSet<WORD> released = g_injectedModifiers;
    g_injectedModifiers.clear();

    if (!releases.isEmpty()) {
        QString error;
        flushInput(releases, &error);
    }

    // RAlt / RCtrl 在扩展键状态下可能需要单独释放。
    QVector<INPUT> extendedReleases;
    if (released.contains(VK_RMENU)) extendedReleases.append(makeKeyInput(VK_RMENU, true, true));
    if (released.contains(VK_RCONTROL)) extendedReleases.append(makeKeyInput(VK_RCONTROL, true, true));
    if (!extendedReleases.isEmpty()) {
        QString error;
        flushInput(extendedReleases, &error);
    }
}

QString Win32Utils::getClipboardText() {
    if (!openClipboardWithRetry()) return QString();

    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return QString();
    }

    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) {
        CloseClipboard();
        return QString();
    }

    const QString result = QString::fromWCharArray(text);
    GlobalUnlock(data);
    CloseClipboard();
    return result;
}

bool Win32Utils::setClipboardText(const QString& text) {
    if (!openClipboardWithRetry()) return false;

    EmptyClipboard();

    const SIZE_T bytes = (static_cast<SIZE_T>(text.length()) + 1) * sizeof(wchar_t);
    HGLOBAL buffer = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!buffer) {
        CloseClipboard();
        return false;
    }

    wchar_t* target = static_cast<wchar_t*>(GlobalLock(buffer));
    if (!target) {
        GlobalFree(buffer);
        CloseClipboard();
        return false;
    }

    std::wmemcpy(target, reinterpret_cast<const wchar_t*>(text.utf16()),
                 static_cast<size_t>(text.length()) + 1);
    GlobalUnlock(buffer);

    if (!SetClipboardData(CF_UNICODETEXT, buffer)) {
        GlobalFree(buffer);
        CloseClipboard();
        return false;
    }

    // 提交成功后内存所有权归系统，不能再 GlobalFree。
    CloseClipboard();
    return true;
}

QString Win32Utils::captureSelectedText(int waitMs) {
    const QString oldText = getClipboardText();
    const DWORD before = GetClipboardSequenceNumber();

    sendKey('C', true);

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < waitMs && GetClipboardSequenceNumber() == before) {
        Sleep(10);
    }

    QString selected;
    if (GetClipboardSequenceNumber() != before) {
        selected = getClipboardText();
    }

    // 只有复制确实发生过，或者原本就有文本时，才恢复剪贴板。
    // 这样不会在“复制失败且原本是图片”时把剪贴板清空。
    if (GetClipboardSequenceNumber() != before || !oldText.isEmpty()) {
        setClipboardText(oldText);
    }

    return selected;
}

QString Win32Utils::getForegroundProcessName() {
    const HWND hwnd = GetForegroundWindow();
    if (!hwnd) return QStringLiteral("global");

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return QStringLiteral("global");

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return QStringLiteral("global");

    WCHAR path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    QString name = QStringLiteral("global");
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = QFileInfo(QString::fromWCharArray(path)).fileName();
    }
    CloseHandle(process);
    return name;
}

void Win32Utils::forceForegroundWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (GetForegroundWindow() == hwnd) return;

    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);

    bool attached = false;
    if (foregroundThread && targetThread && foregroundThread != targetThread) {
        attached = AttachThreadInput(foregroundThread, targetThread, TRUE) != FALSE;
    }

    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);

    if (attached) {
        AttachThreadInput(foregroundThread, targetThread, FALSE);
    }
}

QPoint Win32Utils::physicalToLogical(const POINT& pt) {
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (monitor) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            const QString deviceName = QString::fromWCharArray(info.szDevice);
            for (QScreen* screen : QGuiApplication::screens()) {
                if (QString::compare(screen->name(), deviceName, Qt::CaseInsensitive) != 0) {
                    continue;
                }

                const qreal ratio = screen->devicePixelRatio();
                const QRect logical = screen->geometry();
                if (ratio <= 0.0) break;

                return QPoint(
                    logical.left() + qRound((pt.x - info.rcMonitor.left) / ratio),
                    logical.top() + qRound((pt.y - info.rcMonitor.top) / ratio));
            }
        }
    }

    const QScreen* primary = QGuiApplication::primaryScreen();
    const qreal ratio = primary ? primary->devicePixelRatio() : 1.0;
    return QPoint(qRound(pt.x / ratio), qRound(pt.y / ratio));
}

bool Win32Utils::isKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
