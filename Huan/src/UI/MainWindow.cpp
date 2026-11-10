#include "MainWindow.h"

#include "ActionEngine.h"
#include "ActionRepository.h"
#include "AppConfig.h"
#include "Constants.h"
#include "Win32Utils.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QCoreApplication>
#include <QCursor>
#include <QColorDialog>
#include <QCloseEvent>
#include <QDebug>
#include <QDialog>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QFrame>
#include <QScrollArea>
#include <algorithm>
#include <QSettings>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QKeySequence>
#include <QStandardPaths>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QToolTip>
#include <cmath>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QDrag>
#include <QApplication>
#include <QMouseEvent>


#include <functional>

namespace {

constexpr char kActionMime[] = "application/x-huan-action-id";
// 动作页格子间拖拽交换：携带源格子索引（与上面的 actionId 同时写入同一份 MimeData）
constexpr char kPageIndexMime[] = "application/x-huan-page-index";
// 轮盘内槽位拖拽交换：携带源槽位号
constexpr char kWheelSlotMime[] = "application/x-huan-wheel-slot";

QString actionTypeName(int type) {
    switch (type) {
    case 0: return QStringLiteral("按键组合");
    case 1: return QStringLiteral("运行程序/打开链接");
    case 2: return QStringLiteral("剪贴板转换");
    case 3: return QStringLiteral("外部脚本");
    case 4: return QStringLiteral("多步流水线");
    case 5: return QStringLiteral("输入文本");
    default: return QString();
    }
}

QString quotedPath(const QString& file) {
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(file));
}

bool isClipboardOp(const QString& text) {
    const QString operation = text.trimmed().toLower();
    return operation == QLatin1String("upper") || operation == QLatin1String("lower") ||
           operation == QLatin1String("title") || operation == QLatin1String("trim") ||
           operation == QLatin1String("reverse");
}

// 只接受从动作库拖来的数据。
class SlotTable : public QTableWidget {
public:
    explicit SlotTable(QWidget* parent = nullptr) : QTableWidget(parent) {}

    std::function<void(int slot, int actionId)> onActionDropped;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(kActionMime)) {
            event->acceptProposedAction();
            return;
        }
        QTableWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasFormat(kActionMime)) {
            event->acceptProposedAction();
            return;
        }
        QTableWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        if (!event->mimeData()->hasFormat(kActionMime)) {
            QTableWidget::dropEvent(event);
            return;
        }

        const int actionId = event->mimeData()->data(kActionMime).toInt();
        if (actionId <= 0) {
            event->ignore();
            return;
        }

        const int row = rowAt(event->position().toPoint().y());
        if (row >= 0 && row < rowCount() && onActionDropped) {
            onActionDropped(row, actionId);
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }
};

// 拖动时把动作 id 写进自定义 MIME。
class LibraryTable : public QTableWidget {
public:
    explicit LibraryTable(QWidget* parent = nullptr) : QTableWidget(parent) {}

protected:
    QMimeData* mimeData(const QList<QTableWidgetItem*>& items) const override {
        QMimeData* mime = QTableWidget::mimeData(items);
        if (!items.isEmpty()) {
            const int row = items.first()->row();
            if (QTableWidgetItem* nameItem = item(row, 0)) {
                const int actionId = nameItem->data(Qt::UserRole).toInt();
                mime->setData(kActionMime, QByteArray::number(actionId));
            }
        }
        return mime;
    }
};

} // namespace（临时关闭，让 KeyRecorderWidget 位于全局作用域，与 MainWindow.h 的前向声明一致）

class KeyRecorderWidget : public QWidget {
    Q_OBJECT
public:
    explicit KeyRecorderWidget(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        m_edit = new QLineEdit(this);
        m_edit->setPlaceholderText(QStringLiteral("输入按键组合或点击录制..."));
        
        m_recordBtn = new QPushButton(QStringLiteral("录制"), this);
        m_recordBtn->setCheckable(true);
        m_recordBtn->setStyleSheet(QStringLiteral(
            "QPushButton { padding: 4px 8px; } "
            "QPushButton:checked { background-color: #FFCDD2; color: red; font-weight: bold; }"
        ));
        layout->addWidget(m_edit, 1);
        layout->addWidget(m_recordBtn);
        // 安装事件过滤器到自身：grabKeyboard 后键盘事件送到本控件，先经过 eventFilter
        installEventFilter(this);
        // 点击录制按钮
        connect(m_recordBtn, &QPushButton::toggled, this, [this](bool checked) {
            if (checked) {
                m_edit->setReadOnly(true);
                m_edit->setPlaceholderText(
                    QStringLiteral("请按下组合键（Win 组合键可能被系统拦截，可手动输入）"));
                m_edit->setText(QString());
                m_lastMods.clear();
                m_pressedKeys.clear();
                m_recordBtn->setText(QStringLiteral("停止"));
                // 抓取键盘焦点，开始拦截
                this->grabKeyboard();
                this->setFocus();
            } else {
                stopRecording();
            }
        });
    }
    // === 新增：暴露当前是否处于录制状态 ===
    bool isRecording() const { return m_recordBtn->isChecked(); }
    
    // 暴露内部 QLineEdit，方便其他地方调用
    QLineEdit* lineEdit() const { return m_edit; }
    QString text() const { return m_edit->text(); }
    void setText(const QString& text) { m_edit->setText(text); }
    // 供调用方按动作类型提示（转发给内部输入框）
    void setPlaceholderText(const QString& text) { m_edit->setPlaceholderText(text); }
protected:
    // 拦截所有键盘事件，包括系统快捷键
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (!m_recordBtn->isChecked()) {
            return QWidget::eventFilter(watched, event);
        }
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
            int key = keyEvent->key();
            // 修饰键（Ctrl/Shift/Alt/Win）：只记录按下状态，绝不写进文本。
            // 主键按下时统一从 modifiers 里取修饰键，这样不会出现 "Win+Ctrl+Win+A" 的重复拼接
            if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta) {
                if (key == Qt::Key_Meta && !m_pressedKeys.contains(key)) {
                    QToolTip::showText(mapToGlobal(QPoint(0, height())),
                                       QStringLiteral("Win 组合键可能被 Windows 拦截（如 Win+D / Win+E）。\n"
                                                      "若录制无反应，请停止录制后手动输入。"),
                                       this, QRect(), 4000);
                }
                m_pressedKeys.insert(key);
                keyEvent->accept();
                return true;
            }
            // 如果这个键已经在按下集合里，说明是长按产生的系统重复事件，直接拦截丢弃
            if (m_pressedKeys.contains(key)) {
                keyEvent->accept();
                return true;
            }
            m_pressedKeys.insert(key); // 记录这个键已经按下了
            // === 组装当前这一次的组合键 ===
            QStringList parts;
            if (keyEvent->modifiers() & Qt::ControlModifier) parts << QStringLiteral("Ctrl");
            if (keyEvent->modifiers() & Qt::ShiftModifier) parts << QStringLiteral("Shift");
            if (keyEvent->modifiers() & Qt::AltModifier) parts << QStringLiteral("Alt");
            if (keyEvent->modifiers() & Qt::MetaModifier) parts << QStringLiteral("Win");
            QString keyName;
            if (key >= Qt::Key_A && key <= Qt::Key_Z) keyName = QChar(key);
            else if (key >= Qt::Key_0 && key <= Qt::Key_9) keyName = QChar(key);
            else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) keyName = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
            else if (key == Qt::Key_Space) keyName = QStringLiteral("Space");
            else if (key == Qt::Key_Return || key == Qt::Key_Enter) keyName = QStringLiteral("Enter");
            else if (key == Qt::Key_Escape) keyName = QStringLiteral("Esc");
            else if (key == Qt::Key_Plus) keyName = QStringLiteral("+");
            else {
                keyName = QKeySequence(key).toString(QKeySequence::PortableText);
                if (keyName.isEmpty() && keyEvent->nativeVirtualKey() != 0) {
                    keyName = QStringLiteral("VK_%1").arg(keyEvent->nativeVirtualKey());
                }
            }
            if (keyName.isEmpty()) {
                keyEvent->accept();
                return true;
            }
            parts << keyName;
            const QString comboStr = parts.join(QLatin1Char('+'));
            const QString modsStr = QStringList(parts.mid(0, parts.size() - 1)).join(QLatin1Char('+'));
            const QString currentText = m_edit->text();
            if (currentText.isEmpty()) {
                m_edit->setText(comboStr);
                m_lastMods = modsStr;
            } else if (!modsStr.isEmpty() && modsStr == m_lastMods) {
                // 同一组修饰键下继续按键：Ctrl+A → Ctrl+A+C
                m_edit->setText(currentText + QLatin1Char('+') + keyName);
            } else {
                m_edit->setText(currentText + QLatin1Char('+') + comboStr);
                m_lastMods = modsStr;
            }
            keyEvent->accept();
            return true; // 吞噬按下事件，防止发出声音或触发焦点切换
        } 
        else if (event->type() == QEvent::KeyRelease) {
            QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
            int key = keyEvent->key();
            // 修饰键松开且没有别的键按着 → 本次录制结束
            if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta) {
                m_pressedKeys.remove(key);
                if (m_pressedKeys.isEmpty()) {
                    m_recordBtn->setChecked(false); // 触发停止录制
                }
            } else {
                // 主键松开，从集合中移除，允许下次重新按下
                m_pressedKeys.remove(key);
            }
            keyEvent->accept();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (m_recordBtn->isChecked()) {
            // 关键：拦截 Alt+F4 等窗口关闭事件
            if (event->key() == Qt::Key_F4 && (event->modifiers() & Qt::AltModifier)) {
                event->accept();
                return;
            }
        }
        QWidget::keyPressEvent(event);
    }
private:
    void stopRecording() {
        m_edit->setReadOnly(false);
        m_edit->setPlaceholderText(QStringLiteral("输入按键组合或点击录制..."));
        m_recordBtn->setText(QStringLiteral("录制"));
        m_pressedKeys.clear(); // 关键：每次停止录制必须清空状态
        m_lastMods.clear();
        this->releaseKeyboard();
    }
    QLineEdit* m_edit = nullptr;
    QPushButton* m_recordBtn = nullptr;
    QSet<int> m_pressedKeys; // 新增：记录当前按下的主键，用于防抖
    QString m_lastMods; // 上一段录制使用的修饰键组合（Ctrl+Shift 等），用于判断修饰键是否变化
};

namespace {

class ActionEditDialog : public QDialog {
public:
    ActionEditDialog(QWidget* parent, const LibraryAction& initial, bool isNew)
        : QDialog(parent), m_action(initial) {
        setWindowTitle(isNew ? QStringLiteral("新增动作") : QStringLiteral("编辑动作"));
        resize(540, 230);

        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout();

        m_name = new QLineEdit(initial.name, this);
        m_name->setPlaceholderText(QStringLiteral("显示在轮盘/动作页上的名字"));
        form->addRow(QStringLiteral("名字:"), m_name);

        m_type = new QComboBox(this);
        m_type->addItem(QStringLiteral("按键组合"), 0);
        m_type->addItem(QStringLiteral("运行程序/打开链接"), 1);
        m_type->addItem(QStringLiteral("剪贴板转换"), 2);
        m_type->addItem(QStringLiteral("外部脚本"), 3);
        m_type->addItem(QStringLiteral("多步流水线"), 4);
        m_type->addItem(QStringLiteral("输入文本"), 5);
        form->addRow(QStringLiteral("类型:"), m_type);

        auto* payloadRow = new QHBoxLayout();
        m_payload = new KeyRecorderWidget(this);
        m_payload->setText(initial.payload);
        m_browse = new QToolButton(this);
        m_browse->setText(QStringLiteral("..."));
        m_browse->setFixedWidth(26);
        payloadRow->addWidget(m_payload, 1);
        payloadRow->addWidget(m_browse);
        form->addRow(QStringLiteral("参数:"), payloadRow);
        // 固定说明：系统级快捷键录不到时，用户知道这是系统限制而不是软件出问题
        auto* hotkeyHint = new QLabel(
            QStringLiteral("提示：部分系统级快捷键（如 Win 键开头的组合、Alt+F4）无法直接录制，"
                           "请停止录制后在输入框中手动输入。"),
            this);
        hotkeyHint->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px;"));
        hotkeyHint->setWordWrap(true);
        form->addRow(hotkeyHint);

        layout->addLayout(form);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, [this] { tryAccept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_type, &QComboBox::currentIndexChanged, this, [this](int) { updatePlaceholder(); });
        connect(m_browse, &QToolButton::clicked, this, [this] { browse(); });

        const int index = m_type->findData(initial.type >= 0 ? initial.type : 0);
        m_type->setCurrentIndex(index >= 0 ? index : 0);
        updatePlaceholder();
    }

    LibraryAction result() const {
        LibraryAction action = m_action;
        action.name = m_name->text().trimmed();
        action.type = m_type->currentData().toInt();
        action.payload = m_payload->text().trimmed();
        return action;
    }

protected:
    // === 新增：拦截窗口关闭事件 ===
    bool event(QEvent* e) override {
        // 如果当前正在录制，且系统尝试关闭窗口（例如按下 Alt+F4）
        if (m_payload && m_payload->isRecording() && e->type() == QEvent::Close) {
            e->ignore(); // 忽略关闭请求
            return true; // 拦截事件，不让它继续传递
        }
        return QDialog::event(e);
    }
    // ==========================
    void closeEvent(QCloseEvent* event) override {
        // 双保险：重写 closeEvent
        if (m_payload && m_payload->isRecording()) {
            event->ignore();
            return;
        }
        QDialog::closeEvent(event);
    }

private:
    void updatePlaceholder() {
        switch (m_type->currentData().toInt()) {
        case 0:
            m_payload->setPlaceholderText(QStringLiteral("例如 Ctrl+Shift+Esc"));
            break;
        case 1:
            m_payload->setPlaceholderText(QStringLiteral("例如 C:\\Windows\\notepad.exe 或 https://..."));
            break;
        case 2:
            m_payload->setPlaceholderText(QStringLiteral("upper / lower / title / trim / reverse"));
            break;
        case 3:
            m_payload->setPlaceholderText(QStringLiteral("例如 python \"C:\\scripts\\x.py\""));
            break;
        case 4:
            m_payload->setPlaceholderText(QStringLiteral("JSON 数组，点 ... 编辑"));
            break;
        case 5:
            m_payload->setPlaceholderText(QStringLiteral("要输入的文本，支持变量"));
            break;
        default:
            break;
        }
    }

    void browse() {
        const int type = m_type->currentData().toInt();
        if (type == 1) {
            const QString file = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择程序"), QString(),
                QStringLiteral("可执行文件 (*.exe);;所有文件 (*.*)"));
            if (!file.isEmpty()) m_payload->setText(quotedPath(file));
            return;
        }
        if (type == 3) {
            const QString file = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择脚本"), QString(),
                QStringLiteral("脚本文件 (*.py *.js *.mjs *.ps1 *.bat *.cmd *.vbs *.exe);;所有文件 (*.*)"));
            if (!file.isEmpty()) m_payload->setText(quotedPath(file));
            return;
        }
        if (type == 4) {
            bool ok = false;
            const QString text = QInputDialog::getMultiLineText(
                this, QStringLiteral("多步流水线"),
                QStringLiteral("JSON 数组。示例：\n"
                               "[{\"type\":0,\"payload\":\"Ctrl+C\"},{\"wait\":120},"
                               "{\"type\":5,\"payload\":\"{clipboard}\"}]\n\n"
                               "步骤类型：key/launch/clipboard/script/pipeline/text，"
                               "也可以用数字 0-5。wait 表示等待毫秒数。"),
                m_payload->text(), &ok);
            if (!ok) return;

            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError) {
                QMessageBox::warning(this, QStringLiteral("JSON 格式错误"),
                                     QStringLiteral("%1（位置 %2）")
                                         .arg(error.errorString())
                                         .arg(error.offset));
                return;
            }
            m_payload->setText(QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
            return;
        }
        if (type == 2) {
            QMessageBox::information(
                this, QStringLiteral("剪贴板转换"),
                QStringLiteral("参数只填操作名：\n\nupper 转大写\nlower 转小写\n"
                               "title 首字母大写\ntrim 去掉首尾空白\nreverse 反转文字"));
            return;
        }
        if (type == 5) {
            QMessageBox::information(
                this, QStringLiteral("输入文本"),
                QStringLiteral("会以 Unicode 方式逐字输入，不受输入法影响。\n\n"
                               "可以使用变量：{clipboard} {selected} {scene} {date} {time} {uuid} "
                               "{newline} {env:NAME}"));
            return;
        }

        QMessageBox::information(
            this, QStringLiteral("按键组合"),
            QStringLiteral("支持：Ctrl / Shift / Alt / Win 与任意主键组合。\n\n"
                           "示例：\nCtrl+Shift+Esc\nAlt+F4\nWin+Shift+S\nMediaPlayPause\n"
                           "Ctrl+Left\nNumpadEnter\nCtrl++（Ctrl 加加号）\n\n"
                           "注意：Ctrl+Alt+Del 是系统安全序列，无法模拟。"));
    }

    void tryAccept() {
        const LibraryAction action = result();
        if (action.name.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("名字为空"),
                                 QStringLiteral("请给动作起一个名字。"));
            return;
        }

        if (action.type == 0) {
            const KeyChord chord = Win32Utils::parseKeySequence(action.payload);
            if (!chord.valid) {
                QMessageBox::warning(this, QStringLiteral("按键无效"), chord.error);
                return;
            }
        } else if (action.type == 1 || action.type == 3 || action.type == 5) {
            if (action.payload.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("参数为空"),
                                     QStringLiteral("这个动作类型需要填写参数。"));
                return;
            }
        } else if (action.type == 2) {
            if (!isClipboardOp(action.payload)) {
                QMessageBox::warning(this, QStringLiteral("剪贴板操作无效"),
                                     QStringLiteral("只能填 upper / lower / title / trim / reverse。"));
                return;
            }
        } else if (action.type == 4) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(action.payload.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError ||
                !(document.isArray() || document.isObject())) {
                QMessageBox::warning(this, QStringLiteral("流水线 JSON 无效"), error.errorString());
                return;
            }
        }

        accept();
    }

    LibraryAction m_action;
    QLineEdit* m_name = nullptr;
    QComboBox* m_type = nullptr;
    KeyRecorderWidget* m_payload = nullptr;
    QToolButton* m_browse = nullptr;};

