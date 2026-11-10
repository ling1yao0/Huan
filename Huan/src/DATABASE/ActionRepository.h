#pragma once

#include <QMap>
#include <QString>
#include <QVector>

#include "ActionModel.h"

class ActionRepository {
public:

    // 初始化数据库（建表、插入默认数据、补齐默认设置）。
    static bool init(const QString& dbPath);

    // 根据槽位获取一个动作；当前场景没有时回退到 global。
    static Action getActionBySlot(int slot, const QString& scene);

    // 获取某个场景的 8 个槽位（场景优先，缺失槽位回退 global）。
    static QVector<Action> getAllActions(const QString& scene);

    // 兼容旧代码：读取全表。
    static QVector<Action> getAllActions();

    // ---- 动作库 + 轮盘绑定（v2 数据模型） ----
    static QVector<LibraryAction> getActionLibrary();
    static int saveLibraryAction(const LibraryAction& action); // 返回 id，失败 -1
    static bool deleteLibraryAction(int id);
    static bool incrementActionUsage(int actionId); // 使用次数 +1（id <= 0 直接返回 false）

    // scene -> (slot -> action_id)
    static QMap<int, int> getWheelBindings(const QString& scene);
    static bool bindActionToSlot(int slot, const QString& scene, int actionId);
    static bool unbindSlot(int slot, const QString& scene);

    // 把 global 的绑定物理复制到 scene，返回复制条数；失败 -1。
    static int copyGlobalBindingsToScene(const QString& scene);

    // 中键 / 侧键直接绑定的动作。
    static QVector<MouseBinding> getMouseBindings();
    static bool saveMouseBindings(const QVector<MouseBinding>& bindings);

    // 键值对设置（触发方式、阈值、超时等）。
    static QMap<QString, QString> settings();
    static bool saveSettings(const QMap<QString, QString>& values);
    static QString setting(const QString& key, const QString& defaultValue = QString());

    // 场景管理
    static QVector<QPair<QString, QString>> getScenes(); // <process_name, display_name>
    static bool saveScene(const QString& processName, const QString& displayName);
    static bool deleteScene(const QString& processName);

    // 重命名场景（修改进程名），并同步迁移所有绑定关系
    static bool renameScene(const QString& oldName, const QString& newName, const QString& newDisplayName);

    // 场景轮盘环数（1/2/3 环）
    static int getSceneRings(const QString& scene);
    static bool setSceneRings(const QString& scene, int rings);

    // 场景专属的"外圈隐藏动作"开关：1 开启 / 0 关闭 / -1 继承全局
    static int getSceneHiddenRing(const QString& scene);
    static bool setSceneHiddenRing(const QString& scene, int enabled);


    // ---- 动作页（Action Page）绑定 ----
    // 获取某场景某页的绑定：grid_index -> action_id
    static QMap<int, int> getPageBindings(const QString& scene, int pageIndex);
    static bool bindActionToPage(int pageIndex, int gridIndex, const QString& scene, int actionId);
    static bool unbindPageSlot(int pageIndex, int gridIndex, const QString& scene);

    // 获取场景的总页数
    static int getScenePageCount(const QString& scene);
    // 设置场景的总页数
    static bool setScenePageCount(const QString& scene, int count);

    // 清空指定场景指定页的所有绑定
    static bool clearPage(const QString& scene, int pageIndex);
    
    // 将源场景的某页复制到目标场景的某页（返回复制的格子数，失败-1）
    static int copyPage(const QString& srcScene, int srcPageIndex, const QString& dstScene, int dstPageIndex);

    // 移动页面位置（delta = -1 左移，+1 右移），整页交换 page_index
    static bool movePage(const QString& scene, int currentPage, int delta);

    // 清空某场景的全部轮盘绑定（用于"重置为跟随全局"）
    static bool clearSceneBindings(const QString& scene);

    // 关闭数据库连接（导入备份时使用）
    static void closeDatabase();
};
