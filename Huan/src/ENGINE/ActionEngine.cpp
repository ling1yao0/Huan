#include "ActionEngine.h"

#include "ActionRepository.h"
#include "AppConfig.h"
#include "ScriptRunner.h"
#include "VariableResolver.h"
#include "Win32Utils.h"

#include <QApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>

#include <shellapi.h>

#include <algorithm>

namespace {

bool needsSelection(const QString& payload) {
    return payload.contains(QLatin1String("{selected}"), Qt::CaseInsensitive) ||
           payload.contains(QLatin1String("{selection}"), Qt::CaseInsensitive);
}

QString findExecutable(const QString& name) {
    return QStandardPaths::findExecutable(name);
}

QString titleCase(const QString& text) {
    QString result;
    result.reserve(text.size());
    bool newWord = true;
    for (QChar ch : text) {
        if (ch.isLetterOrNumber()) {
            result += newWord ? ch.toUpper() : ch;
            newWord = false;
        } else {
            result += ch;
            newWord = true;
        }
    }
    return result;
}

QString reverseText(const QString& text) {
    QVector<uint> codePoints = text.toUcs4();
    std::reverse(codePoints.begin(), codePoints.end());
    return QString::fromUcs4(codePoints.constData(), codePoints.size());
}

int typeFromName(const QString& name) {
    const QString normalized = name.trimmed().toLower();
    if (normalized == QLatin1String("key") || normalized == QLatin1String("keys") ||
        normalized == QLatin1String("hotkey")) {
        return 0;
    }
    if (normalized == QLatin1String("launch") || normalized == QLatin1String("run") ||
        normalized == QLatin1String("program")) {
        return 1;
    }
    if (normalized == QLatin1String("clipboard") || normalized == QLatin1String("transform")) {
        return 2;
    }
    if (normalized == QLatin1String("script")) return 3;
    if (normalized == QLatin1String("pipeline")) return 4;
    if (normalized == QLatin1String("text") || normalized == QLatin1String("type")) return 5;
    return -1;
}

} // namespace

class ActionEngine::PipelineRunner final : public QObject {
public:
    PipelineRunner(ActionEngine* engine, const QJsonArray& steps, const QString& scene, Done done)
        : QObject(engine), m_engine(engine), m_steps(steps), m_scene(scene), m_done(std::move(done)) {}

    void start() { next(); }

private:
    void next() {
        if (!m_done) return;

        if (m_index >= m_steps.size()) {
            finish(true, QStringLiteral("流水线执行完成"));
            return;
        }

        const QJsonValue value = m_steps.at(m_index++);
        if (!value.isObject()) {
            finish(false, QStringLiteral("流水线步骤必须是 JSON 对象"));
            return;
        }
        const QJsonObject step = value.toObject();

        const QJsonValue typeValue = step.value(QStringLiteral("type"));
        if (typeValue.isUndefined()) {
            int delay = step.value(QStringLiteral("wait")).toInt(-1);
            if (delay < 0) delay = step.value(QStringLiteral("delay")).toInt(-1);
            if (delay < 0) {
                finish(false, QStringLiteral("流水线步骤缺少 type 或 wait"));
                return;
            }
            QTimer::singleShot(delay, this, [this] { next(); });
            return;
        }

        Action action;
        action.slot = -1;
        action.name = step.value(QStringLiteral("name")).toString();
        action.payload = step.value(QStringLiteral("payload")).toString();
        action.type = typeValue.isString() ? typeFromName(typeValue.toString())
                                           : typeValue.toInt(-1);

        if (action.type < 0 || action.type > 5) {
            finish(false, QStringLiteral("流水线步骤类型无效: %1").arg(typeValue.toVariant().toString()));
            return;
        }

        m_engine->runAction(action, m_scene, [this](bool ok, const QString& message) {
            if (!ok) {
                finish(false, message);
            } else {
                next();
            }
        });
    }

    void finish(bool ok, const QString& message) {
        if (!m_done) return;
        Done done = std::move(m_done);
        m_done = nullptr;
        done(ok, message);
        deleteLater();
    }

    ActionEngine* m_engine = nullptr;
    QJsonArray m_steps;
    QString m_scene;
    Done m_done;
    int m_index = 0;
};

ActionEngine::ActionEngine(QObject* parent) : QObject(parent) {}

ActionEngine* ActionEngine::instance() {
    static ActionEngine* engine = nullptr;
    if (!engine) {
        engine = new ActionEngine(qApp);
    }
    return engine;
}

void ActionEngine::execute(int slot, const QString& scene) {
    const Action action = ActionRepository::getActionBySlot(slot, scene);
    if (action.type < 0) {
        // 空槽位被划过是正常操作，不弹通知。
        qDebug().noquote() << "槽位" << slot << "没有配置动作";
        return;
    }
    executeAction(action, scene);
}

