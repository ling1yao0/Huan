#include "ActionRepository.h"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace {

bool execOrWarn(QSqlQuery& query, const QString& sql) {
    if (query.exec(sql)) return true;
    qWarning().noquote() << "SQL 执行失败:" << sql << "\n " << query.lastError().text();
    return false;
}

QString nonNull(const QString& value) {
    return value.isNull() ? QStringLiteral("") : value;
}

// 旧版 actions 表 -> 动作库 + 绑定。只跑一次，避免每次启动重复导入。
bool migrateLegacyActions() {
    struct LegacyRow {
        int slot = 0;
        QString scene;
        QString name;
        int type = 0;
        QString payload;
    };

    QVector<LegacyRow> rows;
    {
        QSqlQuery oldQuery;
        if (!oldQuery.exec(QStringLiteral(
                "SELECT slot, scene, name, type, payload FROM actions ORDER BY scene, slot"))) {
            qWarning().noquote() << "读取旧动作失败:" << oldQuery.lastError().text();
            return false;
        }
        while (oldQuery.next()) {
            LegacyRow row;
            row.slot = oldQuery.value(0).toInt();
            row.scene = oldQuery.value(1).toString();
            row.name = oldQuery.value(2).toString().trimmed();
            row.type = oldQuery.value(3).toInt();
            row.payload = oldQuery.value(4).toString().trimmed();
            rows.append(row);
        }
    }

    bool ok = true;

    auto ensureLibraryAction = [](const QString& name, int type, const QString& payload) -> int {
        QSqlQuery find;
        find.prepare(QStringLiteral(
            "SELECT id FROM actions_lib WHERE name = ? AND type = ? AND payload = ? LIMIT 1"));
        find.addBindValue(name);
        find.addBindValue(type);
        find.addBindValue(payload);
        if (find.exec() && find.next()) return find.value(0).toInt();

        QSqlQuery insert;
        insert.prepare(QStringLiteral(
            "INSERT INTO actions_lib (name, type, payload) VALUES (?, ?, ?)"));
        insert.addBindValue(name);
        insert.addBindValue(type);
        insert.addBindValue(payload);
        if (!insert.exec()) {
            qWarning().noquote() << "导入动作库失败:" << insert.lastError().text();
            return -1;
        }
        return insert.lastInsertId().toInt();
    };

    for (const LegacyRow& row : rows) {
        if (row.name.isEmpty() && row.payload.isEmpty()) continue;

        const int actionId = ensureLibraryAction(row.name, row.type, row.payload);
        if (actionId < 0) {
            ok = false;
            continue;
        }

        QSqlQuery bind;
        bind.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO wheel_bindings (slot, scene, action_id) VALUES (?, ?, ?)"));
        bind.addBindValue(row.slot);
        bind.addBindValue(row.scene);
        bind.addBindValue(actionId);
        if (!bind.exec()) {
            qWarning().noquote() << "导入轮盘绑定失败:" << bind.lastError().text();
            ok = false;
        }
    }

    if (!ok) return false;

    QSqlQuery marker;
    marker.prepare(QStringLiteral(
        "REPLACE INTO settings (key, value) VALUES ('schema.actions_v2', '1')"));
    return marker.exec();
}

} // namespace

