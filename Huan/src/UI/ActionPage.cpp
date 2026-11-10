#include "ActionPage.h"
#include "ActionEngine.h"
#include "ActionRepository.h"
#include "AppConfig.h"
#include "Constants.h"
#include "Win32Utils.h"

#include <QCloseEvent>
#include <QCursor>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QDragEnterEvent>
#include <QDrag>
#include <QApplication>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QCoreApplication>
#include <QIcon>
#include <QMenu>             // 新增
#include <QAction>          // 新增
#include <QDialog>          // 新增
#include <QFormLayout>      // 新增
#include <QComboBox>        // 新增
#include <QDialogButtonBox> // 新增

// 定义在 MainWindow.cpp（全局作用域），呼出动作页与设置界面共用同一个"新建动作页"弹窗
int showNewPageDialog(QWidget* parent, const QString& currentScene);
#include <QMessageBox>      // 新增

namespace {
constexpr char kActionMime[] = "application/x-huan-action-id";
// 格子间拖拽交换：携带源格子索引
constexpr char kPageIndexMime[] = "application/x-huan-page-index";
constexpr int kCols = 4;
constexpr int kRows = 6;
constexpr int kPageSize = kCols * kRows;
constexpr int kCollapsedSize = 64; // 桌面图标大小

// 给单色图标染色。QPushButton 样式里的 color 只作用于文字，对 QIcon 完全无效，
// 所以"已固定"的蓝色必须直接染到 pixmap 上，改 QSS 是没用的。
QPixmap tintedIcon(const QString& fileName, const QColor& color) {
    const QPixmap src(AppConstants::getUiIconPath(fileName));
    if (src.isNull()) return QPixmap();
    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), color);
    p.end();
    return out;
}

// 固定按钮图标：优先用成对素材（pin_pinned.png / pin_unpinned.png）；
// 成对素材缺失时，拿 pin_unpinned.png 的轮廓分别染成蓝 / 灰兜底 —— 单色图也能看出状态。
void applyPinIcon(QPushButton* btn, bool pinned) {
    if (!btn) return;
    const QPixmap custom(AppConstants::getUiIconPath(
        pinned ? QStringLiteral("pin_pinned.png") : QStringLiteral("pin_unpinned.png")));
    if (!custom.isNull()) {
        btn->setIcon(QIcon(custom));
        return;
    }
    const QPixmap tinted = tintedIcon(QStringLiteral("pin_unpinned.png"),
                                      pinned ? QColor(0x19, 0x76, 0xD2)
                                             : QColor(0x5F, 0x63, 0x68));
    if (!tinted.isNull()) btn->setIcon(QIcon(tinted));
}

QString typeName(int type) {
    switch (type) {
    case 0: return QStringLiteral("按键");
    case 1: return QStringLiteral("程序");
    case 2: return QStringLiteral("剪贴板");
    case 3: return QStringLiteral("脚本");
    case 4: return QStringLiteral("流水线");
    case 5: return QStringLiteral("文本");
    default: return QStringLiteral("");
    }
}

// 轻量级动作编辑弹窗
class PageActionEditDialog : public QDialog {
public:
    PageActionEditDialog(QWidget* parent, const LibraryAction& initial, bool isNew)
        : QDialog(parent), m_action(initial) {
        setWindowTitle(isNew ? QStringLiteral("新增动作") : QStringLiteral("编辑动作"));
        resize(400, 200);
        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout();
        
        m_name = new QLineEdit(initial.name, this);
        form->addRow(QStringLiteral("名字:"), m_name);

        m_type = new QComboBox(this);
        m_type->addItem(QStringLiteral("按键组合"), 0);
        m_type->addItem(QStringLiteral("运行程序/打开链接"), 1);
        m_type->addItem(QStringLiteral("剪贴板转换"), 2);
        m_type->addItem(QStringLiteral("外部脚本"), 3);
        m_type->addItem(QStringLiteral("多步流水线"), 4);
        m_type->addItem(QStringLiteral("输入文本"), 5);
        form->addRow(QStringLiteral("类型:"), m_type);

        m_payload = new QLineEdit(initial.payload, this);
        form->addRow(QStringLiteral("参数:"), m_payload);

        layout->addLayout(form);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, [this] { 
            if (m_name->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, "错误", "名字不能为空");
                return;
            }
            accept(); 
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        int idx = m_type->findData(initial.type >= 0 ? initial.type : 0);
        m_type->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    LibraryAction result() const {
        LibraryAction a = m_action;
        a.name = m_name->text().trimmed();
        a.type = m_type->currentData().toInt();
        a.payload = m_payload->text().trimmed();
        return a;
    }
private:
    LibraryAction m_action;
    QLineEdit* m_name;
    QComboBox* m_type;
    QLineEdit* m_payload;
};

class ActionButton : public QPushButton {
public:
    ActionButton(int index, int actionId = -1, QWidget* parent = nullptr)
        : QPushButton(parent), m_index(index), m_actionId(actionId) {
        setAcceptDrops(true);
    }
    std::function<void(int index, int actionId)> onActionDropped;
    std::function<void(int srcIndex, int dstIndex)> onActionSwapped;

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
        if (m_actionId <= 0) return;

