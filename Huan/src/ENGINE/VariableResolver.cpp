#include "VariableResolver.h"

#include <QRegularExpression>
#include <QUuid>
#include <QtGlobal>

namespace {

QString braceValue(const QString& key, const VariableContext& context) {
    const QString name = key.trimmed().toLower();

    if (name == QLatin1String("clipboard")) return context.clipboard;
    if (name == QLatin1String("selected") || name == QLatin1String("selection")) {
        return context.selection;
    }
    if (name == QLatin1String("scene") || name == QLatin1String("app") ||
        name == QLatin1String("process")) {
        return context.scene;
    }
    if (name == QLatin1String("date")) return context.now.toString(QStringLiteral("yyyy-MM-dd"));
    if (name == QLatin1String("time")) return context.now.toString(QStringLiteral("HH:mm:ss"));
    if (name == QLatin1String("datetime")) {
        return context.now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    if (name == QLatin1String("timestamp")) {
        return QString::number(context.now.toSecsSinceEpoch());
    }
    if (name == QLatin1String("uuid")) {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (name == QLatin1String("newline") || name == QLatin1String("nl")) {
        return QStringLiteral("\n");
    }
    if (name == QLatin1String("tab")) return QStringLiteral("\t");
    if (name.startsWith(QLatin1String("env:"))) {
        const QString envName = key.trimmed().mid(4);
        const QByteArray encoded = envName.toUtf8();
        if (qEnvironmentVariableIsSet(encoded.constData())) {
            return qEnvironmentVariable(encoded.constData());
        }
        return QStringLiteral("{%1}").arg(key);
    }

    return QStringLiteral("{%1}").arg(key); // 未知变量原样保留
}

} // namespace

QString VariableResolver::expand(const QString& input, const VariableContext& context) {
    QString result;
    result.reserve(input.size());

    int index = 0;
    while (index < input.size()) {
        const QChar ch = input.at(index);

        if (ch == QLatin1Char('{')) {
            const int close = input.indexOf(QLatin1Char('}'), index + 1);
            if (close > index) {
                const QString key = input.mid(index + 1, close - index - 1);
                // 只把“像变量名”的内容当变量，正文里的 json / 代码块保持原样。
                static const QRegularExpression namePattern(
                    QStringLiteral("^[A-Za-z_][A-Za-z0-9_:.-]*$"));
                if (namePattern.match(key).hasMatch()) {
                    result += braceValue(key, context);
                    index = close + 1;
                    continue;
                }
            }
            result += ch;
            ++index;
            continue;
        }

        if (ch == QLatin1Char('%')) {
            const int close = input.indexOf(QLatin1Char('%'), index + 1);
            if (close > index + 1) {
                const QString name = input.mid(index + 1, close - index - 1);
                if (!name.contains(QLatin1Char('%'))) {
                    const QByteArray encoded = name.toUtf8();
                    if (qEnvironmentVariableIsSet(encoded.constData())) {
                        result += qEnvironmentVariable(encoded.constData());
                        index = close + 1;
                        continue;
                    }
                }
            }
            result += ch;
            ++index;
            continue;
        }

        result += ch;
        ++index;
    }

    return result;
}