bool ActionRepository::init(const QString& dbPath) {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        qWarning().noquote() << "无法打开数据库:" << dbPath << db.lastError().text();
        return false;
    }

    QSqlQuery query;
    query.exec(QStringLiteral("PRAGMA foreign_keys = ON"));

    // 动作表：使用 (slot, scene) 联合主键，避免不同场景互相覆盖。
    if (!execOrWarn(query,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS actions ("
                                   "slot INTEGER, "
                                   "scene TEXT, "
                                   "name TEXT, "
                                   "type INTEGER, "
                                   "payload TEXT, "
                                   "PRIMARY KEY (slot, scene))"))) {
        return false;
    }

    // 设置表：触发方式、阈值、超时等。
    if (!execOrWarn(query,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS settings ("
                                   "key TEXT PRIMARY KEY, "
                                   "value TEXT NOT NULL DEFAULT '')"))) {
        return false;
    }

    // 中键 / 侧键直接绑定的动作。
    if (!execOrWarn(query,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS mouse_bindings ("
                                   "button TEXT PRIMARY KEY, "
                                   "name TEXT, "
                                   "type INTEGER, "
                                   "payload TEXT)"))) {
        return false;
    }

    // 动作库：动作与场景/槽位解耦，可被多个槽位复用。
    if (!execOrWarn(query,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS actions_lib ("
                                   "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                   "name TEXT NOT NULL DEFAULT '', "
                                   "type INTEGER NOT NULL DEFAULT 0, "
                                   "payload TEXT NOT NULL DEFAULT '')"))) {
        return false;
    }

    // 使用次数统计列：新库需要单独补（CREATE TABLE 里没写），旧库用 ALTER 补。
    // 用 PRAGMA table_info 判断而不是 user_version，这样重复执行也安全。
    {
        bool hasUsageCount = false;
        QSqlQuery pragmaQuery;
        if (pragmaQuery.exec(QStringLiteral("PRAGMA table_info(actions_lib)"))) {
            while (pragmaQuery.next()) {
                if (pragmaQuery.value(1).toString() == QLatin1String("usage_count")) {
                    hasUsageCount = true;
                    break;
                }
            }
        }
        if (!hasUsageCount) {
            QSqlQuery alterQuery;
            if (!execOrWarn(alterQuery, QStringLiteral(
                    "ALTER TABLE actions_lib ADD COLUMN usage_count INTEGER DEFAULT 0"))) {
                return false;
            }
        }
    }

    // 轮盘绑定：某个场景的某个槽位引用动作库中的哪个动作。
    if (!execOrWarn(query,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS wheel_bindings ("
                                   "slot INTEGER NOT NULL, "
                                   "scene TEXT NOT NULL, "
                                   "action_id INTEGER NOT NULL, "
                                   "PRIMARY KEY (slot, scene), "
                                   "FOREIGN KEY (action_id) REFERENCES actions_lib(id) "
                                   "ON DELETE CASCADE)"))) {
        return false;
    }

    // 场景列表，记录用户自定义的场景及显示名
    if (!execOrWarn(query,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS scenes ("
                                   "process_name TEXT PRIMARY KEY, "
                                   "display_name TEXT NOT NULL)"))) {
        return false;
    }

    // 检查 scenes 表是否有 wheel_rings，没有则新增
    bool hasRingsColumn = false;
    QSqlQuery pragmaQuery("PRAGMA table_info(scenes)");
    while (pragmaQuery.next()) {
        if (pragmaQuery.value(1).toString() == QLatin1String("wheel_rings")) {
            hasRingsColumn = true;
            break;
        }
    }
    if (!hasRingsColumn) {
        query.exec("ALTER TABLE scenes ADD COLUMN wheel_rings INTEGER DEFAULT 1");
    }

    // 检查 scenes 表是否有 page_count，没有则新增
    bool hasPageCountColumn = false;
    QSqlQuery pragmaQuery2("PRAGMA table_info(scenes)");
    while (pragmaQuery2.next()) {
        if (pragmaQuery2.value(1).toString() == QLatin1String("page_count")) {
            hasPageCountColumn = true;
            break;
        }
    }
    if (!hasPageCountColumn) {
        query.exec("ALTER TABLE scenes ADD COLUMN page_count INTEGER DEFAULT 1");
    }

    // 检查 scenes 表是否有 enable_hidden_ring，没有则新增（-1 表示继承全局配置）
    bool hasHiddenRingColumn = false;
    QSqlQuery pragmaQuery3("PRAGMA table_info(scenes)");
    while (pragmaQuery3.next()) {
        if (pragmaQuery3.value(1).toString() == QLatin1String("enable_hidden_ring")) {
            hasHiddenRingColumn = true;
            break;
        }
    }
    if (!hasHiddenRingColumn) {
        query.exec("ALTER TABLE scenes ADD COLUMN enable_hidden_ring INTEGER DEFAULT -1");
    }

    // 动作页绑定：场景 -> 页数 -> 格子 -> 动作
    if (!execOrWarn(query,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS page_bindings ("
                                   "page_index INTEGER NOT NULL, "
                                   "grid_index INTEGER NOT NULL, "
                                   "scene TEXT NOT NULL, "
                                   "action_id INTEGER NOT NULL, "
                                   "PRIMARY KEY (page_index, grid_index, scene), "
                                   "FOREIGN KEY (action_id) REFERENCES actions_lib(id) "
                                   "ON DELETE CASCADE)"))) {
        return false;
    }

    // 确保 global 存在（common 已废弃：整项目只有 global 参与继承兜底）
    query.exec("INSERT OR IGNORE INTO scenes (process_name, display_name) VALUES ('global', '全局')");

    // 兼容旧数据：旧版 scenes 表把 wheel_rings 的默认值定成 1，
    // 非 global 场景因此全部"一出生就是独立 1 环"，永远不跟随全局。
    // 这里把它们重置为 -1（继承）。用 PRAGMA user_version 记录已迁移，
    // 保证只跑一次 —— 否则每次启动都会把用户手动设成 1 环的场景又打回继承。
    int schemaVersion = 0;
    if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) {
        schemaVersion = query.value(0).toInt();
    }
    if (schemaVersion < 1) {
        query.exec(QStringLiteral(
            "UPDATE scenes SET wheel_rings = -1 WHERE process_name != 'global' AND wheel_rings = 1"));
        query.exec(QStringLiteral("PRAGMA user_version = 1"));
    }

    // 只有在完全空的数据库里才插入旧版默认动作；新版数据库已有动作库。
    int actionCount = 0;
    if (query.exec(QStringLiteral("SELECT count(*) FROM actions WHERE scene = 'global'")) &&
        query.next()) {
        actionCount = query.value(0).toInt();
    }
    int libraryCount = 0;
    if (query.exec(QStringLiteral("SELECT count(*) FROM actions_lib")) && query.next()) {
        libraryCount = query.value(0).toInt();
    }

    if (actionCount == 0 && libraryCount == 0) {
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (0, 'global', '计算器', 1, 'calc.exe')");
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (1, 'global', '记事本', 1, 'notepad.exe')");
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (2, 'global', '转大写', 2, 'upper')");
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (3, 'global', '转小写', 2, 'lower')");
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (4, 'global', '复制', 0, 'Ctrl+C')");
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (5, 'global', '粘贴', 0, 'Ctrl+V')");
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (6, 'global', '百度', 1, 'https://www.baidu.com')");
        query.exec("INSERT OR IGNORE INTO actions (slot, scene, name, type, payload) VALUES (7, 'global', '退出', 1, 'exit')");
    }
    query.exec("INSERT OR IGNORE INTO actions_lib (id, name, type, payload) VALUES (0, '（空白/已屏蔽）', -1, '')");
    // 清理旧版界面留下的空槽位，避免阻塞 global 回退。
    query.exec("DELETE FROM actions WHERE (name IS NULL OR name = '') "
               "AND (payload IS NULL OR payload = '')");

    // 默认设置只补齐缺失项，不覆盖用户已有配置。
    const QMap<QString, QString> defaults = {
        {QStringLiteral("trigger.mouse"), QStringLiteral("right")},
        {QStringLiteral("trigger.keyboard"), QStringLiteral("")},
        {QStringLiteral("trigger.threshold"), QStringLiteral("3")},
        {QStringLiteral("trigger.longpress"), QStringLiteral("220")},
        {QStringLiteral("trigger.timeout"), QStringLiteral("3000")},
        {QStringLiteral("action.script_timeout"), QStringLiteral("8000")},
    };

    QSqlQuery insertSetting;
    insertSetting.prepare(QStringLiteral("INSERT OR IGNORE INTO settings (key, value) VALUES (?, ?)"));
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        insertSetting.bindValue(0, it.key());
        insertSetting.bindValue(1, nonNull(it.value()));
        if (!insertSetting.exec()) {
            qWarning().noquote() << "写入默认设置失败:" << insertSetting.lastError().text();
        }
    }

    // 旧数据一次性迁移到动作库 + 绑定。
    if (setting(QStringLiteral("schema.actions_v2")) != QLatin1String("1")) {
        if (!migrateLegacyActions()) {
            qWarning() << "旧动作迁移失败，继续使用旧表回退";
        }
    }

    return true;
}

