#include "ui/main_window/MainWindow.h"
#include "ui/main_window/MediaInfoCoordinator.h"
#include "ui/main_window/navigation_model.h"
#include "ui/main_window/overview_analysis.h"
#include "ui/dialogs/FormatSupportDialog.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/theme/AppTheme.h"
#include "infrastructure/logging/Logger.h"

#include "core/domain/model/EbmlInfo.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QScrollArea>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QApplication>
#include <QStyle>
#include <QScreen>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QDebug>
#include <QTabBar>
#include <QTimer>
#include <QFontDatabase>
#include <QPainter>
#include <QWindow>
#include <QPainterPath>
#include <QListWidget>
#include <QLabel>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QActionGroup>
#include <algorithm>

#include <QFileInfo>
#include <QFile>
#include <QByteArray>
#include <QRegularExpression>
#include <QDateTime>
#include <QtGlobal>
#include <cmath>
#ifdef Q_OS_WIN
// WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE: 窗口拖动模态检测。
// 必须定义 NOMINMAX: windows.h 的 min/max 宏会破坏 std::max/std::min/std::clamp
// (此文件是 MainWindow.cpp 中首次引入 windows.h, 前无 NOMINMAX 定义)。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// 侧栏 item 的自定义数据角色（Qt::UserRole 已存 stack 下标，见 PopulateSidebarItems）:
// 导航标记三件套，由 UpdateNavigationBadges 写入、SidebarItemDelegate 画出。
constexpr int kBadgeCountRole = Qt::UserRole + 1;    // 异常条数（int）
constexpr int kBadgeLevelRole = Qt::UserRole + 2;    // 级别 0/1/2/3（int）
constexpr int kScanningRole = Qt::UserRole + 3;      // 是否共用扫描中（bool）

// 侧栏一行分两类：可点击的页面项，与不可点击的分组标题。
// QSS 的 ::item 只能给整列统一样式，无法按行区分；把 QLabel 当作 item widget 挂在
// 不可选中的标题行上也画不出来（只剩一条空行）。所以用委托按行绘制：页面项交给基类
// （保留 QSS 的悬停/选中样式），分组标题用更小、更暗、加粗的字体画成小节标签。

// 导航标记的徽标配色（按级别: 1 警告 / 2 错误 / 3 致命）
QColor BadgeColor(int level) {
    switch (level) {
        case 3: return QColor(0xF8, 0x51, 0x49);  // 致命红
        case 2: return QColor(0xFF, 0x7B, 0x72);  // 错误橙红
        case 1: return QColor(0xD2, 0x99, 0x22);  // 警告橙
        default: return QColor(0x8B, 0x94, 0x9E); // 无级别灰
    }
}

class SidebarItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        // 分组标题登记时置了 Qt::NoItemFlags，没有 ItemIsSelectable，以此区分两类行。
        const bool is_header = !(index.flags() & Qt::ItemIsSelectable);
        if (!is_header) {
            QStyledItemDelegate::paint(painter, option, index);
            DrawBadge(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();  // 文本自己画，不交给默认样式（否则又会拿到页面项的颜色）
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        painter->save();
        QFont font = opt.font;
        font.setPointSizeF(std::max(7.5, font.pointSizeF() - 2.0));
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(QColor(0x6E, 0x76, 0x81));
        painter->drawText(opt.rect.adjusted(16, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
        painter->restore();
    }

private:
    // 页面项右侧的导航标记: 扫描态画强调色小圆点，异常数画彩色数字徽标。
    // 数据来自 UpdateNavigationBadges 写进 item 的角色；两者都没有就不画。
    static void DrawBadge(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) {
        const int count = index.data(kBadgeCountRole).toInt();
        const bool scanning = index.data(kScanningRole).toBool();
        if (count <= 0 && !scanning) return;

        const QRect rect = option.rect;
        const int right = rect.right() - 10;
        painter->save();
        QFont font = option.font;
        font.setPointSizeF(std::max(8.0, font.pointSizeF() - 3.0));
        font.setBold(true);
        painter->setFont(font);

        if (scanning) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0x58, 0xA6, 0xFF));
            painter->drawEllipse(QRect(right - 6, rect.center().y() - 3, 6, 6));
        }
        if (count > 0) {
            const QColor color = BadgeColor(index.data(kBadgeLevelRole).toInt());
            const QString text = count > 99 ? QStringLiteral("99+") : QString::number(count);
            const QFontMetrics fm(font);
            const int pill_width = fm.horizontalAdvance(text) + 10;
            const int pill_right = scanning ? right - 12 : right;
            const QRect pill(pill_right - pill_width, rect.center().y() - 8, pill_width, 16);
            QColor background = color;
            background.setAlpha(38);  // ~15% 透明度底
            painter->setPen(QPen(color, 1.0));
            painter->setBrush(background);
            painter->drawRoundedRect(pill, 8, 8);
            painter->setPen(color);
            painter->drawText(pill, Qt::AlignCenter, text);
        }
        painter->restore();
    }
};

}  // namespace

namespace videoeye {
namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , player_(nullptr)
    , app_bar_(nullptr)
    , sidebar_(nullptr)
    , content_stack_(nullptr)
    , main_splitter_(nullptr)
    , content_splitter_(nullptr)
    , current_media_label_(nullptr)
    , stats_label_(nullptr)
    , analysis_panel_(nullptr)
    , menu_bar_(nullptr)
    , status_bar_(nullptr)
    , media_info_coordinator_(std::make_unique<MediaInfoCoordinator>()) {

    // 创建播放器实例 (MainWindow 拥有; 分析侧与播放模块共用)
    player_ = new player::MediaPlayer(this);

    // 导出协调器: 自连播放器导出信号。两个查询 lambda 保证协调器不持有本窗口的
    // 具体成员 (raw 查询里的 player_panel_ 在 SetupUI 中才创建, lambda 调用时已就绪)。
    export_coordinator_ = new ExportCoordinator(player_, this, this);
    export_coordinator_->SetSourceQueries(
        [this]() { return current_media_url_; },
        [this]() { return player_panel_ && player_panel_->IsShowingRawImage(); });

    // 应用深色主题
    theme::applyDarkTheme();
    
    SetupUI();
    SetupMenuBar();
    SetupStatusBar();
    SetupConnections();
    // 恢复上次播放区显隐状态 (须在 SetupUI 之后)
    player_panel_->RestoreVisibility();
    UpdateMinimumWindowSize();

    setWindowTitle(tr("VideoEye 2.0 - 视频流分析软件"));
    // 初始尺寸同样考虑小屏: 1366x768 的笔记本放不下 1200x800
    const QScreen* primary = QGuiApplication::primaryScreen();
    const QRect available = primary ? primary->availableGeometry() : QRect(0, 0, 1200, 800);
    resize(qMin(1200, available.width()), qMin(800, available.height()));
}

MainWindow::~MainWindow() {
    if (player_) {
        player_->Stop();
    }
}

void MainWindow::SetupUI() {
    // 中央部件
    QWidget* central_widget = new QWidget(this);
    setCentralWidget(central_widget);
    
    QVBoxLayout* root_layout = new QVBoxLayout(central_widget);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);
    
