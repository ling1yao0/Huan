// 输入注入集成测试：真实调用 SendInput，验证组合键、扩展键和 Unicode 输入。
// 测试窗口必须在前台，否则自动跳过，避免把按键打到别的程序里。
#include <QApplication>
#include <QElapsedTimer>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "Win32Utils.h"

namespace {

int g_failures = 0;

QTextStream& out() {
    static QTextStream stream(stdout);
    return stream;
}

void expect(bool condition, const QString& name, const QString& detail = QString()) {
    if (condition) {
        out() << "[PASS] " << name << "\n";
    } else {
        ++g_failures;
        out() << "[FAIL] " << name;
        if (!detail.isEmpty()) out() << "  (" << detail << ")";
        out() << "\n";
    }
}

void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    QWidget window;
    window.setWindowTitle(QStringLiteral("Input Sender Test"));
    window.resize(640, 320);
    window.move(120, 120);

    auto* layout = new QVBoxLayout(&window);
    auto* edit = new QTextEdit(&window);
    layout->addWidget(edit);

    window.show();
    window.raise();
    window.activateWindow();
    edit->setFocus();

    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    Win32Utils::forceForegroundWindow(hwnd);

    // 点击窗口中心，确保它真正拿到前台键盘焦点（Windows 前台锁）。
    RECT rect = {};
    if (GetWindowRect(hwnd, &rect)) {
        SetCursorPos((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
        pump(100);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
    pump(400);
    SetForegroundWindow(hwnd);
    pump(200);

    if (GetForegroundWindow() != hwnd) {
        out() << "[SKIP] test window is not foreground; input test not run\n";
        out().flush();
        return 0;
    }

    auto reset = [edit](const QString& text) {
        edit->setPlainText(text);
        QTextCursor cursor = edit->textCursor();
        cursor.movePosition(QTextCursor::End);
        edit->setTextCursor(cursor);
        edit->setFocus();
    };

    auto cursorPosition = [edit] {
        return edit->textCursor().position();
    };

    out() << "=== Single keys and navigation ===\n";

    reset(QStringLiteral("hello world"));
    Win32Utils::sendKeySequence(QStringLiteral("Home"));
    pump(200);
    expect(cursorPosition() == 0, QStringLiteral("Home moves caret to start"),
           QStringLiteral("position=%1").arg(cursorPosition()));

    Win32Utils::sendKeySequence(QStringLiteral("Shift+End"));
    pump(200);
    expect(edit->textCursor().selectedText() == QStringLiteral("hello world"),
           QStringLiteral("Shift+End selects the line"),
           edit->textCursor().selectedText());

    Win32Utils::sendKeySequence(QStringLiteral("Ctrl+A"));
    pump(200);
    expect(edit->textCursor().selectedText() == QStringLiteral("hello world"),
           QStringLiteral("Ctrl+A selects all"),
           edit->textCursor().selectedText());

    reset(QStringLiteral("abc"));
    Win32Utils::sendKeySequence(QStringLiteral("Backspace"));
    pump(200);
    expect(edit->toPlainText() == QStringLiteral("ab"),
           QStringLiteral("Backspace deletes one character"), edit->toPlainText());

    reset(QString());
    Win32Utils::sendKeySequence(QStringLiteral("A"));
    pump(200);
    expect(edit->toPlainText() == QStringLiteral("a"),
           QStringLiteral("plain letter is typed"), edit->toPlainText());

    reset(QString());
    Win32Utils::sendKeySequence(QStringLiteral("Shift+A"));
    pump(200);
    expect(edit->toPlainText() == QStringLiteral("A"),
           QStringLiteral("Shift produces an uppercase letter"), edit->toPlainText());

    reset(QString());
    Win32Utils::sendKeySequence(QStringLiteral("Shift+1"));
    pump(200);
    expect(edit->toPlainText() == QStringLiteral("!"),
           QStringLiteral("Shift+1 produces '!'"), edit->toPlainText());

    reset(QString());
    Win32Utils::sendText(QStringLiteral("中文😀"));
    pump(400);
    expect(edit->toPlainText() == QStringLiteral("中文😀"),
           QStringLiteral("Unicode text input (including emoji)"), edit->toPlainText());

    reset(QStringLiteral("line1"));
    Win32Utils::sendKeySequence(QStringLiteral("NumpadEnter"));
    pump(200);
    expect(edit->toPlainText() == QStringLiteral("line1\n"),
           QStringLiteral("NumpadEnter inserts a newline"), edit->toPlainText());

    reset(QStringLiteral("one two three"));
    Win32Utils::sendKeySequence(QStringLiteral("Ctrl+Left"));
    pump(200);
    Win32Utils::sendKeySequence(QStringLiteral("Ctrl+Left"));
    pump(200);
    const int position = cursorPosition();
    expect(position > 0 && position < 13, QStringLiteral("Ctrl+Left moves by word"),
           QStringLiteral("position=%1").arg(position));

    out() << "\n";
    if (g_failures == 0) {
        out() << "ALL TESTS PASSED\n";
    } else {
        out() << g_failures << " TEST(S) FAILED\n";
    }
    out().flush();

    return g_failures == 0 ? 0 : 1;
}