// 支持拖拽和双击的网格按钮
class SceneActionButton : public QPushButton {
public:
    SceneActionButton(int actionId, int index, QWidget* parent = nullptr)
        : QPushButton(parent), m_actionId(actionId), m_index(index) {
        setAcceptDrops(true);
    }
    int actionId() const { return m_actionId; }

    std::function<void()> onDoubleClicked;
    std::function<void(int srcIndex, int dstIndex)> onActionSwapped; // 页内拖拽交换

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragStartPosition = event->pos();
        }
        QPushButton::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton)) return;
        if ((event->pos() - m_dragStartPosition).manhattanLength() < QApplication::startDragDistance()) return;

        if (m_actionId > 0) {
            QDrag* drag = new QDrag(this);
            QMimeData* mimeData = new QMimeData;
            // 同一份数据带两个 mime：拖到轮盘用 actionId，拖到其它格子用索引交换
            mimeData->setData(kActionMime, QByteArray::number(m_actionId));
            mimeData->setData(kPageIndexMime, QByteArray::number(m_index));
            drag->setMimeData(mimeData);
            drag->exec(Qt::CopyAction | Qt::MoveAction);
        }
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(kPageIndexMime)) event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasFormat(kPageIndexMime)) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        if (!event->mimeData()->hasFormat(kPageIndexMime)) return;
        const int srcIndex = event->mimeData()->data(kPageIndexMime).toInt();
        if (srcIndex >= 0 && srcIndex != m_index && onActionSwapped) {
            onActionSwapped(srcIndex, m_index);
        }
        event->acceptProposedAction();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onDoubleClicked) {
            onDoubleClicked();
        }
        QPushButton::mouseDoubleClickEvent(event);
    }

private:
    int m_actionId;
    int m_index = -1;
    QPoint m_dragStartPosition;
};

} // namespace


// 可视化轮盘配置组件
class WheelEditorWidget : public QWidget {
public:
    explicit WheelEditorWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAcceptDrops(true);
        setMinimumSize(360, 360);
        setFocusPolicy(Qt::ClickFocus); // 点击后获得焦点，才能接收 Delete
    }
    void setActions(const QVector<Action>& actions) {
        m_actions = actions;
        update();
    }
    void setScene(const QString& scene) { m_scene = scene; }
    void setSelectedSlot(int slot) {
        if (m_selectedSlot != slot) {
            m_selectedSlot = slot;
            update();
        }
    }
    int selectedSlot() const { return m_selectedSlot; }
    void setRings(int rings) {
        if (m_rings != rings) {
            m_rings = rings;
            update();
        }
    }
    // 外圈隐藏动作开关（与设置界面的 Checkbox 同步）
    void setHiddenRingEnabled(bool enabled) {
        if (m_hiddenRingEnabled != enabled) {
            m_hiddenRingEnabled = enabled;
            update();
        }
    }
    // 只要 56~63 里已有绑定，即使开关没勾也画出来，避免"配了却看不到"
    bool hasHiddenBindings() const {
        for (int slot = 56; slot < 64; ++slot) {
            if (slot < m_actions.size() && m_actions[slot].type >= -1) return true;
        }
        return false;
    }
    bool hiddenRingVisible() const { return m_hiddenRingEnabled || hasHiddenBindings(); }
    // 场景内显式覆盖（非继承）的槽位集合：用于预览区分"专属"与"继承"
    // 注意：参数名不能写成那个表示"槽"的 Qt 关键字（moc 会把它当成访问修饰符，
    // 报 "Missing access specifier for ..."）
    void setOverriddenSlots(const QSet<int>& slotSet) {
        if (m_overriddenSlots != slotSet) {
            m_overriddenSlots = slotSet;
            update();
        }
    }
    // 供外部右键菜单按坐标定位槽位（hitTest 是 private，不能直接调）
    int slotAt(const QPoint& pos) { return hitTest(pos); }
    std::function<void(int slot, int actionId)> onActionDropped;
    std::function<void(int slot)> onSlotClicked;
    std::function<void(int srcSlot, int dstSlot)> onSlotSwapped; // 轮盘内拖拽交换
    std::function<void(int slot)> onDeleteRequested;             // Delete 键清空槽位
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Delete && m_selectedSlot >= 0) {
            if (onDeleteRequested) onDeleteRequested(m_selectedSlot);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        int cx = width() / 2;
        int cy = height() / 2;
        // 轮盘主体尺寸不再因隐藏环而缩小；外圈只在最外侧画一圈很窄的提示区
        int outerRadius = qMin(width(), height()) / 2 - 30;
        int innerRadius = 60;
        qreal ringThickness = static_cast<qreal>(outerRadius - innerRadius) / m_rings;
        for (int ring = 0; ring < m_rings; ++ring) {
            int sectorCount = 8 << ring;
            qreal degreesPerSector = 360.0 / sectorCount;
            qreal rIn = innerRadius + ring * ringThickness;
            qreal rOut = rIn + ringThickness;
            qreal textRadius = rIn + ringThickness / 2.0;
            int baseSlot = (ring == 0) ? 0 : (ring == 1 ? 8 : 24);
            for (int i = 0; i < sectorCount; ++i) {
                int slot = baseSlot + i;
                
                // 绘制底盘
                QPainterPath path;
                QRectF rectOuter(cx - rOut, cy - rOut, rOut * 2, rOut * 2);
                QRectF rectInner(cx - rIn, cy - rIn, rIn * 2, rIn * 2);
                
                // 修复：由 -90 改为 90.0 + 22.5 - ...（整体旋转 22.5°，让扇区中心对准正上方）
                qreal startAngle = 90.0 + 22.5 - i * degreesPerSector;
                
                path.arcMoveTo(rectOuter, startAngle);
                path.arcTo(rectOuter, startAngle, -degreesPerSector);
                path.arcTo(rectInner, startAngle - degreesPerSector, degreesPerSector);
                path.closeSubpath();
                QColor bgColor = QColor("#FFFFFF");
                if (m_selectedSlot == slot) bgColor = QColor("#BBDEFB");
                
                painter.setBrush(bgColor);
                if (m_selectedSlot == slot) {
                    painter.setPen(QPen(QColor("#1976D2"), 2));
                } else if (m_hoveredSlot == slot) {
                    painter.setPen(QPen(QColor("#90CAF9"), 1));
                } else {
                    painter.setPen(QPen(QColor("#ECECEC"), 1));
                }
                painter.drawPath(path);
                // 文字
                QString name = "";
                if (slot < m_actions.size()) {
                    if (m_actions[slot].type >= 0) name = m_actions[slot].name;
                    else if (m_actions[slot].type == -1) name = "空白（不继承）";
                }
                if (name.isEmpty()) continue;
                qreal textAngle = (i * degreesPerSector - 90.0 - 22.5 + degreesPerSector / 2.0) * M_PI / 180.0;
                int tx = cx + static_cast<int>(textRadius * std::cos(textAngle));
                int ty = cy + static_cast<int>(textRadius * std::sin(textAngle));
                QFont font(QStringLiteral("Microsoft YaHei"));
                int pointSize = (ring == 0) ? 10 : (ring == 1 ? 9 : 8);
                font.setPointSize(pointSize);
                // 非 global 场景：本场景独立动作加粗蓝色，继承 global 的灰色
                const bool overridden = m_overriddenSlots.contains(slot);
                const bool inheritedScene = !m_scene.isEmpty() && m_scene != QLatin1String("global");
                if (overridden && inheritedScene) font.setBold(true);
                QFontMetrics metrics(font);

                int maxTextWidth = static_cast<int>(2.0 * textRadius * std::sin(M_PI / sectorCount)) - 6;
                const QString shown = metrics.elidedText(name, Qt::ElideRight, maxTextWidth);
                int textHeight = metrics.height();
                painter.setFont(font);
                QColor nameColor(QStringLiteral("#333333"));
                if (m_hoveredSlot == slot) {
                    nameColor = QColor(QStringLiteral("#000000"));
                } else if (inheritedScene) {
                    nameColor = overridden ? QColor(QStringLiteral("#1565C0"))
                                           : QColor(QStringLiteral("#9CA3AF"));
                }
                painter.setPen(nameColor);
                painter.drawText(QRect(tx - maxTextWidth / 2, ty - textHeight / 2, maxTextWidth, textHeight),
                                 Qt::AlignCenter, shown);
            }
        }
        // ===== 外圈隐藏动作（槽位 56~63）：厚度随可用空间自适应，底色固定浅灰 =====
        if (hiddenRingVisible()) {
            const qreal hiddenDegrees = 360.0 / 8.0;
            const qreal maxExtent = qMin(width(), height()) / 2.0 - 8.0; // 距控件边缘保留 8px
            const qreal hiddenThickness = qBound(14.0, maxExtent - outerRadius, 50.0);
            const qreal hiddenOuter = outerRadius + hiddenThickness;
            const qreal hiddenTextRadius = outerRadius + hiddenThickness / 2.0;
            for (int i = 0; i < 8; ++i) {
                const int slot = 56 + i;
                bool bound = false;
                if (slot < m_actions.size()) bound = m_actions[slot].type >= -1;

                const qreal startAngle = 90.0 + 22.5 - i * hiddenDegrees;
                QPainterPath path;
                QRectF rectOuter(cx - hiddenOuter, cy - hiddenOuter, hiddenOuter * 2, hiddenOuter * 2);
                QRectF rectInner(cx - outerRadius, cy - outerRadius, outerRadius * 2, outerRadius * 2);
                path.arcMoveTo(rectOuter, startAngle);
                path.arcTo(rectOuter, startAngle, -hiddenDegrees);
                path.arcTo(rectInner, startAngle - hiddenDegrees, hiddenDegrees);
                path.closeSubpath();

                QColor bgColor = bound ? QColor(240, 240, 240) : QColor(250, 250, 250);
                if (m_selectedSlot == slot) {
                    bgColor = QColor("#BBDEFB");
                } else if (m_hoveredSlot == slot) {
                    bgColor = QColor("#E3F2FD");
                }
                painter.setBrush(bgColor);
                if (m_selectedSlot == slot) {
                    painter.setPen(QPen(QColor("#1976D2"), 2));
                } else if (m_hoveredSlot == slot) {
                    painter.setPen(QPen(QColor("#90CAF9"), 1));
                } else {
                    painter.setPen(QPen(QColor("#E0E0E0"), 1, Qt::DashLine));
                }
                painter.drawPath(path);

                // 环宽太小（<16px）才放弃文字，正常 22px 预览环足够显示一行 8pt 名字
                QString name;
                if (bound && slot < m_actions.size()) name = m_actions[slot].name;
                if (name.isEmpty() || hiddenThickness < 16.0) continue;

                const qreal textAngle =
                    (i * hiddenDegrees - 90.0 - 22.5 + hiddenDegrees / 2.0) * M_PI / 180.0;
                const int tx = cx + static_cast<int>(hiddenTextRadius * std::cos(textAngle));
                const int ty = cy + static_cast<int>(hiddenTextRadius * std::sin(textAngle));
                QFont font(QStringLiteral("Microsoft YaHei"));
                font.setPointSize(8);
                const bool overriddenSlot = m_overriddenSlots.contains(slot);
                const bool inheritedScene =
                    !m_scene.isEmpty() && m_scene != QLatin1String("global");
                if (overriddenSlot && inheritedScene) font.setBold(true);
                QFontMetrics metrics(font);
                const int maxTextWidth =
                    static_cast<int>(2.0 * hiddenTextRadius * std::sin(M_PI / 8.0)) - 6;
                painter.setFont(font);
                QColor hiddenNameColor(QStringLiteral("#333333"));
                if (inheritedScene) {
                    hiddenNameColor = overriddenSlot ? QColor(QStringLiteral("#1565C0"))
                                                     : QColor(QStringLiteral("#9CA3AF"));
                }
                painter.setPen(hiddenNameColor);
                painter.drawText(QRect(tx - maxTextWidth / 2, ty - metrics.height() / 2,
                                       maxTextWidth, metrics.height()),
                                 Qt::AlignCenter, metrics.elidedText(name, Qt::ElideRight, maxTextWidth));
            }
        }
        // 中心圆
        QPen centerPen(QColor("#ECECEC"));
        centerPen.setWidth(1);
        painter.setPen(centerPen);
        painter.setBrush(QColor("#FFFFFF"));
        painter.drawEllipse(QPointF(cx, cy), innerRadius, innerRadius); // 带边框的中心圆
        
        // 下面接着写原有的文字提示代码
        painter.setPen(QColor("#9CA3AF"));
        QFont hintFont(QStringLiteral("Microsoft YaHei"));
        hintFont.setPointSize(8);
        painter.setFont(hintFont);
        painter.drawText(QRect(cx - 46, cy - 20, 92, 40), Qt::AlignCenter, QStringLiteral("拖入动作"));
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(kActionMime) ||
            event->mimeData()->hasFormat(kWheelSlotMime)) {
            event->acceptProposedAction();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasFormat(kActionMime) ||
            event->mimeData()->hasFormat(kWheelSlotMime)) {
            event->acceptProposedAction();
        }
    }
    void dropEvent(QDropEvent* event) override {
        const int slot = hitTest(event->position().toPoint());
        if (slot < 0) {
            event->ignore();
            return;
        }
        // 1. 从动作库拖过来的（绑定）
        if (event->mimeData()->hasFormat(kActionMime)) {
            const int actionId = event->mimeData()->data(kActionMime).toInt();
            if (onActionDropped) onActionDropped(slot, actionId);
            event->acceptProposedAction();
            return;
        }
        // 2. 轮盘内槽位互换（含隐藏环 56~63）
        if (event->mimeData()->hasFormat(kWheelSlotMime)) {
            const int srcSlot = event->mimeData()->data(kWheelSlotMime).toInt();
            if (srcSlot == slot) {
                event->ignore();
                return;
            }
            if (onSlotSwapped) onSlotSwapped(srcSlot, slot);
            event->acceptProposedAction();
            return;
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        int slot = hitTest(event->pos());
        if (slot != m_hoveredSlot) {
            m_hoveredSlot = slot;
            update();
        }
        // 按住左键且起点槽位有绑定 → 发起"槽位交换"拖拽
        if (!(event->buttons() & Qt::LeftButton)) return;
        if (m_dragSlot < 0) return;
        if ((event->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance()) return;

        const int startSlot = m_dragSlot;
        m_dragSlot = -1; // 避免拖拽过程中重复触发
        if (startSlot >= m_actions.size()) return;
        if (m_actions[startSlot].type < -1) return; // -2 表示该槽位没有绑定

        QDrag* drag = new QDrag(this);
        QMimeData* mimeData = new QMimeData;
        mimeData->setData(kWheelSlotMime, QByteArray::number(startSlot));
        drag->setMimeData(mimeData);
        drag->exec(Qt::MoveAction);
        update();
    }
    void mousePressEvent(QMouseEvent* event) override {
        setFocus(Qt::MouseFocusReason); // 不调用基类实现，焦点要自己给
        int slot = hitTest(event->pos());
        if (slot >= 0) {
            setSelectedSlot(slot);
            if (onSlotClicked) onSlotClicked(slot);
            if (event->button() == Qt::LeftButton) {
                m_dragStartPos = event->pos();
                m_dragSlot = slot;
            }
        } else {
            m_dragSlot = -1;
        }
    }
private:
    int hitTest(const QPoint& pos) {
        int cx = width() / 2;
        int cy = height() / 2;
        // 必须与 paintEvent 的半径计算完全一致，否则会出现"光标漂移"
        int outerRadius = qMin(width(), height()) / 2 - 30;
        int innerRadius = 60;
        qreal ringThickness = static_cast<qreal>(outerRadius - innerRadius) / m_rings;
        double dx = pos.x() - cx;
        double dy = pos.y() - cy;
        double dist = std::sqrt(dx * dx + dy * dy);

        // 隐藏环：只要超出外圈即可命中（与呼出轮盘一致，不限距离）
        if (hiddenRingVisible() && dist >= outerRadius) {
            const qreal hiddenDegrees = 360.0 / 8.0;
            double angle = std::atan2(dy, dx) * 180.0 / M_PI;
            angle += 90.0 + 22.5;
            if (angle < 0) angle += 360;
            if (angle >= 360) angle -= 360;
            int sector = static_cast<int>(angle / hiddenDegrees);
            return 56 + sector;
        }

        if (dist < innerRadius || dist > outerRadius) return -1;
        int ring = static_cast<int>((dist - innerRadius) / ringThickness);
        if (ring >= m_rings) ring = m_rings - 1;
        int sectorCount = 8 << ring;
        qreal degreesPerSector = 360.0 / sectorCount;
        
        double angle = std::atan2(dy, dx) * 180.0 / M_PI;
        // 整体旋转 22.5 度：让扇区中心对准正上方
        angle += 90.0 + 22.5;
        if (angle < 0) angle += 360;
        if (angle >= 360) angle -= 360;
        int sector = static_cast<int>(angle / degreesPerSector);
        int baseSlot = (ring == 0) ? 0 : (ring == 1 ? 8 : 24);
        return baseSlot + sector;
    }
    QVector<Action> m_actions;
    QString m_scene;
    int m_hoveredSlot = -1;
    int m_selectedSlot = -1;
    int m_rings = 1;
    bool m_hiddenRingEnabled = false;
    QSet<int> m_overriddenSlots; // 本场景独立绑定的槽位（非继承）
    QPoint m_dragStartPos;
    int m_dragSlot = -1; // 按下左键时的槽位（仅当该槽位有绑定时才允许起拖）
};

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("%1 - 设置 (v%2)")
                       .arg(AppConstants::APP_NAME, AppConstants::APP_VERSION));
    // 显式再设一次：QApplication 的全局图标万一被覆盖，设置窗口仍有自己的
    setWindowIcon(QIcon(AppConstants::getMainIconPath()));

    // 理想 1500x900；屏幕装不下时按可用区域 90% 收缩并居中
    resize(1500, 900);
    setMinimumSize(1000, 600);
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry(); // 已扣除任务栏
        if (avail.width() < 1520 || avail.height() < 920) {
            resize(qMin(1500, static_cast<int>(avail.width() * 0.9)),
                   qMin(900, static_cast<int>(avail.height() * 0.9)));
        }
        // 居中（用 avail.x()/y() 作为基准，任务栏在左/上时也不会偏）
        move(avail.x() + (avail.width() - width()) / 2,
             avail.y() + (avail.height() - height()) / 2);
    }
    setStyleSheet(QStringLiteral(
        "QWidget { background-color: #f5f5f5; }"
        "QTableWidget, QLineEdit, QComboBox, QSpinBox { background-color: white; }"
        "QGroupBox { font-weight: bold; border: 1px solid #d0d0d0; border-radius: 6px; "
        "margin-top: 10px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"));

    // === VS Code 风格：左侧垂直导航 + 右侧页面栈 ===
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_navList = new QListWidget(this);
    // 6 个中文字在 14px 下约 84px，加上 padding 12 与左侧 3px 指示条，112 才不会被省略号截断
    m_navList->setFixedWidth(112);
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // border-left 在未选中项也保留透明色，避免选中时文字右跳 3px
    m_navList->setStyleSheet(QStringLiteral(
        "QListWidget { background-color: #F3F3F3; color: #333333; font-size: 14px; "
        "border: none; border-right: 1px solid #E0E0E0; outline: 0; }"
        "QListWidget::item { min-height: 50px; padding-left: 20px; "
        "border-left: 3px solid transparent; }"
        "QListWidget::item:hover { background-color: #EAEAEA; }"
        "QListWidget::item:selected { background-color: #E8F0FE; color: #1976D2; "
        "border-left: 3px solid #1976D2; }"));
    m_navList->addItem(QStringLiteral("场景"));
    m_navList->addItem(QStringLiteral("轮盘"));
    m_navList->addItem(QStringLiteral("高级鼠标触发"));
    m_navList->addItem(QStringLiteral("动作页"));
    m_navList->addItem(QStringLiteral("排行"));
    m_navList->addItem(QStringLiteral("系统"));
    mainLayout->addWidget(m_navList);

    auto* rightColumn = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    m_stackedWidget = new QStackedWidget(rightColumn);
    m_stackedWidget->addWidget(buildSceneTab());
    m_stackedWidget->addWidget(buildWheelTab());
    m_stackedWidget->addWidget(buildMouseTriggerTab());
    m_stackedWidget->addWidget(buildActionPageTab());
    // 顺序必须与左侧 addItem 一一对应：场景/轮盘/高级鼠标触发/动作页/排行/系统
    rankingPage = buildRankingTab();
    m_stackedWidget->addWidget(rankingPage);
    m_stackedWidget->addWidget(buildSystemTab());
    rightLayout->addWidget(m_stackedWidget, 1);

    // 保存栏固定在内容区底部：五个页面共用，不必为了保存再切回「场景」页
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(10, 6, 10, 8);
    buttonLayout->addStretch();
    saveBtn = new QPushButton(QStringLiteral("保存设置"), rightColumn);
    saveBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #0078D7; color: white; padding: 8px 20px; "
        "border-radius: 4px; font-weight: bold; }"
        "QPushButton:hover { background-color: #006ABC; }"));
    buttonLayout->addWidget(saveBtn);
    rightLayout->addLayout(buttonLayout);

    mainLayout->addWidget(rightColumn, 1);

    connect(m_navList, &QListWidget::currentRowChanged,
            m_stackedWidget, &QStackedWidget::setCurrentIndex);
    // 轮盘预览与动作页网格现在分在不同页，切页时各自从数据库重读一次
    connect(m_stackedWidget, &QStackedWidget::currentChanged, this, [this](int) {
        loadData();
        refreshActionGrid();
        // 排行页不常驻刷新：每次切进来重算一次，保证次数是最新的
        if (m_stackedWidget->currentWidget() == rankingPage) {
            refreshRanking();
        }
    });
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveData);

    m_navList->setCurrentRow(0); // 默认停在「场景」

    // 先加载配置（内部会 AppConfig::reload），再刷数据：
    // 否则首次 loadData → refreshActionGrid 会拿到 reload 之前的旧外观配置
    loadTriggerSettings();
    loadScenes();
    loadData();
    loadMouseBindings();
    refreshRanking(); // 先填一次，避免第一次切到「排行」页时是一片空白

    // 自动备份：所有配置改动最终都会 emit configSaved，这里用 2 秒防抖收敛成一次写盘。
    // 拖拽一次会连发几十个 configSaved，不防抖会对着磁盘狂写。
    m_backupTimer = new QTimer(this);
    m_backupTimer->setSingleShot(true);
    m_backupTimer->setInterval(2000);
    connect(m_backupTimer, &QTimer::timeout, this, &MainWindow::triggerAutoBackup);
    connect(this, &MainWindow::configSaved, this, [this]() {
        if (autoBackupCheck && autoBackupCheck->isChecked()) {
            m_backupTimer->start(); // 每次都重新计时：等用户停手
        }
    });
}