void ActionEngine::executeAction(const Action& action, const QString& scene) {
    ActionEngine* engine = instance();
    engine->runAction(action, scene, [engine](bool ok, const QString& message) {
        engine->notify(ok, message);
    });
}

void ActionEngine::notify(bool ok, const QString& message) {
    if (ok) {
        qDebug().noquote() << "动作完成:" << message;
    } else {
        qWarning().noquote() << "动作失败:" << message;
    }
    emit actionFinished(ok, message);
}

QString ActionEngine::expand(const QString& text, const QString& scene, const QString& selection) {
    if (text.isEmpty()) return text;

    VariableContext context;
    context.scene = scene;
    context.selection = selection;
    if (text.contains(QLatin1String("{clipboard}"), Qt::CaseInsensitive)) {
        context.clipboard = Win32Utils::getClipboardText();
    }
    return VariableResolver::expand(text, context);
}

void ActionEngine::runAction(const Action& action, const QString& scene, Done done) {
    if (!done) done = [](bool, const QString&) {};

    // 使用次数打点：只有引用动作库的动作带 id。
    // 流水线子步骤、旧表兜底拿到的都是 -1，不会误记。
    if (action.id > 0) {
        ActionRepository::incrementActionUsage(action.id);
    }

    switch (action.type) {
    case 0:
        runKeySequenceAction(action.payload, scene, std::move(done));
        break;
    case 1:
        runLaunchAction(action.payload, scene, std::move(done));
        break;
    case 2:
        runClipboardAction(action.payload, scene, std::move(done));
        break;
    case 3:
        runScriptAction(action.payload, scene, std::move(done));
        break;
    case 4:
        runPipelineAction(action.payload, scene, std::move(done));
        break;
    case 5:
        runTextAction(action.payload, scene, std::move(done));
        break;
    default:
        done(false, QStringLiteral("未知动作类型: %1").arg(action.type));
        break;
    }
}

void ActionEngine::runKeySequenceAction(const QString& payload, const QString& scene, Done done) {
    const QString selection = needsSelection(payload) ? Win32Utils::captureSelectedText() : QString();
    const QString sequence = expand(payload, scene, selection).trimmed();

    if (sequence.isEmpty()) {
        done(false, QStringLiteral("按键序列为空"));
        return;
    }

    // 这里不再对 Alt+F4 做任何特殊处理：聚焦是否到位交给
    // WheelWindow::runActionAfterFocus 负责（抢不到焦点就熔断、根本不发键），
    // 发键本身统一走 sendKeySequence。
    // 在桌面上执行 Alt+F4 会被 sendKeySequence 底层的防关机检查拦下，
    // 所以这里不需要任何特判。
    QString error;
    if (!Win32Utils::sendKeySequence(sequence, &error)) {
        done(false, error);
        return;
    }
    done(true, QStringLiteral("已发送按键 %1").arg(sequence));
}

void ActionEngine::runLaunchAction(const QString& payload, const QString& scene, Done done) {
    const QString selection = needsSelection(payload) ? Win32Utils::captureSelectedText() : QString();
    const QString command = expand(payload, scene, selection).trimmed();

    if (command.isEmpty()) {
        done(false, QStringLiteral("启动命令为空"));
        return;
    }

    if (command.compare(QLatin1String("exit"), Qt::CaseInsensitive) == 0) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        done(true, QStringLiteral("正在退出程序"));
        return;
    }

    const QString lower = command.toLower();
    const bool isUrl = lower.startsWith(QLatin1String("http://")) ||
                       lower.startsWith(QLatin1String("https://")) ||
                       lower.startsWith(QLatin1String("mailto:")) ||
                       lower.startsWith(QLatin1String("shell:"));

    auto shellOpen = [&command](const QString& file) {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open",
                                               reinterpret_cast<LPCWSTR>(file.utf16()),
                                               nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    };

    if (isUrl) {
        if (!shellOpen(command)) {
            done(false, QStringLiteral("无法打开链接: %1").arg(command));
            return;
        }
        done(true, QStringLiteral("已打开 %1").arg(command));
        return;
    }

    const QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) {
        done(false, QStringLiteral("启动命令为空"));
        return;
    }

    QString program = parts.first();
    QStringList arguments = parts.mid(1);

    if (!QFileInfo(program).isAbsolute() && !program.contains(QLatin1Char('/')) &&
        !program.contains(QLatin1Char('\\'))) {
        const QString found = findExecutable(program);
        if (!found.isEmpty()) program = found;
    }

    const bool hasArguments = !arguments.isEmpty();
    const bool exists = QFileInfo::exists(program);

    bool ok = false;
    if (hasArguments) {
        ok = QProcess::startDetached(program, arguments);
    } else if (exists) {
        ok = shellOpen(program);
    } else {
        ok = QProcess::startDetached(program, {});
    }

    if (!ok) {
        done(false, QStringLiteral("无法启动: %1").arg(command));
        return;
    }
    done(true, QStringLiteral("已启动 %1").arg(command));
}

