#include "ScriptRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

QString findExecutable(const QString& name) {
    return QStandardPaths::findExecutable(name);
}

void killProcessTree(QProcess* process) {
    if (!process) return;

    const qint64 pid = process->processId();
    if (pid <= 0) {
        process->kill();
        return;
    }

    // 用 startDetached 异步结束整棵进程树，并隐藏 taskkill 的控制台窗口。
    QProcess killer;
    killer.setProgram(QStringLiteral("taskkill.exe"));
    killer.setArguments({QStringLiteral("/PID"), QString::number(pid),
                         QStringLiteral("/T"), QStringLiteral("/F")});
    killer.setStandardOutputFile(QProcess::nullDevice());
    killer.setStandardErrorFile(QProcess::nullDevice());
    killer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    killer.startDetached();
}

} // namespace

ScriptCommand ScriptCommand::resolve(const QString& commandLine) {
    ScriptCommand command;

    const QStringList parts = QProcess::splitCommand(commandLine);
    if (parts.isEmpty()) {
        command.error = QStringLiteral("命令为空");
        return command;
    }

    QStringList arguments = parts;
    const QString first = arguments.takeFirst();
    const QFileInfo fileInfo(first);
    const QString extension = fileInfo.suffix().toLower();

    command.workingDir = fileInfo.exists() ? fileInfo.absolutePath() : QDir::currentPath();

    auto useInterpreter = [&](const QString& executable, const QStringList& options) {
        command.program = executable;
        command.arguments = options + QStringList{first} + arguments;
    };

    if (extension == QLatin1String("py") || extension == QLatin1String("pyw")) {
        QString python = findExecutable(extension == QLatin1String("pyw")
                                            ? QStringLiteral("pythonw")
                                            : QStringLiteral("python"));
        if (python.isEmpty()) python = findExecutable(QStringLiteral("py"));
        if (python.isEmpty()) {
            command.error = QStringLiteral("找不到 Python。请安装 Python，或把 python.exe 加入 PATH。");
            return command;
        }

        const QStringList options =
            QFileInfo(python).fileName().compare(QLatin1String("py.exe"), Qt::CaseInsensitive) == 0
                ? QStringList{QStringLiteral("-3")}
                : QStringList{};
        useInterpreter(python, options);
    } else if (extension == QLatin1String("js") || extension == QLatin1String("mjs") ||
               extension == QLatin1String("cjs")) {
        const QString node = findExecutable(QStringLiteral("node"));
        if (node.isEmpty()) {
            command.error = QStringLiteral("找不到 Node.js。请安装 Node.js，或把 node.exe 加入 PATH。");
            return command;
        }
        useInterpreter(node, {});
    } else if (extension == QLatin1String("ps1")) {
        QString powershell = findExecutable(QStringLiteral("powershell"));
        if (powershell.isEmpty()) powershell = findExecutable(QStringLiteral("pwsh"));
        if (powershell.isEmpty()) {
            command.error = QStringLiteral("找不到 PowerShell。");
            return command;
        }
        useInterpreter(powershell,
                       {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                        QStringLiteral("Bypass"), QStringLiteral("-File")});
    } else if (extension == QLatin1String("bat") || extension == QLatin1String("cmd")) {
        const QString cmd = findExecutable(QStringLiteral("cmd"));
        if (cmd.isEmpty()) {
            command.error = QStringLiteral("找不到 cmd.exe。");
            return command;
        }
        useInterpreter(cmd, {QStringLiteral("/d"), QStringLiteral("/c")});
    } else if (extension == QLatin1String("vbs")) {
        const QString cscript = findExecutable(QStringLiteral("cscript"));
        if (cscript.isEmpty()) {
            command.error = QStringLiteral("找不到 cscript.exe。");
            return command;
        }
        useInterpreter(cscript, {QStringLiteral("//nologo")});
    } else {
        // .exe 或 PATH 中的命令（python -u xxx.py / notepad.exe / node ...）。
        command.program = first;
        command.arguments = arguments;
    }

    if (command.program.isEmpty()) {
        command.error = QStringLiteral("无法解析脚本命令");
    }
    return command;
}