QVector<LibraryAction> ActionRepository::getActionLibrary() {
    QVector<LibraryAction> list;
    QSqlQuery query;
    if (!query.exec(QStringLiteral(
            "SELECT id, name, type, payload, usage_count FROM actions_lib "
            "WHERE id > 0 ORDER BY name COLLATE NOCASE, id"))) {
        qWarning().noquote() << "读取动作库失败:" << query.lastError().text();
        return list;
    }

    while (query.next()) {
        LibraryAction action;
        action.id = query.value(0).toInt();
        action.name = query.value(1).toString();
        action.type = query.value(2).toInt();
        action.payload = query.value(3).toString();
        action.usageCount = query.value(4).toInt();
        list.append(action);
    }
    return list;
}

bool ActionRepository::incrementActionUsage(int actionId) {
    if (actionId <= 0) return false; // id=0 是内置的"空白/已屏蔽"，不算使用
    QSqlQuery query;
    query.prepare(QStringLiteral(
        "UPDATE actions_lib SET usage_count = usage_count + 1 WHERE id = ?"));
    query.addBindValue(actionId);
    return query.exec();
}

int ActionRepository::saveLibraryAction(const LibraryAction& action) {
    if (action.name.trimmed().isEmpty() || action.type < 0 || action.type > 5) return -1;

    QSqlQuery query;
    if (action.id < 0) {
        query.prepare(QStringLiteral(
            "INSERT INTO actions_lib (name, type, payload) VALUES (?, ?, ?)"));
        query.addBindValue(action.name.trimmed());
        query.addBindValue(action.type);
        query.addBindValue(nonNull(action.payload));
        if (!query.exec()) {
            qWarning().noquote() << "新增动作失败:" << query.lastError().text();
            return -1;
        }
        return query.lastInsertId().toInt();
    }

    query.prepare(QStringLiteral(
        "UPDATE actions_lib SET name = ?, type = ?, payload = ? WHERE id = ?"));
    query.addBindValue(action.name.trimmed());
    query.addBindValue(action.type);
    query.addBindValue(nonNull(action.payload));
    query.addBindValue(action.id);
    if (!query.exec()) {
        qWarning().noquote() << "更新动作失败:" << query.lastError().text();
        return -1;
    }
    return action.id;
}

