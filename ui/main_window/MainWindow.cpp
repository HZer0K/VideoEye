#include "ui/main_window/MainWindow.h"
#include "ui/main_window/MediaInfoCoordinator.h"
#include "ui/main_window/menu_bar.h"
#include "ui/main_window/navigation_model.h"
#include "ui/main_window/overview_analysis.h"
#include "ui/main_window/sidebar_navigation.h"
#include "ui/main_window/window_layout.h"
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
    , open_session_(nullptr)
    , analysis_panel_(nullptr)
    , media_info_coordinator_(std::make_unique<MediaInfoCoordinator>()) {

    // 创建播放器实例 (MainWindow 拥有; 分析侧与播放模块共用)
    player_ = new player::MediaPlayer(this);

    // 导出协调器: 自连播放器导出信号。两个查询 lambda 保证协调器不持有本窗口的
    // 具体成员 (raw 查询里的 player_panel_ 在 SetupUI 中才创建, lambda 调用时已就绪)。
    export_coordinator_ = new ExportCoordinator(player_, this, this);
    export_coordinator_->SetSourceQueries(
        [this]() {
            return open_session_ ? open_session_->current_media_url() : QString();
        },
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
    // 分组标题与页面项在同一个列表里，靠委托按行区分绘制（见 sidebar_navigation.cpp）。
    ui::InstallSidebarDelegate(sidebar_);
    
    // 注意：导航项不再硬编码，而是在 SetupContentArea() 末尾由
    // PopulateSidebarItems() 依据 content_stack_ 的真实页面生成，避免增减分析页
    // 后 sidebar 行号与 stack 下标静默错位（曾导致「码率与 GOP」显示成「诊断与报告」）。
    connect(sidebar_, &QListWidget::currentRowChanged, this, &MainWindow::OnSidebarChanged);
}

void MainWindow::PopulateSidebarItems() {
    if (!sidebar_ || !content_stack_) return;

    // 三步都在 ui/main_window/sidebar_navigation.{h,cpp} 里：
    //   1) 从页面控件的动态属性收集元数据（pageId / 标题 / 分组 / 组内排序 / 口径）；
    //   2) BuildNavigationRows 把元数据排成侧栏行（纯口径，见 navigation_model.h）；
    //   3) ApplyNavigationRows 铺进列表并默认选中第一个页面项。
    // 于是增减或重排页面都只改页面自己的登记，侧栏自动跟随。
    const QVector<ui::NavRow> rows =
        ui::BuildNavigationRows(ui::CollectPageNavigation(content_stack_), ui::DefaultNavGroups());
    ui::ApplyNavigationRows(sidebar_, rows);
    // 侧栏与内容区必须显式同步一次：铺条目时信号是静音的。
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
    // 「几条栏 + 两个内容区」怎么凑成最小尺寸是 ui::MinimumWindowSize 的口径
    // （见 ui/main_window/window_layout.h，有单测守着）；这里只负责把各控件的实测
    // 尺寸抄进去。播放区收起时 MinimumHeightHint 返回 0，窗口可以缩得更小。
    ui::WindowLayoutMetrics metrics;
    metrics.app_bar_height = app_bar_ ? app_bar_->height() : 0;
    metrics.menu_bar_height = menuBar() ? menuBar()->sizeHint().height() : 0;
    metrics.status_bar_height = statusBar() ? statusBar()->sizeHint().height() : 0;
    metrics.content_min_height = content_stack_ ? content_stack_->minimumHeight() : 250;
    metrics.player_min_height = player_panel_ ? player_panel_->MinimumHeightHint() : 0;
    metrics.sidebar_width = sidebar_ ? sidebar_->width() : 0;

    const QSize min_sz = ui::MinimumWindowSize(metrics);
    setMinimumSize(min_sz);
    // 本机 WM 在拖拽中完全无视 Qt 的 minimumSize，必须同时告诉原生窗口句柄，
    // 否则拖边框时仍能把窗口缩成一条（见 resizeEvent 里的兜底）。
    if (windowHandle()) {
        windowHandle()->setMinimumSize(min_sz);
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
        // 小屏（典型 1366x768 笔记本）默认走「分析」：口径在 ui::InitialLayoutPreset。
        const auto preset = ui::InitialLayoutPreset(available_height) ==
                                    ui::WindowLayoutPreset::Analysis
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
    if (ui::IsBelowMinimum(cur_sz, min_sz)) {
        resize(cur_sz.expandedTo(min_sz));
    }
}

void MainWindow::SetupMenuBar() {
    // 菜单结构与快捷键全在 ui::BuildMenuBar 里，本窗口只提供每个动作背后该干什么。
    // 需要反向同步勾选状态的那几个动作（播放区显隐 / 两个布局预设）由它返回。
    ui::MenuBarDeps deps;
    deps.open_file = [this]() { OnOpenFile(); };
    deps.open_url = [this]() { OnOpenURL(); };
    deps.exit_app = [this]() { OnExit(); };
    deps.show_format_support = [this]() { OnShowFormatSupport(); };
    deps.export_video_frames = [this]() { export_coordinator_->OnExportVideoFrames(); };
    deps.export_video = [this]() { export_coordinator_->OnExportVideo(); };
    deps.export_audio = [this]() { export_coordinator_->OnExportAudio(); };
    deps.toggle_player_area = [this](bool checked) { OnTogglePlayerArea(checked); };
    deps.apply_layout_preset = [this](PlayerPanel::LayoutPreset preset) {
        if (player_panel_) player_panel_->ApplyLayoutPreset(preset);
    };
    deps.set_seek_mode = [this](model::SeekMode mode) {
        if (player_) player_->SetSeekMode(mode);
    };
    menu_actions_ = ui::BuildMenuBar(menuBar(), deps);
}

void MainWindow::SetupStatusBar() {
    QStatusBar* bar = statusBar();
    bar->showMessage(tr("就绪"));

    // 实时码流统计常驻区 (右端): 用 permanent widget 而非 showMessage,
    // 否则每 10 帧一次的统计会把“已打开 xx”这类临时提示冲掉。
    stats_label_ = new QLabel(bar);
    stats_label_->setFont(theme::font::monoFont(9));
    stats_label_->setStyleSheet(QStringLiteral("color: #8b949e; padding-right: 8px;"));
    stats_label_->setTextInteractionFlags(Qt::NoTextInteraction);
    stats_label_->setVisible(false);  // 有统计数据时才显示
    bar->addPermanentWidget(stats_label_);
}

void MainWindow::SetupOpenSession() {
    // 打开会话控制器: 分派 / 换媒体复位 / 异步收尾 / 下游登记。
    // 依赖在此刻才齐 (player_panel_ / overview_page_ / analysis_panel_ 都是 SetupUI
    // 建的), 所以放在 SetupConnections 开头而不是构造函数里。
    OpenSessionController::Deps deps;
    deps.player = player_;
    deps.player_panel = player_panel_;
    deps.analysis_panel = analysis_panel_;
    deps.ffmpeg_panel = ffmpeg_panel_;
    deps.overview_page = overview_page_;
    deps.media_info = media_info_coordinator_.get();
    // PCM 参数弹窗是 UI 交互 (QInputDialog), 留在本窗口 —— 控制器不认识对话框。
    deps.prompt_pcm = [this](QString& demuxer, int& sample_rate, int& channels) {
        return PromptForPcmSettings(demuxer, sample_rate, channels);
    };
    deps.hide_export_progress = [this]() { export_coordinator_->HideProgress(); };
    deps.apply_macroblock_state = [this]() { ApplyMacroblockAnalysisState(); };
    open_session_ = new OpenSessionController(deps, this);

    connect(open_session_, &OpenSessionController::StatusMessage,
            this, [this](const QString& text, int timeout) {
                statusBar()->showMessage(text, timeout);
            });
    connect(open_session_, &OpenSessionController::CurrentMediaLabelChanged,
            this, [this](const QString& text) {
                if (current_media_label_) current_media_label_->setText(text);
            });
    // 事务式异步打开完成: 收尾在控制器里做 (状态栏/标签/自动播放/诊断扫描)。
    // 过期结果 (期间又开了新文件) 由"没有待收尾登记"判掉, 见 open_session.h。
    connect(player_, &player::MediaPlayer::OpenFinished,
            open_session_, &OpenSessionController::OnPlayerOpenFinished);
}

void MainWindow::SetupConnections() {
    // 播放器信号 - 播放/画面/音频相关已由 PlayerPanel 自行连接 (SetMediaPlayer);
    // 导出进度已由 ExportCoordinator 自连 (其构造函数)。
    // 此处只连接 MainWindow 负责的部分: 打开收尾、分析面板、跨模块转发。
    SetupOpenSession();

    // 导出提示 (协调器只发文本, 状态栏由本窗口转发)
    connect(export_coordinator_, &ExportCoordinator::StatusMessage,
            this, [this](const QString& text, int timeout) {
                statusBar()->showMessage(text, timeout);
            });

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

    // 统一容器结构分析信号（连总线而不是门面：见 core/player/ContainerBus.h）
    player::ContainerBus& container_bus = player_->container_bus();
    connect(&container_bus, &player::ContainerBus::ContainerStructureReady,
            analysis_panel_, &ui::AnalysisPanel::OnContainerStructureReady);
    // 结构分析失败（解析不出来 / 分析器抛异常）：页面显示原因，别让结构页一直空着
    connect(&container_bus, &player::ContainerBus::ContainerStructureFailed,
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
                if (menu_actions_.toggle_player && menu_actions_.toggle_player->isChecked() != visible) {
                    menu_actions_.toggle_player->blockSignals(true);
                    menu_actions_.toggle_player->setChecked(visible);
                    menu_actions_.toggle_player->blockSignals(false);
                }
                UpdateMinimumWindowSize();
            });
    // 布局预设变化时同步视图菜单勾选（预设也可能来自恢复 / 首次自适应，不止菜单）
    connect(player_panel_, &PlayerPanel::LayoutPresetChanged,
            this, [this](PlayerPanel::LayoutPreset preset) {
                const bool analysis = (preset == PlayerPanel::LayoutPreset::Analysis);
                if (menu_actions_.layout_analysis && menu_actions_.layout_analysis->isChecked() != analysis) {
                    menu_actions_.layout_analysis->blockSignals(true);
                    menu_actions_.layout_analysis->setChecked(analysis);
                    menu_actions_.layout_analysis->blockSignals(false);
                }
                if (menu_actions_.layout_monitor && menu_actions_.layout_monitor->isChecked() == analysis) {
                    menu_actions_.layout_monitor->blockSignals(true);
                    menu_actions_.layout_monitor->setChecked(!analysis);
                    menu_actions_.layout_monitor->blockSignals(false);
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
        ui::SelectSidebarRowForStackIndex(sidebar_, i);
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
    // 反查一次 content_stack_ 把下标映回 pageId —— 那段 Qt 侧的读写在
    // ui::ApplySidebarBadges 里，这里只把 AnalysisPanel 的徽标结构翻译成它的入参。
    const QMap<QString, AnalysisPanel::PageBadgeInfo> badges = analysis_panel_->PageBadges();
    ui::ApplySidebarBadges(sidebar_, content_stack_, [&badges](const QString& page_id) {
        const AnalysisPanel::PageBadgeInfo info = badges.value(page_id);
        ui::SidebarBadge badge;
        badge.count = info.issue_count;
        badge.level = info.level;
        badge.scanning = info.scanning;
        return badge;
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

void MainWindow::OnShowFormatSupport() {
    // 清单是现场从本进程链接的 FFmpeg 库枚举出来的，每次打开都重问一遍 ——
    // 换过 FFmpeg 构建（或用户换了一台机器）时不会显示过时的答案。
    FormatSupportDialog dialog(this);
    dialog.exec();
}

bool MainWindow::OpenMedia(const QString& source, bool autoplay) {
    // 分派 / 换媒体复位 / 异步收尾全在 OpenSessionController 里:
    // 本窗口只保留"入口"这一个职责 (命令行参数与菜单都走这里)。
    if (!open_session_) {
        return false;
    }
    return open_session_->Open(source, autoplay);
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