ScriptRunner::ScriptRunner(QObject* parent) : QObject(parent) {}

ScriptRunner::~ScriptRunner() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
    }
}

void ScriptRunner::run(const QString& commandLine, const QString& stdinText, int timeoutMs,
                       Callback callback) {
    if (m_process || m_finished) return;

    m_callback = std::move(callback);
    m_timeoutMs = qMax(200, timeoutMs);

    const ScriptCommand command = ScriptCommand::resolve(commandLine);
    if (!command.valid()) {
        m_result.error = command.error.isEmpty() ? QStringLiteral("脚本命令无效") : command.error;
        finish();
        return;
    }

    m_process = new QProcess(this);
    m_process->setProgram(command.program);
    m_process->setArguments(command.arguments);
    if (!command.workingDir.isEmpty()) {
        m_process->setWorkingDirectory(command.workingDir);
    }
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    // 统一编码，避免中文在 Python / Node 管道里乱码。
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_process->setProcessEnvironment(environment);

    connect(m_process, &QProcess::started, this, [this, stdinText] {
        if (!m_process) return;
        if (!stdinText.isEmpty()) {
            m_process->write(stdinText.toUtf8());
        }
        m_process->closeWriteChannel();
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_finished || error != QProcess::FailedToStart) return;

        m_result.error = QStringLiteral("脚本启动失败: %1")
                             .arg(m_process ? m_process->errorString() : QStringLiteral("未知错误"));
        finish();
    });

    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_finished) return;

                m_result.exitCode = exitCode;
                if (m_process) {
                    m_result.stdoutText = QString::fromUtf8(m_process->readAllStandardOutput());
                    m_result.stderrText = QString::fromUtf8(m_process->readAllStandardError());
                }

                if (m_timedOut) {
                    m_result.timedOut = true;
                    m_result.error = QStringLiteral("脚本执行超时（%1 ms），已强制结束进程")
                                         .arg(m_timeoutMs);
                } else if (exitStatus == QProcess::CrashExit) {
                    m_result.crashed = true;
                    m_result.error = QStringLiteral("脚本异常退出");
                    if (!m_result.stderrText.trimmed().isEmpty()) {
                        m_result.error += QStringLiteral(": ") + m_result.stderrText.trimmed();
                    }
                } else if (exitCode != 0) {
                    m_result.error = QStringLiteral("脚本退出码 %1").arg(exitCode);
                    if (!m_result.stderrText.trimmed().isEmpty()) {
                        m_result.error += QStringLiteral(": ") + m_result.stderrText.trimmed();
                    }
                } else {
                    m_result.ok = true;
                }

                finish();
            });

    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this] {
        if (m_finished || !m_process) return;

        m_timedOut = true;
        killProcessTree(m_process);

        // 极少数情况下 taskkill 也结束不了，再兜底一次。
        QTimer::singleShot(2000, this, [this] {
            if (m_finished) return;
            m_result.error = QStringLiteral("脚本执行超时（%1 ms），且无法结束进程")
                                 .arg(m_timeoutMs);
            finish();
        });
    });
    m_timeoutTimer->start(m_timeoutMs);

    m_process->start();
}

void ScriptRunner::finish() {
    if (m_finished) return;
    m_finished = true;

    if (m_timeoutTimer) {
        m_timeoutTimer->stop();
        m_timeoutTimer->deleteLater();
        m_timeoutTimer = nullptr;
    }

    if (m_process) {
        if (m_process->state() != QProcess::NotRunning) {
            killProcessTree(m_process);
        }
        m_process->deleteLater();
        m_process = nullptr;
    }

    if (m_callback) {
        Callback callback = std::move(m_callback);
        m_callback = nullptr;
        callback(m_result);
    }
}