void MainWindow::refreshAll() {
    refreshLibrary();
    loadData();
}

QWidget* MainWindow::buildWheelTab() {
    auto* page = new QWidget(this);
    // 内容整体靠左 + 限制最大宽度：超宽屏下不会把表单拉成一整条
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    content->setMaximumWidth(800);
    outer->addWidget(content);
    outer->addStretch();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);

    // ---- 第一组：轮盘触发（纵向排列） ----
    auto* trigGroup = new QGroupBox(QStringLiteral("轮盘触发"), page);
    auto* trigForm = new QFormLayout(trigGroup);

    mouseTriggerCombo = new QComboBox(trigGroup);
    mouseTriggerCombo->addItem(QStringLiteral("右键拖拽呼出（默认）"), QStringLiteral("right"));
    mouseTriggerCombo->addItem(QStringLiteral("中键拖拽呼出"), QStringLiteral("middle"));
    mouseTriggerCombo->addItem(QStringLiteral("侧键1 (X1) 拖拽呼出"), QStringLiteral("x1"));
    mouseTriggerCombo->addItem(QStringLiteral("侧键2 (X2) 拖拽呼出"), QStringLiteral("x2"));
    mouseTriggerCombo->addItem(QStringLiteral("不启用鼠标触发"), QStringLiteral("none"));
    trigForm->addRow(QStringLiteral("鼠标触发键:"), mouseTriggerCombo);

    wheelHotkeyEdit = new KeyRecorderWidget(trigGroup);
    wheelHotkeyEdit->setToolTip(
        QStringLiteral("按住热键显示轮盘，移动鼠标选择，松开热键执行；Esc 取消。"));
    auto* hotkeyRow = new QHBoxLayout();
    hotkeyRow->addWidget(wheelHotkeyEdit);
    auto* clearHotkeyButton = new QPushButton(QStringLiteral("清除"), trigGroup);
    hotkeyRow->addWidget(clearHotkeyButton);
    trigForm->addRow(QStringLiteral("轮盘呼出热键:"), hotkeyRow);
    connect(clearHotkeyButton, &QPushButton::clicked, this,
            [this]() { wheelHotkeyEdit->setText(QString()); });
    auto* hotkeyLimitHint = new QLabel(
        QStringLiteral("提示：全局热键仅支持“修饰键 + 1 个主键”（如 Ctrl+Alt+S），不支持连续多个主键（如 Ctrl+Shift+S+H）。多主键序列请录在【动作】里。"),
        trigGroup);
    hotkeyLimitHint->setWordWrap(true);
    hotkeyLimitHint->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px;"));
    trigForm->addRow(hotkeyLimitHint);

    thresholdSpin = new QSpinBox(trigGroup);
    thresholdSpin->setRange(1, 50);
    thresholdSpin->setSuffix(QStringLiteral(" px"));
    trigForm->addRow(QStringLiteral("拖动阈值:"), thresholdSpin);

    longPressSpin = new QSpinBox(trigGroup);
    longPressSpin->setRange(50, 2000);
    longPressSpin->setSuffix(QStringLiteral(" ms"));
    trigForm->addRow(QStringLiteral("长按放弃拦截:"), longPressSpin);

    hideSpin = new QSpinBox(trigGroup);
    hideSpin->setRange(500, 30000);
    hideSpin->setSuffix(QStringLiteral(" ms"));
    trigForm->addRow(QStringLiteral("轮盘自动隐藏:"), hideSpin);

    scriptTimeoutSpin = new QSpinBox(trigGroup);
    scriptTimeoutSpin->setRange(500, 120000);
    scriptTimeoutSpin->setSuffix(QStringLiteral(" ms"));
    trigForm->addRow(QStringLiteral("外部脚本超时:"), scriptTimeoutSpin);

    layout->addWidget(trigGroup);

    // ---- 第二组：轮盘外观 ----
    auto* uiGroup = new QGroupBox(QStringLiteral("轮盘外观"), page);
    auto* uiForm = new QFormLayout(uiGroup);

    wheelSizeSpin = new QSpinBox(uiGroup);
    wheelSizeSpin->setRange(200, 800);
    uiForm->addRow(QStringLiteral("轮盘大小 (px):"), wheelSizeSpin);

    wheelInnerSpin = new QSpinBox(uiGroup);
    wheelInnerSpin->setRange(30, 200);
    uiForm->addRow(QStringLiteral("中心圆半径 (px):"), wheelInnerSpin);

    wheelFontSpin = new QSpinBox(uiGroup);
    wheelFontSpin->setRange(6, 20);
    uiForm->addRow(QStringLiteral("文字字号 (pt):"), wheelFontSpin);

    auto addColorRow = [&](const QString& label, QLineEdit*& edit) {
        auto* row = new QHBoxLayout();
        edit = new QLineEdit(uiGroup);
        edit->setPlaceholderText("#FFFFFF");
        auto* colorBtn = new QPushButton(QStringLiteral("选择"), uiGroup);
        colorBtn->setFixedWidth(50);
        connect(colorBtn, &QPushButton::clicked, this, [this, edit]() {
            QColor color = QColorDialog::getColor(QColor(edit->text()), this, "选择颜色");
            if (color.isValid()) {
                edit->setText(color.name());
            }
        });
        row->addWidget(edit);
        row->addWidget(colorBtn);
        uiForm->addRow(label, row);
    };
    addColorRow(QStringLiteral("背景颜色:"), wheelBgColorEdit);
    addColorRow(QStringLiteral("悬停背景色:"), wheelHoverColorEdit);
    addColorRow(QStringLiteral("线条颜色:"), wheelLineColorEdit);
    addColorRow(QStringLiteral("文字颜色:"), wheelTextColorEdit);
    addColorRow(QStringLiteral("悬停文字颜色:"), wheelTextHoverColorEdit);

    layout->addWidget(uiGroup);
    layout->addStretch();
    return page;
}

QWidget* MainWindow::buildActionPageTab() {
    auto* page = new QWidget(this);
    // 内容整体靠左 + 限制最大宽度：超宽屏下不会把表单拉成一整条
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    content->setMaximumWidth(800);
    outer->addWidget(content);
    outer->addStretch();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);

    // ---- 第一组：动作页快捷键 ----
    auto* hotkeyGroup = new QGroupBox(QStringLiteral("动作页快捷键"), page);
    auto* hotkeyForm = new QFormLayout(hotkeyGroup);

    auto* apHotkeyRow = new QHBoxLayout();
    actionPageHotkeyEdit = new KeyRecorderWidget(hotkeyGroup);
    actionPageHotkeyEdit->setToolTip(QStringLiteral("按下此快捷键呼出悬浮动作页。"));
    apHotkeyRow->addWidget(actionPageHotkeyEdit);
    auto* clearApBtn = new QPushButton(QStringLiteral("清除"), hotkeyGroup);
    apHotkeyRow->addWidget(clearApBtn);
    hotkeyForm->addRow(QStringLiteral("呼出快捷键:"), apHotkeyRow);
    connect(clearApBtn, &QPushButton::clicked, this,
            [this]() { actionPageHotkeyEdit->setText(QString()); });
    auto* hotkeyLimitHint = new QLabel(
        QStringLiteral("提示：全局热键仅支持“修饰键 + 1 个主键”（如 Ctrl+Alt+S），不支持连续多个主键（如 Ctrl+Shift+S+H）。多主键序列请录在【动作】里。"),
        hotkeyGroup);
    hotkeyLimitHint->setWordWrap(true);
    hotkeyLimitHint->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px;"));
    hotkeyForm->addRow(hotkeyLimitHint);

    auto* apHint = new QLabel(
        QStringLiteral("留空表示不使用键盘呼出，仍可通过鼠标动作页按钮/托盘触发。"),
        hotkeyGroup);
    apHint->setWordWrap(true);
    apHint->setStyleSheet(QStringLiteral("color: #888888;"));
    hotkeyForm->addRow(apHint);

    layout->addWidget(hotkeyGroup);

    // ---- 第二组：动作页外观 ----
    auto* uiGroup = new QGroupBox(QStringLiteral("动作页外观"), page);
    auto* uiForm = new QFormLayout(uiGroup);

    apWidthSpin = new QSpinBox(uiGroup);
    apWidthSpin->setRange(300, 1920);
    uiForm->addRow(QStringLiteral("宽 (px):"), apWidthSpin);

    apHeightSpin = new QSpinBox(uiGroup);
    apHeightSpin->setRange(300, 1080);
    uiForm->addRow(QStringLiteral("高 (px):"), apHeightSpin);

    btnWidthSpin = new QSpinBox(uiGroup);
    btnWidthSpin->setRange(50, 300);
    uiForm->addRow(QStringLiteral("格子最小宽:"), btnWidthSpin);

    btnHeightSpin = new QSpinBox(uiGroup);
    btnHeightSpin->setRange(40, 200);
    uiForm->addRow(QStringLiteral("格子最小高:"), btnHeightSpin);

    radiusSpin = new QSpinBox(uiGroup);
    radiusSpin->setRange(0, 30);
    uiForm->addRow(QStringLiteral("圆角 (px):"), radiusSpin);

    apHSpacingSpin = new QSpinBox(uiGroup);
    apHSpacingSpin->setRange(0, 50);
    apHSpacingSpin->setToolTip(QStringLiteral("格子之间的左右间距，越小越紧凑。"));
    uiForm->addRow(QStringLiteral("横间距 (px):"), apHSpacingSpin);

    apVSpacingSpin = new QSpinBox(uiGroup);
    apVSpacingSpin->setRange(0, 50);
    apVSpacingSpin->setToolTip(QStringLiteral("格子之间的上下间距，越小越紧凑。"));
    uiForm->addRow(QStringLiteral("纵间距 (px):"), apVSpacingSpin);

    auto addColorRow = [&](const QString& label, QLineEdit*& edit) {
        auto* row = new QHBoxLayout();
        edit = new QLineEdit(uiGroup);
        edit->setPlaceholderText("#FFFFFF");
        auto* colorBtn = new QPushButton(QStringLiteral("选择"), uiGroup);
        colorBtn->setFixedWidth(50);
        connect(colorBtn, &QPushButton::clicked, this, [this, edit]() {
            QColor color = QColorDialog::getColor(QColor(edit->text()), this, "选择颜色");
            if (color.isValid()) {
                edit->setText(color.name());
            }
        });
        row->addWidget(edit);
        row->addWidget(colorBtn);
        uiForm->addRow(label, row);
    };
    addColorRow(QStringLiteral("面板背景色:"), apBgColorEdit);
    addColorRow(QStringLiteral("格子背景色:"), btnBgColorEdit);
    addColorRow(QStringLiteral("悬停背景色:"), btnHoverColorEdit);
    addColorRow(QStringLiteral("文字颜色:"), textColorEdit);

    layout->addWidget(uiGroup);
    layout->addStretch();
    return page;
}