        QDrag* drag = new QDrag(this);
        QMimeData* mimeData = new QMimeData;
        mimeData->setData(kActionMime, QByteArray::number(m_actionId));
        mimeData->setData(kPageIndexMime, QByteArray::number(m_index));
        drag->setMimeData(mimeData);
        drag->exec(Qt::CopyAction | Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(kPageIndexMime) ||
            event->mimeData()->hasFormat(kActionMime)) {
            event->acceptProposedAction();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasFormat(kPageIndexMime) ||
            event->mimeData()->hasFormat(kActionMime)) {
            event->acceptProposedAction();
        }
    }
    void dropEvent(QDropEvent* event) override {
        // 优先处理"页内格子交换"
        if (event->mimeData()->hasFormat(kPageIndexMime)) {
            const int srcIndex = event->mimeData()->data(kPageIndexMime).toInt();
            if (srcIndex >= 0 && srcIndex != m_index && onActionSwapped) {
                onActionSwapped(srcIndex, m_index);
            }
            event->acceptProposedAction();
            return;
        }
        if (event->mimeData()->hasFormat(kActionMime)) {
            int actionId = event->mimeData()->data(kActionMime).toInt();
            if (actionId > 0 && onActionDropped) onActionDropped(m_index, actionId);
            event->acceptProposedAction();
        }
    }
private:
    int m_index;
    int m_actionId = -1;
    QPoint m_dragStartPosition;
};
} // namespace

