#pragma once

#include <QObject>
#include <QString>

#include <functional>

#include "ActionModel.h"

// 动作执行引擎。所有耗时操作（外部脚本）都是异步的，不阻塞 UI。
class ActionEngine : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(bool ok, const QString& message)>;

    explicit ActionEngine(QObject* parent = nullptr);

    static ActionEngine* instance();

    // 轮盘选中某个槽位后调用。
    static void execute(int slot, const QString& scene);

    // 直接执行一个动作（鼠标高级绑定也走这里）。
    static void executeAction(const Action& action, const QString& scene);

signals:
    // 顶层动作执行结束。失败时 UI 可以弹托盘提示。
    void actionFinished(bool ok, const QString& message);

private:
    class PipelineRunner;

    void runAction(const Action& action, const QString& scene, Done done);

    void runKeySequenceAction(const QString& payload, const QString& scene, Done done);
    void runLaunchAction(const QString& payload, const QString& scene, Done done);
    void runClipboardAction(const QString& payload, const QString& scene, Done done);
    void runScriptAction(const QString& payload, const QString& scene, Done done);
    void runTextAction(const QString& payload, const QString& scene, Done done);
    void runPipelineAction(const QString& payload, const QString& scene, Done done);

    void notify(bool ok, const QString& message);

    static QString expand(const QString& text, const QString& scene, const QString& selection);
};