    // === 1. 顶部应用栏 ===
    SetupAppBar();
    root_layout->addWidget(app_bar_);
    
    // === 2. 水平分割器: 侧栏 | 主内容 ===
    main_splitter_ = new QSplitter(Qt::Horizontal, central_widget);
    main_splitter_->setChildrenCollapsible(false);
    root_layout->addWidget(main_splitter_);
    
    // === 3. 左侧导航栏 ===
    SetupSidebar();
    main_splitter_->addWidget(sidebar_);
    
    // === 4. 右侧主内容区 ===
    SetupContentArea();
    main_splitter_->addWidget(content_splitter_);
    
    // 设置分割比例: 侧栏 200px, 主内容区弹性
    main_splitter_->setStretchFactor(0, 0);
    main_splitter_->setStretchFactor(1, 1);
    main_splitter_->setSizes({200, 1000});

    // UI 健康度探针: 主线程被长任务阻塞时, 定时器回调会被推迟。
    // 通过回调间隔即可测出"界面卡住多久"(用于定位打开/播放期卡顿, 仅写日志)。
    ui_health_timer_ = new QTimer(this);
    ui_health_timer_->setTimerType(Qt::PreciseTimer);
    connect(ui_health_timer_, &QTimer::timeout, this, [this]() {
        const auto now = std::chrono::steady_clock::now();
        if (ui_health_last_.time_since_epoch().count() != 0) {
            const double gap_ms =
                std::chrono::duration<double, std::milli>(now - ui_health_last_).count();
            if (gap_ms >= 300.0) {
                LOG_WARN("[perf] 主线程阻塞 " + std::to_string(static_cast<long long>(gap_ms)) +
                         " ms (定时器周期 100ms)");
            }
        }
        ui_health_last_ = now;
    });
    ui_health_timer_->start(100);
}

void MainWindow::SetupAppBar() {
    app_bar_ = new QWidget(this);
    app_bar_->setObjectName("AppBar");
    app_bar_->setFixedHeight(48);
    
    QHBoxLayout* layout = new QHBoxLayout(app_bar_);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(12);
    
    // Logo + 应用名
    QLabel* logo_label = new QLabel(QStringLiteral("\xF0\x9F\x91\x81"), app_bar_);  // eye emoji
    logo_label->setFixedSize(24, 24);
    logo_label->setAlignment(Qt::AlignCenter);
    QFont logoFont;
    logoFont.setPixelSize(16);
    logo_label->setFont(logoFont);
    layout->addWidget(logo_label);
    
    QLabel* title_label = new QLabel("VideoEye", app_bar_);
    title_label->setObjectName("AppTitle");
    layout->addWidget(title_label);
    
    QLabel* version_badge = new QLabel("2.0", app_bar_);
    version_badge->setObjectName("VersionBadge");
    layout->addWidget(version_badge);
    
    layout->addSpacing(24);
    
    // 媒体路径显示
    current_media_label_ = new QLabel(tr("未选择媒体"), app_bar_);
    current_media_label_->setObjectName("MediaPath");
    current_media_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(current_media_label_, 1);
    
    // 操作按钮
    QPushButton* open_file_btn = new QPushButton(tr("打开文件"), app_bar_);
    open_file_btn->setObjectName("PrimaryButton");
    connect(open_file_btn, &QPushButton::clicked, this, &MainWindow::OnOpenFile);
    layout->addWidget(open_file_btn);
    
    QPushButton* open_url_btn = new QPushButton(tr("打开URL"), app_bar_);
    connect(open_url_btn, &QPushButton::clicked, this, &MainWindow::OnOpenURL);
    layout->addWidget(open_url_btn);
}

void MainWindow::SetupSidebar() {
    sidebar_ = new QListWidget(this);
    sidebar_->setObjectName("Sidebar");
    sidebar_->setFixedWidth(200);
    sidebar_->setIconSize(QSize(16, 16));
    sidebar_->setSpacing(0);
    sidebar_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 分组标题与页面项在同一个列表里，靠委托按行区分绘制（见文件顶部 SidebarItemDelegate）。
    sidebar_->setItemDelegate(new SidebarItemDelegate(sidebar_));
    
    // 注意：导航项不再硬编码，而是在 SetupContentArea() 末尾由
    // PopulateSidebarItems() 依据 content_stack_ 的真实页面生成，避免增减分析页
    // 后 sidebar 行号与 stack 下标静默错位（曾导致「码率与 GOP」显示成「诊断与报告」）。
    connect(sidebar_, &QListWidget::currentRowChanged, this, &MainWindow::OnSidebarChanged);
}

void MainWindow::PopulateSidebarItems() {
    if (!sidebar_ || !content_stack_) return;

    // 页面元数据（pageId / 标题 / 分组 / 组内排序 / 口径）由各页面在登记时写入控件属性；
    // 侧栏完全由这些元数据生成，不再依赖固定页序与硬编码分组下标 —— 以后增减或重排页面
    // 时只改页面自己的登记，侧栏自动跟随（见 ui/main_window/navigation_model.h）。
    //   1) 页面项一律带 UserRole=stack 下标，切换按下标走 —— 行号与下标彻底解耦
    //      （过去分组标题会占行，靠行号当下标必然错位）；
    //   2) 分组标题不可选中、没有 UserRole，OnSidebarChanged 遇到它直接返回；
    //   3) 分组标题用委托（SidebarItemDelegate）画成更小更暗的小节标签，与可点击的
    //      页面项在视觉上区分开（否则「概览」会和「媒体信息」长得一样却点不动）。
    //      单页分组（如「概览」只有「媒体信息」）已由 BuildNavigationRows 去掉标题。
    QVector<ui::PageNavigation> pages;
    for (int i = 0; i < content_stack_->count(); ++i) {
        QWidget* page = content_stack_->widget(i);
        ui::PageNavigation nav;
        nav.stack_index = i;
        nav.page_id = page->property("pageId").toString();
        nav.title = page->property("pageTitle").toString();
        nav.group_id = page->property("pageGroup").toString();
        nav.scope = page->property("pageScope").toString();
        nav.order = page->property("pageOrder").toInt();
        if (nav.title.isEmpty()) nav.title = tr("页面 %1").arg(i);
        pages.append(nav);
    }

    // 分组 id 稳定（与页面登记对应），标题本地化；顺序即侧栏从上到下的分组顺序。
    const QVector<ui::NavGroup> groups = {
        {QStringLiteral("overview"), tr("概览")},
        {QStringLiteral("playback"), tr("播放监看")},
        {QStringLiteral("file_parse"), tr("文件解析")},
        {QStringLiteral("full_quality"), tr("全片质量")},
        {QStringLiteral("report_tools"), tr("报告与工具")},
    };
    const QVector<ui::NavRow> rows = ui::BuildNavigationRows(pages, groups);

    sidebar_->blockSignals(true);
    sidebar_->clear();
    for (const ui::NavRow& row : rows) {
        QListWidgetItem* item = new QListWidgetItem(row.text);
        if (row.is_header) {
            // 小节标签：不可选中，委托据此把它画成更小更暗的标题（见 SidebarItemDelegate）。
            item->setFlags(Qt::NoItemFlags);
            item->setSizeHint(QSize(200, 28));
        } else {
            item->setSizeHint(QSize(200, 34));
            item->setData(Qt::UserRole, row.stack_index);
        }
        sidebar_->addItem(item);
    }
    sidebar_->blockSignals(false);

    // 默认选中第一个页面项（分组标题没有 UserRole，落在它上面不会切换），
    // 并显式同步一次 stack，保证界面与内容区一致。
    for (int row = 0; row < sidebar_->count(); ++row) {
        if (sidebar_->item(row)->data(Qt::UserRole).isValid()) {
            sidebar_->setCurrentRow(row);
            break;
        }
    }
    content_stack_->setCurrentIndex(0);
}