ActionPage::ActionPage(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // --- 1. 折叠时的图标 ---
    m_logoLabel = new QLabel(this);
    m_logoLabel->setAlignment(Qt::AlignCenter);
    m_logoLabel->setStyleSheet("background: transparent;");
    
    // 统一走 Constants，折叠态 logo 与托盘/窗口图标同一个文件
    QIcon icon(AppConstants::getMainIconPath());
    if (!icon.isNull()) {
        m_logoLabel->setPixmap(icon.pixmap(kCollapsedSize, kCollapsedSize));
    } else {
        m_logoLabel->setText("MM"); 
        m_logoLabel->setStyleSheet("color: #1976D2; font-weight: bold; font-size: 20px; background: transparent;");
    }
    m_logoLabel->hide();
    mainLayout->addWidget(m_logoLabel);

    // --- 2. 展开时的面板 ---
    m_container = new QWidget(this);
    m_container->setObjectName("ActionPageMain");
    m_container->setStyleSheet(QStringLiteral(
        "QWidget#ActionPageMain { background-color: #F8F9FA; border-radius: 8px; border: 1px solid #D0D0D0; }"
        "QLabel { background: transparent; }"
    ));
    
    auto* layout = new QVBoxLayout(m_container);
    layout->setContentsMargins(10, 10, 10, 10);

    // 顶部标题栏
    auto* topBar = new QHBoxLayout();
    m_titleLabel = new QLabel(this);
    m_titleLabel->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: bold; color: #333;"));
    
    // 获取图标路径的辅助 Lambda：统一走 Constants，不再自己拼路径
    auto getIcon = [](const QString& name) {
        return QIcon(AppConstants::getUiIconPath(name));
    };
    m_pinBtn = new QPushButton(this);
    m_collapseBtn = new QPushButton(this);
    m_closeBtn = new QPushButton(this);
    // 加载图标。固定按钮走 applyPinIcon：固定=蓝、未固定=灰，缺素材也能看出状态
    QIcon colIcon = getIcon("collapse.png");
    QIcon clsIcon = getIcon("close.png");
    applyPinIcon(m_pinBtn, false);
    
    if (!colIcon.isNull()) m_collapseBtn->setIcon(colIcon);
    else m_collapseBtn->setText(QStringLiteral("➖"));
    
    if (!clsIcon.isNull()) m_closeBtn->setIcon(clsIcon);
    else m_closeBtn->setText(QStringLiteral("✖"));
    m_pinBtn->setIconSize(QSize(16, 16));
    m_collapseBtn->setIconSize(QSize(16, 16));
    m_closeBtn->setIconSize(QSize(16, 16));
    // 换成图标之后按钮上没有文字了，tooltip 就是唯一的说明
    m_pinBtn->setToolTip(QStringLiteral("固定：保持动作页展开"));
    m_collapseBtn->setToolTip(QStringLiteral("折叠为桌面图标"));
    m_closeBtn->setToolTip(QStringLiteral("关闭动作页"));
    // 按钮通用样式：去边框、悬停反馈
    QString btnBaseStyle = QStringLiteral(
        "QPushButton { border: none; background: transparent; border-radius: 4px; padding: 2px; }"
        "QPushButton:hover { background: #E0E0E0; }"
    );
    m_pinBtn->setStyleSheet(btnBaseStyle);
    m_collapseBtn->setStyleSheet(btnBaseStyle);
    m_closeBtn->setStyleSheet(btnBaseStyle + QStringLiteral("QPushButton:hover { background: #FFCDD2; }"));
    
    m_pinBtn->setFixedSize(24, 24);
    m_collapseBtn->setFixedSize(24, 24);
    m_closeBtn->setFixedSize(24, 24);

    topBar->addWidget(m_titleLabel, 1);
    topBar->addWidget(m_pinBtn);
    topBar->addWidget(m_collapseBtn);
    topBar->addWidget(m_closeBtn);
    layout->addLayout(topBar);

    // 网格区
    m_gridHost = new QWidget(this);
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(4);
    layout->addWidget(m_gridHost, 1);

    // 底部翻页栏
    auto* bottom = new QHBoxLayout();
    m_prevBtn = new QPushButton(this);
    m_nextBtn = new QPushButton(this);
    
    // 新增：新建页按钮
    m_addPageBtn = new QPushButton(QStringLiteral("+"), this);
    m_addPageBtn->setFixedSize(24, 24);
    m_addPageBtn->setStyleSheet(QStringLiteral("QPushButton { border: none; background: #EAEAEA; border-radius: 12px; color: #555; font-weight: bold; } QPushButton:hover { background: #D0D0D0; }"));
    connect(m_addPageBtn, &QPushButton::clicked, this, &ActionPage::addNewPage);
    
    m_pageLabel = new QLabel(QStringLiteral("1 / 1"), this);
    
    QIcon prevIcon = getIcon("prev.png");
    QIcon nextIcon = getIcon("next.png");
    if (!prevIcon.isNull()) m_prevBtn->setIcon(prevIcon);
    else m_prevBtn->setText(QStringLiteral("◀"));
    
    if (!nextIcon.isNull()) m_nextBtn->setIcon(nextIcon);
    else m_nextBtn->setText(QStringLiteral("▶"));
    m_prevBtn->setIconSize(QSize(16, 16));
    m_nextBtn->setIconSize(QSize(16, 16));
    
    // 箭头按钮样式（如果有图标，背景透明更清爽；没图标则显示圆形背景）
    QString arrowStyle = prevIcon.isNull() ? 
        QStringLiteral("QPushButton { border: none; background: #EAEAEA; border-radius: 14px; color: #555; font-weight: bold; min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; } QPushButton:hover { background: #D0D0D0; } QPushButton:disabled { background: transparent; color: #CCC; }") :
        QStringLiteral("QPushButton { border: none; background: transparent; border-radius: 4px; padding: 4px; } QPushButton:hover { background: #E0E0E0; } QPushButton:disabled { opacity: 0.5; }");
    m_prevBtn->setStyleSheet(arrowStyle);
    m_nextBtn->setStyleSheet(arrowStyle);
    m_prevBtn->setFixedSize(28, 28);
    m_nextBtn->setFixedSize(28, 28);
    m_pageLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: bold; color: #666;"));
    m_pageLabel->setAlignment(Qt::AlignCenter);
    
    bottom->addWidget(m_prevBtn);
    bottom->addStretch();
    bottom->addWidget(m_addPageBtn); // 放在页码左边
    bottom->addWidget(m_pageLabel);
    bottom->addStretch();
    bottom->addWidget(m_nextBtn);
    layout->addLayout(bottom);

    mainLayout->addWidget(m_container);

    connect(m_prevBtn, &QPushButton::clicked, this, [this] { switchPage(-1); });
    connect(m_nextBtn, &QPushButton::clicked, this, [this] { switchPage(1); });
    connect(m_pinBtn, &QPushButton::clicked, this, &ActionPage::togglePin);
    connect(m_collapseBtn, &QPushButton::clicked, this, &ActionPage::toggleCollapse);
    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::hide);

    m_collapseTimer = new QTimer(this);
    m_collapseTimer->setSingleShot(true);
    connect(m_collapseTimer, &QTimer::timeout, this, [this] {
        if (!m_isPinned && !m_isCollapsed && underMouse() == false) {
            setCollapsed(true);
        }
    });

    applyTheme(); // 构造完成后应用配置的尺寸与样式
}