bool ActionRepository::deleteLibraryAction(int id) {
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) return false;

    QSqlQuery unbind(db);
    unbind.prepare(QStringLiteral("DELETE FROM wheel_bindings WHERE action_id = ?"));
    unbind.addBindValue(id);
    if (!unbind.exec()) {
        db.rollback();
        return false;
    }

    QSqlQuery remove(db);
    remove.prepare(QStringLiteral("DELETE FROM actions_lib WHERE id = ?"));
    remove.addBindValue(id);
    if (!remove.exec()) {
        db.rollback();
        return false;
    }

    return db.commit();
}

QMap<int, int> ActionRepository::getWheelBindings(const QString& scene) {
    QMap<int, int> bindings;
    QSqlQuery query;
    query.prepare(QStringLiteral(
        "SELECT slot, action_id FROM wheel_bindings WHERE scene = ?"));
    query.addBindValue(scene);
    if (!query.exec()) {
        qWarning().noquote() << "读取轮盘绑定失败:" << query.lastError().text();
        return bindings;
    }

    while (query.next()) {
        bindings.insert(query.value(0).toInt(), query.value(1).toInt());
    }
    return bindings;
}

bool ActionRepository::bindActionToSlot(int slot, const QString& scene, int actionId) {
    if (slot < 0 || scene.isEmpty() || actionId < 0) return false;

    QSqlQuery query;
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO wheel_bindings (slot, scene, action_id) VALUES (?, ?, ?)"));
    query.addBindValue(slot);
    query.addBindValue(scene);
    query.addBindValue(actionId);
    if (!query.exec()) {
        qWarning().noquote() << "绑定动作失败:" << query.lastError().text();
        return false;
    }
    return true;
}

bool ActionRepository::unbindSlot(int slot, const QString& scene) {
    QSqlQuery query;
    query.prepare(QStringLiteral("DELETE FROM wheel_bindings WHERE slot = ? AND scene = ?"));
    query.addBindValue(slot);
    query.addBindValue(scene);
    if (!query.exec()) {
        qWarning().noquote() << "解绑动作失败:" << query.lastError().text();
        return false;
    }
    return true;
}

int ActionRepository::copyGlobalBindingsToScene(const QString& scene) {
    if (scene.isEmpty() || scene == QLatin1String("global")) return 0;

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) return -1;

    QSqlQuery clear(db);
    clear.prepare(QStringLiteral("DELETE FROM wheel_bindings WHERE scene = ?"));
    clear.addBindValue(scene);
    if (!clear.exec()) {
        db.rollback();
        return -1;
    }

    QSqlQuery copy(db);
    copy.prepare(QStringLiteral(
        "INSERT INTO wheel_bindings (slot, scene, action_id) "
        "SELECT slot, ?, action_id FROM wheel_bindings WHERE scene = 'global'"));
    copy.addBindValue(scene);
    if (!copy.exec()) {
        db.rollback();
        return -1;
    }

    const int count = copy.numRowsAffected();
    if (!db.commit()) {
        db.rollback();
        return -1;
    }
    return count;
}