QWidget* MainWindow::buildSceneTab() {
    auto* page = new QWidget(this);
    auto* layout = new QHBoxLayout(page); // 场景列表 -> 轮盘 -> 动作页 三栏

    // === 左侧：场景列表 ===
    auto* leftPanel = new QWidget(page);
    leftPanel->setFixedWidth(200);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    sceneFilterEdit = new QLineEdit(leftPanel);
    sceneFilterEdit->setPlaceholderText(QStringLiteral("筛选场景..."));
    connect(sceneFilterEdit, &QLineEdit::textChanged, this, [this]() { applySceneFilter(); });
    leftLayout->addWidget(sceneFilterEdit);

    sceneList = new QListWidget(leftPanel);
    sceneList->setStyleSheet(QStringLiteral("QListWidget::item { min-height: 40px; }"));
    leftLayout->addWidget(sceneList, 1);

    auto* sceneBtnLayout = new QHBoxLayout();
    addSceneBtn = new QPushButton(QStringLiteral("+"), leftPanel);
    editSceneBtn = new QPushButton(QStringLiteral("编辑"), leftPanel);
    delSceneBtn = new QPushButton(QStringLiteral("-"), leftPanel);
    sceneBtnLayout->addWidget(addSceneBtn);
    sceneBtnLayout->addWidget(editSceneBtn);
    sceneBtnLayout->addWidget(delSceneBtn);
    leftLayout->addLayout(sceneBtnLayout);

    connect(addSceneBtn, &QPushButton::clicked, this, &MainWindow::addSceneFromWindow);
    connect(editSceneBtn, &QPushButton::clicked, this, &MainWindow::editScene);
    connect(delSceneBtn, &QPushButton::clicked, this, &MainWindow::deleteScene);
    connect(sceneList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { loadData(); });

    layout->addWidget(leftPanel);

    // === 右侧：轮盘 + 动作页 ===
    auto* rightArea = new QWidget(page);
    auto* rightLayout = new QVBoxLayout(rightArea);
    rightLayout->setContentsMargins(10, 10, 10, 10);

    // ---- 顶部：环数 / 外圈隐藏动作 / 重置为跟随全局 ----
    auto* topLayout = new QHBoxLayout();
    topLayout->addWidget(new QLabel(QStringLiteral("轮盘环数:"), rightArea));
    ringCombo = new QComboBox(rightArea);
    ringCombo->addItem(QStringLiteral("1 环 (8)"), 1);
    ringCombo->addItem(QStringLiteral("2 环 (8+16)"), 2);
    ringCombo->addItem(QStringLiteral("3 环 (8+16+32)"), 3);
    connect(ringCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        QString scene = currentSceneKey();
        if (scene.isEmpty()) return;
        ActionRepository::setSceneRings(scene, ringCombo->currentData().toInt());
        loadData(); // 刷新轮盘预览
        emit configSaved();
    });
    topLayout->addWidget(ringCombo);

    // 外圈隐藏动作：按场景保存（-1 继承全局 / 0 关 / 1 开）
    hiddenRingCheck = new QCheckBox(QStringLiteral("启用外圈隐藏动作 (继承全局)"), rightArea);
    // 不要用 image: url(:/qt-project.org/...)：Qt6 取不到该内置资源，勾选后会显示成空白方块
    hiddenRingCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { spacing: 5px; } "
        "QCheckBox::indicator { width: 14px; height: 14px; } "
        "QCheckBox::indicator:unchecked { border: 1px solid #888; background: #FFF; border-radius: 2px; } "
        "QCheckBox::indicator:checked { border: 1px solid #0078D7; background: #0078D7; border-radius: 2px; }"));
    connect(hiddenRingCheck, &QCheckBox::toggled, this, [this](bool checked) {
        const QString scene = currentSceneKey();
        if (!scene.isEmpty()) {
            // 手动勾/取消即从"继承"切成本场景专属值
            ActionRepository::setSceneHiddenRing(scene, checked ? 1 : 0);
        }
        if (hiddenRingCheck) {
            hiddenRingCheck->setText(QStringLiteral("启用外圈隐藏动作 (场景专属)"));
        }
        if (wheelEditor) {
            wheelEditor->setHiddenRingEnabled(checked);
            wheelEditor->update();
        }
        emit configSaved();
    });
    topLayout->addWidget(hiddenRingCheck);

    copyGlobalButton = new QPushButton(QStringLiteral("重置为跟随全局"), rightArea);
    copyGlobalButton->setToolTip(
        QStringLiteral("清除本场景的所有独立轮盘配置，之后完全跟随全局轮盘。"));
    connect(copyGlobalButton, &QPushButton::clicked, this, &MainWindow::copyGlobalToScene);
    topLayout->addWidget(copyGlobalButton);
    topLayout->addStretch();
    rightLayout->addLayout(topLayout);

    auto* hint = new QLabel(
        QStringLiteral("在右侧格子空白处【右键新建】动作，把格子里的动作拖到左侧扇区即可绑定；"
                       "按住扇区拖动可交换位置。"),
        rightArea);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #666666;"));
    rightLayout->addWidget(hint);

    // === 核心左右分栏（轮盘 + 动作页） ===
    auto* splitter = new QSplitter(Qt::Horizontal, rightArea);

    // ---- 左：轮盘预览 ----
    wheelEditor = new WheelEditorWidget(splitter);
    wheelEditor->onActionDropped = [this](int slot, int actionId) { applyBinding(slot, actionId); };
    wheelEditor->onSlotClicked = [this](int slot) {
        currentSelectedSlot = slot;
    };
    // 轮盘内槽位互换（含外层隐藏环 56~63）
    wheelEditor->onSlotSwapped = [this](int srcSlot, int dstSlot) {
        if (srcSlot == dstSlot) return;
        QString scene = currentSceneKey();
        if (scene.isEmpty()) scene = QStringLiteral("global");

        // 用"本场景 + global 继承"的合并视图取两端动作，
        // 否则非 global 场景里继承来的动作会被当成空槽，交换只完成一半
        QMap<int, int> effective = ActionRepository::getWheelBindings(scene);
        if (scene != QLatin1String("global")) {
            const QMap<int, int> globalBindings =
                ActionRepository::getWheelBindings(QStringLiteral("global"));
            for (auto it = globalBindings.constBegin(); it != globalBindings.constEnd(); ++it) {
                if (!effective.contains(it.key())) effective.insert(it.key(), it.value());
            }
        }
        const int srcActionId = effective.value(srcSlot, 0);
        const int dstActionId = effective.value(dstSlot, 0);
        // 目标槽的内容（有则绑定、空则解绑）落到源槽
        if (dstActionId > 0) {
            ActionRepository::bindActionToSlot(srcSlot, scene, dstActionId);
        } else {
            ActionRepository::unbindSlot(srcSlot, scene);
        }
        // 源槽的内容落到目标槽
        if (srcActionId > 0) {
            ActionRepository::bindActionToSlot(dstSlot, scene, srcActionId);
        } else {
            ActionRepository::unbindSlot(dstSlot, scene);
        }

        // 先设选中态再 loadData，否则选中晚一帧、看起来像没交换成功
        currentSelectedSlot = dstSlot;
        loadData();
        emit configSaved();
    };
    // Delete 键清空当前选中槽位
    wheelEditor->onDeleteRequested = [this](int slot) { doDeleteSlot(slot); };
    // 右键菜单：清空 / 强制空白 / 恢复继承
    wheelEditor->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(wheelEditor, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int slot = wheelEditor->slotAt(pos);
        if (slot < 0) return;
        // 右键即选中，后续 Delete 也作用在这个槽
        currentSelectedSlot = slot;
        wheelEditor->setSelectedSlot(slot);

        const QString scene = currentSceneKey();
        if (scene.isEmpty()) return;

        const QMap<int, int> local = ActionRepository::getWheelBindings(scene);
        // action_id == 0 是内置的"（空白/已屏蔽）"动作，用来压住 global 继承
        const bool blocked = local.value(slot, -1) == 0;
        const bool isGlobal = (scene == QLatin1String("global"));

        // 当前槽实际生效的动作 id：本场景有绑定（含 id=0 屏蔽）就用它，否则回退 global。
        // 和 onSlotSwapped 用同一套合并视图，不能只看本场景的绑定表。
        int currentActionId = local.value(slot, 0);
        if (!isGlobal && !local.contains(slot)) {
            currentActionId =
                ActionRepository::getWheelBindings(QStringLiteral("global")).value(slot, 0);
        }

        // 剪贴板只存 id：动作可能已被"彻底删除"，失效就丢弃
        if (m_copiedActionId > 0 && !isActionAliveInLibrary(m_copiedActionId)) {
            m_copiedActionId = -1;
        }

        QMenu menu(this);
        if (currentActionId > 0) {
            QAction* actCopy = menu.addAction(QStringLiteral("复制轮盘动作"));
            connect(actCopy, &QAction::triggered, this, [this, currentActionId]() {
                m_copiedActionId = currentActionId;
                QToolTip::showText(QCursor::pos(),
                                   QStringLiteral("已复制动作，可在动作页或其它场景粘贴"),
                                   this, QRect(), 1200);
            });
        }
        if (m_copiedActionId > 0) {
            QAction* actPaste = menu.addAction(QStringLiteral("粘贴动作到此槽位"));
            connect(actPaste, &QAction::triggered, this, [this, slot, scene]() {
                ActionRepository::bindActionToSlot(slot, scene, m_copiedActionId);
                currentSelectedSlot = slot;
                loadData();
                emit configSaved();
            });
        }
        if (currentActionId > 0 || m_copiedActionId > 0) menu.addSeparator();

        QAction* actClear = menu.addAction(QStringLiteral("清空此槽位 (Delete)"));
        QAction* actBlank = menu.addAction(QStringLiteral("强制设为空白（不继承全局）"));
        QAction* actRestore = menu.addAction(QStringLiteral("恢复继承全局"));
        actBlank->setEnabled(!isGlobal && !blocked);
        actRestore->setEnabled(!isGlobal && blocked);

        connect(actClear, &QAction::triggered, this, [this, slot]() { doDeleteSlot(slot); });
        connect(actBlank, &QAction::triggered, this, [this, slot, scene]() {
            ActionRepository::bindActionToSlot(slot, scene, 0);
            currentSelectedSlot = slot;
            loadData();
            emit configSaved();
        });
        connect(actRestore, &QAction::triggered, this, [this, slot, scene]() {
            ActionRepository::unbindSlot(slot, scene);
            currentSelectedSlot = slot;
            loadData();
            emit configSaved();
        });
        menu.exec(QCursor::pos());
    });
    splitter->addWidget(wheelEditor);

    // ---- 右：动作页网格 ----
    auto* side = new QWidget(splitter);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);

    auto* sideTitle = new QLabel(QStringLiteral("当前场景动作页（可拖拽）"), side);
    sideTitle->setStyleSheet(QStringLiteral("font-weight: bold;"));
    sideLayout->addWidget(sideTitle);

    auto* scroll = new QScrollArea(side);
    scroll->setWidgetResizable(true);
    actionGridHost = new QWidget(scroll);
    actionGridLayout = new QGridLayout(actionGridHost);
    actionGridLayout->setContentsMargins(0, 0, 0, 0);
    actionGridLayout->setAlignment(Qt::AlignTop); // 实际间距在 refreshActionGrid() 里按 config 覆盖
    scroll->setWidget(actionGridHost);
    sideLayout->addWidget(scroll, 1);

    auto* bottomBar = new QHBoxLayout();
    // 4 个导航按钮换成图标：素材本身是蓝色，压在浅灰圆底上很清楚。
    // add / del 保留原来的 + / - 白字 —— 这两个图标同样是蓝色，压在蓝底 / 红底上等于看不见，
    // 而彩色圆底是"新增 / 删除"唯一的语义标识。
    auto setPageBtnIcon = [](QPushButton* btn, const QString& file, const QString& fallbackText) {
        const QIcon icon(AppConstants::getUiIconPath(file));
        if (!icon.isNull()) {
            btn->setIcon(icon);
            btn->setIconSize(QSize(16, 16));
        } else {
            btn->setText(fallbackText);
        }
    };

    auto* prevBtn = new QPushButton(side);
    auto* nextBtn = new QPushButton(side);
    auto* addPageBtn = new QPushButton(QStringLiteral("+"), side);
    auto* delPageBtn = new QPushButton(QStringLiteral("-"), side);
    auto* moveLeftBtn = new QPushButton(side);
    auto* moveRightBtn = new QPushButton(side);

    setPageBtnIcon(prevBtn, QStringLiteral("prev.png"), QStringLiteral("◀"));
    setPageBtnIcon(nextBtn, QStringLiteral("next.png"), QStringLiteral("▶"));
    setPageBtnIcon(moveLeftBtn, QStringLiteral("prev.png"), QStringLiteral("<<"));
    setPageBtnIcon(moveRightBtn, QStringLiteral("next.png"), QStringLiteral(">>"));

    prevBtn->setToolTip(QStringLiteral("上一页"));
    nextBtn->setToolTip(QStringLiteral("下一页"));
    moveLeftBtn->setToolTip(QStringLiteral("将当前页与前一页交换位置"));
    moveRightBtn->setToolTip(QStringLiteral("将当前页与后一页交换位置"));
    actionGridPageLabel = new QLabel(QStringLiteral("1 / 1"), side);
    actionGridPageLabel->setAlignment(Qt::AlignCenter);

    QString btnStyle = QStringLiteral("QPushButton { border-radius: 12px; background: #EAEAEA; color: #333; font-weight: bold; min-width:24px; max-width:24px; max-height:24px; } QPushButton:hover { background: #D0D0D0; }");
    prevBtn->setStyleSheet(btnStyle);
    nextBtn->setStyleSheet(btnStyle);
    moveLeftBtn->setStyleSheet(btnStyle);
    moveRightBtn->setStyleSheet(btnStyle);
    addPageBtn->setStyleSheet(QStringLiteral("QPushButton { border-radius: 12px; background: #0078D7; color: white; font-weight: bold; min-width:24px; max-width:24px; max-height:24px; } QPushButton:hover { background: #006ABC; }"));
    delPageBtn->setStyleSheet(QStringLiteral("QPushButton { border-radius: 12px; background: #D32F2F; color: white; font-weight: bold; min-width:24px; max-width:24px; max-height:24px; } QPushButton:hover { background: #B71C1C; }"));

    bottomBar->addWidget(moveLeftBtn);
    bottomBar->addWidget(prevBtn);
    bottomBar->addWidget(addPageBtn);
    bottomBar->addStretch();
    bottomBar->addWidget(actionGridPageLabel);
    bottomBar->addStretch();
    bottomBar->addWidget(delPageBtn);
    bottomBar->addWidget(nextBtn);
    bottomBar->addWidget(moveRightBtn);
    sideLayout->addLayout(bottomBar);

    connect(prevBtn, &QPushButton::clicked, this, [this]() {
        if (actionGridPage > 0) { actionGridPage--; refreshActionGrid(); }
    });
    connect(nextBtn, &QPushButton::clicked, this, [this]() {
        int total = ActionRepository::getScenePageCount(currentSceneKey());
        if (actionGridPage + 1 < total) { actionGridPage++; refreshActionGrid(); }
    });
    // 新增页：弹出通用弹窗，可选空白 / 从全局复制 / 从其它场景复制
    connect(addPageBtn, &QPushButton::clicked, this, [this]() {
        int newPageIndex = showNewPageDialog(this, currentSceneKey());
        if (newPageIndex >= 0) {
            actionGridPage = newPageIndex; // 跳转到新页
            refreshActionGrid();
            emit configSaved();
        }
    });
    // 删除页：清空本页数据，后续页整体前移一位
    connect(delPageBtn, &QPushButton::clicked, this, [this]() {
        QString scene = currentSceneKey();
        int total = ActionRepository::getScenePageCount(scene);
        if (total <= 1) {
            QMessageBox::information(this, "提示", "至少需要保留一页。");
            return;
        }
        if (QMessageBox::question(this, "确认删除", "确定删除当前页及其中所有动作吗？") == QMessageBox::Yes) {
            ActionRepository::clearPage(scene, actionGridPage);
            QSqlQuery q;
            q.prepare("UPDATE page_bindings SET page_index = page_index - 1 WHERE scene = ? AND page_index > ?");
            q.addBindValue(scene);
            q.addBindValue(actionGridPage);
            q.exec();

            ActionRepository::setScenePageCount(scene, total - 1);
            if (actionGridPage >= total - 1) {
                actionGridPage = total - 2;
            }
            refreshActionGrid();
            emit configSaved();
        }
    });
    // 页面排序：与前/后一页交换 page_index，视图跟随移动后的当前页
    connect(moveLeftBtn, &QPushButton::clicked, this, [this]() {
        if (ActionRepository::movePage(currentSceneKey(), actionGridPage, -1)) {
            actionGridPage--;
            refreshActionGrid();
            emit configSaved();
        }
    });
    connect(moveRightBtn, &QPushButton::clicked, this, [this]() {
        if (ActionRepository::movePage(currentSceneKey(), actionGridPage, 1)) {
            actionGridPage++;
            refreshActionGrid();
            emit configSaved();
        }
    });
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);
    rightLayout->addWidget(splitter, 1);

    auto* deleteHint = new QLabel(
        QStringLiteral("提示：选中槽位后按 Delete 键，或右键槽位，可清空该槽位。"), rightArea);
    deleteHint->setStyleSheet(QStringLiteral("color: #666666;"));
    rightLayout->addWidget(deleteHint);

    layout->addWidget(rightArea, 1);
    return page;
}