void ActionPage::showAtCursor(const QString& scene) {
    m_scene = scene;
    // 自己（Huan.exe）和资源管理器都算"没有明确场景"，退回 global。
    // exe 名走 selfExeName()，以后再改名也不用回来动这一行。
    if (m_scene.compare(AppConstants::selfExeName(), Qt::CaseInsensitive) == 0 ||
        m_scene == QLatin1String("explorer.exe")) {
        m_scene = QStringLiteral("global");
    }

    m_currentPage = 0;
    m_previousHwnd = reinterpret_cast<quintptr>(GetForegroundWindow());
    m_titleLabel->setText(QStringLiteral("场景: %1").arg(m_scene));
    
    refresh();
    
    if (m_isCollapsed) {
        setCollapsed(false);
    }

    if (!isVisible()) {
        const QPoint cursor = QCursor::pos();
        QPoint target = cursor + QPoint(16, 16);
        if (QScreen* screen = QGuiApplication::screenAt(cursor)) {
            const QRect available = screen->availableGeometry();
            target.setX(qBound(available.left(), target.x(), available.right() - width()));
            target.setY(qBound(available.top(), target.y(), available.bottom() - height()));
        }
        move(target);
        show();
    }
    raise();
    activateWindow();
}

void ActionPage::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_isDragging = true;
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
    }
}

void ActionPage::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragPosition);
        event->accept();
    }
}

void ActionPage::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_isDragging = false;
    }
}

void ActionPage::togglePin() {
    m_isPinned = !m_isPinned;
    // 原来这里靠 setStyleSheet 改 color 表达"已固定"，但按钮显示的是 QIcon，
    // color 只对文字生效 —— 图标一直没变过。改成直接换染色的 pixmap。
    applyPinIcon(m_pinBtn, m_isPinned);
    m_pinBtn->setStyleSheet(QStringLiteral(
        "QPushButton { border: none; background: transparent; border-radius: 4px; padding: 2px; }"
        "QPushButton:hover { background: #E0E0E0; }"));
    if (m_isPinned) {
        m_collapseTimer->stop();
        setCollapsed(false);
    }
}

void ActionPage::toggleCollapse() {
    setCollapsed(!m_isCollapsed);
}

void ActionPage::setCollapsed(bool collapsed) {
    m_isCollapsed = collapsed;
    if (m_isCollapsed) {
        setFixedSize(kCollapsedSize, kCollapsedSize); // 折叠时保持固定小尺寸
        m_container->hide();
        m_logoLabel->show();
    } else {
        int width = AppConfig::instance().intValue(QStringLiteral("ui.actionpage.width"), 500, 300, 1920);
        int height = AppConfig::instance().intValue(QStringLiteral("ui.actionpage.height"), 680, 300, 1080);
        setFixedSize(width, height);
        m_logoLabel->hide();
        m_container->show();
    }
}