Action ActionRepository::getActionBySlot(int slot, const QString& scene) {
    Action action;
    action.slot = slot;
    action.type = -1;

    auto readBinding = [&](const QString& targetScene) -> bool {
        QSqlQuery query;
        query.prepare(QStringLiteral(
            "SELECT a.id, a.name, a.type, a.payload FROM wheel_bindings b "
            "JOIN actions_lib a ON a.id = b.action_id "
            "WHERE b.slot = ? AND b.scene = ?"));
        query.addBindValue(slot);
        query.addBindValue(targetScene);
        if (!query.exec() || !query.next()) return false;

        action.id = query.value(0).toInt();
        action.name = query.value(1).toString();
        action.type = query.value(2).toInt();
        action.payload = query.value(3).toString();
        return true;
    };

    if (readBinding(scene)) return action;

    if (scene != QLatin1String("global") && readBinding(QStringLiteral("global"))) {
        return action;
    }

    // 迁移未完成时继续用旧表兜底（迁移成功后不再走这里，避免“解绑后复活”）。
    if (setting(QStringLiteral("schema.actions_v2")) != QLatin1String("1")) {
        auto readLegacy = [&](const QString& targetScene) -> bool {
            QSqlQuery query;
            query.prepare(QStringLiteral(
                "SELECT name, type, payload FROM actions WHERE slot = ? AND scene = ?"));
            query.addBindValue(slot);
            query.addBindValue(targetScene);
            if (!query.exec() || !query.next()) return false;

            action.name = query.value(0).toString();
            action.type = query.value(1).toInt();
            action.payload = query.value(2).toString();
            return true;
        };

        if (readLegacy(scene)) return action;
        if (scene != QLatin1String("global") && readLegacy(QStringLiteral("global"))) {
            return action;
        }
    }

    action.id = -1;
    action.name.clear();
    action.type = -1;
    action.payload.clear();
    return action;
}

QVector<Action> ActionRepository::getAllActions(const QString& scene) {
    QMap<int, Action> actionMap;

    auto readBindings = [&](const QString& targetScene, bool onlyMissing) {
        QSqlQuery query;
        query.prepare(QStringLiteral(
            "SELECT b.slot, a.id, a.name, a.type, a.payload FROM wheel_bindings b "
            "JOIN actions_lib a ON a.id = b.action_id "
            "WHERE b.scene = ? ORDER BY b.slot"));
        query.addBindValue(targetScene);
        if (!query.exec()) return;

        while (query.next()) {
            const int slot = query.value(0).toInt();
            if (onlyMissing && actionMap.contains(slot)) continue;

            Action action;
            action.slot = slot;
            action.id = query.value(1).toInt();
            action.name = query.value(2).toString();
            action.type = query.value(3).toInt();
            action.payload = query.value(4).toString();
            actionMap[slot] = action;
        }
    };

    readBindings(scene, false);
    if (scene != QLatin1String("global")) {
        readBindings(QStringLiteral("global"), true);
    }

    if (actionMap.isEmpty() &&
        setting(QStringLiteral("schema.actions_v2")) != QLatin1String("1")) {
        auto readLegacy = [&](const QString& targetScene, bool onlyMissing) {
            QSqlQuery query;
            query.prepare(QStringLiteral(
                "SELECT slot, name, type, payload FROM actions WHERE scene = ? ORDER BY slot"));
            query.addBindValue(targetScene);
            if (!query.exec()) return;

            while (query.next()) {
                const int slot = query.value(0).toInt();
                if (onlyMissing && actionMap.contains(slot)) continue;

                Action action;
                action.slot = slot;
                action.name = query.value(1).toString();
                action.type = query.value(2).toInt();
                action.payload = query.value(3).toString();
                actionMap[slot] = action;
            }
        };

        readLegacy(scene, false);
        if (scene != QLatin1String("global")) {
            readLegacy(QStringLiteral("global"), true);
        }
    }

    QVector<Action> list;
    list.reserve(actionMap.size());
    for (auto it = actionMap.constBegin(); it != actionMap.constEnd(); ++it) {
        list.append(it.value());
    }
    return list;
}

QVector<Action> ActionRepository::getAllActions() {
    QVector<Action> list;
    QSqlQuery query(QStringLiteral("SELECT slot, name, type, payload FROM actions ORDER BY slot"));
    while (query.next()) {
        Action act;
        act.slot = query.value(0).toInt();
        act.name = query.value(1).toString();
        act.type = query.value(2).toInt();
        act.payload = query.value(3).toString().trimmed();
        list.append(act);
    }
    return list;
}

QVector<MouseBinding> ActionRepository::getMouseBindings() {
    QVector<MouseBinding> list;
    QSqlQuery query(QStringLiteral("SELECT button, name, type, payload FROM mouse_bindings"));
    while (query.next()) {
        MouseBinding binding;
        binding.button = query.value(0).toString();
        binding.name = query.value(1).toString();
        binding.type = query.value(2).toInt();
        binding.payload = query.value(3).toString();
        list.append(binding);
    }
    return list;
}