void MainWindow::SetupContentArea() {
    // 右侧主内容区: 垂直分割器 (播放模块 | 分析内容区)
    content_splitter_ = new QSplitter(Qt::Vertical, this);
    content_splitter_->setChildrenCollapsible(false);
    
    // === 上半区: 播放模块 (视频区 + 控制栏, 可整体收起) ===
    // 播放相关的一切都在 PlayerPanel 内: 视频 widget、控制栏、音频可视化、
    // Raw 序列。MainWindow 只负责把它放进分割器并注入 MediaPlayer。
    player_panel_ = new PlayerPanel(content_splitter_);
    player_panel_->SetMediaPlayer(player_);
    player_panel_->SetSplitter(content_splitter_);
    
    // === 下半区: 内容堆栈 ===
    content_stack_ = new QStackedWidget(content_splitter_);
    content_stack_->setMinimumHeight(250);
    
    // Page 0: 首页（总览）—— 文件摘要 + 分析状态 + 最重要异常 + 完整媒体信息。
    // 原来的"媒体信息"页是一段不可换行的纯文本，用户打开文件后仍不知道该看哪个
    // 分析页；现在首页把"该看什么"收在一屏里，完整媒体信息整段保留在下方。
    // 页面不持有 AnalysisPanel / MediaPlayer，数据由本窗口喂入、跳转意图信号外抛。
    overview_page_ = new OverviewPage(content_stack_);
    overview_page_->setProperty("pageTitle", tr("总览"));
    overview_page_->setProperty("pageId", QStringLiteral("overview"));
    overview_page_->setProperty("pageGroup", QStringLiteral("overview"));
    overview_page_->setProperty("pageOrder", 0);
    overview_page_->setProperty("pageScope", QString());
    content_stack_->addWidget(overview_page_);
    
    // Page 1-N: 分析面板各页
    // AnalysisPanel 会创建自己的页面并添加到 content_stack_
    analysis_panel_ = new ui::AnalysisPanel(content_stack_);
    analysis_panel_->PopulateStackedWidget(content_stack_);
    analysis_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Page N: FFmpeg 命令工作台
    // 与上面的分析页不同，这一页不套 QScrollArea —— 它自己内部就有 splitter 与滚动条，
    // 再套一层会让"输出区"拿不到剩余高度。
    ffmpeg_panel_ = new ui::FfmpegPanel(content_stack_);
    ffmpeg_panel_->setProperty("pageTitle", tr("FFmpeg 命令"));
    ffmpeg_panel_->setProperty("pageId", QStringLiteral("ffmpeg_command"));
    ffmpeg_panel_->setProperty("pageGroup", QStringLiteral("report_tools"));
    ffmpeg_panel_->setProperty("pageOrder", 1);
    ffmpeg_panel_->setProperty("pageScope", QString());
    content_stack_->addWidget(ffmpeg_panel_);

    // 页面全部注册完毕后才生成侧边栏条目，保证行号 == stack 下标
    PopulateSidebarItems();
    
    content_splitter_->addWidget(player_panel_);
    content_splitter_->addWidget(content_stack_);

    content_splitter_->setStretchFactor(0, 3);
    content_splitter_->setStretchFactor(1, 2);
    content_splitter_->setSizes({480, 320});
    
    // 防止下区缩太小
    content_splitter_->setCollapsible(0, false);
    content_splitter_->setCollapsible(1, false);
}

void MainWindow::OnSidebarChanged(int row) {
    if (!content_stack_ || !sidebar_ || row < 0 || row >= sidebar_->count()) return;
    // 页面项带 UserRole=stack 下标（行号因分组标题会错位，不能当下标用）；
    // 分组标题没有 UserRole，选中它时直接返回。
    const QVariant page_index = sidebar_->item(row)->data(Qt::UserRole);
    if (!page_index.isValid()) return;
    const int index = page_index.toInt();
    if (index < 0 || index >= content_stack_->count()) {
        qWarning() << "侧边栏页面项下标" << index << "超出页面数" << content_stack_->count()
                   << "，页面栈与导航列表不一致";
        return;
    }
    content_stack_->setCurrentIndex(index);
}

void MainWindow::OnTogglePlayerArea(bool checked) {
    player_panel_->SetPlayerAreaVisible(checked);
}

void MainWindow::ApplyMacroblockAnalysisState() {
    // 实际采集状态 = 用户主动启用 || MV 叠加需求。两个来源分别记录，关闭叠加时
    // 只回退到用户原先的选择，不会顺手把用户开启的分析也关掉。
    const bool enabled = macroblock_.ActualEnabled();
    if (player_) player_->SetAnalysisFeature(model::AnalysisFeature::Macroblock, enabled);
    // 同步宏块页勾选框，避免"分析其实开着，页面却显示未启用"（反之亦然）。
    if (analysis_panel_) analysis_panel_->SetMacroblockAnalysisEnabled(enabled);
}

