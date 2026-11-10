#pragma once

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVector>
#include <QWidget>
#include <QGridLayout>

#include "ActionModel.h"

class WheelEditorWidget;
class KeyRecorderWidget;
class QTimer;
class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // 重新读取动作库和当前场景绑定（外部保存后调用）。
    void refreshAll();

signals:
    void configSaved();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void loadData(); // 加载当前场景的轮盘绑定
    void saveData(); // 保存触发/系统设置
    void copyGlobalToScene();
    void addLibraryAction();
    void editLibraryAction();
    void deleteLibraryAction();
    void refreshLibrary();
    
    void loadScenes();
    void addSceneFromWindow();
    void editScene();
    void deleteScene();
    void exportDatabase();
    void importDatabase();

private:
    QWidget* buildSceneTab();         // 主界面：场景列表 + 轮盘预览 + 动作页网格（三栏联动）
    QWidget* buildWheelTab();         // 设置：轮盘触发参数 + 轮盘外观
    QWidget* buildMouseTriggerTab();  // 高级鼠标触发（按键直接执行动作）
    QWidget* buildActionPageTab();    // 设置：动作页呼出快捷键 + 动作页外观
    QWidget* buildSystemTab();        // 系统：开机自启、全局热键、数据备份
    QWidget* buildRankingTab();       // 排行：动作使用次数热度榜
    void refreshRanking();            // 清空并重填排行卡片（切到该页时调用）

    void loadTriggerSettings();
    void loadMouseBindings();
    void loadAutostartState();
    bool applyAutostartState();

    void applyBinding(int slot, int actionId);
    void selectLibraryRow(int actionId);

    QString currentSceneKey() const;
    void applySceneFilter(); // 按筛选框关键字隐藏不匹配的场景条目
    bool isActionAliveInLibrary(int actionId) const; // 剪贴板里的动作是否还在动作库中
    void addActionTypeItems(QComboBox* combo) const;
    QWidget* makeParamEditor(QComboBox* typeCombo, const QString& initialText);
    void browseForParam(QComboBox* typeCombo, KeyRecorderWidget* edit);
    void triggerAutoBackup(); // 自动备份：把 data.db 覆写到备份目录

    // 主窗口骨架：左侧垂直导航（场景/轮盘/高级鼠标触发/动作页/系统）+ 右侧页面栈
    QListWidget* m_navList = nullptr;
    QStackedWidget* m_stackedWidget = nullptr;

    // 轮盘绑定
    WheelEditorWidget* wheelEditor = nullptr; // 可视化轮盘
    int currentSelectedSlot = -1;             // 当前选中的扇区
    QPushButton* copyGlobalButton = nullptr;
    QComboBox* ringCombo = nullptr; // 轮盘环数（1/2/3 环）

    // 动作库
    QTableWidget* libraryTable = nullptr;
    QVector<LibraryAction> m_library;

    // 排行页：用 page 指针判断「当前是不是排行页」，避免硬编码 stacked 索引
    QWidget* rankingPage = nullptr;
    QWidget* rankingGridHost = nullptr;          // 卡片容器（QScrollArea 的 widget）
    QGridLayout* rankingGridLayout = nullptr;    // 卡片网格，refreshRanking 负责清空重填

    // 触发方式 / 系统
    QComboBox* mouseTriggerCombo = nullptr;
    KeyRecorderWidget* wheelHotkeyEdit = nullptr; // 轮盘呼出热键
    QSpinBox* thresholdSpin = nullptr;
    QSpinBox* longPressSpin = nullptr;
    QSpinBox* hideSpin = nullptr;
    QSpinBox* scriptTimeoutSpin = nullptr;
    QTableWidget* bindingTable = nullptr;
    QCheckBox* autostartCheck = nullptr;

    QPushButton* saveBtn = nullptr;

    // 数据备份：路径只读展示，通过『浏览…』改；改动即时落库
    QLineEdit* autoBackupPathEdit = nullptr;
    QPushButton* browseBackupPathBtn = nullptr;
    QCheckBox* autoBackupCheck = nullptr; // 开启自动备份
    QTimer* m_backupTimer = nullptr;      // 防抖：连续改动只在停手 2 秒后写一次


    QListWidget* sceneList = nullptr;
    // 程序内动作剪贴板：只存动作库里的 id（-1 表示空）。
    // 跨页、跨场景粘贴都靠它，程序退出即丢失。
    int m_copiedActionId = -1;
    QLineEdit* sceneFilterEdit = nullptr; // 场景筛选（loadScenes 重建列表后需要重新套用）
    QPushButton* addSceneBtn = nullptr;
    QPushButton* editSceneBtn = nullptr;
    QPushButton* delSceneBtn = nullptr;
    KeyRecorderWidget* actionPageHotkeyEdit = nullptr; // 动作页呼出热键
    KeyRecorderWidget* settingsHotkeyEdit = nullptr;   // 呼出设置界面
    KeyRecorderWidget* forceQuitEdit = nullptr;        // 强制退出

    // 界面设置（动作页外观自定义）
    QSpinBox* apWidthSpin = nullptr;
    QSpinBox* apHeightSpin = nullptr;
    QSpinBox* btnWidthSpin = nullptr;
    QSpinBox* btnHeightSpin = nullptr;
    QSpinBox* radiusSpin = nullptr;
    QSpinBox* apHSpacingSpin = nullptr;
    QSpinBox* apVSpacingSpin = nullptr;
    QLineEdit* apBgColorEdit = nullptr;
    QLineEdit* btnBgColorEdit = nullptr;
    QLineEdit* btnHoverColorEdit = nullptr;
    QLineEdit* textColorEdit = nullptr;

    // 轮盘 UI 控件
    QSpinBox* wheelSizeSpin = nullptr;
    QSpinBox* wheelInnerSpin = nullptr;
    QSpinBox* wheelFontSpin = nullptr;
    QLineEdit* wheelBgColorEdit = nullptr;
    QLineEdit* wheelHoverColorEdit = nullptr;
    QLineEdit* wheelLineColorEdit = nullptr;
    QLineEdit* wheelTextColorEdit = nullptr;
    QLineEdit* wheelTextHoverColorEdit = nullptr;
    QCheckBox* hiddenRingCheck = nullptr; // 外圈隐藏动作开关


    QWidget* actionGridHost = nullptr;
    QGridLayout* actionGridLayout = nullptr;
    int actionGridPage = 0;
    QLabel* actionGridPageLabel = nullptr;
    void refreshActionGrid();
    // 清空某个轮盘槽位：全局场景直接解绑；局部场景按"独立配置→恢复继承 / 继承→强制空白"处理
    void doDeleteSlot(int slot);
};

// 新建动作页的通用弹窗（MainWindow 与 ActionPage 共用）。
// 返回新建页的 0-based 页索引；取消返回 -1。
int showNewPageDialog(QWidget* parent, const QString& currentScene);