QWidget* MainWindow::buildMouseTriggerTab() {
    auto* page = new QWidget(this);
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    content->setMaximumWidth(920); // 三行表格在这个宽度下比例最协调
    outer->addWidget(content);
    outer->addStretch();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);

    auto* bindingGroup =
        new QGroupBox(QStringLiteral("高级鼠标触发（按键直接执行动作，不经过轮盘）"), page);
    auto* bindingLayout = new QVBoxLayout(bindingGroup);

    bindingTable = new QTableWidget(3, 4, bindingGroup);
    bindingTable->setHorizontalHeaderLabels(
        {QStringLiteral("鼠标键"), QStringLiteral("名字"), QStringLiteral("类型"),
         QStringLiteral("参数")});
    bindingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    bindingTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    bindingTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    bindingTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    bindingTable->verticalHeader()->setVisible(false);
    bindingTable->verticalHeader()->setDefaultSectionSize(36);
    bindingTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    const QVector<MouseButton> buttons = {MouseButton::Middle, MouseButton::X1, MouseButton::X2};
    for (int row = 0; row < buttons.size(); ++row) {
        const MouseButton button = buttons.at(row);

        auto* buttonItem = new QTableWidgetItem(mouseButtonDisplayName(button));
        // 键名（middle/x1/x2）必须存进 UserRole：loadMouseBindings / saveData 都靠它定位该行
        buttonItem->setData(Qt::UserRole, mouseButtonKey(button));
        buttonItem->setFlags(buttonItem->flags() & ~Qt::ItemIsEditable);
        bindingTable->setItem(row, 0, buttonItem);

        bindingTable->setItem(row, 1, new QTableWidgetItem(QString()));

        auto* typeCombo = new QComboBox(bindingTable);
        addActionTypeItems(typeCombo);
        bindingTable->setCellWidget(row, 2, typeCombo);

        bindingTable->setCellWidget(row, 3, makeParamEditor(typeCombo, QString()));
    }

    bindingLayout->addWidget(bindingTable);
    auto* bindingHint = new QLabel(
        QStringLiteral("注意：如果某个键同时在「轮盘」页被选为轮盘触发键，以轮盘触发为准。"),
        bindingGroup);
    bindingHint->setWordWrap(true);
    bindingHint->setStyleSheet(QStringLiteral("color: #666666;"));
    bindingLayout->addWidget(bindingHint);

    layout->addWidget(bindingGroup);
    layout->addStretch();

    loadMouseBindings();
    return page;
}

QWidget* MainWindow::buildSystemTab() {
    auto* page = new QWidget(this);
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    content->setMaximumWidth(800);
    outer->addWidget(content);
    outer->addStretch();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);

    auto* systemGroup = new QGroupBox(QStringLiteral("启动与系统"), content);
    auto* form = new QFormLayout(systemGroup);

    autostartCheck = new QCheckBox(QStringLiteral("开机自动启动（当前用户，无需管理员权限）"), systemGroup);
    form->addRow(autostartCheck);

    auto* note = new QLabel(
        QStringLiteral("写入 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run，不会触发 UAC。\n"
                       "移动程序位置后，重新勾选保存即可更新启动路径。"),
        systemGroup);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #666666;"));
    form->addRow(note);

    // ---- 数据备份与同步 ----
    auto* backupGroup = new QGroupBox(QStringLiteral("数据备份与同步（兼容多版本）"), page);
    auto* backupLayout = new QVBoxLayout(backupGroup);

    // 备份路径：只读展示 + 浏览。路径要真实存在才可靠，所以不让手打
    auto* pathRow = new QHBoxLayout();
    pathRow->addWidget(new QLabel(QStringLiteral("备份路径:"), backupGroup));
    autoBackupPathEdit = new QLineEdit(backupGroup);
    autoBackupPathEdit->setReadOnly(true);
    autoBackupPathEdit->setToolTip(
        QStringLiteral("导出备份时的默认目录，也是自动备份的写入位置；点『浏览…』更换。"));
    pathRow->addWidget(autoBackupPathEdit, 1);
    browseBackupPathBtn = new QPushButton(QStringLiteral("浏览…"), backupGroup);
    pathRow->addWidget(browseBackupPathBtn);
    backupLayout->addLayout(pathRow);

    autoBackupCheck = new QCheckBox(
        QStringLiteral("开启自动备份（配置有改动时覆写备份目录下的 data.db）"), backupGroup);
    autoBackupCheck->setToolTip(
        QStringLiteral("勾选后，每次配置改动（含拖拽绑定）都会在停手 2 秒后把数据库覆写到备份目录。"));
    backupLayout->addWidget(autoBackupCheck);
    connect(autoBackupCheck, &QCheckBox::toggled, this, [this](bool checked) {
        QMap<QString, QString> backupSettings;
        backupSettings.insert(QStringLiteral("backup.auto"), checked ? QStringLiteral("1")
                                                                    : QStringLiteral("0"));
        ActionRepository::saveSettings(backupSettings);
        AppConfig::instance().reload();
        // 刚勾上就先存一份，避免"勾了但还没改动过 = 一份备份都没有"
        if (checked) triggerAutoBackup();
    });

    auto* backupBtnRow = new QHBoxLayout();
    auto* exportBtn = new QPushButton(QStringLiteral("导出数据备份"), backupGroup);
    exportBtn->setStyleSheet(QStringLiteral("QPushButton { padding: 6px; }"));
    auto* importBtn = new QPushButton(QStringLiteral("导入数据备份"), backupGroup);
    importBtn->setStyleSheet(QStringLiteral("QPushButton { padding: 6px; }"));
    backupBtnRow->addWidget(exportBtn);
    backupBtnRow->addWidget(importBtn);
    backupBtnRow->addStretch();
    backupLayout->addLayout(backupBtnRow);

    connect(exportBtn, &QPushButton::clicked, this, &MainWindow::exportDatabase);
    connect(importBtn, &QPushButton::clicked, this, &MainWindow::importDatabase);

    // 选完目录立即落库：用户可能不点『保存设置』就关窗口，这条路径必须当场生效
    connect(browseBackupPathBtn, &QPushButton::clicked, this, [this]() {
        if (!autoBackupPathEdit) return;
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择备份文件夹"), autoBackupPathEdit->text().trimmed());
        if (dir.isEmpty()) return;
        autoBackupPathEdit->setText(QDir::toNativeSeparators(dir));
        QMap<QString, QString> backupSettings;
        backupSettings.insert(QStringLiteral("backup.path"), dir);
        ActionRepository::saveSettings(backupSettings);
        AppConfig::instance().reload(); // 让内存缓存立刻跟上，否则 loadTriggerSettings 还会读到旧值
    });
    
    layout->addWidget(systemGroup);

    // ================= 新增：全局系统热键（软件级行为，与轮盘/动作页触发无关） =================
    auto* hotkeyGroup = new QGroupBox(QStringLiteral("全局系统热键（随时生效）"), page);
    auto* hotkeyForm = new QFormLayout(hotkeyGroup);

    // 1. 呼出设置界面
    settingsHotkeyEdit = new KeyRecorderWidget(hotkeyGroup);
    settingsHotkeyEdit->setToolTip(QStringLiteral("任何时候按下此键即可呼出当前设置窗口。"));
    auto* setHotkeyRow = new QHBoxLayout();
    setHotkeyRow->addWidget(settingsHotkeyEdit);
    auto* clearSetBtn = new QPushButton(QStringLiteral("清除"), hotkeyGroup);
    setHotkeyRow->addWidget(clearSetBtn);
    hotkeyForm->addRow(QStringLiteral("呼出设置界面:"), setHotkeyRow);
    connect(clearSetBtn, &QPushButton::clicked, this,
            [this]() { settingsHotkeyEdit->setText(QString()); });

    // 2. 强制退出程序
    forceQuitEdit = new KeyRecorderWidget(hotkeyGroup);
    forceQuitEdit->setToolTip(
        QStringLiteral("防患于未然：当软件卡死、鼠标被绑架时，按下此键强制结束进程。"));
    auto* forceQuitRow = new QHBoxLayout();
    forceQuitRow->addWidget(forceQuitEdit);
    auto* clearQuitBtn = new QPushButton(QStringLiteral("清除"), hotkeyGroup);
    forceQuitRow->addWidget(clearQuitBtn);
    hotkeyForm->addRow(QStringLiteral("强制退出程序:"), forceQuitRow);
    connect(clearQuitBtn, &QPushButton::clicked, this,
            [this]() { forceQuitEdit->setText(QString()); });

    auto* hotkeyNote = new QLabel(
        QStringLiteral("若未设置强制退出热键，当软件发生死锁时，请使用任务管理器结束 %1 进程。")
            .arg(AppConstants::selfExeName()),
        hotkeyGroup);
    hotkeyNote->setWordWrap(true);
    hotkeyNote->setStyleSheet(QStringLiteral("color: #666666;"));
    hotkeyForm->addRow(hotkeyNote);

    auto* hotkeyLimitHint = new QLabel(
        QStringLiteral("提示：全局热键仅支持“修饰键 + 1 个主键”（如 Ctrl+Alt+S），不支持连续多个主键（如 Ctrl+Shift+S+H）。多主键序列请录在【动作】里。"),
        hotkeyGroup);
    hotkeyLimitHint->setWordWrap(true);
    hotkeyLimitHint->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px;"));
    hotkeyForm->addRow(hotkeyLimitHint);

    layout->addWidget(hotkeyGroup);
    layout->addWidget(backupGroup); // 备份属于低频操作，排在最后
    layout->addStretch();

    loadAutostartState();
    return page;
}

void MainWindow::loadData() {
    
    if (currentSelectedSlot >= 64) {
        currentSelectedSlot = -1;
    }

    if (!wheelEditor) return;
    QString scene = currentSceneKey();
    if (scene.isEmpty()) scene = QStringLiteral("global");

    QVector<Action> actions = ActionRepository::getAllActions(scene);

    // 56 个普通槽位 + 8 个隐藏环槽位（56~63）
    QVector<Action> merged(64);
    for (int i = 0; i < 64; ++i) {
        merged[i].slot = i;
        merged[i].type = -2;
        merged[i].name = "";
    }
    for (const auto& a : actions) {
        if (a.slot >= 0 && a.slot < 64) {
            merged[a.slot] = a;
            if (a.type == -1) {
                merged[a.slot].name = QStringLiteral("空白（不继承）");
            }
        }
    }

    // 本场景显式覆盖的槽位（id > 0 才算独立动作；id == 0 是"强制空白"占位）
    QSet<int> overriddenSlots;
    {
        const QMap<int, int> localBindings = ActionRepository::getWheelBindings(scene);
        for (auto it = localBindings.constBegin(); it != localBindings.constEnd(); ++it) {
            if (it.value() > 0) overriddenSlots.insert(it.key());
        }
    }

    wheelEditor->setScene(scene);
    wheelEditor->setActions(merged);
    wheelEditor->setOverriddenSlots(overriddenSlots);
    // 同步隐藏环配置：-1 表示继承全局，否则使用场景专属值
    if (hiddenRingCheck) {
        const int sceneHidden = ActionRepository::getSceneHiddenRing(scene);
        bool hiddenRingEnabled = false;
        hiddenRingCheck->blockSignals(true);
        if (sceneHidden == -1) {
            hiddenRingEnabled =
                AppConfig::instance().intValue(QStringLiteral("ui.wheel.enable_hidden_ring"), 0) == 1;
            hiddenRingCheck->setText(QStringLiteral("启用外圈隐藏动作 (继承全局)"));
        } else {
            hiddenRingEnabled = (sceneHidden == 1);
            hiddenRingCheck->setText(QStringLiteral("启用外圈隐藏动作 (场景专属)"));
        }
        hiddenRingCheck->setChecked(hiddenRingEnabled);
        hiddenRingCheck->blockSignals(false);
        wheelEditor->setHiddenRingEnabled(hiddenRingEnabled);
    }
    wheelEditor->setSelectedSlot(currentSelectedSlot); // 确保切换场景时选中状态一致

    // 同步环数
    if (ringCombo) {
        int rings = ActionRepository::getSceneRings(scene);
        ringCombo->blockSignals(true);
        ringCombo->setCurrentIndex(ringCombo->findData(rings));
        ringCombo->blockSignals(false);
        wheelEditor->setRings(rings); // 通知轮盘预览控件改变环数
    }
    // 新增：联动刷新右侧动作页网格
    refreshActionGrid();
}

QWidget* MainWindow::buildRankingTab() {
    auto* page = new QWidget(this);
    // 与其它设置页保持一致：外层 HBox + 限宽内容 + 右侧留白。
    // 这一页用固定宽而不是 setMaximumWidth：让外框稳定停在 800，卡片左右才有对称留白可居中
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    content->setFixedWidth(800);
    outer->addWidget(content);
    outer->addStretch();

    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 点卡片会真的执行动作，把风险写在明面上
    auto* hintLabel = new QLabel(
        QStringLiteral("提示：单击卡片会立即执行该动作（真实发出按键 / 启动程序 / 运行脚本），请注意不要误点。"
                       "次数在下一次进入本页时刷新。"),
        content);
    hintLabel->setWordWrap(true);
    hintLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px;"));
    layout->addWidget(hintLabel);

    // 外框：用默认样式的 QGroupBox，和轮盘页 / 动作页页 / 系统页的组保持同一套观感
    auto* rankGroup = new QGroupBox(QStringLiteral("动作使用热度排行"), content);
    auto* groupLayout = new QVBoxLayout(rankGroup);

    auto* scroll = new QScrollArea(rankGroup);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    // 固定 3 列，宽度方向不允许出现横向滚动条；万一某台机器字体更宽也不会横向抖动
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 视口锁 3 行：10 上边距 + 3 × 100 卡片 + 2 × 10 间距 + 10 下边距 = 340。
    // 第 4 行的起始 y 正好是 340，落在可视区之外；不足 3 行时内容高 ≤ 340，滚动条自动隐藏。
    scroll->setFixedHeight(340);

    rankingGridHost = new QWidget(scroll);
    rankingGridHost->setStyleSheet(QStringLiteral("background: transparent;"));

    // 居中不靠 QGridLayout 的 alignment：容器变宽时 grid 的列会被拉开，卡片间距就不再均匀。
    // 让网格保持自身宽度（3 × 210 + 2 × 10 间距 + 20 边距 = 670），左右各顶一个 stretch 才是真居中。
    auto* hostLayout = new QHBoxLayout(rankingGridHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->addStretch();
    auto* gridWrap = new QWidget(rankingGridHost);
    rankingGridLayout = new QGridLayout(gridWrap);
    rankingGridLayout->setContentsMargins(10, 10, 10, 10); // 与上面 340 的算式绑死，改一处要同时改两处
    rankingGridLayout->setSpacing(10);
    rankingGridLayout->setAlignment(Qt::AlignTop); // 只贴顶，横向交给外层两个 stretch
    hostLayout->addWidget(gridWrap);
    hostLayout->addStretch();

    scroll->setWidget(rankingGridHost);
    groupLayout->addWidget(scroll);

    layout->addWidget(rankGroup);
    layout->addStretch(); // 外框贴顶，不撑满整页高度

    return page;
}

void MainWindow::refreshRanking() {
    if (!rankingGridLayout || !rankingGridHost) return;

    // 清空旧卡片：takeAt 交出所有权，widget 走 deleteLater，item 自己 delete
    while (QLayoutItem* item = rankingGridLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide(); // 先隐藏：deleteLater 要等到下一轮事件循环，不藏会和新卡片叠一帧重影
            w->deleteLater();
        }
        delete item;
    }

    QVector<LibraryAction> lib = ActionRepository::getActionLibrary();
    std::sort(lib.begin(), lib.end(), [](const LibraryAction& a, const LibraryAction& b) {
        if (a.usageCount != b.usageCount) return a.usageCount > b.usageCount;
        // 次数相同时按名字排，否则每次刷新同分卡片顺序随机乱跳，看着像 bug
        return a.name.localeAwareCompare(b.name) < 0;
    });

    int row = 0;
    int col = 0;
    constexpr int kColCount = 3; // 3 列 × 3 行 = 一屏 9 张，第 10 张起出竖条

    for (const LibraryAction& act : lib) {
        if (act.usageCount <= 0) continue; // 一次都没用过的不上榜

        // 关键：QPushButton 不解析富文本，直接 setText("<b>x</b>") 会把尖括号原样画出来。
        // 所以用「按钮当卡片 + 内部 QLabel」，并让 QLabel 鼠标穿透，点哪儿都算点按钮。
        auto* card = new QPushButton(rankingGridHost);
        card->setFixedSize(210, 100); // 3 列 × 210 + 2 × 10 = 650，落在约 736 的可用宽里，横向不用滚动
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(QStringLiteral(
            "QPushButton { text-align: left; padding: 0px; "
            "border: 1px solid #E0E0E0; border-radius: 8px; background: #FFFFFF; }"
            "QPushButton:hover { background: #F0F7FF; border-color: #1976D2; }"));

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 10, 12, 10);
        cardLayout->setSpacing(2);

        auto* nameLabel = new QLabel(
            QStringLiteral("<b>%1</b>").arg(act.name.toHtmlEscaped()), card);
        auto* typeLabel = new QLabel(
            QStringLiteral("类型: %1").arg(actionTypeName(act.type).toHtmlEscaped()), card);
        auto* countLabel = new QLabel(
            QStringLiteral("已使用 %1 次").arg(act.usageCount), card);

        nameLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        typeLabel->setStyleSheet(
            QStringLiteral("background: transparent; border: none; color: #666666; font-size: 12px;"));
        countLabel->setStyleSheet(
            QStringLiteral("background: transparent; border: none; color: #1976D2; font-weight: bold;"));

        for (auto* label : {nameLabel, typeLabel, countLabel}) {
            label->setAttribute(Qt::WA_TransparentForMouseEvents, true); // 鼠标穿透到卡片按钮
        }

        cardLayout->addWidget(nameLabel);
        cardLayout->addWidget(typeLabel);
        cardLayout->addWidget(countLabel);
        cardLayout->addStretch();

        connect(card, &QPushButton::clicked, this, [this, act]() {
            Action a;
            a.id = act.id; // 带上 id 才会被统计，否则点一次白点
            a.slot = -1;
            a.name = act.name;
            a.type = act.type;
            a.payload = act.payload;
            ActionEngine::executeAction(a, currentSceneKey());
        });

        rankingGridLayout->addWidget(card, row, col);
        if (++col >= kColCount) {
            col = 0;
            ++row;
        }
    }

    if (row == 0 && col == 0) {
        auto* emptyLabel = new QLabel(
            QStringLiteral("暂无使用记录。执行一次动作后，这里会显示热度排行。"), rankingGridHost);
        emptyLabel->setStyleSheet(QStringLiteral("color: #999999; font-size: 13px; background: transparent;"));
        rankingGridLayout->addWidget(emptyLabel, 0, 0);
    }
}

