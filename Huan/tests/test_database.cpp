// 数据库层测试：设置持久化、鼠标绑定、场景回退。
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>

#include "ActionRepository.h"
#include "AppConfig.h"

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

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        out() << "[FAIL] cannot create temp dir\n";
        return 1;
    }

    const QString dbPath = tempDir.filePath(QStringLiteral("test.db"));
    expect(ActionRepository::init(dbPath), QStringLiteral("database initializes"));

    const QMap<QString, QString> defaults = ActionRepository::settings();
    expect(defaults.contains(QStringLiteral("trigger.keyboard")) &&
               defaults.value(QStringLiteral("trigger.keyboard")).isEmpty() &&
               !defaults.value(QStringLiteral("trigger.keyboard")).isNull(),
           QStringLiteral("empty keyboard setting is stored as empty string, not NULL"));

    expect(ActionRepository::getActionBySlot(0, QStringLiteral("global")).name ==
               QStringLiteral("计算器"),
           QStringLiteral("default global actions are inserted"));

    const QVector<LibraryAction> library = ActionRepository::getActionLibrary();
    expect(library.size() >= 8, QStringLiteral("legacy defaults are migrated into the library"),
           QStringLiteral("size=%1").arg(library.size()));
    expect(ActionRepository::getWheelBindings(QStringLiteral("global")).size() == 8,
           QStringLiteral("global bindings are migrated"));

    QMap<QString, QString> values = defaults;
    values.insert(QStringLiteral("trigger.keyboard"), QString());
    values.insert(QStringLiteral("trigger.mouse"), QStringLiteral("middle"));
    values.insert(QStringLiteral("trigger.threshold"), QStringLiteral("9"));
    values.insert(QStringLiteral("action.script_timeout"), QStringLiteral("12000"));

    expect(ActionRepository::saveSettings(values), QStringLiteral("settings save"));
    AppConfig::instance().reload();
    expect(AppConfig::instance().keyboardTrigger().isEmpty(),
           QStringLiteral("null QString hotkey stays empty"));
    expect(AppConfig::instance().mouseTrigger() == QStringLiteral("middle"),
           QStringLiteral("mouse trigger round-trips"));
    expect(AppConfig::instance().dragThreshold() == 9,
           QStringLiteral("integer setting round-trips"));
    expect(AppConfig::instance().scriptTimeoutMs() == 12000,
           QStringLiteral("script timeout round-trips"));

    MouseBinding binding;
    binding.button = QStringLiteral("middle");
    binding.name = QStringLiteral("粘贴");
    binding.type = 0;
    binding.payload = QStringLiteral("Ctrl+V");

    QVector<MouseBinding> bindings;
    bindings.append(binding);
    expect(ActionRepository::saveMouseBindings(bindings), QStringLiteral("mouse binding save"));

    const QVector<MouseBinding> loadedBindings = ActionRepository::getMouseBindings();
    expect(loadedBindings.size() == 1 &&
               loadedBindings.first().button == QStringLiteral("middle") &&
               loadedBindings.first().payload == QStringLiteral("Ctrl+V"),
           QStringLiteral("mouse binding round-trips"));

    // 场景动作优先，缺失槽位回退 global。
    LibraryAction sceneAction;
    sceneAction.name = QStringLiteral("场景动作");
    sceneAction.type = 0;
    sceneAction.payload = QStringLiteral("Ctrl+Shift+P");

    const int sceneActionId = ActionRepository::saveLibraryAction(sceneAction);
    expect(sceneActionId > 0, QStringLiteral("library action is created"),
           QStringLiteral("id=%1").arg(sceneActionId));
    expect(ActionRepository::bindActionToSlot(0, QStringLiteral("Code.exe"), sceneActionId),
           QStringLiteral("library action is bound to a scene slot"));

    const Action boundAction = ActionRepository::getActionBySlot(0, QStringLiteral("Code.exe"));
    expect(boundAction.name == QStringLiteral("场景动作") &&
               boundAction.payload == QStringLiteral("Ctrl+Shift+P"),
           QStringLiteral("scene binding takes precedence"), boundAction.name);

    const Action fallback = ActionRepository::getActionBySlot(1, QStringLiteral("Code.exe"));
    expect(fallback.name == QStringLiteral("记事本"),
           QStringLiteral("missing slot falls back to global"), fallback.name);

    const QVector<Action> merged = ActionRepository::getAllActions(QStringLiteral("Code.exe"));
    expect(merged.size() == 8, QStringLiteral("merged view has 8 slots"),
           QStringLiteral("size=%1").arg(merged.size()));

    const int copied = ActionRepository::copyGlobalBindingsToScene(QStringLiteral("Notepad.exe"));
    expect(copied == 8, QStringLiteral("global bindings are physically copied to a scene"),
           QStringLiteral("count=%1").arg(copied));
    expect(ActionRepository::getWheelBindings(QStringLiteral("Notepad.exe")).size() == 8,
           QStringLiteral("copied scene has its own bindings"));

    expect(ActionRepository::unbindSlot(0, QStringLiteral("Code.exe")),
           QStringLiteral("unbind scene slot"));
    expect(ActionRepository::getActionBySlot(0, QStringLiteral("Code.exe")).name ==
               QStringLiteral("计算器"),
           QStringLiteral("unbound slot falls back to global"));

    expect(ActionRepository::bindActionToSlot(0, QStringLiteral("Code.exe"), sceneActionId),
           QStringLiteral("rebind before delete"));
    expect(ActionRepository::deleteLibraryAction(sceneActionId),
           QStringLiteral("delete library action"));
    expect(ActionRepository::getWheelBindings(QStringLiteral("Code.exe")).isEmpty(),
           QStringLiteral("deleting an action removes its bindings"));

    // 关闭连接，否则 Windows 上临时目录无法删除。
    QSqlDatabase::database().close();
    QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);

    out() << "\n";
    if (g_failures == 0) {
        out() << "ALL TESTS PASSED\n";
    } else {
        out() << g_failures << " TEST(S) FAILED\n";
    }
    out().flush();

    return g_failures == 0 ? 0 : 1;
}