bool ActionRepository::saveMouseBindings(const QVector<MouseBinding>& bindings) {
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) return false;

    QSqlQuery clear(db);
    if (!clear.exec(QStringLiteral("DELETE FROM mouse_bindings"))) {
        db.rollback();
        return false;
    }

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO mouse_bindings (button, name, type, payload) VALUES (?, ?, ?, ?)"));
    for (const MouseBinding& binding : bindings) {
        if (binding.button.isEmpty() || binding.type < 0) continue;

        insert.bindValue(0, binding.button);
        insert.bindValue(1, binding.name);
        insert.bindValue(2, binding.type);
        insert.bindValue(3, nonNull(binding.payload));
        if (!insert.exec()) {
            qWarning().noquote() << "保存鼠标绑定失败:" << insert.lastError().text();
            db.rollback();
            return false;
        }
    }

    return db.commit();
}

QMap<QString, QString> ActionRepository::settings() {
    QMap<QString, QString> values;
    QSqlQuery query(QStringLiteral("SELECT key, value FROM settings"));
    while (query.next()) {
        values.insert(query.value(0).toString(), query.value(1).toString());
    }
    return values;
}

bool ActionRepository::saveSettings(const QMap<QString, QString>& values) {
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("REPLACE INTO settings (key, value) VALUES (?, ?)"));
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        query.bindValue(0, it.key());
        query.bindValue(1, nonNull(it.value()));
        if (!query.exec()) {
            qWarning().noquote() << "保存设置失败:" << it.key() << query.lastError().text();
            db.rollback();
            return false;
        }
    }

    return db.commit();
}

QString ActionRepository::setting(const QString& key, const QString& defaultValue) {
    QSqlQuery query;
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return defaultValue;
}

QVector<QPair<QString, QString>> ActionRepository::getScenes() {
    QVector<QPair<QString, QString>> list;
    QSqlQuery query(QStringLiteral("SELECT process_name, display_name FROM scenes ORDER BY process_name = 'global' DESC, display_name"));
    while (query.next()) {
        list.append({query.value(0).toString(), query.value(1).toString()});
    }
    return list;
}

bool ActionRepository::saveScene(const QString& processName, const QString& displayName) {
    if (processName.isEmpty() || displayName.isEmpty()) return false;
    QSqlQuery query;
    // 用 UPSERT 代替 INSERT OR REPLACE：后者会"删旧行再插新行"，
    // 导致 page_count / wheel_rings / enable_hidden_ring 被重置成默认值
    //
    // wheel_rings 显式写 -1：新场景一律"继承全局"。
    // 表默认值是 1，不显式给就会一出生就打破继承。
    // ON CONFLICT 只更新 display_name，所以重命名/再保存不会把已独立配置的环数打回 -1。
    query.prepare(QStringLiteral("INSERT INTO scenes (process_name, display_name, wheel_rings) VALUES (?, ?, -1) "
                                 "ON CONFLICT(process_name) DO UPDATE SET display_name = excluded.display_name"));
    query.addBindValue(processName.trimmed().toLower());
    query.addBindValue(displayName.trimmed());
    return query.exec();
}

bool ActionRepository::deleteScene(const QString& processName) {
    if (processName == QLatin1String("global")) return false;
    QSqlDatabase db = QSqlDatabase::database();
    db.transaction();
    QSqlQuery q1(db), q2(db);
    
    // 删除场景相关绑定
    q1.prepare(QStringLiteral("DELETE FROM wheel_bindings WHERE scene = ?"));
    q1.addBindValue(processName);
    q1.exec();

    QSqlQuery q3(db);
    q3.prepare(QStringLiteral("DELETE FROM page_bindings WHERE scene = ?"));
    q3.addBindValue(processName);
    q3.exec();
    
    // 删除场景（如果有 page_bindings 表也要一并删，目前暂不涉及）
    q2.prepare(QStringLiteral("DELETE FROM scenes WHERE process_name = ?"));
    q2.addBindValue(processName);
    q2.exec();
    
    return db.commit();
}


QMap<int, int> ActionRepository::getPageBindings(const QString& scene, int pageIndex) {
    QMap<int, int> bindings;
    QSqlQuery query;
    query.prepare(QStringLiteral("SELECT grid_index, action_id FROM page_bindings WHERE scene = ? AND page_index = ?"));
    query.addBindValue(scene);
    query.addBindValue(pageIndex);
    if (query.exec()) {
        while (query.next()) {
            bindings.insert(query.value(0).toInt(), query.value(1).toInt());
        }
    }
    return bindings;
}

bool ActionRepository::bindActionToPage(int pageIndex, int gridIndex, const QString& scene, int actionId) {
    QSqlQuery query;
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO page_bindings (page_index, grid_index, scene, action_id) VALUES (?, ?, ?, ?)"));
    query.addBindValue(pageIndex);
    query.addBindValue(gridIndex);
    query.addBindValue(scene);
    query.addBindValue(actionId);
    return query.exec();
}