void MainWindow::refreshActionGrid() {
    if (!actionGridLayout) return;

    // 清空旧按钮
    QLayoutItem* item;
    while ((item = actionGridLayout->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    QString scene = currentSceneKey();
    if (scene.isEmpty()) scene = QStringLiteral("global");

    QMap<int, int> bindings = ActionRepository::getPageBindings(scene, actionGridPage);
    QVector<LibraryAction> library = ActionRepository::getActionLibrary();
    QMap<int, LibraryAction> libMap;
    for (const auto& a : library) libMap.insert(a.id, a);

    // 尺寸可以参考全局设置中的值，让预览和实际一致
    const AppConfig& config = AppConfig::instance();
    int btnW = config.intValue(QStringLiteral("ui.actionpage.btn_width"), 100, 50, 300);
    int btnH = config.intValue(QStringLiteral("ui.actionpage.btn_height"), 70, 40, 200);
    int hSpacing = config.intValue(QStringLiteral("ui.actionpage.h_spacing"), 4, 0, 50);
    int vSpacing = config.intValue(QStringLiteral("ui.actionpage.v_spacing"), 4, 0, 50);
    
    actionGridLayout->setHorizontalSpacing(hSpacing);
    actionGridLayout->setVerticalSpacing(vSpacing);

    for (int i = 0; i < 24; ++i) {
        int r = i / 4;
        int c = i % 4;
        
        int actionId = bindings.value(i, -1);
        auto* btn = new SceneActionButton(actionId, i, actionGridHost);
        btn->setMinimumSize(btnW, btnH);
        
        // === 新增：强制尺寸策略和列宽均分 ===
        btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        actionGridLayout->setColumnStretch(c, 1);

        QString btnStyle = "QPushButton { text-align: center; border: 1px solid #d0d0d0; border-radius: 6px; background: #ffffff; color: #333; padding: 4px; } "
                           "QPushButton:hover { background: #eaf3ff; border-color: #0078D7; }";
        btn->setStyleSheet(btnStyle);

        if (actionId > 0 && libMap.contains(actionId)) {
            LibraryAction act = libMap.value(actionId);
            btn->setText(QStringLiteral("%1\n(%2)").arg(act.name, actionTypeName(act.type)));
            
            // 修复：使用自定义的双击回调
            btn->onDoubleClicked = [this, actionId]() {
                auto lib = ActionRepository::getActionLibrary();
                for (const auto& a : lib) {
                    if (a.id == actionId) {
                        ActionEditDialog dlg(this, a, false);
                        if (dlg.exec() == QDialog::Accepted) {
                            ActionRepository::saveLibraryAction(dlg.result());
                            refreshActionGrid();
                            loadData();
                            emit configSaved();
                        }
                        break;
                    }
                }
            };
        } else {
            btn->setText(QStringLiteral("空白"));
            btn->setStyleSheet(btnStyle + "QPushButton { color: #aaa; }");
        }
        
        // 右键菜单
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::customContextMenuRequested, this, [this, i, actionId, scene]() {
            // 剪贴板只存 id：动作可能已被"彻底删除"，失效就丢弃，避免粘出幽灵绑定
            if (m_copiedActionId > 0 && !isActionAliveInLibrary(m_copiedActionId)) {
                m_copiedActionId = -1;
            }

            QMenu menu(this);
            if (actionId > 0) {
                QAction* actCopy = menu.addAction(QStringLiteral("复制动作"));
                connect(actCopy, &QAction::triggered, this, [this, actionId]() {
                    m_copiedActionId = actionId;
                    QToolTip::showText(QCursor::pos(),
                                       QStringLiteral("已复制动作，可在其它格子、页或场景粘贴"),
                                       this, QRect(), 1200);
                });
            }
            if (m_copiedActionId > 0) {
                QAction* actPaste = menu.addAction(QStringLiteral("粘贴动作"));
                connect(actPaste, &QAction::triggered, this, [this, i, scene]() {
                    ActionRepository::bindActionToPage(actionGridPage, i, scene, m_copiedActionId);
                    refreshActionGrid();
                    loadData(); // 同步刷新左侧轮盘预览
                    emit configSaved();
                });
            }
            if (actionId > 0 || m_copiedActionId > 0) menu.addSeparator();

            if (actionId > 0) {
                QAction* actEdit = menu.addAction("编辑动作");
                QAction* actRemove = menu.addAction("移除绑定");
                QAction* actDelete = menu.addAction("彻底删除动作");
                
                connect(actRemove, &QAction::triggered, [this, i, scene]() {
                    ActionRepository::unbindPageSlot(actionGridPage, i, scene);
                    refreshActionGrid();
                });
                connect(actEdit, &QAction::triggered, [this, actionId]() {
                    auto lib = ActionRepository::getActionLibrary();
                    for (const auto& a : lib) {
                        if (a.id == actionId) {
                            ActionEditDialog dlg(this, a, false);
                            if (dlg.exec() == QDialog::Accepted) {
                                ActionRepository::saveLibraryAction(dlg.result());
                                refreshActionGrid();
                                loadData();
                                emit configSaved();
                            }
                            break;
                        }
                    }
                });
                connect(actDelete, &QAction::triggered, [this, actionId]() {
                    if (QMessageBox::question(this, "删除", "确定从动作库彻底删除该动作？") == QMessageBox::Yes) {
                        ActionRepository::deleteLibraryAction(actionId);
                        refreshActionGrid();
                        loadData();
                        emit configSaved();
                    }
                });
            } else {
                QAction* actAdd = menu.addAction("新建动作并绑定到此处");
                connect(actAdd, &QAction::triggered, [this, i, scene]() {
                    ActionEditDialog dlg(this, LibraryAction{}, true);
                    if (dlg.exec() == QDialog::Accepted) {
                        int id = ActionRepository::saveLibraryAction(dlg.result());
                        if (id > 0) {
                            ActionRepository::bindActionToPage(actionGridPage, i, scene, id);
                            refreshActionGrid();
                        }
                    }
                });
            }
            menu.exec(QCursor::pos());
        });

        // 页内拖拽交换：把源格子与目标格子的动作对调，空格子则解绑对应端
        btn->onActionSwapped = [this, scene](int srcIdx, int dstIdx) {
            if (srcIdx == dstIdx) return;
            const QMap<int, int> current = ActionRepository::getPageBindings(scene, actionGridPage);
            const int srcActionId = current.value(srcIdx, -1);
            const int dstActionId = current.value(dstIdx, -1);

            if (dstActionId > 0) {
                ActionRepository::bindActionToPage(actionGridPage, srcIdx, scene, dstActionId);
            } else {
                ActionRepository::unbindPageSlot(actionGridPage, srcIdx, scene);
            }
            if (srcActionId > 0) {
                ActionRepository::bindActionToPage(actionGridPage, dstIdx, scene, srcActionId);
            } else {
                ActionRepository::unbindPageSlot(actionGridPage, dstIdx, scene);
            }
            refreshActionGrid();
            emit configSaved();
        };

        actionGridLayout->addWidget(btn, r, c);
    }
    
    // 让 6 行平分高度
    for (int r = 0; r < 6; ++r) {
        actionGridLayout->setRowStretch(r, 1);
    }
    // 同步页码显示
    int totalPages = ActionRepository::getScenePageCount(scene);
    actionGridPageLabel->setText(QStringLiteral("%1 / %2").arg(actionGridPage + 1).arg(totalPages));
}
void MainWindow::applyBinding(int slot, int actionId) {
    const QString scene = currentSceneKey();
    if (scene.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少场景"),
                             QStringLiteral("请先选择或填写场景名。"));
        return;
    }

    if (!ActionRepository::bindActionToSlot(slot, scene, actionId)) {
        QMessageBox::warning(this, QStringLiteral("绑定失败"),
                             QStringLiteral("无法把动作绑定到槽位 %1。").arg(slot));
        return;
    }

    currentSelectedSlot = slot; // 绑定后自动选中（必须先设，loadData 会据此回填选中态）
    loadData();
    emit configSaved();
}

void MainWindow::doDeleteSlot(int slot) {
    if (slot < 0 || slot >= 64) return;
    const QString scene = currentSceneKey();
    if (scene.isEmpty()) return;

    if (scene == QLatin1String("global")) {
        // 全局场景：没有继承对象，直接删除绑定
        ActionRepository::unbindSlot(slot, scene);
    } else {
        const QMap<int, int> local = ActionRepository::getWheelBindings(scene);
        if (local.contains(slot)) {
            // 本场景独立配置（含 id=0 的强制空白）→ 删除，恢复继承全局
            ActionRepository::unbindSlot(slot, scene);
        } else {
            // 本来就从 global 继承来的 → Delete 表示"这个槽我不要"，
            // 写 id=0 压住继承，否则删完 global 又会把它填回来
            ActionRepository::bindActionToSlot(slot, scene, 0);
        }
    }

    // 先设选中再刷新，保证删除后选中框仍停在这个槽
    currentSelectedSlot = slot;
    loadData();
    wheelEditor->setSelectedSlot(slot);
    emit configSaved();
}

void MainWindow::copyGlobalToScene() {
    const QString scene = currentSceneKey();
    if (scene.isEmpty() || scene == QLatin1String("global")) {
        QMessageBox::information(this, QStringLiteral("无法操作"),
                                 QStringLiteral("请先选择一个非 global 的场景。"));
        return;
    }

    if (QMessageBox::question(
            this, QStringLiteral("重置为跟随全局"),
            QStringLiteral("将清除场景“%1”的全部独立轮盘配置，改为完全跟随全局轮盘。\n"
                           "之后全局修改会实时同步到这里。是否继续？").arg(scene)) != QMessageBox::Yes) {
        return;
    }

    if (!ActionRepository::clearSceneBindings(scene)) {
        QMessageBox::warning(this, QStringLiteral("重置失败"),
                             QStringLiteral("数据库写入失败，请稍后重试。"));
        return;
    }

    loadData();
    emit configSaved();
}

void MainWindow::refreshLibrary() {
    m_library = ActionRepository::getActionLibrary();

    // 兜底：如果旧版动作库表格还存在（如某些调用路径），同步刷新
    if (libraryTable) {
        libraryTable->setRowCount(m_library.size());
        for (int row = 0; row < m_library.size(); ++row) {
            const LibraryAction& action = m_library.at(row);

            auto* nameItem = new QTableWidgetItem(action.name);
            nameItem->setData(Qt::UserRole, action.id);
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
            nameItem->setToolTip(QStringLiteral("%1\n%2").arg(actionTypeName(action.type), action.payload));
            libraryTable->setItem(row, 0, nameItem);

            auto* typeItem = new QTableWidgetItem(actionTypeName(action.type));
            typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
            libraryTable->setItem(row, 1, typeItem);

            auto* paramItem = new QTableWidgetItem(action.payload);
            paramItem->setFlags(paramItem->flags() & ~Qt::ItemIsEditable);
            libraryTable->setItem(row, 2, paramItem);
        }
    }
}



void MainWindow::selectLibraryRow(int actionId) {
    if (!libraryTable) return;

    for (int row = 0; row < libraryTable->rowCount(); ++row) {
        QTableWidgetItem* item = libraryTable->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == actionId) {
            libraryTable->selectRow(row);
            libraryTable->setCurrentCell(row, 0);
            libraryTable->scrollToItem(item);
            return;
        }
    }
}

void MainWindow::addLibraryAction() {
    ActionEditDialog dialog(this, LibraryAction{}, true);
    if (dialog.exec() != QDialog::Accepted) return;

    const int id = ActionRepository::saveLibraryAction(dialog.result());
    if (id < 0) {
        QMessageBox::warning(this, QStringLiteral("新增失败"),
                             QStringLiteral("动作库写入失败。"));
        return;
    }

    refreshLibrary();
    emit configSaved();
}

void MainWindow::editLibraryAction() {
    if (!libraryTable) return;

    const int row = libraryTable->currentRow();
    if (row < 0 || row >= m_library.size()) {
        QMessageBox::information(this, QStringLiteral("未选择动作"),
                                 QStringLiteral("请先在动作列表里选中一行。"));
        return;
    }

    const LibraryAction current = m_library.at(row);
    ActionEditDialog dialog(this, current, false);
    if (dialog.exec() != QDialog::Accepted) return;

    const int id = ActionRepository::saveLibraryAction(dialog.result());
    if (id < 0) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("动作库写入失败。"));
        return;
    }

    refreshLibrary();
    loadData();
    emit configSaved();
}

void MainWindow::deleteLibraryAction() {
    if (!libraryTable) return;

    const int row = libraryTable->currentRow();
    if (row < 0 || row >= m_library.size()) {
        QMessageBox::information(this, QStringLiteral("未选择动作"),
                                 QStringLiteral("请先在动作列表里选中一行。"));
        return;
    }

    const LibraryAction action = m_library.at(row);
    if (QMessageBox::question(
            this, QStringLiteral("删除动作"),
            QStringLiteral("删除“%1”？绑定到它的槽位也会被解绑。").arg(action.name)) !=
        QMessageBox::Yes) {
        return;
    }

    if (!ActionRepository::deleteLibraryAction(action.id)) {
        QMessageBox::warning(this, QStringLiteral("删除失败"), QStringLiteral("数据库写入失败。"));
        return;
    }

    refreshLibrary();
    loadData();
    emit configSaved();
}

void MainWindow::loadTriggerSettings() {
    AppConfig::instance().reload();
    const AppConfig& config = AppConfig::instance();

    const int mouseIndex = mouseTriggerCombo ? mouseTriggerCombo->findData(config.mouseTrigger()) : -1;
    if (mouseTriggerCombo) {
        mouseTriggerCombo->setCurrentIndex(mouseIndex >= 0 ? mouseIndex : 0);
    }

    // KeyRecorderWidget 直接读纯文本（配置里就是 "Win"，不再需要转 Meta）
    if (wheelHotkeyEdit) {
        wheelHotkeyEdit->setText(config.keyboardTrigger());
    }

    // 加上判空，防止之前那种由于局部变量覆盖造成的崩溃
    if (actionPageHotkeyEdit) {
        actionPageHotkeyEdit->setText(config.actionPageHotkey());
    }
    if (settingsHotkeyEdit) {
        settingsHotkeyEdit->setText(config.settingsHotkey());
    }
    if (forceQuitEdit) {
        forceQuitEdit->setText(config.forceQuitHotkey());
    }

    if (thresholdSpin) thresholdSpin->setValue(config.dragThreshold());
    if (longPressSpin) longPressSpin->setValue(config.longPressMs());
    if (hideSpin) hideSpin->setValue(config.wheelHideMs());
    if (scriptTimeoutSpin) scriptTimeoutSpin->setValue(config.scriptTimeoutMs());

    // 加载 UI 设置
    if (apWidthSpin) apWidthSpin->setValue(config.intValue(QStringLiteral("ui.actionpage.width"), 500, 300, 1920));
    if (apHeightSpin) apHeightSpin->setValue(config.intValue(QStringLiteral("ui.actionpage.height"), 680, 300, 1080));
    if (btnWidthSpin) btnWidthSpin->setValue(config.intValue(QStringLiteral("ui.actionpage.btn_width"), 100, 50, 300));
    if (btnHeightSpin) btnHeightSpin->setValue(config.intValue(QStringLiteral("ui.actionpage.btn_height"), 70, 40, 200));
    if (radiusSpin) radiusSpin->setValue(config.intValue(QStringLiteral("ui.actionpage.radius"), 6, 0, 30));
    if (apHSpacingSpin) apHSpacingSpin->setValue(config.intValue(QStringLiteral("ui.actionpage.h_spacing"), 4, 0, 50));
    if (apVSpacingSpin) apVSpacingSpin->setValue(config.intValue(QStringLiteral("ui.actionpage.v_spacing"), 4, 0, 50));
    
    if (apBgColorEdit) apBgColorEdit->setText(config.value(QStringLiteral("ui.actionpage.bg_color"), "#F8F9FA"));
    if (btnBgColorEdit) btnBgColorEdit->setText(config.value(QStringLiteral("ui.actionpage.btn_bg"), "#FFFFFF"));
    if (btnHoverColorEdit) btnHoverColorEdit->setText(config.value(QStringLiteral("ui.actionpage.btn_hover_bg"), "#eaf3ff"));
    if (textColorEdit) textColorEdit->setText(config.value(QStringLiteral("ui.actionpage.text_color"), "#333333"));

    // 备份目录（只读展示，取配置里的值）
    if (autoBackupPathEdit) {
        autoBackupPathEdit->setText(
            QDir::toNativeSeparators(config.value(QStringLiteral("backup.path"))));
    }
    if (autoBackupCheck) {
        // setChecked 会发 toggled，一旦接上就立刻触发一次备份 + 写库，回填时必须屏蔽
        autoBackupCheck->blockSignals(true);
        autoBackupCheck->setChecked(config.value(QStringLiteral("backup.auto")) == QLatin1String("1"));
        autoBackupCheck->blockSignals(false);
    }

    // 加载轮盘 UI 设置
    if (wheelSizeSpin) {
        wheelSizeSpin->setValue(config.intValue(QStringLiteral("ui.wheel.size"), 400, 200, 800));
        wheelInnerSpin->setValue(config.intValue(QStringLiteral("ui.wheel.inner_radius"), 50, 30, 200));
        wheelFontSpin->setValue(config.intValue(QStringLiteral("ui.wheel.font_size"), 10, 6, 20));
        
        wheelBgColorEdit->setText(config.value(QStringLiteral("ui.wheel.bg_color"), "#FEFEFE"));
        wheelHoverColorEdit->setText(config.value(QStringLiteral("ui.wheel.hover_color"), "#ECECEC"));
        wheelLineColorEdit->setText(config.value(QStringLiteral("ui.wheel.line_color"), "#ECECEC"));
        wheelTextColorEdit->setText(config.value(QStringLiteral("ui.wheel.text_color"), "#333333"));
        wheelTextHoverColorEdit->setText(config.value(QStringLiteral("ui.wheel.text_hover_color"), "#000000"));
    }
}


