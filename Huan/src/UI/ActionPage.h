#pragma once
#include <QWidget>
#include <QVector>
#include <QLabel>
#include <QTimer>
#include "ActionModel.h"
#include <windows.h>

class QGridLayout;
class QPushButton;

class ActionPage : public QWidget {
    Q_OBJECT
public:
    explicit ActionPage(QWidget* parent = nullptr);
    void applyTheme(); // 新增

    void refresh();
    void showAtCursor(const QString& scene);
    bool isCollapsed() const { return m_isCollapsed; }
    bool isPinned() const { return m_isPinned; }

signals:
    void configSaved();

public slots:
    void addNewPage();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    
    // 无边框拖动和折叠逻辑
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void switchPage(int delta);
    void execute(const Action& action);
    void togglePin();
    void toggleCollapse();
    void setCollapsed(bool collapsed);

    // UI 组件
    QWidget* m_container = nullptr;    // 展开时的完整面板
    QLabel* m_logoLabel = nullptr;     // 折叠时显示的桌面图标
    QLabel* m_titleLabel = nullptr;
    QPushButton* m_pinBtn = nullptr;
    QPushButton* m_collapseBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QGridLayout* m_grid = nullptr;
    QWidget* m_gridHost = nullptr;
    QPushButton* m_prevBtn = nullptr;
    QPushButton* m_nextBtn = nullptr;
    QPushButton* m_addPageBtn = nullptr;
    QLabel* m_pageLabel = nullptr;

    QString m_scene = QStringLiteral("global");
    int m_currentPage = 0;
    quintptr m_previousHwnd = 0;

    // 状态控制
    bool m_isPinned = false;       // 是否常驻置顶
    bool m_isCollapsed = false;    // 是否折叠为 Logo
    bool m_isDragging = false;     // 是否正在拖动
    QPoint m_dragPosition;
    QTimer* m_collapseTimer = nullptr; // 鼠标离开后自动折叠的定时器
};