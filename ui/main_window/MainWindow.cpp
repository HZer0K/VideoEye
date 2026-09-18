#include "MainWindow.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/theme/AppTheme.h"
#include "ui/dialogs/MediaExportDialog.h"
#include "core/exporter/MediaExporter.h"
#include "utils/Logger.h"
#include "utils/ScopedTimer.h"
#include "core/model/EbmlInfo.h"
#include "core/model/ContainerStructureInfo.h"
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
#include <QPainterPath>
#include <QListWidget>
#include <QStackedWidget>
#include <QActionGroup>
#include <algorithm>
#include <QProgressDialog>
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
    , status_bar_(nullptr) {

    // 创建播放器实例 (MainWindow 拥有; 分析侧与播放模块共用)
    player_ = new player::MediaPlayer(this);

    // 应用深色主题
    theme::applyDarkTheme();
    
    SetupUI();
    SetupMenuBar();
    SetupStatusBar();
    SetupConnections();
    // 恢复上次播放区显隐状态 (须在 SetupUI 之后; 若为收起态, Vulkan 会延迟到
    // 展开时由 PlayerPanel 内部 video widget 的 showEvent 触发初始化)
    player_panel_->RestoreVisibility();
    // Vulkan: 由 PlayerPanel 拥有 context/renderer, 失败自动回退 CPU
    player_panel_->InitVulkan();
    UpdateMinimumWindowSize();

    setWindowTitle(tr("VideoEye 2.0 - 视频流分析软件"));
    resize(1200, 800);
}