bool ActionRepository::unbindPageSlot(int pageIndex, int gridIndex, const QString& scene) {
    QSqlQuery query;
    query.prepare(QStringLiteral("DELETE FROM page_bindings WHERE page_index = ? AND grid_index = ? AND scene = ?"));
    query.addBindValue(pageIndex);
    query.addBindValue(gridIndex);
    query.addBindValue(scene);
    return query.exec();
}

int ActionRepository::getSceneRings(const QString& scene) {
    // 语义：wheel_rings = -1 表示本场景处于「继承全局」状态；
    //       1/2/3 才是本场景的独立配置。
    // 所以非法的值（-1 或越界）必须继续向下查 global，不能就地兜底成 1 ——
    // 否则继承状态会被"1 环"顶掉，全局改成 2/3 环时本场景永远不跟随。
    QSqlQuery query;
    query.prepare(QStringLiteral("SELECT wheel_rings FROM scenes WHERE process_name = ?"));
    query.addBindValue(scene);
    if (query.exec() && query.next()) {
        const int rings = query.value(0).toInt();
        if (rings >= 1 && rings <= 3) return rings; // 独立配置，直接用
    }

    // 未配置 / -1：非 global 场景继承 global 的环数
    if (scene != QLatin1String("global")) {
        QSqlQuery q2;
        q2.prepare(QStringLiteral("SELECT wheel_rings FROM scenes WHERE process_name = 'global'"));
        if (q2.exec() && q2.next()) {
            const int rings = q2.value(0).toInt();
            if (rings >= 1 && rings <= 3) return rings;
        }
    }
    return 1; // 兜底：连 global 都没配（旧库）时按 1 环
}

bool ActionRepository::setSceneRings(const QString& scene, int rings) {
    // -1 = 恢复继承全局（UI 目前只会传 1/2/3，这里保留通路）
    if (rings != -1) {
        if (rings < 1) rings = 1;
        if (rings > 3) rings = 3;
    }
    QSqlQuery query;
    query.prepare(QStringLiteral("UPDATE scenes SET wheel_rings = ? WHERE process_name = ?"));
    query.addBindValue(rings);
    query.addBindValue(scene);
    return query.exec();
}

int ActionRepository::getSceneHiddenRing(const QString& scene) {
    QSqlQuery query;
    query.prepare(QStringLiteral("SELECT enable_hidden_ring FROM scenes WHERE process_name = ?"));
    query.addBindValue(scene);
    if (query.exec() && query.next()) {
        if (query.value(0).isNull()) return -1;
        const int enabled = query.value(0).toInt();
        if (enabled < 0) return -1;
        return enabled == 1 ? 1 : 0;
    }
    return -1;
}

bool ActionRepository::setSceneHiddenRing(const QString& scene, int enabled) {
    if (scene.isEmpty()) return false;
    QSqlQuery query;
    query.prepare(QStringLiteral("UPDATE scenes SET enable_hidden_ring = ? WHERE process_name = ?"));
    query.addBindValue(enabled);
    query.addBindValue(scene);
    return query.exec();
}

void ActionRepository::closeDatabase() {
    const QString connectionName = QSqlDatabase::database().connectionName();
    QSqlDatabase::database().close();
    QSqlDatabase::removeDatabase(connectionName);
}

bool ActionRepository::renameScene(const QString& oldName, const QString& newName, const QString& newDisplayName) {
    if (oldName == newName) {
        // 如果进程名没变，只更新显示名称
        return saveScene(oldName, newDisplayName);
    }
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) return false;
    // 1. 检查新名字是否已经存在
    QSqlQuery check(db);
    check.prepare(QStringLiteral("SELECT 1 FROM scenes WHERE process_name = ?"));
    check.addBindValue(newName);
    if (check.exec() && check.next()) {
        db.rollback();
        return false; // 名字已被占用
    }
    // 2. 插入新的场景记录（自定义字段必须全部迁移，否则改个名就丢配置）
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO scenes (process_name, display_name, wheel_rings, page_count, enable_hidden_ring) "
        "SELECT ?, ?, wheel_rings, page_count, enable_hidden_ring FROM scenes WHERE process_name = ?"));
    insert.addBindValue(newName);
    insert.addBindValue(newDisplayName);
    insert.addBindValue(oldName);
    if (!insert.exec()) {
        db.rollback();
        return false;
    }
    // 3. 迁移轮盘绑定
    QSqlQuery moveWheel(db);
    moveWheel.prepare(QStringLiteral("UPDATE wheel_bindings SET scene = ? WHERE scene = ?"));
    moveWheel.addBindValue(newName);
    moveWheel.addBindValue(oldName);
    if (!moveWheel.exec()) {
        db.rollback();
        return false;
    }
    // 4. 迁移动作页绑定
    QSqlQuery movePage(db);
    movePage.prepare(QStringLiteral("UPDATE page_bindings SET scene = ? WHERE scene = ?"));
    movePage.addBindValue(newName);
    movePage.addBindValue(oldName);
    if (!movePage.exec()) {
        db.rollback();
        return false;
    }
    // 5. 删除旧的场景记录
    QSqlQuery removeOld(db);
    removeOld.prepare(QStringLiteral("DELETE FROM scenes WHERE process_name = ?"));
    removeOld.addBindValue(oldName);
    if (!removeOld.exec()) {
        db.rollback();
        return false;
    }
    return db.commit();
}

