#pragma once

#include <QDateTime>
#include <QString>

// 变量替换的上下文。所有字段在展开前由调用方填好。
struct VariableContext {
    QString scene;      // 当前场景，通常是前台进程名，例如 Code.exe
    QString selection;  // 当前选中的文字（可能为空）
    QString clipboard;  // 当前剪贴板文字（可能为空）
    QDateTime now = QDateTime::currentDateTime();
};

// 支持的动作参数变量：
//   {clipboard}            当前剪贴板
//   {selected} / {selection} 当前选中文字
//   {scene} / {app} / {process} 当前前台进程名
//   {date} {time} {datetime} {timestamp} {uuid}
//   {newline} {tab}
//   {env:NAME}             环境变量
//   %NAME%                 Windows 风格环境变量（兼容旧写法）
// 未知变量原样保留，避免误伤用户文本。
class VariableResolver {
public:
    static QString expand(const QString& input, const VariableContext& context);
};