MainWindow::~MainWindow() {
    if (player_) {
        player_->Stop();
    }
    // MediaInfo 后台线程可能还在跑: 先标记失效再 join, 避免回调打到已析构的控件
    ++mediainfo_generation_;
    if (mediainfo_worker_.joinable()) {
        mediainfo_worker_.join();
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

    sidebar_->blockSignals(true);
    sidebar_->clear();

    const int page_count = content_stack_->count();
    for (int i = 0; i < page_count; ++i) {
        QString title = content_stack_->widget(i)->property("pageTitle").toString();
        if (title.isEmpty()) {
            // 分析页由 AnalysisPanel 提供标题；媒体信息页等外部页走这里
            title = (i == 0) ? tr("媒体信息") : tr("页面 %1").arg(i);
        }
        QListWidgetItem* list_item = new QListWidgetItem(title);
        list_item->setSizeHint(QSize(200, 34));
        sidebar_->addItem(list_item);
    }
    sidebar_->blockSignals(false);

    // 默认选中第一项（blockSignals 期间 setCurrentRow 不会触发切换，显式同步一次）
    sidebar_->setCurrentRow(0);
    content_stack_->setCurrentIndex(0);
}

void MainWindow::SetupContentArea() {
    // 右侧主内容区: 垂直分割器 (播放模块 | 分析内容区)
    content_splitter_ = new QSplitter(Qt::Vertical, this);
    content_splitter_->setChildrenCollapsible(false);
    
    // === 上半区: 播放模块 (视频区 + 控制栏, 可整体收起) ===
    // 播放相关的一切都在 PlayerPanel 内: 视频 widget、控制栏、Vulkan/GDI 渲染、
    // 音频可视化、Raw 序列。MainWindow 只负责把它放进分割器并注入 MediaPlayer。
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

void MainWindow::OnSidebarChanged(int index) {
    if (!content_stack_ || index < 0) return;
    if (index >= content_stack_->count()) {
        qWarning() << "侧边栏行号" << index << "超出页面数" << content_stack_->count()
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
    // 确保 Vulkan 渲染器在窗口显示后初始化 (原生窗口句柄就绪)。
    // 即使子 Widget 的 showEvent 由于平台时序未触发, 这里也能兜底初始化。
    // 播放区收起时 PlayerPanel 内部会跳过, 留到展开时再建 surface / swapchain。
    if (player_panel_) {
        player_panel_->TryInitializeVulkan();
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
    // popup 几何更新统一在 nativeEvent 的 WM_WINDOWPOSCHANGED 处理
    // (此时 Qt 布局已完成, 视频 widget 几何才准确; resizeEvent 中滞后)。
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        // 拖动相关的渲染处理全部转发给 PlayerPanel:
        // WM_ENTERSIZEMOVE/EXITSIZEMOVE 只发给顶层窗口, 子 widget 收不到,
        // 因此这里只做转发, 具体逻辑见 PlayerPanel::OnDragStateChanged。
        if (msg->message == WM_ENTERSIZEMOVE) {
            if (player_panel_) player_panel_->OnDragStateChanged(true);
        } else if (msg->message == WM_EXITSIZEMOVE) {
            if (player_panel_) player_panel_->OnDragStateChanged(false);
        } else if (msg->message == WM_WINDOWPOSCHANGED) {
            // 窗口位置/尺寸变化后派发 (在 WM_SIZE 之后, Qt 布局已完成):
            // 视频 widget 几何此时才准确, 更新 popup 几何并立即以最近帧呈现
            // — 解码线程帧循环更新有最长一帧间隔的滞后, 拖动中会露出残留。
            if (player_panel_) player_panel_->OnWindowPosChanged();
        }
    }
#else
    Q_UNUSED(eventType); Q_UNUSED(message); Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::SetupMenuBar() {
    menu_bar_ = menuBar();
    
    // 文件菜单
    QMenu* file_menu = menu_bar_->addMenu(tr("文件"));
    file_menu->addAction(tr("打开本地文件"), QKeySequence::Open, this, &MainWindow::OnOpenFile);
    file_menu->addAction(tr("打开URL"), QKeySequence("Ctrl+U"), this, &MainWindow::OnOpenURL);
    // 导出 子菜单
    QMenu* export_menu = file_menu->addMenu(tr("导出"));
    export_frames_action_ = export_menu->addAction(tr("导出视频帧..."), this, &MainWindow::OnExportVideoFrames);
    export_menu->addAction(tr("导出视频..."), this, &MainWindow::OnExportVideo);
    export_menu->addAction(tr("导出音频..."), this, &MainWindow::OnExportAudio);
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
    // 播放器信号 - 播放/画面/音频相关已由 PlayerPanel 自行连接 (SetMediaPlayer)。
    // 此处只连接 MainWindow 负责的部分: 导出进度、分析面板、跨模块转发。
    connect(player_, &player::MediaPlayer::VideoFrameExportProgress,
            this, &MainWindow::OnVideoFrameExportProgress);
    connect(player_, &player::MediaPlayer::VideoFrameExportFinished,
            this, &MainWindow::OnVideoFrameExportFinished);
    connect(player_, &player::MediaPlayer::VideoFrameExportCanceled,
            this, [this](int exported_frames, const QString& output_dir) {
                if (export_progress_dialog_) {
                    export_progress_dialog_->reset();
                    export_progress_dialog_->hide();
                }
                const QString msg = tr("已取消导出：已导出 %1 帧\n输出目录：%2").arg(exported_frames).arg(output_dir);
                statusBar()->showMessage(msg);
                QMessageBox::information(this, tr("导出已取消"), msg);
            });
    connect(player_, &player::MediaPlayer::VideoFrameExportError,
            this, &MainWindow::OnVideoFrameExportError);
    connect(player_, &player::MediaPlayer::VideoFrameExportStarted,
            this, [this](int total_frames) {
                export_total_frames_ = total_frames;
                if (!export_progress_dialog_) {
                    export_progress_dialog_ = new QProgressDialog(tr("正在导出视频帧..."),
                                                                  tr("终止"),
                                                                  0,
                                                                  total_frames > 0 ? total_frames : 0,
                                                                  this);
                    export_progress_dialog_->setWindowModality(Qt::ApplicationModal);
                    export_progress_dialog_->setAutoClose(false);
                    export_progress_dialog_->setAutoReset(false);
                    connect(export_progress_dialog_, &QProgressDialog::canceled, this, [this]() {
                        if (!player_) return;
                        statusBar()->showMessage(tr("正在终止导出..."));
                        if (active_export_ == ActiveExport::Media) player_->CancelMediaExport();
                        else player_->CancelVideoFrameExport();
                    });
                } else {
                    export_progress_dialog_->setMaximum(total_frames > 0 ? total_frames : 0);
                }
                export_progress_dialog_->setValue(0);
                export_progress_dialog_->setLabelText(tr("正在导出视频帧..."));
                export_progress_dialog_->show();
            });

    // 音视频导出信号
    connect(player_, &player::MediaPlayer::MediaExportProgress,
            this, &MainWindow::OnMediaExportProgress);
    connect(player_, &player::MediaPlayer::MediaExportFinished,
            this, &MainWindow::OnMediaExportFinished);
    connect(player_, &player::MediaPlayer::MediaExportError,
            this, &MainWindow::OnMediaExportError);
    connect(player_, &player::MediaPlayer::MediaExportStarted,
            this, [this](qint64 duration_ms) {
                Q_UNUSED(duration_ms);
                if (!export_progress_dialog_) {
                    export_progress_dialog_ = new QProgressDialog(tr("正在导出..."),
                                                                  tr("终止"), 0, 100, this);
                    export_progress_dialog_->setWindowModality(Qt::ApplicationModal);
                    export_progress_dialog_->setAutoClose(false);
                    export_progress_dialog_->setAutoReset(false);
                    connect(export_progress_dialog_, &QProgressDialog::canceled, this, [this]() {
                        if (!player_) return;
                        statusBar()->showMessage(tr("正在终止导出..."));
                        if (active_export_ == ActiveExport::Media) player_->CancelMediaExport();
                        else player_->CancelVideoFrameExport();
                    });
                } else {
                    export_progress_dialog_->setMaximum(100);
                }
                export_progress_dialog_->setValue(0);
                export_progress_dialog_->setLabelText(tr("正在导出音视频..."));
                export_progress_dialog_->show();
            });
    connect(player_, &player::MediaPlayer::MediaExportCanceled,
            this, [this](const QString& output_path) {
                if (export_progress_dialog_) {
                    export_progress_dialog_->reset();
                    export_progress_dialog_->hide();
                }
                const QString msg = tr("已取消导出：%1").arg(output_path);
                statusBar()->showMessage(msg);
                QMessageBox::information(this, tr("导出已取消"), msg);
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
    connect(player_, &player::MediaPlayer::MacroblockInfoReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateMacroblockInfo);
    // MV 叠加: 同时转发到播放模块的视频叠加层
    connect(player_, &player::MediaPlayer::MacroblockInfoReady,
            player_panel_, &PlayerPanel::OnMacroblockInfoForOverlay);
    connect(player_, &player::MediaPlayer::SceneChangeReady,
            analysis_panel_, &ui::AnalysisPanel::OnSceneChangeDetected);
    
    // 面板开关信号 -> MediaPlayer 控制
    connect(analysis_panel_, &ui::AnalysisPanel::AnalysisFeatureToggled,
            this, [this](int feature, bool enabled) {
                using AF = ui::AnalysisPanel::AnalysisFeature;
                AF feat = static_cast<AF>(feature);
                if (!player_) return;
                switch (feat) {
                case AF::StreamStats:
                    player_->EnableAnalysis(enabled);
                    break;
                case AF::VideoFrame:
                    player_->SetFrameTypeAnalysisEnabled(enabled);
                    break;
                case AF::AudioFrame:
                    player_->SetAudioFrameAnalysisEnabled(enabled);
                    break;
                case AF::Packet:
                    player_->SetPacketAnalysisEnabled(enabled);
                    break;
                case AF::Event:
                    player_->SetEventAnalysisEnabled(enabled);
                    break;
                case AF::SyncSample:
                    player_->SetSyncAnalysisEnabled(enabled);
                    break;
                case AF::Timeline:
                    player_->SetTimelineAnalysisEnabled(enabled);
                    break;
                case AF::ContainerStructure:
                    player_->SetContainerStructureEnabled(enabled);
                    break;
                case AF::Macroblock:
                    player_->SetMacroblockAnalysisEnabled(enabled);
                    // 宏块分析关闭时联动关闭 MV 叠加
                    if (!enabled) {
                        player_panel_->SetMvOverlayEnabled(false);
                    }
                    break;
                case AF::SceneChange:
                    player_->SetSceneChangeAnalysisEnabled(enabled);
                    break;
                default:
                    break;
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
}

void MainWindow::OnOpenFile() {
    qDebug() << "\n========== OnOpenFile START ==========";
    
    QString filename = QFileDialog::getOpenFileName(this,
        tr("打开媒体文件"), "",
        tr("媒体文件 (*.mp4 *.avi *.mkv *.flv *.ts *.mp3 *.aac *.wav *.pcm *.yuv *.nv12 *.rgb *.bgr *.yuy2 *.raw);;所有文件 (*)"));
    
    qDebug() << "[1] 选择的文件:" << filename;
    
    if (filename.isEmpty()) {
        qDebug() << "[1.5] 文件名为空，返回";
        return;
    }
    OpenMedia(filename, true);
    
    qDebug() << "========== OnOpenFile END ==========\n";
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

    // 打开新文件前先终止进行中的导出 (与停止播放配套)
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        player_->CancelVideoFrameExport();
        export_progress_dialog_->reset();
        export_progress_dialog_->hide();
    }
    // 停止当前播放并清理播放模块状态 (Raw/音频可视化/进度条等)
    player_panel_->StopPlayback();

    const QString suffix = QFileInfo(source).suffix().toLower();
    if (suffix == "yuv" || suffix == "nv12" || suffix == "rgb" ||
        suffix == "bgr" || suffix == "yuy2" || suffix == "raw") {
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

        // MediaInfo 解析 PCM
        {
            analyzer::MediaInfoAnalyzer mi;
            if (mi.Open(source)) {
                mediainfo_text_->setPlainText(mi.GetCompleteInfo());
            } else {
                mediainfo_text_->setPlainText(tr("(无法解析 PCM 媒体信息)"));
            }
        }

        if (autoplay) {
            player_->Play();
        }
        return true;
    }

    player_panel_->SetRawImageMode(false);
    player_panel_->SetCurrentSource(source);
    bool open_result = false;
    {
        VE_PERF("MediaPlayer::Open");
        open_result = player_->Open(source);
    }

    // 打开失败 (文件损坏/截断/格式不受支持/无可播放流) 不再直接中止:
    // 仍把文件加载到分析模块, 由媒体信息/文件结构/诊断扫描给出错误原因。
    if (open_result) {
        statusBar()->showMessage(tr("已打开: %1").arg(source));
    } else {
        statusBar()->showMessage(tr("无法播放 (已进入分析模式): %1").arg(player_->GetLastError()), 0);
    }
    if (current_media_label_) {
        current_media_label_->setText(open_result ? source : tr("%1 (无法播放)").arg(source));
    }
    current_media_url_ = source;
    analysis_panel_->SetCurrentVideoPath(source);

    // MediaInfo 解析 (异常文件也可能部分解析成功, 尽力而为)。
    // 大文件/复杂容器下 MediaInfoLib 全量解析可能要到秒级, 放在后台线程跑,
    // 结果用 generation 校验后再回主线程贴文本, 避免快速切换文件时结果串台。
    StartMediaInfoAnalysis(source);

    // 同步已启用的分析功能到播放器 (复选框默认勾选但未触发信号)
    analysis_panel_->EmitInitialFeatureStates();

    if (open_result) {
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
    }
    return true;
}

void MainWindow::StartMediaInfoAnalysis(const QString& source) {
    const quint64 generation = ++mediainfo_generation_;
    mediainfo_text_->setPlainText(tr("(正在解析媒体信息…)"));

    if (mediainfo_worker_.joinable()) {
        // 上一次还没跑完: 等它结束再起新的, 避免并发持有 MediaInfo 句柄
        mediainfo_worker_.join();
    }

    mediainfo_worker_ = std::thread([this, source, generation]() {
        QString text;
        {
            VE_PERF("MediaInfo 解析(后台线程)");
            analyzer::MediaInfoAnalyzer mi;
            text = mi.Open(source) ? mi.GetCompleteInfo() : tr("(无法解析媒体信息)");
        }
        QMetaObject::invokeMethod(this, [this, generation, text]() {
            if (generation != mediainfo_generation_) return;   // 已经切到别的文件
            mediainfo_text_->setPlainText(text);
        }, Qt::QueuedConnection);
    });
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

void MainWindow::OnExportVideoFrames() {
    if (!player_) {
        return;
    }
    if (current_media_url_.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("请先打开一个视频文件"));
        return;
    }
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        QMessageBox::information(this, tr("提示"), tr("正在导出中，请先终止或等待完成"));
        return;
    }
    active_export_ = ActiveExport::Frames;

    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择导出目录"), "");
    if (dir.isEmpty()) {
        return;
    }

    bool ok = false;
    const QStringList items = {
        "jpg",
        "yuv",
        "rgb"
    };
    const QString format = QInputDialog::getItem(this, tr("导出格式"),
                                                 tr("选择导出格式:"),
                                                 items, 0, false, &ok);
    if (!ok || format.isEmpty()) {
        return;
    }

    int quality = 90;
    if (format == "jpg") {
        quality = QInputDialog::getInt(this, tr("JPG质量"),
                                       tr("JPG质量(1-100):"),
                                       90, 1, 100, 1, &ok);
        if (!ok) {
            return;
        }
    }

    const int interval = QInputDialog::getInt(this, tr("抽帧间隔"),
                                              tr("每 N 帧导出 1 帧 (N>=1):"),
                                              1, 1, 1000000, 1, &ok);
    if (!ok) {
        return;
    }

    statusBar()->showMessage(tr("开始导出视频帧..."));
    player_->StartVideoFrameExport(dir, format, quality, interval);
}

void MainWindow::OnExportVideo() {
    if (!player_ || current_media_url_.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("请先打开一个视频文件"));
        return;
    }
    if (player_panel_ && player_panel_->IsShowingRawImage()) {
        QMessageBox::information(this, tr("提示"), tr("当前为图像文件，无法导出视频"));
        return;
    }
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        QMessageBox::information(this, tr("提示"), tr("正在导出中，请先终止或等待完成"));
        return;
    }
    active_export_ = ActiveExport::Media;
    ui::MediaExportDialog dlg(this, exporter::ExportKind::Video, current_media_url_, player_->GetDuration());
    if (dlg.exec() != QDialog::Accepted) return;
    statusBar()->showMessage(tr("开始导出视频..."));
    player_->StartMediaExport(dlg.GetOptions());
}

void MainWindow::OnExportAudio() {
    if (!player_ || current_media_url_.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("请先打开一个视频文件"));
        return;
    }
    if (player_panel_ && player_panel_->IsShowingRawImage()) {
        QMessageBox::information(this, tr("提示"), tr("当前为图像文件，无法导出音频"));
        return;
    }
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        QMessageBox::information(this, tr("提示"), tr("正在导出中，请先终止或等待完成"));
        return;
    }
    active_export_ = ActiveExport::Media;
    ui::MediaExportDialog dlg(this, exporter::ExportKind::Audio, current_media_url_, player_->GetDuration());
    if (dlg.exec() != QDialog::Accepted) return;
    statusBar()->showMessage(tr("开始导出音频..."));
    player_->StartMediaExport(dlg.GetOptions());
}

void MainWindow::OnExit() {
    close();
}

void MainWindow::OnVideoFrameExportProgress(int exported_frames) {
    if (export_progress_dialog_) {
        if (export_total_frames_ > 0) {
            export_progress_dialog_->setMaximum(export_total_frames_);
            export_progress_dialog_->setValue(std::min(exported_frames, export_total_frames_));
        } else {
            export_progress_dialog_->setMaximum(0);
            export_progress_dialog_->setValue(0);
        }
        export_progress_dialog_->setLabelText(tr("已导出 %1 帧").arg(exported_frames));
    }
    statusBar()->showMessage(tr("已导出 %1 帧").arg(exported_frames));
}

void MainWindow::OnVideoFrameExportFinished(const QString& output_dir) {
    if (export_progress_dialog_) {
        export_progress_dialog_->reset();
        export_progress_dialog_->hide();
    }
    statusBar()->showMessage(tr("导出完成: %1").arg(output_dir));
}

void MainWindow::OnVideoFrameExportError(const QString& message) {
    if (export_progress_dialog_) {
        export_progress_dialog_->reset();
        export_progress_dialog_->hide();
    }
    statusBar()->showMessage(tr("导出失败: %1").arg(message));
    QMessageBox::warning(this, tr("导出失败"), message);
}

void MainWindow::OnMediaExportProgress(int percent) {
    if (export_progress_dialog_) {
        export_progress_dialog_->setMaximum(100);
        export_progress_dialog_->setValue(percent);
        export_progress_dialog_->setLabelText(tr("正在导出音视频... %1%").arg(percent));
    }
}

void MainWindow::OnMediaExportFinished(const QString& output_path) {
    if (export_progress_dialog_) {
        export_progress_dialog_->reset();
        export_progress_dialog_->hide();
    }
    const QString msg = tr("导出完成: %1").arg(output_path);
    statusBar()->showMessage(msg);
    QMessageBox::information(this, tr("导出完成"), msg);
}

void MainWindow::OnMediaExportError(const QString& message) {
    if (export_progress_dialog_) {
        export_progress_dialog_->reset();
        export_progress_dialog_->hide();
    }
    statusBar()->showMessage(tr("导出失败: %1").arg(message));
    QMessageBox::warning(this, tr("导出失败"), message);
}

} // namespace ui
} // namespace videoeye
