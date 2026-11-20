#pragma once
#include <QString>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace AppConstants {
    // 显示名：窗口标题 / 托盘提示 / 气泡通知标题 / 安装包显示名。
    // 工程代号仍是 Huan（CMake 项目名、Huan.exe、单实例 key、data.db）。
    inline const QString APP_NAME = QStringLiteral("Huan");

    inline const QString APP_VERSION = QStringLiteral("1.1.2"); // 版本号：用于窗口标题 / 托盘提示 / 后续更新检查

    // 旧版用注册表 HKCU\...\Run 做自启，值名就是这个。现在自启改走任务计划，
    // 这里的名字只用于把旧残留清干净（写入端已经不再用它）。
    inline const QString AUTOSTART_KEY = QStringLiteral("Huan");

    // 任务计划里的任务名：taskschd.msc → 任务计划程序库 下能直接看到这一个。
    inline const QString AUTOSTART_TASK = QStringLiteral("HuanAutoStart");

    // 当前进程的 exe 文件名（原样大小写，如 "Huan.exe"）。
    // 代码里所有"这是不是我自己"的判断都走这里，别再硬编码 exe 名 ——
    // 项目改名时只要 project() 换了，这些判断自动跟着走，不会再漏。
    inline QString selfExeName() {
        return QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    }

    // data.db 放在用户的 AppData 下，不放在 exe 旁边。
    // 装到 C:\Program Files 后，程序目录对普通用户是只读的，库放那里会建不出来、程序直接起不来。
    // 依赖 main() 里 setApplicationName 已经调用过（AppDataLocation 由它拼出来），
    // 所以这里刻意不设 organizationName —— 多一层目录会让老用户的库看起来"丢了"。
    inline QString getDbPath() {
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dataDir.trimmed().isEmpty()) {
            // 极端情况下（系统没给 Roaming 路径）退到 Local，保证至少有个可写目录
            dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        }
        if (dataDir.trimmed().isEmpty()) {
            dataDir = QCoreApplication::applicationDirPath();
        }
        QDir().mkpath(dataDir);
        return dataDir + QStringLiteral("/data.db");
    }

    // resources 已按用途拆成子目录，路径统一从这里拼，业务代码里不要再手写 "/resources/..."。
    // 目录名带空格是刻意的（与资源管理器里的分类名一致），QIcon / QPixmap 都能正常处理，不用转义。
    inline QString getResourcePath(const QString& subDir, const QString& fileName) {
        return QCoreApplication::applicationDirPath() + QStringLiteral("/resources/") + subDir
               + QLatin1Char('/') + fileName;
    }

    // 主图标：托盘 / 主窗口 / 动作页折叠态 logo / exe 图标（app.rc）共用同一份素材
    inline QString getMainIconPath() {
        return getResourcePath(QStringLiteral("Logo and Main Icon"), QStringLiteral("main.png"));
    }

    // 界面控制图标：翻页 / 固定 / 折叠 / 关闭 / 新增 / 删除
    inline QString getUiIconPath(const QString& fileName) {
        return getResourcePath(QStringLiteral("UI control icons"), fileName);
    }

    // 动作类型图标：按键 / 程序 / 剪贴板 / 脚本 / 流水线 / 文本
    inline QString getActionTypeIconPath(const QString& fileName) {
        return getResourcePath(QStringLiteral("Action Type Icon"), fileName);
    }
}
