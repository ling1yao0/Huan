// 按键解析 / 变量替换的离线单元测试。
// 不发送真实按键，不会影响当前桌面。
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QTextStream>
#include <QTime>

#include "VariableResolver.h"
#include "Win32Utils.h"

namespace {

int g_failures = 0;

QTextStream& out() {
    static QTextStream stream(stdout);
    return stream;
}

bool hasModifier(const QVector<KeyInput>& modifiers, WORD vk) {
    for (const KeyInput& modifier : modifiers) {
        if (modifier.vk == vk) return true;
    }
    return false;
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

void expectChord(const QString& input, WORD vk, bool extended, int modifierCount) {
    const KeyChord chord = Win32Utils::parseKeySequence(input);
    const QString detail = QStringLiteral("valid=%1 vk=%2 ext=%3 mods=%4 err=%5")
                               .arg(chord.valid)
                               .arg(chord.key.vk)
                               .arg(chord.key.extended)
                               .arg(chord.modifiers.size())
                               .arg(chord.error);
    expect(chord.valid && chord.key.vk == vk && chord.key.extended == extended &&
               chord.modifiers.size() == modifierCount,
           QStringLiteral("parse \"%1\"").arg(input), detail);
}

void expectInvalid(const QString& input) {
    const KeyChord chord = Win32Utils::parseKeySequence(input);
    expect(!chord.valid, QStringLiteral("reject \"%1\"").arg(input),
           QStringLiteral("unexpected success: vk=%1").arg(chord.key.vk));
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    out() << "=== Key parser ===\n";

    expectChord(QStringLiteral("Ctrl+Shift+A"), 'A', false, 2);
    expectChord(QStringLiteral("Ctrl+V"), 'V', false, 1);
    expectChord(QStringLiteral("F12"), VK_F12, false, 0);
    expectChord(QStringLiteral("Ctrl+F4"), VK_F4, false, 1);
    expectChord(QStringLiteral("Win+Shift+S"), 'S', false, 2);
    expectChord(QStringLiteral("Meta+Space"), VK_SPACE, false, 1);
    expectChord(QStringLiteral("Ctrl+Left"), VK_LEFT, true, 1);
    expectChord(QStringLiteral("Ctrl+Shift+Esc"), VK_ESCAPE, false, 2);
    expectChord(QStringLiteral("NumpadEnter"), VK_RETURN, true, 0);
    expectChord(QStringLiteral("Num5"), VK_NUMPAD5, false, 0);
    expectChord(QStringLiteral("MediaPlayPause"), VK_MEDIA_PLAY_PAUSE, true, 0);
    expectChord(QStringLiteral("Ctrl+Shift+Alt+Win+K"), 'K', false, 4);
    expectChord(QStringLiteral("Ctrl++"), VK_OEM_PLUS, false, 2);
    expectChord(QStringLiteral("Ctrl+Plus"), VK_OEM_PLUS, false, 2);
    expectChord(QStringLiteral("Print"), VK_SNAPSHOT, true, 0);
    expectChord(QStringLiteral("Menu"), VK_APPS, true, 0);
    expectChord(QStringLiteral("Alt+F4"), VK_F4, false, 1);
    expectChord(QStringLiteral(";"), VK_OEM_1, false, 0);

    expectInvalid(QString());
    expectInvalid(QStringLiteral("Ctrl"));
    expectInvalid(QStringLiteral("Ctrl+Unknown"));
    expectInvalid(QStringLiteral("Ctrl+A+B"));
    expectInvalid(QStringLiteral("Ctrl+Alt+Del"));

    {
        const KeyChord chord = Win32Utils::parseKeySequence(QStringLiteral("Ctrl+?"));
        expect(chord.valid && hasModifier(chord.modifiers, VK_CONTROL) &&
                   hasModifier(chord.modifiers, VK_SHIFT),
               QStringLiteral("Ctrl+? auto-adds Shift"),
               QStringLiteral("valid=%1 mods=%2").arg(chord.valid).arg(chord.modifiers.size()));
    }
    {
        const KeyChord chord = Win32Utils::parseKeySequence(QStringLiteral("RAlt+Tab"));
        expect(chord.valid && chord.key.vk == VK_TAB && hasModifier(chord.modifiers, VK_RMENU),
               QStringLiteral("RAlt is an extended modifier"),
               QStringLiteral("valid=%1").arg(chord.valid));
    }

    out() << "\n=== Variable resolver ===\n";

    VariableContext context;
    context.scene = QStringLiteral("Code.exe");
    context.selection = QStringLiteral("abc");
    context.clipboard = QStringLiteral("XY");
    context.now = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5));

    const QString expanded = VariableResolver::expand(
        QStringLiteral("{scene}|{selected}|{clipboard}|{date}|{time}|{datetime}"), context);
    expect(expanded == QStringLiteral("Code.exe|abc|XY|2026-01-02|03:04:05|2026-01-02 03:04:05"),
           QStringLiteral("core variables"), expanded);

    expect(VariableResolver::expand(QStringLiteral("hello {unknown}"), context) ==
               QStringLiteral("hello {unknown}"),
           QStringLiteral("unknown variable is preserved"));

    expect(VariableResolver::expand(QStringLiteral("{not a var}"), context) ==
               QStringLiteral("{not a var}"),
           QStringLiteral("text with spaces is not a variable"));

    expect(VariableResolver::expand(QStringLiteral("100%"), context) == QStringLiteral("100%"),
           QStringLiteral("lone percent is preserved"));

    expect(VariableResolver::expand(QStringLiteral("{newline}"), context) == QStringLiteral("\n"),
           QStringLiteral("newline variable"));

    const QString env = VariableResolver::expand(QStringLiteral("{env:WINDIR}"), context);
    expect(!env.isEmpty() && !env.startsWith(QLatin1String("{env:")),
           QStringLiteral("{env:WINDIR}"), env);

    const QString envPercent = VariableResolver::expand(QStringLiteral("%WINDIR%"), context);
    expect(!envPercent.isEmpty() && !envPercent.startsWith(QLatin1Char('%')),
           QStringLiteral("%WINDIR%"), envPercent);

    out() << "\n";
    if (g_failures == 0) {
        out() << "ALL TESTS PASSED\n";
    } else {
        out() << g_failures << " TEST(S) FAILED\n";
    }
    out().flush();

    return g_failures == 0 ? 0 : 1;
}