void ActionPage::applyTheme() {
    const AppConfig& config = AppConfig::instance();
    
    // 1. 读取并应用面板大小
    int width = config.intValue(QStringLiteral("ui.actionpage.width"), 500, 300, 1920);
    int height = config.intValue(QStringLiteral("ui.actionpage.height"), 680, 300, 1080);
    if (!m_isCollapsed) {
        setFixedSize(width, height);
    }
    // 2. 读取颜色
    QString bgColor = config.value(QStringLiteral("ui.actionpage.bg_color"), "#F8F9FA");
    int radius = config.intValue(QStringLiteral("ui.actionpage.radius"), 6, 0, 30);
    // 3. 应用主面板样式
    m_container->setStyleSheet(QStringLiteral(
        "QWidget#ActionPageMain { background-color: %1; border-radius: %2px; border: 1px solid #D0D0D0; }"
        "QLabel { background: transparent; }"
    ).arg(bgColor).arg(radius));
    // 4. 刷新网格以应用按钮样式
    refresh();
}

void ActionPage::enterEvent(QEnterEvent* event) {
    m_collapseTimer->stop();
    if (m_isCollapsed) {
        setCollapsed(false);
    }
    QWidget::enterEvent(event);
}

void ActionPage::leaveEvent(QEvent* event) {
    if (!m_isPinned && isVisible()) {
        m_collapseTimer->start(1500);
    }
    QWidget::leaveEvent(event);
}