void MainWindow::loadMouseBindings() {
    if (!bindingTable) return;

    QHash<QString, MouseBinding> bindings;
    for (const MouseBinding& binding : ActionRepository::getMouseBindings()) {
        bindings.insert(binding.button.trimmed().toLower(), binding);
    }

    for (int row = 0; row < 3; ++row) {
        QTableWidgetItem* buttonItem = bindingTable->item(row, 0);
        if (!buttonItem) continue;

        const QString key = buttonItem->data(Qt::UserRole).toString();
        const auto it = bindings.constFind(key);
        if (it == bindings.constEnd()) continue;

        bindingTable->item(row, 1)->setText(it->name);

        auto* typeCombo = qobject_cast<QComboBox*>(bindingTable->cellWidget(row, 2));
        if (typeCombo) {
            const int index = typeCombo->findData(it->type);
            if (index >= 0) typeCombo->setCurrentIndex(index);
        }

        QWidget* paramWidget = bindingTable->cellWidget(row, 3);
        if (paramWidget) {
            if (auto* recorder = paramWidget->findChild<KeyRecorderWidget*>()) {
                recorder->setText(it->payload);
            } else if (auto* edit = paramWidget->findChild<QLineEdit*>()) {
                edit->setText(it->payload);
            }
        }
    }
}

void MainWindow::loadAutostartState() {
    if (!autostartCheck) return;

    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    // 用 applicationFilePath() 而不是 applicationDirPath() + 拼文件名：
    // exe 换名时这里不用改，也不会拼错路径。
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString expected = QStringLiteral("\"%1\"").arg(exe);
    autostartCheck->setChecked(run.value(AppConstants::AUTOSTART_KEY).toString() == expected);
}

bool MainWindow::applyAutostartState() {
    if (!autostartCheck) return false;

    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    const QString key = AppConstants::AUTOSTART_KEY;

    if (autostartCheck->isChecked()) {
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue(key, QStringLiteral("\"%1\"").arg(exe));
    } else {
        run.remove(key);
    }

    // 旧名（mousemouse 时代）留下的启动项：新版界面读的是新 key，看不到它，
    // 而它指向的旧 exe 已经不存在了，留着只会在开机时静默失败 —— 顺手清掉。
    run.remove(QStringLiteral("ItervanceMouse"));

    run.sync();
    return run.status() == QSettings::NoError;
}

void MainWindow::addActionTypeItems(QComboBox* combo) const {
    combo->addItem(QStringLiteral("按键组合"), 0);
    combo->addItem(QStringLiteral("运行程序/打开链接"), 1);
    combo->addItem(QStringLiteral("剪贴板转换"), 2);
    combo->addItem(QStringLiteral("外部脚本"), 3);
    combo->addItem(QStringLiteral("多步流水线"), 4);
    combo->addItem(QStringLiteral("输入文本"), 5);
}

QWidget* MainWindow::makeParamEditor(QComboBox* typeCombo, const QString& initialText) {
    auto* container = new QWidget();
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* edit = new KeyRecorderWidget(container);
    edit->setText(initialText);
    
    // 兼容原来的逻辑，获取内部的 QLineEdit 传给它
    QLineEdit* innerEdit = edit->lineEdit();
    Q_UNUSED(innerEdit);
    auto* browseButton = new QToolButton(container);
    browseButton->setText(QStringLiteral("..."));
    browseButton->setFixedWidth(26);
    browseButton->setToolTip(QStringLiteral("浏览文件 / 编辑流水线 JSON / 查看参数示例"));
    layout->addWidget(edit, 1);
    layout->addWidget(browseButton);

    connect(browseButton, &QToolButton::clicked, this,
            [this, typeCombo, edit] { browseForParam(typeCombo, edit); });

    return container;
}

void MainWindow::browseForParam(QComboBox* typeCombo, KeyRecorderWidget* edit) {
    const int type = typeCombo->currentData().toInt();

    if (type == 1) {
        const QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择程序"), QString(),
            QStringLiteral("可执行文件 (*.exe);;所有文件 (*.*)"));
        if (!file.isEmpty()) edit->setText(quotedPath(file));
        return;
    }

    if (type == 3) {
        const QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择脚本"), QString(),
            QStringLiteral("脚本文件 (*.py *.js *.mjs *.ps1 *.bat *.cmd *.vbs *.exe);;所有文件 (*.*)"));
        if (!file.isEmpty()) edit->setText(quotedPath(file));
        return;
    }

    if (type == 4) {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(
            this, QStringLiteral("多步流水线"),
            QStringLiteral("JSON 数组。示例：\n"
                           "[{\"type\":0,\"payload\":\"Ctrl+C\"},{\"wait\":120},"
                           "{\"type\":5,\"payload\":\"{clipboard}\"}]\n\n"
                           "步骤类型：key/launch/clipboard/script/pipeline/text，"
                           "也可以用数字 0-5。wait 表示等待毫秒数。"),
            edit->text(), &ok);
        if (!ok) return;

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError) {
            QMessageBox::warning(this, QStringLiteral("JSON 格式错误"),
                                 QStringLiteral("%1（位置 %2）")
                                     .arg(error.errorString())
                                     .arg(error.offset));
            return;
        }
        edit->setText(QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
        return;
    }

    if (type == 2) {
        QMessageBox::information(
            this, QStringLiteral("剪贴板转换"),
            QStringLiteral("参数只填操作名：\n\nupper 转大写\nlower 转小写\n"
                           "title 首字母大写\ntrim 去掉首尾空白\nreverse 反转文字"));
        return;
    }

    if (type == 5) {
        QMessageBox::information(
            this, QStringLiteral("输入文本"),
            QStringLiteral("会以 Unicode 方式逐字输入，不受输入法影响。\n\n"
                           "可以使用变量：{clipboard} {selected} {scene} {date} {time} {uuid} "
                           "{newline} {env:NAME}"));
        return;
    }

    QMessageBox::information(
        this, QStringLiteral("按键组合"),
        QStringLiteral("你可以直接在该输入框中点击，然后按下键盘进行录制。\n\n"
                       "支持复杂的序列，例如：Ctrl+Shift+A+S+D（同时按下）。\n\n"
                       "注意：Ctrl+Alt+Del 是系统安全序列，无法模拟。"));
}

QString MainWindow::currentSceneKey() const {
    if (sceneList && sceneList->currentItem()) {
        return sceneList->currentItem()->data(Qt::UserRole).toString();
    }
    return QStringLiteral("global");
}

void MainWindow::saveData() {
    auto warning = [this](const QString& title, const QString& text) {
        QMessageBox::warning(this, title, text);
    };

    // === 修改：增加单键拦截校验 ===
    // 4 个热键各取一次文本，校验和后面 settings.insert 共用同一份值
    const QString hotkey = wheelHotkeyEdit ? wheelHotkeyEdit->text().trimmed() : QString();
    const QString apHotkey = actionPageHotkeyEdit ? actionPageHotkeyEdit->text().trimmed() : QString();
    const QString setHotkey = settingsHotkeyEdit ? settingsHotkeyEdit->text().trimmed() : QString();
    const QString quitHotkey = forceQuitEdit ? forceQuitEdit->text().trimmed() : QString();

    // 全局热键统一校验：格式合法 / 不能是单键 / 只能有 1 个主键
    auto validateHotkey = [&warning](const QString& name, const QString& text) -> bool {
        if (text.isEmpty()) return true; // 留空 = 不启用

        const KeyChord chord = Win32Utils::parseKeySequence(text);
        if (!chord.valid) {
            warning(name + QStringLiteral("无效"), chord.error);
            return false;
        }
        // 不能是"单独一个常规键"，否则等于从系统手里抢走一个打字键
        if (chord.modifiers.isEmpty()
            && (chord.key.vk < VK_F1 || chord.key.vk > VK_F24)) {
            warning(name + QStringLiteral("过于简单"),
                    QStringLiteral("全局快捷键不能是单一常规按键（如字母、空格），请加上 Ctrl/Alt/Shift 等修饰键。"));
            return false;
        }
        // 全局钩子按"修饰键 + 主键"匹配：多主键序列在按下第一个主键时就触发了，
        // 后面那几个键根本匹配不到。这里直接拦掉，别让用户配一个半路被打断的热键
        if (chord.keys.size() > 1) {
            warning(name + QStringLiteral("格式错误"),
                    QStringLiteral("全局快捷键仅支持“修饰键 + 1 个主键”的组合（例如 Ctrl+Alt+S），"
                                   "不支持连续多个主键（例如 Ctrl+Shift+S+H）。\n\n"
                                   "需要多主键的宏或连招，请在【动作】里录制，不要设为全局热键。"));
            return false;
        }
        return true;
    };

    if (!validateHotkey(QStringLiteral("轮盘呼出热键"), hotkey)) return;
    if (!validateHotkey(QStringLiteral("动作页快捷键"), apHotkey)) return;
    if (!validateHotkey(QStringLiteral("呼出设置界面热键"), setHotkey)) return;
    if (!validateHotkey(QStringLiteral("强制退出热键"), quitHotkey)) return;
    // ==============================

    QVector<MouseBinding> mouseBindings;
    for (int row = 0; row < 3; ++row) {
        QTableWidgetItem* buttonItem = bindingTable->item(row, 0);
        if (!buttonItem) continue;

        const QString key = buttonItem->data(Qt::UserRole).toString();
        const QString name = bindingTable->item(row, 1)->text().trimmed();

        auto* typeCombo = qobject_cast<QComboBox*>(bindingTable->cellWidget(row, 2));
        const int type = typeCombo ? typeCombo->currentData().toInt() : 0;

        // 在 saveData() 中
        QString payload;
        if (QWidget* paramWidget = bindingTable->cellWidget(row, 3)) {
            if (auto* recorder = paramWidget->findChild<KeyRecorderWidget*>()) {
                payload = recorder->text().trimmed();
            } else if (auto* edit = paramWidget->findChild<QLineEdit*>()) {
                payload = edit->text().trimmed();
            }
        }

        if (name.isEmpty() && payload.isEmpty()) continue;

        if (payload.isEmpty()) {
            warning(QStringLiteral("鼠标绑定参数为空"),
                    QStringLiteral("%1 的动作参数不能为空。")
                        .arg(mouseButtonDisplayName(mouseButtonFromKey(key))));
            return;
        }

        if (type == 0) {
            const KeyChord chord = Win32Utils::parseKeySequence(payload);
            if (!chord.valid) {
                warning(QStringLiteral("鼠标绑定按键无效"), chord.error);
                return;
            }
        } else if (type == 2) {
            if (!isClipboardOp(payload)) {
                warning(QStringLiteral("鼠标绑定操作无效"),
                        QStringLiteral("只能填 upper / lower / title / trim / reverse。"));
                return;
            }
        } else if (type == 4) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError ||
                !(document.isArray() || document.isObject())) {
                warning(QStringLiteral("鼠标绑定流水线无效"), error.errorString());
                return;
            }
        }

        MouseBinding binding;
        binding.button = key;
        binding.name = name.isEmpty() ? mouseButtonDisplayName(mouseButtonFromKey(key)) : name;
        binding.type = type;
        binding.payload = payload;
        mouseBindings.append(binding);
    }

    const QString mouseTrigger = mouseTriggerCombo
        ? mouseTriggerCombo->currentData().toString()
        : QStringLiteral("right");
    if (mouseTrigger == QLatin1String("none") && hotkey.isEmpty() && mouseBindings.isEmpty()) {
        warning(QStringLiteral("没有触发方式"),
                QStringLiteral("鼠标触发和键盘热键都没有启用，轮盘将无法呼出。\n"
                               "请至少启用一种触发方式，或配置一个鼠标高级绑定。"));
        return;
    }

    QMap<QString, QString> settings;
    // 每个控件都判空再取值：Tab 拆分后控件分散在五个页面里，这里只防意外，不影响正常路径
    if (mouseTriggerCombo) settings.insert(QStringLiteral("trigger.mouse"), mouseTrigger);
    if (wheelHotkeyEdit) settings.insert(QStringLiteral("trigger.keyboard"), hotkey);
    if (thresholdSpin) settings.insert(QStringLiteral("trigger.threshold"), QString::number(thresholdSpin->value()));
    if (longPressSpin) settings.insert(QStringLiteral("trigger.longpress"), QString::number(longPressSpin->value()));
    if (hideSpin) settings.insert(QStringLiteral("trigger.timeout"), QString::number(hideSpin->value()));
    if (scriptTimeoutSpin) settings.insert(QStringLiteral("action.script_timeout"), QString::number(scriptTimeoutSpin->value()));

    // 保存 UI 设置（动作页外观自定义）
    if (apWidthSpin) {
        settings.insert(QStringLiteral("ui.actionpage.width"), QString::number(apWidthSpin->value()));
        settings.insert(QStringLiteral("ui.actionpage.height"), QString::number(apHeightSpin->value()));
        settings.insert(QStringLiteral("ui.actionpage.btn_width"), QString::number(btnWidthSpin->value()));
        settings.insert(QStringLiteral("ui.actionpage.btn_height"), QString::number(btnHeightSpin->value()));
        settings.insert(QStringLiteral("ui.actionpage.radius"), QString::number(radiusSpin->value()));
        settings.insert(QStringLiteral("ui.actionpage.bg_color"), apBgColorEdit->text());
        settings.insert(QStringLiteral("ui.actionpage.btn_bg"), btnBgColorEdit->text());
        settings.insert(QStringLiteral("ui.actionpage.btn_hover_bg"), btnHoverColorEdit->text());
        settings.insert(QStringLiteral("ui.actionpage.text_color"), textColorEdit->text());
    }
    if (apHSpacingSpin) {
        settings.insert(QStringLiteral("ui.actionpage.h_spacing"), QString::number(apHSpacingSpin->value()));
        settings.insert(QStringLiteral("ui.actionpage.v_spacing"), QString::number(apVSpacingSpin->value()));
    }
    if (wheelSizeSpin) {
        settings.insert(QStringLiteral("ui.wheel.size"), QString::number(wheelSizeSpin->value()));
        settings.insert(QStringLiteral("ui.wheel.inner_radius"), QString::number(wheelInnerSpin->value()));
        settings.insert(QStringLiteral("ui.wheel.font_size"), QString::number(wheelFontSpin->value()));
        settings.insert(QStringLiteral("ui.wheel.bg_color"), wheelBgColorEdit->text());
        settings.insert(QStringLiteral("ui.wheel.hover_color"), wheelHoverColorEdit->text());
        settings.insert(QStringLiteral("ui.wheel.line_color"), wheelLineColorEdit->text());
        settings.insert(QStringLiteral("ui.wheel.text_color"), wheelTextColorEdit->text());
        settings.insert(QStringLiteral("ui.wheel.text_hover_color"), wheelTextHoverColorEdit->text());
    }
    
    // 修复：放到 settings 声明之后
    settings.insert(QStringLiteral("trigger.actionpage_keyboard"), apHotkey);

    // 新增：呼出设置界面 / 强制退出快捷键（复用上面校验时的取值，留空表示禁用）
    settings.insert(QStringLiteral("trigger.settings_keyboard"), setHotkey);
    settings.insert(QStringLiteral("trigger.force_quit_keyboard"), quitHotkey);
    // 备份目录随『保存设置』一起落库。留空时不写，避免把默认值覆盖成空串
    if (autoBackupPathEdit && !autoBackupPathEdit->text().trimmed().isEmpty()) {
        settings.insert(QStringLiteral("backup.path"), autoBackupPathEdit->text().trimmed());
    }
    if (autoBackupCheck) {
        settings.insert(QStringLiteral("backup.auto"),
                        autoBackupCheck->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    }

    if (!ActionRepository::saveSettings(settings)) {
        warning(QStringLiteral("保存失败"), QStringLiteral("设置写入失败。"));
        return;
    }

    if (!ActionRepository::saveMouseBindings(mouseBindings)) {
        warning(QStringLiteral("保存失败"), QStringLiteral("鼠标绑定写入失败。"));
        return;
    }

    const bool autostartOk = applyAutostartState();

    AppConfig::instance().reload();
    emit configSaved();

    QMessageBox::information(
        this, QStringLiteral("保存成功"),
        QStringLiteral("触发方式、鼠标绑定和系统设置已保存并立即生效。\n"
                       "轮盘绑定和动作库是拖拽/编辑时即时保存的。%1")
            .arg(autostartOk ? QString()
                             : QStringLiteral("\n\n注意：开机自启写入注册表失败。")));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    hide();
    event->ignore();
}