void MainWindow::UpdateMinimumWindowSize() {
    // 播放区收起时 MinimumHeightHint 返回 0, 窗口可以缩得更小
    const int player_min = player_panel_ ? player_panel_->MinimumHeightHint() : 0;
    int content_min = content_stack_ ? content_stack_->minimumHeight() : 250;

    int bars = 0;
    if (app_bar_) {
        bars += app_bar_->height();
    }
    if (menuBar()) {
        bars += menuBar()->sizeHint().height();
    }
    if (statusBar()) {
        bars += statusBar()->sizeHint().height();
    }

    int min_height = bars + content_min + player_min;

    int min_width = 900;
    if (sidebar_) {
        min_width += sidebar_->width();
    }

    setMinimumSize(min_width, min_height);
    if (windowHandle()) {
        windowHandle()->setMinimumSize(QSize(min_width, min_height));
    }
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    UpdateMinimumWindowSize();
    // 首次显示时 splitter 才拿到真实尺寸，这时才应用布局预设。
    // 小屏（< 860 可用高度，典型 1366x768 笔记本）默认走「分析」—— 原来的固定
    // 480px 播放区 + 250px 内容区连菜单栏一起超过 768，窗口根本放不下。
    if (!layout_preset_applied_) {
        layout_preset_applied_ = true;
        const QScreen* screen = windowHandle() ? windowHandle()->screen()
                                               : QGuiApplication::primaryScreen();
        const int available_height = screen ? screen->availableGeometry().height() : 0;
        const auto preset = (available_height > 0 && available_height < 860)
                                ? PlayerPanel::LayoutPreset::Analysis
                                : PlayerPanel::LayoutPreset::Monitor;
        player_panel_->RestoreLayoutPreset(preset);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    // 本机 WM 在拖拽中完全无视所有尺寸约束，必须强制 resize 阻止缩成一线
    UpdateMinimumWindowSize();
    const QSize min_sz = minimumSize();
    const QSize cur_sz = size();
    if (cur_sz.width() < min_sz.width() || cur_sz.height() < min_sz.height()) {
        resize(cur_sz.expandedTo(min_sz));
    }
}

void MainWindow::SetupMenuBar() {
    menu_bar_ = menuBar();
    
    // 文件菜单
    QMenu* file_menu = menu_bar_->addMenu(tr("文件"));
    file_menu->addAction(tr("打开本地文件"), QKeySequence::Open, this, &MainWindow::OnOpenFile);
    file_menu->addAction(tr("打开URL"), QKeySequence("Ctrl+U"), this, &MainWindow::OnOpenURL);
    // 导出 子菜单
    QMenu* export_menu = file_menu->addMenu(tr("导出"));
    export_frames_action_ = export_menu->addAction(tr("导出视频帧..."), export_coordinator_, &ExportCoordinator::OnExportVideoFrames);
    export_menu->addAction(tr("导出视频..."), export_coordinator_, &ExportCoordinator::OnExportVideo);
    export_menu->addAction(tr("导出音频..."), export_coordinator_, &ExportCoordinator::OnExportAudio);
    file_menu->addSeparator();
    file_menu->addAction(tr("退出"), QKeySequence::Quit, this, &MainWindow::OnExit);
    
    // 帮助菜单 (先声明, 以便在其之前插入"播放设置")
    QMenu* help_menu = menu_bar_->addMenu(tr("帮助"));
    help_menu->addAction(tr("支持的格式与编解码器..."), this, &MainWindow::OnShowFormatSupport);
    help_menu->addSeparator();
    help_menu->addAction(tr("关于"), this, []() {
        QMessageBox::about(nullptr, QObject::tr("关于"),
                          QObject::tr("VideoEye 2.0\n现代化的视频流分析软件"));
    });

    // 播放设置菜单 (位于 "文件" 与 "帮助" 之间)
    QMenu* playback_menu = new QMenu(tr("播放设置"), menu_bar_);
    menu_bar_->insertMenu(help_menu->menuAction(), playback_menu);

    // 视图菜单 (位于 "播放设置" 与 "帮助" 之间)
    QMenu* view_menu = new QMenu(tr("视图"), menu_bar_);
    menu_bar_->insertMenu(help_menu->menuAction(), view_menu);

    toggle_player_action_ = new QAction(tr("显示播放区"), this);
    toggle_player_action_->setCheckable(true);
    toggle_player_action_->setChecked(true);
    toggle_player_action_->setShortcut(QKeySequence(QStringLiteral("F9")));
    toggle_player_action_->setStatusTip(tr("收起播放区可让分析区占满整个内容区 (VideoEye 以分析为主)"));
    connect(toggle_player_action_, &QAction::toggled, this, &MainWindow::OnTogglePlayerArea);
    view_menu->addAction(toggle_player_action_);

    // 播放区布局预设: 监看 = 播放区 58%; 分析 = 播放区 30% (小屏默认走分析)。
    // 与上面的显隐开关正交 —— 预设管比例, 显隐管在不在。
    QMenu* layout_menu = view_menu->addMenu(tr("播放区布局"));
    QActionGroup* layout_ag = new QActionGroup(layout_menu);
    layout_ag->setExclusive(true);
    layout_monitor_action_ = layout_ag->addAction(tr("监看布局"));
    layout_monitor_action_->setCheckable(true);
    layout_monitor_action_->setStatusTip(tr("播放区占大头，适合盯画面 / 逐帧定位"));
    layout_analysis_action_ = layout_ag->addAction(tr("分析布局"));
    layout_analysis_action_->setCheckable(true);
    layout_analysis_action_->setStatusTip(tr("分析区占大头，适合看表格与扫描结果"));
    layout_monitor_action_->setChecked(true);
    layout_menu->addAction(layout_monitor_action_);
    layout_menu->addAction(layout_analysis_action_);
    connect(layout_ag, &QActionGroup::triggered, this, [this](QAction* action) {
        if (!player_panel_ || !action) return;
        player_panel_->ApplyLayoutPreset(
            action == layout_analysis_action_ ? PlayerPanel::LayoutPreset::Analysis
                                              : PlayerPanel::LayoutPreset::Monitor);
    });

    QMenu* seek_mode_menu = playback_menu->addMenu(tr("seek方式"));
    QActionGroup* seek_ag = new QActionGroup(playback_menu);
    seek_ag->setExclusive(true);

    QAction* act_keyframe = seek_ag->addAction(tr("关键帧"));
    act_keyframe->setCheckable(true);
    act_keyframe->setData(static_cast<int>(model::SeekMode::NearestKeyframe));
    act_keyframe->setChecked(true); // 默认: 关键帧 (与原行为一致)
    seek_mode_menu->addAction(act_keyframe);

    QAction* act_exact = seek_ag->addAction(tr("精确值"));
    act_exact->setCheckable(true);
    act_exact->setData(static_cast<int>(model::SeekMode::ExactFrame));
    seek_mode_menu->addAction(act_exact);

    connect(seek_ag, &QActionGroup::triggered, this, [this](QAction* a) {
        if (a && player_) player_->SetSeekMode(static_cast<model::SeekMode>(a->data().toInt()));
    });
}

void MainWindow::SetupStatusBar() {
    status_bar_ = statusBar();
    status_bar_->showMessage(tr("就绪"));

    // 实时码流统计常驻区 (右端): 用 permanent widget 而非 showMessage,
    // 否则每 10 帧一次的统计会把“已打开 xx”这类临时提示冲掉。
    stats_label_ = new QLabel(status_bar_);
    stats_label_->setFont(theme::font::monoFont(9));
    stats_label_->setStyleSheet(QStringLiteral("color: #8b949e; padding-right: 8px;"));
    stats_label_->setTextInteractionFlags(Qt::NoTextInteraction);
    stats_label_->setVisible(false);  // 有统计数据时才显示
    status_bar_->addPermanentWidget(stats_label_);
}

void MainWindow::SetupConnections() {
    // 播放器信号 - 播放/画面/音频相关已由 PlayerPanel 自行连接 (SetMediaPlayer);
    // 导出进度已由 ExportCoordinator 自连 (其构造函数)。
    // 此处只连接 MainWindow 负责的部分: 打开收尾、分析面板、跨模块转发。

    // 导出提示 (协调器只发文本, 状态栏由本窗口转发)
    connect(export_coordinator_, &ExportCoordinator::StatusMessage,
            this, [this](const QString& text, int timeout) {
                statusBar()->showMessage(text, timeout);
            });

    // 事务式异步打开完成: 在 UI 线程统一收尾 (状态栏/标签/自动播放/诊断扫描)。
    // 过期结果 (期间又开了新文件) 由 MediaPlayer 丢弃且不发此信号。
    connect(player_, &player::MediaPlayer::OpenFinished,
            this, &MainWindow::OnPlayerOpenFinished);

    // 媒体信息文本: 打开链路在后台探测时顺带格式化 (不再二次探测), 提交成功后贴进
    // 首页的「完整媒体信息」区; 顺序上先于 OpenFinished 到达。
    connect(player_, &player::MediaPlayer::MediaInfoTextReady,
            this, [this](const QString& text) {
                if (overview_page_ && !text.isEmpty()) {
                    overview_page_->SetMediaInfoText(text);
                }
            });

    // 播放器信号连接 - 分析功能 (实时分析)
    connect(player_, &player::MediaPlayer::StreamStatsReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateStreamStats);
    connect(player_, &player::MediaPlayer::VideoFrameListReset,
            analysis_panel_, &ui::AnalysisPanel::ResetVideoFrameList);
    connect(player_, &player::MediaPlayer::VideoFrameInfoReady,
            analysis_panel_, &ui::AnalysisPanel::AppendVideoFrameInfo);
    connect(player_, &player::MediaPlayer::AudioFrameListReset,
            analysis_panel_, &ui::AnalysisPanel::ResetAudioFrameList);
    connect(player_, &player::MediaPlayer::AudioFrameInfoReady,
            analysis_panel_, &ui::AnalysisPanel::AppendAudioFrameInfo);
    connect(player_, &player::MediaPlayer::PacketListReset,
            analysis_panel_, &ui::AnalysisPanel::ResetPacketList);
    connect(player_, &player::MediaPlayer::PacketInfoReady,
            analysis_panel_, &ui::AnalysisPanel::AppendPacketInfo);
    connect(player_, &player::MediaPlayer::AnalysisEventListReset,
            analysis_panel_, &ui::AnalysisPanel::ResetAnalysisEventList);
    connect(player_, &player::MediaPlayer::AnalysisEventReady,
            analysis_panel_, &ui::AnalysisPanel::AppendAnalysisEvent);
    connect(player_, &player::MediaPlayer::SyncSampleListReset,
            analysis_panel_, &ui::AnalysisPanel::ResetSyncSampleList);
    connect(player_, &player::MediaPlayer::SyncSampleReady,
            analysis_panel_, &ui::AnalysisPanel::AppendSyncSample);
    connect(player_, &player::MediaPlayer::TimelineEventListReset,
            analysis_panel_, &ui::AnalysisPanel::ResetTimelineEventList);
    connect(player_, &player::MediaPlayer::TimelineEventReady,
            analysis_panel_, &ui::AnalysisPanel::AppendTimelineEvent);
    // 诊断页"跳转到问题帧" -> 播放器 seek
    connect(analysis_panel_, &ui::AnalysisPanel::SeekRequested,
            this, [this](double seconds) {
                if (player_) player_->Seek(seconds, player_->GetSeekMode());
            });

    // 统一容器结构分析信号
    connect(player_, &player::MediaPlayer::ContainerStructureReady,
            analysis_panel_, &ui::AnalysisPanel::OnContainerStructureReady);
    // 结构分析失败（解析不出来 / 分析器抛异常）：页面显示原因，别让结构页一直空着
    connect(player_, &player::MediaPlayer::ContainerStructureFailed,
            analysis_panel_, &ui::AnalysisPanel::OnContainerStructureFailed);
    connect(player_, &player::MediaPlayer::MacroblockInfoReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateMacroblockInfo);
    // MV 叠加: 同时转发到播放模块的视频叠加层
    connect(player_, &player::MediaPlayer::MacroblockInfoReady,
            player_panel_, &PlayerPanel::OnMacroblockInfoForOverlay);
    connect(player_, &player::MediaPlayer::SceneChangeReady,
            analysis_panel_, &ui::AnalysisPanel::OnSceneChangeDetected);
    // 画面质量 / 视觉缺陷（播放时逐采样帧产出）
    connect(player_, &player::MediaPlayer::VisualDefectReset,
            analysis_panel_, &ui::AnalysisPanel::OnVisualDefectReset);
    connect(player_, &player::MediaPlayer::VisualDefectFrameReady,
            analysis_panel_, &ui::AnalysisPanel::OnVisualDefectFrame);
    connect(player_, &player::MediaPlayer::VisualDefectReady,
            analysis_panel_, &ui::AnalysisPanel::OnVisualDefectDetected);
    connect(player_, &player::MediaPlayer::VisualDefectStatsReady,
            analysis_panel_, &ui::AnalysisPanel::OnVisualDefectStats);
    // 面板改采样档位 / 阈值 -> 播放器
    connect(analysis_panel_, &ui::AnalysisPanel::VisualDefectOptionsChanged,
            player_, &player::MediaPlayer::SetVisualDefectOptions);
    // 全文件扫描拿到素材自带起始时码 -> 播放器时间轴旁的 SMPTE 时码显示
    connect(analysis_panel_, &ui::AnalysisPanel::StartTimecodeReady,
            player_panel_, &PlayerPanel::SetStartTimecode);
    
    // 面板开关信号 -> MediaPlayer 控制
    connect(analysis_panel_, &ui::AnalysisPanel::AnalysisFeatureToggled,
            this, [this](int feature, bool enabled) {
                using AF = ui::AnalysisPanel::AnalysisFeature;
                AF feat = static_cast<AF>(feature);
                if (!player_) return;

                // 宏块分析要为 MV 叠加让路：它是"用户主动启用"与"叠加需求"的或。
                // 页面开关只代表用户意图 —— 用户关掉它时，叠加也依赖不成立，一起关。
                if (feat == AF::Macroblock) {
                    macroblock_.OnUserToggle(enabled);
                    if (!enabled && player_panel_) {
                        // 同步叠加按钮（内部会回调 MvOverlayToggled(false)，幂等）。
                        player_panel_->SetMvOverlayEnabled(false);
                    }
                    ApplyMacroblockAnalysisState();
                    return;
                }

                // MediaPlayer 直接认 model::AnalysisFeature（面板的 AF 就是它的别名），
                // 所以这里不再需要一个逐个翻译的 switch —— 只剩转发。
                player_->SetAnalysisFeature(feat, enabled);
            });

    // MV 叠加开关 -> 宏块分析（叠加需要分析，但"需要"与"用户启用"分开记，
    // 关闭叠加只回退到用户原先的选择，而不是无条件关掉分析）。
    connect(player_panel_, &PlayerPanel::MvOverlayToggled,
            this, [this](bool enabled) {
                macroblock_.OnOverlayToggle(enabled);
                ApplyMacroblockAnalysisState();
            });
    
    // 播放模块的信号桥接 (播放区内部的控件连接见 PlayerPanel::SetupConnections)
    connect(player_panel_, &PlayerPanel::StatusMessage,
            this, [this](const QString& text, int timeout) {
                statusBar()->showMessage(text, timeout);
            });
    connect(player_panel_, &PlayerPanel::ErrorMessage,
            this, [this](const QString& text) {
                statusBar()->showMessage(text);
            });
    connect(player_panel_, &PlayerPanel::RawImageInfoReady,
            this, [this](const QString& info) {
                if (overview_page_) overview_page_->SetMediaInfoText(info);
            });
    connect(analysis_panel_, &ui::AnalysisPanel::StatusMessage,
            this, [this](const QString& text) {
                if (statusBar()) statusBar()->showMessage(text);
            });
    connect(ffmpeg_panel_, &ui::FfmpegPanel::StatusMessage,
            this, [this](const QString& text) {
                if (statusBar()) statusBar()->showMessage(text);
            });
    // 实时码流统计: 空串表示停止/换源, 隐藏常驻区
    connect(player_panel_, &PlayerPanel::StreamStatsDisplayReady,
            this, [this](const QString& text) {
                if (!stats_label_) return;
                stats_label_->setText(text);
                stats_label_->setVisible(!text.isEmpty());
            });
    // 播放区显隐变化时同步菜单勾选并重算窗口最小尺寸
    connect(player_panel_, &PlayerPanel::VisibilityChanged,
            this, [this](bool visible) {
                if (toggle_player_action_ && toggle_player_action_->isChecked() != visible) {
                    toggle_player_action_->blockSignals(true);
                    toggle_player_action_->setChecked(visible);
                    toggle_player_action_->blockSignals(false);
                }
                UpdateMinimumWindowSize();
            });
    // 布局预设变化时同步视图菜单勾选（预设也可能来自恢复 / 首次自适应，不止菜单）
    connect(player_panel_, &PlayerPanel::LayoutPresetChanged,
            this, [this](PlayerPanel::LayoutPreset preset) {
                const bool analysis = (preset == PlayerPanel::LayoutPreset::Analysis);
                if (layout_analysis_action_ && layout_analysis_action_->isChecked() != analysis) {
                    layout_analysis_action_->blockSignals(true);
                    layout_analysis_action_->setChecked(analysis);
                    layout_analysis_action_->blockSignals(false);
                }
                if (layout_monitor_action_ && layout_monitor_action_->isChecked() == analysis) {
                    layout_monitor_action_->blockSignals(true);
                    layout_monitor_action_->setChecked(!analysis);
                    layout_monitor_action_->blockSignals(false);
                }
            });

    // ---- 首页（总览）接线 ----
    // 1) 文件摘要: 在 OnPlayerOpenFinished 里做，**不再单开一个 OpenFinished 连接**。
    //    两个槽都要读 pending_open_source_，而 Qt 按连接顺序调用：先跑的那个把
    //    pending_open_source_ 清空之后，后跑的只能读到空串 —— 打开失败时首页就
    //    拿不到文件名了。摘要与状态栏收尾同属"这一次打开"，放一个槽里顺序才确定。
    // 2) 分析状态: 面板在扫描起止/报告重算/换文件时发信号，这里拉快照刷新首页
    connect(analysis_panel_, &ui::AnalysisPanel::DiagnosticsStateChanged,
            this, &MainWindow::RefreshOverviewAnalysis);
    // 2b) 同一信号也驱动侧栏导航标记（异常数徽标 / 扫描圆点）
    connect(analysis_panel_, &ui::AnalysisPanel::DiagnosticsStateChanged,
            this, &MainWindow::UpdateNavigationBadges);
    // 3) 首页跳转 / 重新分析意图
    connect(overview_page_, &OverviewPage::NavigateToPage,
            this, &MainWindow::ShowPageById);
    connect(overview_page_, &OverviewPage::RescanRequested, this, [this]() {
        if (analysis_panel_) analysis_panel_->StartDiagnosticsScanForCurrentFile();
    });
    // 4) 媒体信息解析结果回到 UI 线程后贴到首页 (协调器只发文本, 不碰 UI 控件)
    connect(media_info_coordinator_.get(), &MediaInfoCoordinator::InfoReady,
            this, [this](const QString& text) {
                if (overview_page_) overview_page_->SetMediaInfoText(text);
            });
}

void MainWindow::ShowPageById(const QString& page_id) {
    if (!content_stack_) return;
    for (int i = 0; i < content_stack_->count(); ++i) {
        if (content_stack_->widget(i)->property("pageId").toString() != page_id) continue;
        content_stack_->setCurrentIndex(i);
        // 同步侧栏选中：行号因分组标题与 stack 下标错位，按 UserRole 反查
        if (sidebar_) {
            for (int row = 0; row < sidebar_->count(); ++row) {
                if (sidebar_->item(row)->data(Qt::UserRole).toInt() != i) continue;
                const QSignalBlocker blocker(sidebar_);
                sidebar_->setCurrentRow(row);
                break;
            }
        }
        return;
    }
    qWarning() << "跳转目标页面不存在:" << page_id;
}

void MainWindow::RefreshOverviewAnalysis() {
    if (!overview_page_ || !analysis_panel_) return;

    // 本函数只做两件事：把 AnalysisPanel 的只读快照抄进 Input，把算好的摘要交给首页。
    // 「什么算失败」「异常按什么顺序取前几条」都在 ui::BuildOverviewAnalysisSummary 里
    // （ui/main_window/overview_analysis.h），那里有单测守着。
    ui::OverviewAnalysisInput input;
    input.has_file = overview_page_->HasFile();
    input.scan_running = analysis_panel_->IsScanRunning();
    input.error = analysis_panel_->DiagnosticsError();
    input.has_result = analysis_panel_->HasDiagnosticsResult();
    // 报告的引用只在本次调用里借用：DiagnosticsReport() 返回的是面板手上那份，
    // 下一次扫描落地会被覆盖，所以组装函数内部一律拷值、不留指针。
    const model::QcReport* report =
        input.has_result ? &analysis_panel_->DiagnosticsReport() : nullptr;
    input.report = report;

    overview_page_->SetAnalysisSummary(ui::BuildOverviewAnalysisSummary(input));
}

void MainWindow::UpdateNavigationBadges() {
    if (!sidebar_ || !content_stack_ || !analysis_panel_) return;

    // 徽标按 pageId 归集（AnalysisPanel::PageBadges），侧栏 item 手上只有 stack 下标，
    // 反查一次 content_stack_ 把下标映回 pageId。
    const QMap<QString, AnalysisPanel::PageBadgeInfo> badges = analysis_panel_->PageBadges();

    bool changed = false;
    for (int row = 0; row < sidebar_->count(); ++row) {
        QListWidgetItem* item = sidebar_->item(row);
        const QVariant stack_index = item->data(Qt::UserRole);
        if (!stack_index.isValid()) continue;  // 分组标题没有徽标
        QWidget* page = content_stack_->widget(stack_index.toInt());
        if (!page) continue;  // 侧栏项与 stack 不同步（换页/重建的窗口期）时别解引用
        const QString page_id = page->property("pageId").toString();

        const AnalysisPanel::PageBadgeInfo info = badges.value(page_id);
        const int count = info.issue_count;
        const int level = info.level;
        const bool scanning = info.scanning;

        if (item->data(kBadgeCountRole).toInt() != count ||
            item->data(kBadgeLevelRole).toInt() != level ||
            item->data(kScanningRole).toBool() != scanning) {
            item->setData(kBadgeCountRole, count);
            item->setData(kBadgeLevelRole, level);
            item->setData(kScanningRole, scanning);
            changed = true;
        }
    }
    if (changed) sidebar_->update();
}

void MainWindow::OnOpenFile() {
    QString filename = QFileDialog::getOpenFileName(this,
        tr("打开媒体文件"), "",
        tr("媒体文件 (*.mp4 *.avi *.mkv *.flv *.ts *.mp3 *.aac *.wav *.pcm *.yuv *.nv12 *.rgb *.bgr *.yuy2 *.raw);;"
           "流媒体清单 (*.m3u8 *.mpd);;所有文件 (*)"));
    if (filename.isEmpty()) {
        return;
    }
    OpenMedia(filename, true);
}

void MainWindow::OnOpenURL() {
    bool ok;
    QString url = QInputDialog::getText(this, tr("打开URL"),
                                        tr("输入流媒体URL:"),
                                        QLineEdit::Normal,
                                        "rtmp://", &ok);
    
    if (ok && !url.isEmpty()) {
        OpenMedia(url, true);
    }
}

void MainWindow::OnShowFormatSupport() {
    // 清单是现场从本进程链接的 FFmpeg 库枚举出来的，每次打开都重问一遍 ——
    // 换过 FFmpeg 构建（或用户换了一台机器）时不会显示过时的答案。
    FormatSupportDialog dialog(this);
    dialog.exec();
}

bool MainWindow::OpenMedia(const QString& source, bool autoplay) {
    if (source.isEmpty()) {
        return false;
    }
    if (!player_) {
        return false;
    }

    // 任何一次新打开都先作废上一次异步打开的收尾回调: 只有真正走 OpenAsync 的
    // 路径会在下面重新登记 pending_open_source_。这样 raw/pcm 等同步分支期间
    // 若有旧的异步结果迟到, OnPlayerOpenFinished 会因 pending 为空而直接忽略。
    pending_open_source_.clear();
    pending_open_autoplay_ = false;

    // 打开新文件前收掉导出进度框。注意这里**只**管界面那部分 ——
    // "终止进行中的导出"是媒体生命周期的保证, 归 MediaPlayer::CancelAllExports(),
    // 由 OpenInternal() 统一负责（打开媒体的入口不止这一个）。以前这里只调了
    // CancelVideoFrameExport(): 媒体转码导出既不会被取消、排队请求也没清,
    // 旧任务还能把完成信号串回新媒体的界面。
    export_coordinator_->HideProgress();
    // 停止当前播放并清理播放模块状态 (Raw/音频可视化/进度条等)
    player_panel_->StopPlayback();

    const QString suffix = QFileInfo(source).suffix().toLower();
    if (suffix == "yuv" || suffix == "nv12" || suffix == "rgb" ||
        suffix == "bgr" || suffix == "yuy2" || suffix == "raw") {
        // 裸图像不走 player_->Open(), 拿不到 OpenInternal 里那次统一取消 ——
        // 这里显式补上, 否则旧媒体的抽帧/转码导出会继续在后台跑。
        player_->CancelAllExports();
        player_panel_->SetRawImageMode(true);
        player_panel_->SetCurrentSource(QString());
        current_media_url_.clear();
        if (!player_panel_->LoadRawImageFile(source)) {
            statusBar()->showMessage(tr("打开失败: %1").arg(source));
            return false;
        }
        statusBar()->showMessage(tr("已打开图像: %1").arg(source));
        if (current_media_label_) {
            current_media_label_->setText(source);
        }
        // 首页登记文件名（Raw 图像没有 StreamInfo 可摘要）；媒体信息框保留
        // LoadRawImageFile 通过 RawImageInfoReady 写入的 Raw 详情
        // (旧行为: 此处会用"无 MediaInfo 数据"覆盖掉, 导致 Raw 信息一闪而过)
        overview_page_->SetOpenedFileWithoutSummary(source);
        return true;
    }

    if (suffix == "pcm") {
        QString demuxer_name;
        int sample_rate = 44100;
        int channels = 2;
        if (!PromptForPcmSettings(demuxer_name, sample_rate, channels)) {
            statusBar()->showMessage(tr("已取消打开 PCM: %1").arg(source));
            return false;
        }

        player_panel_->SetRawImageMode(false);
        player_panel_->SetCurrentSource(source);
        const bool open_result = player_->OpenRawPcm(source, demuxer_name, sample_rate, channels);
        if (!open_result) {
            statusBar()->showMessage(tr("打开 PCM 失败: %1").arg(source));
            return false;
        }

        statusBar()->showMessage(tr("已打开 PCM: %1").arg(source));
        if (current_media_label_) {
            current_media_label_->setText(
                tr("%1 [PCM %2, %3 Hz, %4 ch]").arg(source, demuxer_name).arg(sample_rate).arg(channels));
        }
        current_media_url_ = source;
        analysis_panel_->SetCurrentVideoPath(source);
        if (ffmpeg_panel_) ffmpeg_panel_->SetCurrentFile(source);
        overview_page_->SetOpenedFileWithoutSummary(source);

        // 媒体信息解析 PCM (裸流无法自动探测, 需带上用户选择的格式参数) —— 贴到首页
        overview_page_->SetMediaInfoText(
            media_info_coordinator_->DescribeRawPcm(source, demuxer_name, sample_rate, channels));

        if (autoplay) {
            player_->Play();
        }
        return true;
    }

    player_panel_->SetRawImageMode(false);
    player_panel_->SetCurrentSource(source);
    current_media_url_ = source;
    analysis_panel_->SetCurrentVideoPath(source);
    if (ffmpeg_panel_) ffmpeg_panel_->SetCurrentFile(source);

    // 事务式异步打开: UI 线程只做"换媒体复位"并登记收尾信息, 耗时的
    // avformat 打开/探测被派到后台线程; 完成后经 OpenFinished 排回 UI 线程提交。
    // 收尾 (状态栏/标签/自动播放/诊断扫描) 见 OnPlayerOpenFinished。
    // 打开失败也不再直接中止: 仍把文件加载到分析模块, 由媒体信息/文件结构/
    // 诊断扫描给出错误原因。
    pending_open_source_ = source;
    pending_open_autoplay_ = autoplay;
    player_->OpenAsync(source);

    // 同步已启用的分析功能到播放器 (复选框默认勾选但未触发信号)。
    // 须放在 OpenAsync 之后: 它内部先做了换媒体复位。
    analysis_panel_->EmitInitialFeatureStates();
    // 宏块分析不在 EmitInitialFeatureStates 里重发（见其注释），换文件后在此按
    // "用户启用 || 叠加需求"重新下发，同时校准宏块页勾选框与播放器特征开关。
    ApplyMacroblockAnalysisState();

    // 媒体信息解析: 不再独立探测 —— 打开链路在后台探测时已顺带把同一个上下文
    // 格式化好，提交成功后经 MediaInfoTextReady 贴进首页（见 SetupConnections）；
    // 这里只放占位文本。打开失败的"分析模式"由 OnPlayerOpenFinished 退回协调器
    // 做尽力而为的独立解析（此时没有可复用的上下文）。
    overview_page_->SetMediaInfoText(tr("(正在解析媒体信息…)"));

    return true;
}

void MainWindow::OnPlayerOpenFinished(bool ok) {
    const QString source = pending_open_source_;
    const bool autoplay = pending_open_autoplay_;
    pending_open_source_.clear();
    pending_open_autoplay_ = false;
    // 过期/无关的回调 (对应请求已被后续打开取代) 直接忽略。
    if (source.isEmpty() || !player_) {
        return;
    }

    if (ok) {
        statusBar()->showMessage(tr("已打开: %1").arg(source));
    } else {
        statusBar()->showMessage(tr("无法播放 (已进入分析模式): %1").arg(player_->GetLastError()), 0);
    }
    if (current_media_label_) {
        current_media_label_->setText(ok ? source : tr("%1 (无法播放)").arg(source));
    }

    // ---- 首页文件摘要（见 SetupConnections 里"首页接线"的第 1 条注释）----
    // 打开成功时贴 StreamInfo；失败（分析模式）至少要把文件名留在首页上。
    if (overview_page_) {
        if (ok) {
            overview_page_->SetFileSummary(player_->GetStreamInfo());
        } else if (!source.isEmpty()) {
            overview_page_->SetOpenedFileWithoutSummary(source);
        }
    }

    if (ok) {
        if (autoplay) {
            player_->Play();
        }
        // 打开即分析: 后台自动跑一次全文件诊断扫描 (扩展名/容器一致性、GOP、时间戳、
        // 音频 QC 等规则在「诊断与报告」页直接给出原因与修复建议; 大文件可在该页取消)。
        analysis_panel_->StartDiagnosticsScanForCurrentFile();
    } else {
        // 分析模式: 补跑文件结构分析与全文件诊断扫描,
        // 让「文件结构」「诊断与报告」「码率与 GOP」页展示该文件的具体错误。
        player_->RequestContainerStructureAnalysis(source);
        analysis_panel_->StartDiagnosticsScanForCurrentFile();
        // 打开失败时没有可复用的 AVFormatContext（提交被整体丢弃），媒体信息
        // 退回协调器做一次尽力而为的独立解析（异常文件也可能部分解析成功）。
        media_info_coordinator_->StartAsync(source);
    }
}

bool MainWindow::PromptForPcmSettings(QString& demuxer_name, int& sample_rate, int& channels) {
    struct PcmFormatOption {
        const char* label;
        const char* demuxer;
    };

    const std::vector<PcmFormatOption> formats = {
        {"s16le (16-bit little-endian)", "pcm_s16le"},
        {"s16be (16-bit big-endian)", "pcm_s16be"},
        {"u8 (8-bit unsigned)", "pcm_u8"},
        {"s24le (24-bit little-endian)", "pcm_s24le"},
        {"s24be (24-bit big-endian)", "pcm_s24be"},
        {"s32le (32-bit little-endian)", "pcm_s32le"},
        {"f32le (32-bit float little-endian)", "pcm_f32le"},
        {"f32be (32-bit float big-endian)", "pcm_f32be"}
    };

    QStringList labels;
    for (const auto& format : formats) {
        labels << QString::fromLatin1(format.label);
    }

    bool ok = false;
    const QString selected = QInputDialog::getItem(
        this, tr("PCM 参数"), tr("采样格式:"), labels, 0, false, &ok);
    if (!ok || selected.isEmpty()) {
        return false;
    }

    for (const auto& format : formats) {
        if (selected == QString::fromLatin1(format.label)) {
            demuxer_name = QString::fromLatin1(format.demuxer);
            break;
        }
    }
    if (demuxer_name.isEmpty()) {
        return false;
    }

    sample_rate = QInputDialog::getInt(
        this, tr("PCM 参数"), tr("采样率 (Hz):"), sample_rate, 8000, 384000, 1000, &ok);
    if (!ok) {
        return false;
    }

    channels = QInputDialog::getInt(
        this, tr("PCM 参数"), tr("声道数:"), channels, 1, 8, 1, &ok);
    if (!ok) {
        return false;
    }

    return true;
}

void MainWindow::OnExit() {
    close();
}

} // namespace ui
} // namespace videoeye