int ActionRepository::getScenePageCount(const QString& scene) {
    QSqlQuery query;
    query.prepare(QStringLiteral("SELECT page_count FROM scenes WHERE process_name = ?"));
    query.addBindValue(scene);
    if (query.exec() && query.next()) {
        int count = query.value(0).toInt();
        if (count < 1) count = 1;
        return count;
    }
    // 如果没有记录（比如未配置），默认1页
    return 1;
}

bool ActionRepository::setScenePageCount(const QString& scene, int count) {
    if (count < 1) count = 1;
    QSqlQuery query;
    query.prepare(QStringLiteral("UPDATE scenes SET page_count = ? WHERE process_name = ?"));
    query.addBindValue(count);
    query.addBindValue(scene);
    return query.exec();
}

bool ActionRepository::clearPage(const QString& scene, int pageIndex) {
    QSqlQuery query;
    query.prepare(QStringLiteral("DELETE FROM page_bindings WHERE scene = ? AND page_index = ?"));
    query.addBindValue(scene);
    query.addBindValue(pageIndex);
    return query.exec();
}

int ActionRepository::copyPage(const QString& srcScene, int srcPageIndex, const QString& dstScene, int dstPageIndex) {
    // 1. 先清空目标页
    if (!clearPage(dstScene, dstPageIndex)) return -1;
    // 2. 直接通过 SQL 进行跨页面的数据迁移（效率最高）
    QSqlQuery query;
    query.prepare(QStringLiteral(
        "INSERT INTO page_bindings (page_index, grid_index, scene, action_id) "
        "SELECT ?, grid_index, ?, action_id FROM page_bindings "
        "WHERE scene = ? AND page_index = ?"));
    query.addBindValue(dstPageIndex);
    query.addBindValue(dstScene);
    query.addBindValue(srcScene);
    query.addBindValue(srcPageIndex);
    
    if (!query.exec()) return -1;
    return query.numRowsAffected();
}

bool ActionRepository::clearSceneBindings(const QString& scene) {
    if (scene.isEmpty() || scene == QLatin1String("global")) return false;
    QSqlQuery query;
    query.prepare(QStringLiteral("DELETE FROM wheel_bindings WHERE scene = ?"));
    query.addBindValue(scene);
    return query.exec();
}

bool ActionRepository::movePage(const QString& scene, int currentPage, int delta) {
    if (currentPage < 0 || delta == 0) return false;
    const int targetPage = currentPage + delta;
    const int totalPages = getScenePageCount(scene);
    if (targetPage < 0 || targetPage >= totalPages) return false;

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) return false;

    QSqlQuery query(db);
    // 1. 先把当前页挪到临时负数索引，避免与目标页的 (scene,page_index,grid) 唯一约束撞车
    query.prepare(QStringLiteral(
        "UPDATE page_bindings SET page_index = -1 - page_index WHERE scene = ? AND page_index = ?"));
    query.addBindValue(scene);
    query.addBindValue(currentPage);
    if (!query.exec()) {
        db.rollback();
        return false;
    }
    // 2. 目标页占用当前页的位置
    query.prepare(QStringLiteral(
        "UPDATE page_bindings SET page_index = ? WHERE scene = ? AND page_index = ?"));
    query.addBindValue(currentPage);
    query.addBindValue(scene);
    query.addBindValue(targetPage);
    if (!query.exec()) {
        db.rollback();
        return false;
    }
    // 3. 临时页落到目标位置（此时无论哪一页为空都能得到正确结果）
    query.prepare(QStringLiteral(
        "UPDATE page_bindings SET page_index = ? WHERE scene = ? AND page_index = ?"));
    query.addBindValue(targetPage);
    query.addBindValue(scene);
    query.addBindValue(-1 - currentPage);
    if (!query.exec()) {
        db.rollback();
        return false;
    }
    return db.commit();
}