void ActionPage::refresh() {
    if (m_gridHost->layout()) {
        QLayoutItem* item;
        while ((item = m_grid->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
    }

    // 1. 检查场景回退
    QString targetScene = m_scene;
    QMap<int, int> bindings = ActionRepository::getPageBindings(targetScene, m_currentPage);
    
    if (bindings.isEmpty() && targetScene != QLatin1String("global")) {
        targetScene = QStringLiteral("global");
        bindings = ActionRepository::getPageBindings(targetScene, m_currentPage);
        m_titleLabel->setText(QStringLiteral("场景: %1 (继承全局)").arg(m_scene));
    } else {
        m_titleLabel->setText(QStringLiteral("场景: %1").arg(m_scene));
    }
    // 2. 紧凑布局控制 (如果配置没自定义，这里把默认间距改小)
    int hSpacing = AppConfig::instance().intValue(QStringLiteral("ui.actionpage.h_spacing"), 4, 0, 50);
    int vSpacing = AppConfig::instance().intValue(QStringLiteral("ui.actionpage.v_spacing"), 4, 0, 50);
    
    m_grid->setHorizontalSpacing(hSpacing);
    m_grid->setVerticalSpacing(vSpacing);
    m_grid->setContentsMargins(4, 4, 4, 4); // 边缘保持一点微小留白
    QVector<LibraryAction> library = ActionRepository::getActionLibrary();
    QMap<int, LibraryAction> libMap;
    for (const auto& a : library) libMap.insert(a.id, a);
    // 读取配置的颜色和尺寸
    const AppConfig& config = AppConfig::instance();
    int btnW = config.intValue(QStringLiteral("ui.actionpage.btn_width"), 100, 50, 300);
    int btnH = config.intValue(QStringLiteral("ui.actionpage.btn_height"), 70, 40, 200);
    QString btnBg = config.value(QStringLiteral("ui.actionpage.btn_bg"), "#FFFFFF");
    QString btnHoverBg = config.value(QStringLiteral("ui.actionpage.btn_hover_bg"), "#eaf3ff");
    QString textColor = config.value(QStringLiteral("ui.actionpage.text_color"), "#333333");
    int radius = config.intValue(QStringLiteral("ui.actionpage.radius"), 6, 0, 30);
    QString btnStyle = QStringLiteral(
        "QPushButton { text-align: center; border: 1px solid #d0d0d0; border-radius: %1px; "
        "background: %2; color: %3; padding: 2px; } "
        "QPushButton:hover { background: %4; border-color: #0078D7; }"
    ).arg(radius).arg(btnBg, textColor, btnHoverBg);

    for (int i = 0; i < kPageSize; ++i) {
        int r = i / kCols;
        int c = i % kCols;

        const int boundId = bindings.value(i, -1);
        auto* btn = new ActionButton(i, boundId, m_gridHost);
        btn->setMinimumSize(btnW, btnH);
        
        // === 新增：强制按钮尺寸策略，防止被长文字撑大 ===
        btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        // 强制每一列平分宽度
        m_grid->setColumnStretch(c, 1); 
        
        btn->setStyleSheet(btnStyle);

        if (bindings.contains(i)) {
            int actionId = bindings.value(i);
            if (libMap.contains(actionId)) {
                LibraryAction act = libMap.value(actionId);

                // 优化：使用 QFontMetrics 截断过长的文字，防止换行破坏排版
                QFontMetrics metrics(btn->font());
                QString displayText = QStringLiteral("%1\n(%2)").arg(act.name, typeName(act.type));
                // 你可以根据需要调整截断的宽度 (btnW - 10)
                // btn->setText(metrics.elidedText(displayText, Qt::ElideRight, btnW - 10)); 
                // 但如果希望保留两行显示，直接 setText 也可以，因为设置了 Ignored，按钮不会被撑大，只会内部显示省略号
                btn->setText(displayText);
                
                connect(btn, &QPushButton::clicked, this, [this, act] {
                    Action a;
                    a.id = act.id; // 不带 id 的话引擎那边统计不到，点了也白点
                    a.slot = -1;
                    a.name = act.name;
                    a.type = act.type;
                    a.payload = act.payload;
                    execute(a);
                });
            }
        } else {
            btn->setText(QStringLiteral("空白"));
            btn->setStyleSheet(btnStyle + "QPushButton { color: #aaa; background: #FAFAFA; }");
        }

        btn->onActionDropped = [this](int idx, int actionId) {
            ActionRepository::bindActionToPage(m_currentPage, idx, m_scene, actionId);
            refresh();
        };

        // 页内拖拽交换：源格子与目标格子对调，任一端为空则解绑该端
        btn->onActionSwapped = [this](int srcIdx, int dstIdx) {
            if (srcIdx == dstIdx) return;
            const QMap<int, int> current = ActionRepository::getPageBindings(m_scene, m_currentPage);
            const int srcActionId = current.value(srcIdx, -1);
            const int dstActionId = current.value(dstIdx, -1);

            if (dstActionId > 0) {
                ActionRepository::bindActionToPage(m_currentPage, srcIdx, m_scene, dstActionId);
            } else {
                ActionRepository::unbindPageSlot(m_currentPage, srcIdx, m_scene);
            }
            if (srcActionId > 0) {
                ActionRepository::bindActionToPage(m_currentPage, dstIdx, m_scene, srcActionId);
            } else {
                ActionRepository::unbindPageSlot(m_currentPage, dstIdx, m_scene);
            }
            refresh();
            emit configSaved();
        };
        
        m_grid->addWidget(btn, r, c);
    }
    
    for (int r = 0; r < kRows; ++r) {
        m_grid->setRowStretch(r, 1);
    }

    // 3. 页码显示 "1 / N"
    int totalPages = ActionRepository::getScenePageCount(targetScene);
    if (m_currentPage >= totalPages) {
        m_currentPage = totalPages - 1;
    }
    m_pageLabel->setText(QStringLiteral("%1 / %2").arg(m_currentPage + 1).arg(totalPages));
    m_prevBtn->setEnabled(m_currentPage > 0);
    m_nextBtn->setEnabled(m_currentPage + 1 < totalPages);
}


void ActionPage::addNewPage() {
    // 防御：当前场景为空时回退到 global，防止底层 setScenePageCount 静默失败
    if (m_scene.isEmpty()) {
        m_scene = QStringLiteral("global");
    }
    int newPageIndex = showNewPageDialog(this, m_scene);
    if (newPageIndex >= 0) {
        m_currentPage = newPageIndex;
        refresh();
        emit configSaved();
    }
}

void ActionPage::switchPage(int delta) {
    int newPage = m_currentPage + delta;
    if (newPage >= 0) {
        m_currentPage = newPage;
        refresh();
    }
}

void ActionPage::execute(const Action& action) {
    const quintptr target = m_previousHwnd;
    const QString scene = m_scene;

    // 关键：无论是否置顶，执行前都必须隐藏自己，否则按键会发给面板自己
    bool wasPinned = m_isPinned;
    hide(); 

    if (target != 0) {
        Win32Utils::forceForegroundWindow(reinterpret_cast<HWND>(target));
    }

    QTimer::singleShot(150, this, [this, action, scene, wasPinned] {
        ActionEngine::executeAction(action, scene);
        // 如果之前是置顶状态，执行完后重新显示出来
        if (wasPinned && m_isPinned) {
            show();
            raise();
        }
    });
}

void ActionPage::closeEvent(QCloseEvent* event) {
    hide();
    event->ignore();
}

void ActionPage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QWidget::keyPressEvent(event);
}