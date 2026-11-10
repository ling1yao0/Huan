// 脚本执行器集成测试：真实启动 Python / cmd / PowerShell，但不会注入鼠标键盘。
// 用 ASCII 输出，避免控制台代码页干扰。
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include "ScriptRunner.h"

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

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(content.toUtf8()) >= 0;
}

ScriptRunner::Result runSync(const QString& command, const QString& input, int timeoutMs,
                             qint64* elapsedMs = nullptr) {
    ScriptRunner::Result result;
    bool done = false;

    auto* runner = new ScriptRunner();
    QEventLoop loop;
    QTimer safety;
    safety.setSingleShot(true);
    QObject::connect(&safety, &QTimer::timeout, &loop, &QEventLoop::quit);

    QElapsedTimer timer;
    timer.start();

    runner->run(command, input, timeoutMs, [&](const ScriptRunner::Result& value) {
        result = value;
        done = true;
        loop.quit();
    });

    if (!done) {
        safety.start(timeoutMs + 6000);
        loop.exec();
    }

    if (elapsedMs) *elapsedMs = timer.elapsed();
    if (!done) {
        result.ok = false;
        result.error = QStringLiteral("测试框架等待超时");
    }

    delete runner;
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    out() << "=== Script command resolution ===\n";

    {
        const ScriptCommand spec =
            ScriptRunner::resolve(QStringLiteral("python -u \"C:\\a b\\x.py\""));
        expect(spec.valid() &&
                   QFileInfo(spec.program).fileName().startsWith(QLatin1String("python"),
                                                                 Qt::CaseInsensitive) &&
                   spec.arguments.contains(QStringLiteral("C:\\a b\\x.py")),
               QStringLiteral("explicit python command line"), spec.error);
    }
    {
        const ScriptCommand spec = ScriptRunner::resolve(QStringLiteral("\"C:\\a b\\x.py\""));
        expect(spec.valid() &&
                   QFileInfo(spec.program).fileName().startsWith(QLatin1String("python"),
                                                                 Qt::CaseInsensitive) &&
                   spec.arguments.size() == 1 &&
                   spec.arguments.first() == QStringLiteral("C:\\a b\\x.py"),
               QStringLiteral(".py path is mapped to python"), spec.error);
    }
    {
        const ScriptCommand spec = ScriptRunner::resolve(QStringLiteral("\"C:\\a b\\x.ps1\""));
        expect(spec.valid() && spec.arguments.contains(QStringLiteral("-File")),
               QStringLiteral(".ps1 path is mapped to powershell -File"), spec.error);
    }
    {
        const ScriptCommand spec = ScriptRunner::resolve(QStringLiteral("\"C:\\a b\\x.bat\""));
        expect(spec.valid() &&
                   QFileInfo(spec.program).fileName().compare(QLatin1String("cmd.exe"),
                                                              Qt::CaseInsensitive) == 0 &&
                   spec.arguments.contains(QStringLiteral("/c")),
               QStringLiteral(".bat path is mapped to cmd /c"), spec.error);
    }
    {
        const ScriptCommand spec = ScriptRunner::resolve(QStringLiteral("notepad.exe"));
        expect(spec.valid() && spec.arguments.isEmpty(),
               QStringLiteral("plain executable is passed through"), spec.error);
    }
    {
        const ScriptCommand spec = ScriptRunner::resolve(QString());
        expect(!spec.valid(), QStringLiteral("empty command is rejected"));
    }

    out() << "\n=== Script execution ===\n";

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        out() << "[FAIL] cannot create temp dir\n";
        return 1;
    }

    const QString echoScript = tempDir.filePath(QStringLiteral("echo.py"));
    writeFile(echoScript,
              QStringLiteral("import sys\n"
                             "sys.stdin.reconfigure(encoding='utf-8')\n"
                             "sys.stdout.reconfigure(encoding='utf-8')\n"
                             "data = sys.stdin.read()\n"
                             "print('收到:' + data)\n"));

    {
        const ScriptRunner::Result result =
            runSync(QStringLiteral("python \"%1\"").arg(QDir::toNativeSeparators(echoScript)),
                    QStringLiteral("中文ABC"), 8000);
        expect(result.ok, QStringLiteral("python exits 0"), result.error);
        expect(result.stdoutText.contains(QStringLiteral("收到:中文ABC")),
               QStringLiteral("python stdin/stdout is UTF-8"),
               result.stdoutText.trimmed());
    }

    {
        const ScriptRunner::Result result =
            runSync(QStringLiteral("python \"%1\"").arg(QDir::toNativeSeparators(echoScript)),
                    QString(), 8000);
        expect(result.ok && result.stdoutText.trimmed() == QStringLiteral("收到:"),
               QStringLiteral("empty stdin becomes EOF"), result.stdoutText.trimmed());
    }

    const QString failScript = tempDir.filePath(QStringLiteral("fail.py"));
    writeFile(failScript,
              QStringLiteral("import sys\n"
                             "sys.stderr.write('boom')\n"
                             "sys.exit(3)\n"));
    {
        const ScriptRunner::Result result =
            runSync(QStringLiteral("python \"%1\"").arg(QDir::toNativeSeparators(failScript)),
                    QString(), 8000);
        expect(!result.ok && result.exitCode == 3,
               QStringLiteral("non-zero exit code is reported"),
               QStringLiteral("code=%1 error=%2").arg(result.exitCode).arg(result.error));
        expect(result.stderrText.contains(QStringLiteral("boom")),
               QStringLiteral("stderr is captured"), result.stderrText.trimmed());
    }

    const QString sleepScript = tempDir.filePath(QStringLiteral("sleep.py"));
    writeFile(sleepScript, QStringLiteral("import time\nwhile True:\n    time.sleep(0.2)\n"));
    {
        qint64 elapsed = 0;
        const ScriptRunner::Result result =
            runSync(QStringLiteral("python \"%1\"").arg(QDir::toNativeSeparators(sleepScript)),
                    QString(), 600, &elapsed);
        expect(!result.ok && result.timedOut,
               QStringLiteral("timeout kills infinite script"), result.error);
        expect(elapsed < 8000,
               QStringLiteral("timeout returns promptly"),
               QStringLiteral("%1 ms").arg(elapsed));
    }

    const QString batchScript = tempDir.filePath(QStringLiteral("hello.bat"));
    writeFile(batchScript, QStringLiteral("@echo off\r\necho bat-ok\r\n"));
    {
        const ScriptRunner::Result result =
            runSync(QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(batchScript)),
                    QString(), 8000);
        expect(result.ok && result.stdoutText.contains(QStringLiteral("bat-ok")),
               QStringLiteral("batch file runs via cmd /c"),
               result.error + QStringLiteral(" | ") + result.stdoutText.trimmed());
    }

    if (!QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty()) {
        const QString psScript = tempDir.filePath(QStringLiteral("hello.ps1"));
        writeFile(psScript, QStringLiteral("Write-Output \"ps-ok\"\n"));
        const ScriptRunner::Result result =
            runSync(QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(psScript)),
                    QString(), 20000);
        expect(result.ok && result.stdoutText.contains(QStringLiteral("ps-ok")),
               QStringLiteral("PowerShell script runs"),
               result.error + QStringLiteral(" | ") + result.stdoutText.trimmed());
    } else {
        out() << "[SKIP] PowerShell not installed\n";
    }

    out() << "\n";
    if (g_failures == 0) {
        out() << "ALL TESTS PASSED\n";
    } else {
        out() << g_failures << " TEST(S) FAILED\n";
    }
    out().flush();

    return g_failures == 0 ? 0 : 1;
}