void ActionEngine::runClipboardAction(const QString& payload, const QString&, Done done) {
    const QString operation = payload.trimmed().toLower();
    if (operation.isEmpty()) {
        done(false, QStringLiteral("剪贴板转换参数为空"));
        return;
    }

    const QString oldClipboard = Win32Utils::getClipboardText();
    const QString selected = Win32Utils::captureSelectedText();
    if (selected.isEmpty()) {
        done(false, QStringLiteral("没有选中可转换的文字"));
        return;
    }

    QString result;
    if (operation == QLatin1String("upper")) {
        result = selected.toUpper();
    } else if (operation == QLatin1String("lower")) {
        result = selected.toLower();
    } else if (operation == QLatin1String("title")) {
        result = titleCase(selected);
    } else if (operation == QLatin1String("trim")) {
        result = selected.trimmed();
    } else if (operation == QLatin1String("reverse")) {
        result = reverseText(selected);
    } else {
        done(false, QStringLiteral("未知剪贴板操作“%1”。可用：upper / lower / title / trim / reverse")
                       .arg(operation));
        return;
    }

    if (!Win32Utils::setClipboardText(result)) {
        done(false, QStringLiteral("写入剪贴板失败"));
        return;
    }

    Win32Utils::sendKey('V', true);

    // 等目标程序读完剪贴板，再恢复原内容，最后才报告完成。
    // 这样流水线的下一步不会和剪贴板恢复相互抢时序。
    QTimer::singleShot(250, this,
                       [oldClipboard, result, done = std::move(done)]() mutable {
                           if (Win32Utils::getClipboardText() == result) {
                               Win32Utils::setClipboardText(oldClipboard);
                           }
                           done(true, QStringLiteral("剪贴板转换完成（%1 个字符）").arg(result.size()));
                       });
}

void ActionEngine::runTextAction(const QString& payload, const QString& scene, Done done) {
    const QString selection = needsSelection(payload) ? Win32Utils::captureSelectedText() : QString();
    const QString text = expand(payload, scene, selection);

    if (text.isEmpty()) {
        done(false, QStringLiteral("要输入的文本为空"));
        return;
    }

    QString error;
    if (!Win32Utils::sendText(text, &error)) {
        done(false, error);
        return;
    }
    done(true, QStringLiteral("已输入 %1 个字符").arg(text.size()));
}

void ActionEngine::runScriptAction(const QString& payload, const QString& scene, Done done) {
    // 先保存原始剪贴板，再抓取选中文字（captureSelectedText 会恢复剪贴板）。
    const QString oldClipboard = Win32Utils::getClipboardText();
    const QString selection = Win32Utils::captureSelectedText();
    const QString command = expand(payload, scene, selection).trimmed();

    if (command.isEmpty()) {
        done(false, QStringLiteral("脚本命令为空"));
        return;
    }

    auto* runner = new ScriptRunner(this);
    runner->run(command, selection, AppConfig::instance().scriptTimeoutMs(),
                [this, runner, oldClipboard, done = std::move(done)](
                    const ScriptRunner::Result& result) mutable {
                    runner->deleteLater();

                    if (!result.ok) {
                        done(false, result.error);
                        return;
                    }

                    const QString output = result.stdoutText.trimmed();
                    if (output.isEmpty()) {
                        done(true, QStringLiteral("脚本执行完成（没有输出，未粘贴）"));
                        return;
                    }

                    if (!Win32Utils::setClipboardText(output)) {
                        done(false, QStringLiteral("脚本已执行，但写入剪贴板失败"));
                        return;
                    }

                    Win32Utils::sendKey('V', true);
                    QTimer::singleShot(
                        250, this,
                        [oldClipboard, output, done = std::move(done)]() mutable {
                            if (Win32Utils::getClipboardText() == output) {
                                Win32Utils::setClipboardText(oldClipboard);
                            }
                            done(true, QStringLiteral("脚本输出已粘贴（%1 个字符）")
                                           .arg(output.size()));
                        });
                });
}

void ActionEngine::runPipelineAction(const QString& payload, const QString& scene, Done done) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        done(false, QStringLiteral("流水线 JSON 解析失败: %1（位置 %2）")
                       .arg(parseError.errorString())
                       .arg(parseError.offset));
        return;
    }

    QJsonArray steps;
    if (document.isArray()) {
        steps = document.array();
    } else if (document.isObject()) {
        steps = document.object().value(QStringLiteral("steps")).toArray();
    }

    if (steps.isEmpty()) {
        done(false, QStringLiteral("流水线没有任何步骤"));
        return;
    }

    auto* runner = new PipelineRunner(this, steps, scene, std::move(done));
    runner->start();
}
