#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;
class QTimer;

// 把用户填写的“脚本参数”解析成可执行的 program + arguments。
// 支持：.py / .pyw / .js / .mjs / .cjs / .ps1 / .bat / .cmd / .vbs / .exe，
// 也支持直接写 "python xxx.py" 这类完整命令行。
struct ScriptCommand {
    QString program;
    QStringList arguments;
    QString workingDir;
    QString error;

    bool valid() const { return error.isEmpty() && !program.isEmpty(); }

    static ScriptCommand resolve(const QString& commandLine);
};

// 异步执行一个脚本：
//   stdin  <- UTF-8 文本
//   stdout -> UTF-8 文本（由调用方决定是否粘贴）
//   超时会杀死整个进程树，不会卡住主线程。
class ScriptRunner : public QObject {
public:
    struct Result {
        bool ok = false;
        bool timedOut = false;
        bool crashed = false;
        int exitCode = -1;
        QString stdoutText;
        QString stderrText;
        QString error;
    };

    using Callback = std::function<void(const Result& result)>;

    explicit ScriptRunner(QObject* parent = nullptr);
    ~ScriptRunner() override;

    void run(const QString& commandLine, const QString& stdinText, int timeoutMs,
             Callback callback);

    static ScriptCommand resolve(const QString& commandLine) {
        return ScriptCommand::resolve(commandLine);
    }

private:
    void finish();

    QProcess* m_process = nullptr;
    QTimer* m_timeoutTimer = nullptr;
    bool m_finished = false;
    bool m_timedOut = false;
    int m_timeoutMs = 8000;
    Callback m_callback;
    Result m_result;
};