void MainWindow::loadScenes() {
    if (!sceneList) return;
    
    QString current = currentSceneKey();
    sceneList->blockSignals(true); // 暂时屏蔽信号，防止 clear 时触发 currentItemChanged
    sceneList->clear();

    QListWidgetItem* selectedItem = nullptr;
    for (const auto& pair : ActionRepository::getScenes()) {
        auto* item = new QListWidgetItem(pair.second + QStringLiteral("\n") + pair.first, sceneList);
        item->setData(Qt::UserRole, pair.first);
        if (pair.first == current) {
            selectedItem = item;
        }
    }
    
    if (selectedItem) {
        sceneList->setCurrentItem(selectedItem);
    } else if (sceneList->count() > 0) {
        sceneList->setCurrentRow(0);
    }
    
    sceneList->blockSignals(false);
    loadData(); // 手动触发右侧数据刷新
    applySceneFilter(); // 列表重建后重新套用筛选关键字
}

bool MainWindow::isActionAliveInLibrary(int actionId) const {
    if (actionId <= 0) return false;
    const QVector<LibraryAction> lib = ActionRepository::getActionLibrary();
    for (const auto& a : lib) {
        if (a.id == actionId) return true;
    }
    return false;
}

void MainWindow::applySceneFilter() {
    if (!sceneList) return;
    const QString keyword = sceneFilterEdit ? sceneFilterEdit->text().trimmed() : QString();
    for (int i = 0; i < sceneList->count(); ++i) {
        QListWidgetItem* item = sceneList->item(i);
        if (!item) continue;
        // 条目文本是「显示名 + 换行 + 进程名」，两个都能命中
        const bool match = keyword.isEmpty() ||
                           item->text().contains(keyword, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
}

void MainWindow::addSceneFromWindow() {
    // 提示用户切换窗口
    QMessageBox::information(this, QStringLiteral("添加场景"), 
        QStringLiteral("点击确定后，请在 3 秒内切换到你要添加的目标程序窗口。\n程序将自动抓取该窗口的进程名。"));
    
    this->hide(); // 隐藏主窗口，让用户能看到目标程序
    
    QTimer::singleShot(3000, this, [this]() {
        QString processName = Win32Utils::getForegroundProcessName();
        this->show();
        this->raise();
        this->activateWindow();
        
        // 如果抓取失败，或者抓到了自己
        if (processName.isEmpty() ||
            processName.compare(AppConstants::selfExeName(), Qt::CaseInsensitive) == 0 ||
            processName == QLatin1String("global")) {
            bool ok = false;
            processName = QInputDialog::getText(this, QStringLiteral("手动添加"),
                QStringLiteral("未能抓取到有效窗口。\n请手动输入进程名 (例如 chrome.exe):"),
                QLineEdit::Normal, QString(), &ok);
            if (!ok || processName.trimmed().isEmpty()) return;
        }
        
        bool ok = false;
        QString displayName = QInputDialog::getText(this, QStringLiteral("添加场景"),
            QStringLiteral("进程名: %1\n请输入显示名称:").arg(processName),
            QLineEdit::Normal, processName, &ok);
            
        if (ok && !displayName.trimmed().isEmpty()) {
            if (ActionRepository::saveScene(processName, displayName)) {
                
                // === 新增：询问是否复制全局动作页 ===
                if (QMessageBox::question(this, QStringLiteral("初始化动作页"),
                    QStringLiteral("场景创建成功！\n是否将“全局”场景的第 1 页动作复制到该新场景的第 1 页？\n\n选择“否”将创建一个空白页。"),
                    QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                    
                    ActionRepository::copyPage(QStringLiteral("global"), 0, processName.trimmed().toLower(), 0);
                }
                // ====================================
                
                loadScenes();
                emit configSaved();
                
                // 自动选中刚添加的场景
                QString targetProcess = processName.trimmed().toLower();
                for(int i = 0; i < sceneList->count(); ++i) {
                    if (sceneList->item(i)->data(Qt::UserRole).toString() == targetProcess) {
                        sceneList->setCurrentRow(i);
                        break;
                    }
                }
            } else {
                QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("保存失败，可能已存在。"));
            }
        }
    });
}

void MainWindow::editScene() {
    QListWidgetItem* item = sceneList->currentItem();
    if (!item) return;
    
    QString oldProcessName = item->data(Qt::UserRole).toString();
    QString oldDisplayName = item->text().split("\n").first();
    
    // 内置场景不允许修改进程名，只能改显示名称
    bool isBuiltIn = (oldProcessName == QLatin1String("global"));
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("编辑场景"));
    dlg.setMinimumWidth(450);
    // 关键点：强制窗口置顶，防止用户点击其他窗口时这个对话框被挡住
    dlg.setWindowFlags(dlg.windowFlags() | Qt::WindowStaysOnTopHint);
    
    auto* layout = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    // 1. 进程名编辑
    auto* processRow = new QHBoxLayout();
    auto* processEdit = new QLineEdit(oldProcessName, &dlg);
    processEdit->setEnabled(!isBuiltIn); // 内置场景禁用输入
    processRow->addWidget(processEdit, 1);
    
    auto* grabBtn = new QPushButton(QStringLiteral("抓取当前窗口"), &dlg);
    grabBtn->setVisible(!isBuiltIn);
    processRow->addWidget(grabBtn);
    form->addRow(QStringLiteral("进程名:"), processRow);
    // 2. 显示名称编辑
    auto* displayEdit = new QLineEdit(oldDisplayName, &dlg);
    form->addRow(QStringLiteral("显示名称:"), displayEdit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    // 抓取按钮逻辑（倒计时并获取前台窗口）
    QObject::connect(grabBtn, &QPushButton::clicked, &dlg, [&dlg, processEdit, displayEdit, grabBtn]() {
        grabBtn->setEnabled(false);
        
        auto* timer = new QTimer(&dlg);
        int* secondsLeft = new int(3); // 使用指针以便在 lambda 中修改
        grabBtn->setText(QStringLiteral("请点击目标窗口 (%1s)").arg(*secondsLeft));
        
        QObject::connect(timer, &QTimer::timeout, &dlg, [&dlg, processEdit, displayEdit, grabBtn, timer, secondsLeft]() mutable {
            (*secondsLeft)--;
            if (*secondsLeft > 0) {
                grabBtn->setText(QStringLiteral("请点击目标窗口 (%1s)").arg(*secondsLeft));
            } else {
                timer->stop();
                timer->deleteLater();
                grabBtn->setText(QStringLiteral("抓取当前窗口"));
                grabBtn->setEnabled(true);
                
                // 抓取当前获得焦点的窗口进程名
                QString processName = Win32Utils::getForegroundProcessName().toLower();
                
                if (!processName.isEmpty() &&
                    processName.compare(AppConstants::selfExeName(), Qt::CaseInsensitive) != 0 &&
                    processName != QLatin1String("explorer.exe")) {
                    QString oldName = processEdit->text();
                    processEdit->setText(processName);
                    
                    // 如果原来的显示名称等于旧的进程名，顺便把显示名称也改了
                    if (displayEdit->text() == oldName) {
                        displayEdit->setText(processName);
                    }
                } else {
                    QMessageBox::warning(&dlg, QStringLiteral("抓取失败"), 
                        QStringLiteral("未能抓取到有效窗口（可能点到了桌面或本程序），请手动输入进程名。"));
                }
            }
        });
        timer->start(1000); // 1秒触发一次
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&]() {
        QString newName = processEdit->text().trimmed().toLower();
        QString newDisplayName = displayEdit->text().trimmed();
        
        if (newName.isEmpty() || newDisplayName.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("错误"), QStringLiteral("进程名和显示名称不能为空。"));
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        QString newName = processEdit->text().trimmed().toLower();
        QString newDisplayName = displayEdit->text().trimmed();
        if (oldProcessName != newName && (newName == QLatin1String("global"))) {
            QMessageBox::warning(this, QStringLiteral("修改失败"), QStringLiteral("不能将进程名修改为内置保留字。"));
            return;
        }
        if (!ActionRepository::renameScene(oldProcessName, newName, newDisplayName)) {
            QMessageBox::warning(this, QStringLiteral("修改失败"), 
                QStringLiteral("修改失败，新进程名可能已被其他场景使用。"));
            return;
        }
        
        loadScenes(); // 重新加载左侧列表
        emit configSaved(); // 通知其他界面刷新
    }
}

void MainWindow::deleteScene() {
    QListWidgetItem* item = sceneList->currentItem();
    if (!item) return;
    
    QString processName = item->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, QStringLiteral("删除"), QStringLiteral("确定删除此场景及其所有绑定吗？")) != QMessageBox::Yes) return;
    
    if (ActionRepository::deleteScene(processName)) {
        loadScenes();
        emit configSaved();
    } else {
        QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("内置场景无法删除。"));
    }
}

void MainWindow::triggerAutoBackup() {
    if (!autoBackupCheck || !autoBackupCheck->isChecked()) return;
    if (!autoBackupPathEdit) return;

    const QString backupDir = autoBackupPathEdit->text().trimmed();
    if (backupDir.isEmpty()) return;

    QDir dir(backupDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qWarning() << "自动备份失败：无法创建备份目录" << backupDir;
        return;
    }

    const QString dbPath = AppConstants::getDbPath();
    if (!QFile::exists(dbPath)) return;

    const QString targetFile = dir.filePath(QStringLiteral("data.db"));
    // 备份目录被设成程序目录时，targetFile 就是正在使用的 data.db 本身；
    // 再往下走就是 remove 掉自己再 copy 自己，等于把数据库删了。必须提前挡掉。
    if (QDir::cleanPath(targetFile).compare(QDir::cleanPath(dbPath), Qt::CaseInsensitive) == 0) {
        qWarning() << "自动备份跳过：备份目录与数据库目录相同" << backupDir;
        return;
    }

    // 先 checkpoint，否则 .db 里可能还没包含 WAL 中的最新写入
    QSqlQuery q;
    q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));

    // 先写临时文件再替换：中途任何一步失败，上一次的备份仍然完好
    const QString tmpFile = targetFile + QStringLiteral(".tmp");
    if (QFile::exists(tmpFile) && !QFile::remove(tmpFile)) {
        qWarning() << "自动备份失败：旧的临时文件删不掉" << tmpFile;
        return;
    }
    if (!QFile::copy(dbPath, tmpFile)) {
        qWarning() << "自动备份失败：无法写入临时文件" << tmpFile;
        return;
    }
    if (QFile::exists(targetFile) && !QFile::remove(targetFile)) {
        QFile::remove(tmpFile);
        qWarning() << "自动备份失败：旧备份被占用，无法覆盖" << targetFile;
        return;
    }
    if (!QFile::rename(tmpFile, targetFile)) {
        QFile::remove(tmpFile);
        qWarning() << "自动备份失败：临时文件改名失败" << targetFile;
        return;
    }
    qDebug() << "自动备份成功：" << targetFile;
}

void MainWindow::exportDatabase() {
    const QString dbPath = AppConstants::getDbPath();
    if (!QFile::exists(dbPath)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("数据库文件不存在！"));
        return;
    }

    // 默认落到配置的备份目录；目录被删或配置为空时逐级兜底，最后退到程序目录
    QString defaultDir = autoBackupPathEdit ? autoBackupPathEdit->text().trimmed() : QString();
    if (defaultDir.isEmpty()) {
        defaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (!defaultDir.isEmpty()) defaultDir += QStringLiteral("/Huan_Backup");
    }
    if (defaultDir.isEmpty()) {
        defaultDir = QCoreApplication::applicationDirPath();
    }
    QDir().mkpath(defaultDir); // 目录可能已经被用户删掉，重建出来

    const QString defaultName =
        QDir(defaultDir).filePath(QStringLiteral("Huan_Backup_%1.db")
                                      .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmm")));
    const QString targetFile = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出数据备份"), defaultName,
        QStringLiteral("数据库备份 (*.db);;所有文件 (*.*)"));
    if (targetFile.isEmpty()) return;

    // 先做一次 WAL checkpoint，确保数据全部写入磁盘文件
    QSqlQuery q;
    q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    if (QFile::exists(targetFile)) QFile::remove(targetFile);

    if (QFile::copy(dbPath, targetFile)) {
        QMessageBox::information(this, QStringLiteral("导出成功"),
            QStringLiteral("数据已成功导出至：\n%1\n\n后续版本升级后，旧备份会自动兼容并补齐新配置。").arg(targetFile));
    } else {
        QMessageBox::critical(this, QStringLiteral("导出失败"), QStringLiteral("无法写入目标文件，请检查权限。"));
    }
}

void MainWindow::importDatabase() {
    const QString sourceFile = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择数据备份"), QString(), 
        QStringLiteral("数据库备份 (*.db);;所有文件 (*.*)"));
        
    if (sourceFile.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("确认导入"), 
        QStringLiteral("导入将覆盖当前所有的动作、场景和设置，是否继续？\n（程序会自动备份您当前的数据）")) != QMessageBox::Yes) {
        return;
    }
    // 1. 关闭数据库释放文件锁
    ActionRepository::closeDatabase();
    const QString dbPath = AppConstants::getDbPath();
    const QString bakPath = dbPath + QStringLiteral(".bak");
    // 2. 备份当前数据
    if (QFile::exists(dbPath)) {
        QFile::remove(bakPath); // 删掉旧的 bak
        QFile::rename(dbPath, bakPath);
    }
    // 关库之后残留的 WAL/SHM 属于旧库。不清掉的话，新库一打开 SQLite 会把这些残留当成
    // 新库未提交的事务重放，导入后的数据就串了（表现是"导入成功但内容还是旧的"）。
    QFile::remove(dbPath + QStringLiteral("-wal"));
    QFile::remove(dbPath + QStringLiteral("-shm"));
    // 3. 覆盖数据库文件
    if (!QFile::copy(sourceFile, dbPath)) {
        // 恢复备份
        if (QFile::exists(bakPath)) QFile::rename(bakPath, dbPath);
        QMessageBox::critical(this, QStringLiteral("导入失败"), QStringLiteral("无法写入数据库文件，请检查权限。"));
        ActionRepository::init(dbPath); // 重新初始化
        return;
    }
    // 4. 重新初始化数据库（会自动完成旧版表结构到新版的迁移）
    if (!ActionRepository::init(dbPath)) {
        QMessageBox::critical(this, QStringLiteral("导入失败"), QStringLiteral("导入的数据库损坏或版本不兼容。"));
        return;
    }
    // 5. 重新加载配置和界面
    AppConfig::instance().reload();
    refreshAll();
    loadTriggerSettings();
    loadMouseBindings();
    loadAutostartState();
    loadScenes();
    emit configSaved();
    QMessageBox::information(this, QStringLiteral("导入成功"), 
        QStringLiteral("数据已成功恢复并重新加载。\n旧数据已备份为 data.db.bak"));
}

// ================= 新建动作页通用弹窗（MainWindow 与 ActionPage 共用） =================
// 注意：必须放在全局作用域（不能放进上面的匿名 namespace），否则 ActionPage.cpp 无法链接到它。
int showNewPageDialog(QWidget* parent, const QString& currentScene) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("新建动作页"));
    dlg.setMinimumWidth(320);

    auto* layout = new QVBoxLayout(&dlg);

    auto* sourceLabel = new QLabel(QStringLiteral("内容来源:"), &dlg);
    auto* comboSource = new QComboBox(&dlg);
    comboSource->addItem(QStringLiteral("创建空白页"), QStringLiteral("empty"));
    comboSource->addItem(QStringLiteral("从全局复制"), QStringLiteral("global"));
    for (const auto& pair : ActionRepository::getScenes()) {
        const QString key = pair.first;
        if (key == currentScene || key == QLatin1String("global")) continue;
        comboSource->addItem(QStringLiteral("从场景 [%1] 复制").arg(pair.second), key);
    }

    auto* pageLabel = new QLabel(QStringLiteral("源页码:"), &dlg);
    auto* comboPage = new QComboBox(&dlg);
    pageLabel->setVisible(false);
    comboPage->setVisible(false);

    // 来源切换时刷新页码下拉框：源场景只有 1 页时不必让用户选
    QObject::connect(comboSource, &QComboBox::currentIndexChanged, &dlg, [&]() {
        const QString src = comboSource->currentData().toString();
        comboPage->clear();
        if (src == QLatin1String("empty")) {
            pageLabel->setVisible(false);
            comboPage->setVisible(false);
            return;
        }
        const int pages = ActionRepository::getScenePageCount(src);
        if (pages > 1) {
            for (int i = 0; i < pages; ++i) {
                comboPage->addItem(QStringLiteral("第 %1 页").arg(i + 1), i);
            }
        } else {
            comboPage->addItem(QStringLiteral("第 1 页"), 0);
        }
        pageLabel->setVisible(pages > 1);
        comboPage->setVisible(pages > 1);
    });

    layout->addWidget(sourceLabel);
    layout->addWidget(comboSource);
    layout->addWidget(pageLabel);
    layout->addWidget(comboPage);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return -1;

    // 保证 scenes 表里有当前场景行，否则下面 UPDATE page_count 不会生效（新页"看不见"）
    if (!currentScene.isEmpty()) {
        QSqlQuery ensure;
        ensure.prepare(
            QStringLiteral("INSERT OR IGNORE INTO scenes (process_name, display_name) VALUES (?, ?)"));
        ensure.addBindValue(currentScene);
        ensure.addBindValue(currentScene == QLatin1String("global") ? QStringLiteral("全局")
                                                                   : currentScene);
        ensure.exec();
    }

    const int newPageIndex = ActionRepository::getScenePageCount(currentScene);
    ActionRepository::setScenePageCount(currentScene, newPageIndex + 1);

    const QString src = comboSource->currentData().toString();
    if (src != QLatin1String("empty")) {
        const int srcPage = comboPage->currentData().toInt();
        ActionRepository::copyPage(src, srcPage, currentScene, newPageIndex);
    }
    return newPageIndex;
}

// KeyRecorderWidget 带 Q_OBJECT 且定义在本 .cpp 中，AUTOMOC 要求在此 include 其 moc 输出
#include "MainWindow.moc"

