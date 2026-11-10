#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QSharedMemory>
#include <QSystemTrayIcon>
#include <QTimer>

#include "ActionEngine.h"
#include "ActionPage.h"
#include "ActionRepository.h"
#include "AppConfig.h"
#include "Constants.h"
#include "HookManager.h"
#include "MainWindow.h"
#include "WheelWindow.h"
#include "Win32Utils.h" // ExitProcess

int main(int argc, char* argv[]) {
    // 多屏 + 不同缩放时使用小数缩放，避免轮盘位置出现整数量化漂移。
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    // 应用身份统一走 Constants，不再在这里硬编码字符串
    QCoreApplication::setApplicationName(AppConstants::APP_NAME);

    // 图标只读一次：交给 QApplication 之后，设置窗口、动作页、各种对话框都会默认继承
    const QIcon appIcon(AppConstants::getMainIconPath());
    app.setWindowIcon(appIcon);

    // 关闭所有窗口时不要退出：轮盘和设置窗口都只是隐藏。
    app.setQuitOnLastWindowClosed(false);

    // 单实例限制，防止两个钩子互相冲突。
    QSharedMemory sharedMem(QStringLiteral("Huan_Key"));
    if (!sharedMem.create(1)) {
        QMessageBox::warning(nullptr, QStringLiteral("警告"),
                             QStringLiteral("程序已经在运行中，请勿重复打开！"));
        return 0;
    }

    // 数据库统一放在用户的 AppData 下（见 AppConstants::getDbPath）：装在 Program Files 时
    // 程序目录对普通用户只读，库放那里会直接建不出来。
    const QString dbPath = AppConstants::getDbPath();

    // 老版本把库放在 exe 旁边。这里做一次性搬运：新位置还没有库、而程序目录有库时，
    // 连主库带 WAL/SHM 一起复制过去。
    // 只复制、不删除原文件 —— 万一迁移中途出岔子，旧库还在原地，数据不会凭空消失。
    const QString oldDbPath = QCoreApplication::applicationDirPath() + QStringLiteral("/data.db");
    if (!QFile::exists(dbPath) && QFile::exists(oldDbPath)) {
        if (!QFile::copy(oldDbPath, dbPath)) {
            QMessageBox::critical(nullptr, QStringLiteral("数据迁移失败"),
                QStringLiteral("检测到旧版数据库：\n%1\n\n但没能复制到新位置：\n%2\n\n"
                               "请确认旧版程序已经退出后重新运行；"
                               "或手动把 data.db 复制到上面那个目录。")
                    .arg(QDir::toNativeSeparators(oldDbPath), QDir::toNativeSeparators(dbPath)));
            return -1;
        }
        // WAL 里可能还有没 checkpoint 的写入，一并带过去；带不过去最多丢最后一次写入
        const QString oldWal = oldDbPath + QStringLiteral("-wal");
        if (QFile::exists(oldWal)) QFile::copy(oldWal, dbPath + QStringLiteral("-wal"));
        const QString oldShm = oldDbPath + QStringLiteral("-shm");
        if (QFile::exists(oldShm)) QFile::copy(oldShm, dbPath + QStringLiteral("-shm"));
        qWarning() << "数据库已从" << oldDbPath << "迁移到" << dbPath;
    }

    if (!ActionRepository::init(dbPath)) {
        QMessageBox::critical(nullptr, QStringLiteral("数据库错误"),
                              QStringLiteral("无法初始化数据库：\n%1").arg(dbPath));
        return -1;
    }

    AppConfig::instance().reload();
    ActionEngine::instance();

    WheelWindow wheel;

    QSystemTrayIcon trayIcon;
    trayIcon.setIcon(appIcon); // appIcon 在 main 开头已创建，这里复用
    trayIcon.setToolTip(QStringLiteral("%1 运行中 (v%2)")
                            .arg(AppConstants::APP_NAME, AppConstants::APP_VERSION));

    QMenu trayMenu;
    trayMenu.setStyleSheet(QStringLiteral("QMenu { background-color: white; color: black; }"));

    QAction* settingsAction = trayMenu.addAction(QStringLiteral("打开设置"));
    trayMenu.addSeparator();
    QAction* quitAction = trayMenu.addAction(QStringLiteral("退出程序"));

    MainWindow mainWindow;
    ActionPage actionPage;
    wheel.setEditorWindow(reinterpret_cast<HWND>(mainWindow.winId()));

    // 开发/调试用：--settings 启动时直接打开配置窗口。
    if (app.arguments().contains(QStringLiteral("--settings"))) {
        mainWindow.show();
        mainWindow.raise();
        mainWindow.activateWindow();
    }

    if (app.arguments().contains(QStringLiteral("--preview-wheel"))) {
        QTimer::singleShot(600, &wheel, &WheelWindow::showPreview);
    }

    QObject::connect(settingsAction, &QAction::triggered, [&] {
        mainWindow.show();
        mainWindow.raise();
        mainWindow.activateWindow();
    });

    QObject::connect(quitAction, &QAction::triggered, [&] {
        sharedMem.detach();
        QApplication::quit();
    });

    QObject::connect(&mainWindow, &MainWindow::configSaved, [&] {
        wheel.reloadConfig();         // 这里会读取最新的 UI 配置并重新 resize
        wheel.update();               // 强制触发重绘，让颜色变化立刻显示
        wheel.refreshFromDatabase();
        actionPage.applyTheme(); // 新增：让动作页应用新皮肤
        actionPage.refresh();    // 新增：重新加载内容
    });

    // 动作页内新建页（或其它改动）后，通知主窗口刷新绑定预览与页数
    QObject::connect(&actionPage, &ActionPage::configSaved, [&] {
        mainWindow.refreshAll();
    });

        QObject::connect(&wheel, &WheelWindow::triggerActionPage, [&] {
        // 如果动作页当前已显示且处于展开状态，则按快捷键直接隐藏
        if (actionPage.isVisible() && !actionPage.isCollapsed()) {
            actionPage.hide();
        } else {
            QString scene = Win32Utils::getForegroundProcessName().toLower();
            actionPage.showAtCursor(scene);
        }
    });

    // 新增：呼出设置界面
    QObject::connect(&wheel, &WheelWindow::triggerSettingsWindow, [&] {
        mainWindow.show();
        mainWindow.raise();
        mainWindow.activateWindow();
    });

    // 新增：强制退出（先 detach 单实例锁，再直接结束进程）
    QObject::connect(&wheel, &WheelWindow::triggerForceQuit, [&] {
        qDebug().noquote() << QStringLiteral("触发强制退出热键，立即结束进程");
        sharedMem.detach(); // 释放单实例锁，避免下次启动提示"程序已经在运行中"
        ::ExitProcess(0);   // 硬退出：主线程或钩子死锁时也能立刻生效
    });

    QObject::connect(ActionEngine::instance(), &ActionEngine::actionFinished, [&](bool ok,
                                                                                 const QString& message) {
        if (!ok && trayIcon.isVisible()) {
            trayIcon.showMessage(QStringLiteral("动作执行失败"), message,
                                 QSystemTrayIcon::Warning, 4000);
        }
    });

    trayIcon.setContextMenu(&trayMenu);
    trayIcon.show();

    // 左键点击托盘图标时也弹出菜单（popup 非阻塞）。
    QObject::connect(&trayIcon, &QSystemTrayIcon::activated, [&](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            trayMenu.popup(QCursor::pos());
        }
    });

    HookManager hookManager;

    const bool mouseHookInstalled = hookManager.install(
        [&wheel](WPARAM wParam, MSLLHOOKSTRUCT* mouse) {
            return wheel.handleGlobalMouse(wParam, mouse);
        });

    const bool keyboardHookInstalled = hookManager.installKeyboard(
        [&wheel](WPARAM wParam, KBDLLHOOKSTRUCT* keyboard) {
            return wheel.handleGlobalKey(wParam, keyboard);
        });

    if (!mouseHookInstalled || !keyboardHookInstalled) {
        const QString message = QStringLiteral("全局钩子安装失败（鼠标:%1 键盘:%2）。"
                                               "某些安全软件可能会拦截低级钩子。")
                                    .arg(mouseHookInstalled ? QStringLiteral("成功") : QStringLiteral("失败"))
                                    .arg(keyboardHookInstalled ? QStringLiteral("成功") : QStringLiteral("失败"));
        qWarning().noquote() << message;
        trayIcon.showMessage(QStringLiteral("启动警告"), message, QSystemTrayIcon::Warning, 6000);
    }

    const int ret = app.exec();

    sharedMem.detach();
    return ret;
}
