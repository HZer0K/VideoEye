#include "ui/main_window/MainWindow.h"
#include "ui/main_window/MediaInfoCoordinator.h"
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
#include <QSignalBlocker>
#include <QDebug>
#include <QTabBar>
#include <QTimer>
#include <QFontDatabase>
#include <QPainter>
#include <QWindow>
#include <QPainterPath>
#include <QListWidget>
#include <QStackedWidget>
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
    , mediainfo_text_(nullptr)
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
    resize(1200, 800);
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
    
    // 注意：导航项不再硬编码，而是在 SetupContentArea() 末尾由
    // PopulateSidebarItems() 依据 content_stack_ 的真实页面生成，避免增减分析页
    // 后 sidebar 行号与 stack 下标静默错位（曾导致「码率与 GOP」显示成「诊断与报告」）。
    connect(sidebar_, &QListWidget::currentRowChanged, this, &MainWindow::OnSidebarChanged);
}

void MainWindow::PopulateSidebarItems() {
    if (!sidebar_ || !content_stack_) return;

    const int page_count = content_stack_->count();

    // 侧边栏按功能分组。两条硬规则：
    //   1) 页面项一律带 UserRole=stack 下标，切换按下标走 —— 行号与下标彻底解耦
    //      （分组标题会占行，靠行号当下标必然错位）；
    //   2) 分组标题不可选中、没有 UserRole，OnSidebarChanged 遇到它直接返回。
    // 分组范围依赖当前页序；页面数量与预期不符（增减页面）时退化为平铺列表，
    // 宁可少分组也不静默错位（曾导致「码率与 GOP」显示成「诊断与报告」）。
    constexpr int kExpectedPageCount = 16;
    const struct { int first; int last; QString title; } groups[] = {
        {0, 0, tr("媒体信息")},
        {1, 6, tr("实时分析")},
        {7, 12, tr("全文件扫描")},
        {13, 14, tr("诊断与报告")},
        {15, 15, tr("工具")},
    };

    sidebar_->blockSignals(true);
    sidebar_->clear();

    auto add_page_item = [this](int stack_index) {
        QString title = content_stack_->widget(stack_index)->property("pageTitle").toString();
        if (title.isEmpty()) {
            // 分析页由 AnalysisPanel 提供标题；媒体信息页等外部页走这里
            title = (stack_index == 0) ? tr("媒体信息") : tr("页面 %1").arg(stack_index);
        }
        QListWidgetItem* list_item = new QListWidgetItem(title);
        list_item->setSizeHint(QSize(200, 34));
        list_item->setData(Qt::UserRole, stack_index);
        sidebar_->addItem(list_item);
    };

    if (page_count == kExpectedPageCount) {
        for (const auto& group : groups) {
            if (group.last > group.first) {
                // 单页分组不加标题头（标题与唯一页面项同名，加了反而重复）
                QListWidgetItem* header = new QListWidgetItem(group.title);
                header->setFlags(Qt::NoItemFlags);
                header->setSizeHint(QSize(200, 26));
                sidebar_->addItem(header);
            }
            for (int i = group.first; i <= group.last; ++i) add_page_item(i);
        }
    } else {
        for (int i = 0; i < page_count; ++i) add_page_item(i);
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
    
    // Page 0: 媒体信息
    QWidget* mediainfo_page = new QWidget(content_stack_);
    QVBoxLayout* mediainfo_layout = new QVBoxLayout(mediainfo_page);
    mediainfo_layout->setContentsMargins(0, 0, 0, 0);
    QScrollArea* mediainfo_scroll = new QScrollArea(mediainfo_page);
    mediainfo_scroll->setWidgetResizable(true);
    mediainfo_scroll->setFrameShape(QFrame::NoFrame);
    mediainfo_text_ = new QTextEdit(mediainfo_scroll);
    mediainfo_text_->setReadOnly(true);
    mediainfo_text_->setFont(theme::font::monoFont(9));
    mediainfo_text_->setLineWrapMode(QTextEdit::NoWrap);
    mediainfo_text_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mediainfo_scroll->setWidget(mediainfo_text_);
    mediainfo_layout->addWidget(mediainfo_scroll);
    mediainfo_text_->setPlainText(tr("请打开一个媒体文件以查看详细信息"));
    mediainfo_page->setProperty("pageTitle", tr("媒体信息"));
    content_stack_->addWidget(mediainfo_page);
    
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
    // 文本框; 顺序上先于 OpenFinished 到达。
    connect(player_, &player::MediaPlayer::MediaInfoTextReady,
            this, [this](const QString& text) {
                if (!mediainfo_text_ || text.isEmpty()) return;
                mediainfo_text_->setPlainText(text);
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
    // 时间轴与同步诊断（demux 层 packet 时间 / decode 层 frame 时间）
    connect(player_, &player::MediaPlayer::TimelinePacketReady,
            analysis_panel_, &ui::AnalysisPanel::OnTimelinePacket);
    connect(player_, &player::MediaPlayer::FrameTimingReady,
            analysis_panel_, &ui::AnalysisPanel::OnFrameTiming);
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

                // MediaPlayer 直接认 model::AnalysisFeature（面板的 AF 就是它的别名），
                // 所以这里不再需要一个逐个翻译的 switch —— 只剩转发，外加一条纯 UI
                // 侧的联动。新增分析维度时本文件不用改。
                player_->SetAnalysisFeature(feat, enabled);
                // 宏块分析关闭时联动关闭 MV 叠加
                if (feat == AF::Macroblock && !enabled) {
                    player_panel_->SetMvOverlayEnabled(false);
                }
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
                mediainfo_text_->setPlainText(info);
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
    // 媒体信息解析结果回到 UI 线程后贴到文本框 (协调器只发文本, 不碰 UI 控件)
    connect(media_info_coordinator_.get(), &MediaInfoCoordinator::InfoReady,
            this, [this](const QString& text) {
                mediainfo_text_->setPlainText(text);
            });
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
        // 媒体信息框保留 LoadRawImageFile 通过 RawImageInfoReady 写入的 Raw 详情
        // (旧行为: 此处会用“无 MediaInfo 数据”覆盖掉, 导致 Raw 信息一闪而过)
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

    // 媒体信息解析 PCM (裸流无法自动探测, 需带上用户选择的格式参数)
    mediainfo_text_->setPlainText(
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

    // 媒体信息解析: 不再独立探测 —— 打开链路在后台探测时已顺带把同一个上下文
    // 格式化好，提交成功后经 MediaInfoTextReady 贴进文本框（见 SetupConnections）；
    // 这里只放占位文本。打开失败的"分析模式"由 OnPlayerOpenFinished 退回协调器
    // 做尽力而为的独立解析（此时没有可复用的上下文）。
    mediainfo_text_->setPlainText(tr("(正在解析媒体信息…)"));

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
