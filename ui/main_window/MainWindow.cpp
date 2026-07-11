#include "MainWindow.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/theme/AppTheme.h"
#include "ui/dialogs/MediaExportDialog.h"
#include "core/exporter/MediaExporter.h"
#include "core/model/EbmlInfo.h"
#include "core/model/ContainerStructureInfo.h"
#include "core/model/AudioVisualizationFrame.h"
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
    , video_widget_(nullptr)
    , control_bar_(nullptr)
    , seek_slider_(nullptr)
    , play_pause_button_(nullptr)
    , stop_button_(nullptr)
    , prev_frame_button_(nullptr)
    , next_frame_button_(nullptr)
    , time_label_(nullptr)
    , mediainfo_text_(nullptr)
    , current_media_label_(nullptr)
    , analysis_panel_(nullptr)
    , menu_bar_(nullptr)
    , status_bar_(nullptr) {
    
    // 创建播放器实例
    player_ = new player::MediaPlayer(this);
    
    // 应用深色主题
    theme::applyDarkTheme();
    
    SetupUI();
    SetupMenuBar();
    SetupStatusBar();
    SetupConnections();
    UpdateMinimumWindowSize();
    audio_vis_timer_.start();

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
    
    // 导航项 (媒体信息 + 8个分析功能: 流分析/帧分析/包/事件与时间轴/响度/直方图/结构/宏块)
    QStringList nav_items = {
        tr("媒体信息"),
        tr("流分析"),
        tr("帧分析"),
        tr("数据包"),
        tr("事件与时间轴"),
        tr("音频响度"),
        tr("直方图"),
        tr("容器结构"),
        tr("宏块分析"),
        tr("场景切换"),
        tr("质量评估")
    };
    
    for (const QString& item : nav_items) {
        QListWidgetItem* list_item = new QListWidgetItem(item);
        list_item->setSizeHint(QSize(200, 34));
        sidebar_->addItem(list_item);
    }
    
    // 默认选中第一项
    sidebar_->setCurrentRow(0);
    
    connect(sidebar_, &QListWidget::currentRowChanged, this, &MainWindow::OnSidebarChanged);
}

void MainWindow::SetupContentArea() {
    // 右侧主内容区: 垂直分割器 (视频区+控制栏 | 分析内容区)
    content_splitter_ = new QSplitter(Qt::Vertical, this);
    content_splitter_->setChildrenCollapsible(false);
    
    // === 上半区: 视频 + 控制栏 ===
    QWidget* top_area = new QWidget(content_splitter_);
    QVBoxLayout* top_layout = new QVBoxLayout(top_area);
    top_layout->setContentsMargins(0, 0, 0, 0);
    top_layout->setSpacing(0);
    
    // 视频显示区
    video_widget_ = new VulkanVideoWidget(top_area);
    video_widget_->setMinimumSize(320, 160);
    video_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    top_layout->addWidget(video_widget_);
    
    // 控制栏
    control_bar_ = new QWidget(top_area);
    control_bar_->setObjectName("ControlBar");
    control_bar_->setFixedHeight(56);
    
    QHBoxLayout* control_layout = new QHBoxLayout(control_bar_);
    control_layout->setContentsMargins(16, 8, 16, 8);
    control_layout->setSpacing(12);
    
    // 播放/暂停按钮 (圆形主按钮)
    play_pause_button_ = new QPushButton(control_bar_);
    play_pause_button_->setObjectName("playPauseButton");
    play_pause_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    play_pause_button_->setIconSize(QSize(16, 16));
    play_pause_button_->setToolTip(tr("播放/暂停"));
    control_layout->addWidget(play_pause_button_);
    
    // 停止按钮
    stop_button_ = new QPushButton(control_bar_);
    stop_button_->setObjectName("stopButton");
    stop_button_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    stop_button_->setIconSize(QSize(14, 14));
    stop_button_->setToolTip(tr("停止"));
    control_layout->addWidget(stop_button_);
    
    // 上一帧/下一帧 (默认隐藏)
    prev_frame_button_ = new QPushButton(tr("上一帧"), control_bar_);
    prev_frame_button_->setVisible(false);
    control_layout->addWidget(prev_frame_button_);
    
    next_frame_button_ = new QPushButton(tr("下一帧"), control_bar_);
    next_frame_button_->setVisible(false);
    control_layout->addWidget(next_frame_button_);
    
    // 定位方式 (关键帧 / 精确值) 已移至顶部菜单: 播放设置 → seek方式

    // 进度条
    seek_slider_ = new QSlider(Qt::Horizontal, control_bar_);
    seek_slider_->setRange(0, 0);
    control_layout->addWidget(seek_slider_, 1);
    
    // 时间显示
    time_label_ = new QLabel(tr("00:00:00 / 00:00:00"), control_bar_);
    time_label_->setObjectName("TimeLabel");
    time_label_->setMinimumWidth(140);
    time_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    control_layout->addWidget(time_label_);
    
    top_layout->addWidget(control_bar_);
    
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
    content_stack_->addWidget(mediainfo_page);
    
    // Page 1-10: 分析面板各页
    // AnalysisPanel 会创建自己的页面并添加到 content_stack_
    analysis_panel_ = new ui::AnalysisPanel(content_stack_);
    analysis_panel_->PopulateStackedWidget(content_stack_);
    analysis_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
    content_splitter_->addWidget(top_area);
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
    content_stack_->setCurrentIndex(index);
}

void MainWindow::UpdateMinimumWindowSize() {
    const int video_min = video_widget_ ? video_widget_->minimumHeight() : 160;
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

    int min_height = bars + content_min + video_min + 56; // 56 = control bar height

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
}

void MainWindow::SetupConnections() {
    // 播放器信号连接 - 基本功能
    connect(player_, &player::MediaPlayer::StateChanged,
            this, &MainWindow::OnStateChanged);
    connect(player_, &player::MediaPlayer::FrameReady,
            this, &MainWindow::OnFrameReady);
    connect(player_, &player::MediaPlayer::PositionChanged,
            this, &MainWindow::OnPositionChanged);
    connect(player_, &player::MediaPlayer::Error,
            this, &MainWindow::OnError);
    connect(player_, &player::MediaPlayer::PlaybackFinished,
            this, &MainWindow::OnPlaybackFinished);
    connect(player_, &player::MediaPlayer::MediaModeChanged,
            this, &MainWindow::OnMediaModeChanged);
    connect(player_, &player::MediaPlayer::AudioLevelReady,
            this, &MainWindow::OnAudioLevelReady);
    connect(player_, &player::MediaPlayer::AudioVisualizationReady,
            this, &MainWindow::OnAudioVisualizationForDisplay);
    connect(player_, &player::MediaPlayer::AudioVisualizationReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateAudioLoudness);
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
    connect(player_, &player::MediaPlayer::HistogramReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateHistogram);
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
    
    // 统一容器结构分析信号
    connect(player_, &player::MediaPlayer::ContainerStructureReady,
            analysis_panel_, &ui::AnalysisPanel::OnContainerStructureReady);
    connect(player_, &player::MediaPlayer::MacroblockInfoReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateMacroblockInfo);
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
                    if (!enabled) {
                        player_->SetHistogramEnabled(false);
                    }
                    break;
                case AF::VideoFrame:
                    player_->SetFrameTypeAnalysisEnabled(enabled);
                    break;
                case AF::Histogram:
                    if (enabled) player_->EnableAnalysis(true);
                    player_->SetHistogramEnabled(enabled);
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
                case AF::AudioLoudness:
                    // 音频响度复用 AudioVisualization 数据流，无需单独控制
                    break;
                case AF::ContainerStructure:
                    player_->SetContainerStructureEnabled(enabled);
                    break;
                case AF::Macroblock:
                    player_->SetMacroblockAnalysisEnabled(enabled);
                    break;
                case AF::SceneChange:
                    player_->SetSceneChangeAnalysisEnabled(enabled);
                    break;
                case AF::Quality:
                    // 质量评估为离线批量任务, 由面板内的"开始评估"触发, 此处无需控制播放器
                    break;
                default:
                    break;
                }
            });
    
    // 控件信号连接
    connect(play_pause_button_, &QPushButton::clicked, this, &MainWindow::OnPlayPause);
    connect(stop_button_, &QPushButton::clicked, this, &MainWindow::OnStop);
    connect(prev_frame_button_, &QPushButton::clicked, this, &MainWindow::OnPrevRawFrame);
    connect(next_frame_button_, &QPushButton::clicked, this, &MainWindow::OnNextRawFrame);
    // 进度条交互:
    //  - 拖动中由 sliderMoved 做"节流的关键帧预览" (画面跟手且不过度占用 UI 线程)
    //  - valueChanged 仅在非拖动时生效 (键盘方向键/程序化跳转), 按当前模式定位
    //  - 释放时由 sliderReleased 按当前选择的定位方式 (关键帧/精确值) 做最终定位
    connect(seek_slider_, &QSlider::sliderPressed, this, [this]() {
        slider_dragging_ = true;
        if (player_) player_->SetSeekDragging(true); // 拖动期间抑制音频, 避免杂音
    });
    connect(seek_slider_, &QSlider::sliderMoved, this, [this](int v) {
        // 拖动中: 实时关键帧预览 (画面跟手)。节流到 ~100ms 一次, 避免每像素都调 av_seek_frame。
        if (showing_raw_image_) { ShowRawFrame(v); return; }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - last_drag_seek_ms_ < 100) return;
        last_drag_seek_ms_ = now;
        if (player_) player_->Seek(v, model::SeekMode::NearestKeyframe);
    });
    connect(seek_slider_, &QSlider::valueChanged, this, [this](int v) {
        if (slider_dragging_) return; // 拖动中由 sliderMoved 处理预览, 此处跳过
        OnSeek(v); // 键盘/程序化跳转: 按当前模式定位
    });
    connect(seek_slider_, &QSlider::sliderReleased, this, [this]() {
        slider_dragging_ = false;
        if (player_) player_->SetSeekDragging(false); // 恢复音频
        OnSeek(seek_slider_->value()); // 释放时按当前定位方式真正 seek
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

    OnStop();

    const QString suffix = QFileInfo(source).suffix().toLower();
    if (suffix == "yuv" || suffix == "nv12" || suffix == "rgb" ||
        suffix == "bgr" || suffix == "yuy2" || suffix == "raw") {
        showing_raw_image_ = true;
        current_media_url_.clear();
        if (!LoadRawImageFile(source)) {
            statusBar()->showMessage(tr("打开失败: %1").arg(source));
            return false;
        }
        statusBar()->showMessage(tr("已打开图像: %1").arg(source));
        if (current_media_label_) {
            current_media_label_->setText(source);
        }
        mediainfo_text_->setPlainText(tr("(原始图像文件，无 MediaInfo 数据)"));
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

        showing_raw_image_ = false;
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

    showing_raw_image_ = false;
    const bool open_result = player_->Open(source);
    if (!open_result) {
        statusBar()->showMessage(tr("打开失败: %1").arg(source));
        return false;
    }

    statusBar()->showMessage(tr("已打开: %1").arg(source));
    if (current_media_label_) {
        current_media_label_->setText(source);
    }
    current_media_url_ = source;
    analysis_panel_->SetCurrentVideoPath(source);

    // MediaInfo 解析
    {
        analyzer::MediaInfoAnalyzer mi;
        if (mi.Open(source)) {
            mediainfo_text_->setPlainText(mi.GetCompleteInfo());
        } else {
            mediainfo_text_->setPlainText(tr("(无法解析媒体信息)"));
        }
    }

    // 同步已启用的分析功能到播放器 (复选框默认勾选但未触发信号)
    analysis_panel_->EmitInitialFeatureStates();

    if (autoplay) {
        player_->Play();
    }
    return true;
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
    if (showing_raw_image_) {
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
    if (showing_raw_image_) {
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

void MainWindow::OnPrevRawFrame() {
    if (showing_raw_image_ && raw_current_frame_ > 0) {
        ShowRawFrame(raw_current_frame_ - 1);
    }
}

void MainWindow::OnNextRawFrame() {
    if (showing_raw_image_ && raw_current_frame_ + 1 < raw_total_frames_) {
        ShowRawFrame(raw_current_frame_ + 1);
    }
}

bool MainWindow::LoadRawImageFile(const QString& filename) {
    const QString suffix = QFileInfo(filename).suffix().toLower();
    if (suffix != "yuv" && suffix != "nv12" && suffix != "rgb" &&
        suffix != "bgr" && suffix != "yuy2" && suffix != "raw") {
        return false;
    }

    int default_width = 1920;
    int default_height = 1080;
    {
        const QString name = QFileInfo(filename).fileName().toLower();
        QRegularExpression re_wh(R"((\d{2,5})\s*[xX]\s*(\d{2,5}))");
        QRegularExpressionMatch m = re_wh.match(name);
        if (m.hasMatch()) {
            bool ok_w = false;
            bool ok_h = false;
            const int w = m.captured(1).toInt(&ok_w);
            const int h = m.captured(2).toInt(&ok_h);
            if (ok_w && ok_h && w > 0 && h > 0) {
                default_width = w;
                default_height = h;
            }
        } else {
            QRegularExpression re_w_h(R"(w(\d{2,5}).*h(\d{2,5}))", QRegularExpression::CaseInsensitiveOption);
            m = re_w_h.match(name);
            if (m.hasMatch()) {
                bool ok_w = false;
                bool ok_h = false;
                const int w = m.captured(1).toInt(&ok_w);
                const int h = m.captured(2).toInt(&ok_h);
                if (ok_w && ok_h && w > 0 && h > 0) {
                    default_width = w;
                    default_height = h;
                }
            }
        }
    }

    bool ok = false;
    const int width = QInputDialog::getInt(this, tr("图像宽度"),
                                           tr("请输入宽度(px):"),
                                           default_width, 1, 16384, 1, &ok);
    if (!ok) {
        return false;
    }
    const int height = QInputDialog::getInt(this, tr("图像高度"),
                                            tr("请输入高度(px):"),
                                            default_height, 1, 16384, 1, &ok);
    if (!ok) {
        return false;
    }

    const QString lower_name = QFileInfo(filename).fileName().toLower();
    const QStringList formats = {
        "YUV420P(I420)",
        "NV12",
        "YUY2",
        "RGB24",
        "BGR24"
    };

    QString default_format = "YUV420P(I420)";
    if (suffix == "nv12" || lower_name.contains("nv12")) {
        default_format = "NV12";
    } else if (suffix == "yuy2" || lower_name.contains("yuy2")) {
        default_format = "YUY2";
    } else if (suffix == "rgb" || lower_name.contains("rgb24") || lower_name.contains("_rgb")) {
        default_format = "RGB24";
    } else if (suffix == "bgr" || lower_name.contains("bgr24") || lower_name.contains("_bgr")) {
        default_format = "BGR24";
    }

    const QString selected = QInputDialog::getItem(this, tr("原始图像格式"),
                                                   tr("选择像素格式:"),
                                                   formats,
                                                   formats.indexOf(default_format),
                                                   false, &ok);
    if (!ok || selected.isEmpty()) {
        return false;
    }

    qint64 frame_size = 0;
    if (selected == "RGB24" || selected == "BGR24") {
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3;
    } else if (selected == "YUV420P(I420)" || selected == "NV12") {
        if ((width % 2) != 0 || (height % 2) != 0) {
            QMessageBox::warning(this, tr("尺寸不支持"),
                                 tr("%1 仅支持偶数宽高。").arg(selected));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3 / 2;
    } else if (selected == "YUY2") {
        if ((width % 2) != 0) {
            QMessageBox::warning(this, tr("尺寸不支持"),
                                 tr("YUY2 仅支持偶数宽度。"));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 2;
    }

    const qint64 file_size = QFileInfo(filename).size();
    if (frame_size <= 0 || file_size < frame_size) {
        QMessageBox::warning(this, tr("打开失败"),
                             tr("文件大小不足以组成一帧。\n单帧大小: %1 字节\n文件大小: %2 字节")
                                 .arg(frame_size)
                                 .arg(file_size));
        return false;
    }

    raw_image_path_ = filename;
    raw_pixel_format_ = selected;
    raw_width_ = width;
    raw_height_ = height;
    raw_frame_size_ = frame_size;
    raw_total_frames_ = std::max(1, static_cast<int>(file_size / frame_size));
    raw_current_frame_ = 0;

    const qint64 remain = file_size % frame_size;
    mediainfo_text_->setPlainText(
        tr("Raw Image Sequence\n"
           "File: %1\n"
           "Format: %2\n"
           "Size: %3x%4\n"
           "Frame Size: %5 bytes\n"
           "Total Frames: %6\n"
           "Ignored Tail Bytes: %7")
            .arg(filename)
            .arg(selected)
            .arg(width)
            .arg(height)
            .arg(frame_size)
            .arg(raw_total_frames_)
            .arg(remain));

    return ShowRawFrame(0);
}

bool MainWindow::ShowRawFrame(int frame_index) {
    if (!showing_raw_image_ || raw_image_path_.isEmpty() || raw_frame_size_ <= 0 ||
        frame_index < 0 || frame_index >= raw_total_frames_) {
        return false;
    }

    QFile file(raw_image_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("打开失败"), tr("无法读取文件: %1").arg(raw_image_path_));
        return false;
    }

    const qint64 offset = static_cast<qint64>(frame_index) * raw_frame_size_;
    if (!file.seek(offset)) {
        QMessageBox::warning(this, tr("定位失败"),
                             tr("无法定位到第 %1 帧。").arg(frame_index + 1));
        return false;
    }

    const QByteArray data = file.read(raw_frame_size_);
    file.close();
    if (data.size() != raw_frame_size_) {
        QMessageBox::warning(this, tr("读取失败"),
                             tr("读取第 %1 帧失败，期望 %2 字节，实际 %3 字节。")
                                 .arg(frame_index + 1)
                                 .arg(raw_frame_size_)
                                 .arg(data.size()));
        return false;
    }

    auto clamp_to_u8 = [](int value) -> uchar {
        return static_cast<uchar>(qBound(0, value, 255));
    };
    auto yuv_to_rgb = [&](int Y, int U, int V, uchar* dst) {
        const int u = U - 128;
        const int v = V - 128;
        const int r = Y + static_cast<int>(1.402 * v);
        const int g = Y - static_cast<int>(0.344136 * u + 0.714136 * v);
        const int b = Y + static_cast<int>(1.772 * u);
        dst[0] = clamp_to_u8(r);
        dst[1] = clamp_to_u8(g);
        dst[2] = clamp_to_u8(b);
    };

    QImage img(raw_width_, raw_height_, QImage::Format_RGB888);
    if (img.isNull()) {
        return false;
    }

    if (raw_pixel_format_ == "RGB24" || raw_pixel_format_ == "BGR24") {
        const uchar* src = reinterpret_cast<const uchar*>(data.constData());
        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const uchar* src_row = src + static_cast<qint64>(j) * raw_width_ * 3;
            for (int i = 0; i < raw_width_; ++i) {
                const int src_idx = i * 3;
                if (raw_pixel_format_ == "RGB24") {
                    row[src_idx + 0] = src_row[src_idx + 0];
                    row[src_idx + 1] = src_row[src_idx + 1];
                    row[src_idx + 2] = src_row[src_idx + 2];
                } else {
                    row[src_idx + 0] = src_row[src_idx + 2];
                    row[src_idx + 1] = src_row[src_idx + 1];
                    row[src_idx + 2] = src_row[src_idx + 0];
                }
            }
        }
    } else if (raw_pixel_format_ == "YUV420P(I420)" || raw_pixel_format_ == "NV12") {
        const qint64 y_size = static_cast<qint64>(raw_width_) * static_cast<qint64>(raw_height_);
        const uchar* y_plane = reinterpret_cast<const uchar*>(data.constData());
        const uchar* uv_plane = y_plane + y_size;
        const qint64 uv_size = y_size / 4;
        const uchar* u_plane = (raw_pixel_format_ == "NV12") ? nullptr : uv_plane;
        const uchar* v_plane = (raw_pixel_format_ == "NV12") ? nullptr : (uv_plane + uv_size);

        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const int uv_j_i420 = (j / 2) * (raw_width_ / 2);
            const int uv_j_nv12 = (j / 2) * raw_width_;
            for (int i = 0; i < raw_width_; ++i) {
                const int y_idx = j * raw_width_ + i;
                int U = 0;
                int V = 0;
                if (raw_pixel_format_ == "NV12") {
                    const int uv_idx = uv_j_nv12 + (i / 2) * 2;
                    U = static_cast<int>(uv_plane[uv_idx]);
                    V = static_cast<int>(uv_plane[uv_idx + 1]);
                } else {
                    const int uv_idx = uv_j_i420 + (i / 2);
                    U = static_cast<int>(u_plane[uv_idx]);
                    V = static_cast<int>(v_plane[uv_idx]);
                }
                yuv_to_rgb(static_cast<int>(y_plane[y_idx]), U, V, row + i * 3);
            }
        }
    } else if (raw_pixel_format_ == "YUY2") {
        const uchar* src = reinterpret_cast<const uchar*>(data.constData());
        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const uchar* src_row = src + static_cast<qint64>(j) * raw_width_ * 2;
            for (int i = 0; i < raw_width_; i += 2) {
                const int idx = i * 2;
                const int y0 = static_cast<int>(src_row[idx + 0]);
                const int u = static_cast<int>(src_row[idx + 1]);
                const int y1 = static_cast<int>(src_row[idx + 2]);
                const int v = static_cast<int>(src_row[idx + 3]);
                yuv_to_rgb(y0, u, v, row + i * 3);
                if (i + 1 < raw_width_) {
                    yuv_to_rgb(y1, u, v, row + (i + 1) * 3);
                }
            }
        }
    } else {
        return false;
    }

    raw_current_frame_ = frame_index;
    video_widget_->SetFallbackImage(img);
    UpdateRawNavigationState();
    statusBar()->showMessage(tr("Raw 帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_));
    return true;
}

void MainWindow::UpdateRawNavigationState() {
    const bool raw_mode = showing_raw_image_ && raw_total_frames_ > 0;

    if (prev_frame_button_) {
        prev_frame_button_->setVisible(raw_mode);
        prev_frame_button_->setEnabled(raw_mode && raw_current_frame_ > 0);
    }
    if (next_frame_button_) {
        next_frame_button_->setVisible(raw_mode);
        next_frame_button_->setEnabled(raw_mode && raw_current_frame_ + 1 < raw_total_frames_);
    }
    if (play_pause_button_) {
        play_pause_button_->setEnabled(!raw_mode);
    }

    if (raw_mode) {
        QSignalBlocker blocker(seek_slider_);
        seek_slider_->setRange(0, std::max(0, raw_total_frames_ - 1));
        seek_slider_->setValue(raw_current_frame_);
        seek_slider_->setEnabled(raw_total_frames_ > 1);
        time_label_->setText(tr("帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_));
    } else {
        seek_slider_->setEnabled(true);
        time_label_->setText(tr("00:00:00 / 00:00:00"));
    }
}

void MainWindow::OnPlayPause() {
    if (!player_) {
        return;
    }
    
    if (showing_raw_image_) {
        statusBar()->showMessage(tr("当前为图像文件，无法播放"));
        return;
    }
    
    const auto state = player_->GetState();
    
    // 如果处于Idle/Stopped/Error状态，先打开媒体
    if ((state == model::PlayerState::Idle ||
         state == model::PlayerState::Stopped ||
         state == model::PlayerState::Error) &&
        !current_media_url_.isEmpty()) {
        if (!player_->Open(current_media_url_)) {
            statusBar()->showMessage(tr("打开失败: %1").arg(current_media_url_));
            return;
        }
    }
    
    // 根据当前状态切换播放/暂停
    if (state == model::PlayerState::Playing) {
        player_->Pause();
    } else {
        player_->Play();
    }
}

void MainWindow::ResetVideoUI() {
    video_widget_->Clear();
    seek_slider_->setValue(0);
    seek_slider_->setRange(0, 0);
    time_label_->setText(tr("00:00:00 / 00:00:00"));
}

void MainWindow::OnStop() {
    // 清理其他状态（不立即清理视频UI，让OnStateChanged统一处理）
    audio_level_history_.clear();
    spectrum_history_.clear();
    audio_only_mode_ = false;
    audio_vis_last_render_ms_ = -1;
    audio_vis_smoothed_ = 0.0;
    audio_vis_target_ = 0.0;
    latest_spectrum_bins_.clear();
    latest_waveform_points_.clear();
    smoothed_spectrum_bins_.clear();
    album_cover_ = QImage();
    showing_raw_image_ = false;
    raw_image_path_.clear();
    raw_pixel_format_.clear();
    raw_width_ = 0;
    raw_height_ = 0;
    raw_frame_size_ = 0;
    raw_total_frames_ = 0;
    raw_current_frame_ = 0;
    UpdateRawNavigationState();
    
    // 停止播放器，会触发OnStateChanged(Stopped)来清理视频UI
    if (player_) {
        player_->Stop();
    }
    
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        player_->CancelVideoFrameExport();
        export_progress_dialog_->reset();
        export_progress_dialog_->hide();
    }
}

void MainWindow::OnSeek(int value) {
    if (showing_raw_image_) {
        ShowRawFrame(value);
        return;
    }
    if (!player_) return;
    // 去重: 释放进度条时 sliderReleased 与尾随的 valueChanged 会用相同目标值
    // 在短时间内各调一次 OnSeek, 这里跳过重复的那次, 避免双 seek。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (value == last_seek_value_ && now - last_seek_time_ < 100) return;
    last_seek_value_ = value;
    last_seek_time_ = now;
    // 按当前选择的定位方式 (关键帧 / 精确帧) 执行 seek
    player_->Seek(value, player_->GetSeekMode());
}

void MainWindow::OnStateChanged(model::PlayerState state) {
    QString state_text;
    switch (state) {
        case model::PlayerState::Idle:
            state_text = tr("空闲");
            break;
        case model::PlayerState::Loading:
            state_text = tr("加载中");
            break;
        case model::PlayerState::Playing:
            state_text = tr("播放中");
            break;
        case model::PlayerState::Paused:
            state_text = tr("已暂停");
            break;
        case model::PlayerState::Stopped:
            state_text = tr("已停止");
            // 停止状态下清理UI，确保最后一帧被清除
            QMetaObject::invokeMethod(this, [this]() {
                ResetVideoUI();
            }, Qt::QueuedConnection);
            break;
        case model::PlayerState::Error:
            state_text = tr("错误");
            break;
    }
    
    statusBar()->showMessage(state_text);
    
    // 更新播放/暂停按钮的图标
    if (state == model::PlayerState::Playing) {
        play_pause_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    } else {
        play_pause_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    }

    // 更新视频区叠加层状态
    ui::VideoOverlayInfo overlay;
    overlay.is_playing = (state == model::PlayerState::Playing);
    overlay.has_video = !audio_only_mode_ && !showing_raw_image_;
    overlay.status = state_text;
    if (state == model::PlayerState::Playing) {
        overlay.fps = last_overlay_fps_;
    }
    video_widget_->SetOverlayInfo(overlay);
    video_widget_->SetCenterPlayButtonVisible(!overlay.is_playing && overlay.has_video);
}

void MainWindow::OnFrameReady(const QImage& frame) {
    // 如果播放器已停止，忽略帧更新
    if (player_ && player_->GetState() == model::PlayerState::Stopped) {
        return;
    }
    
    // 在纯音频模式下，将封面存储为专辑封面而不是直接显示
    if (audio_only_mode_) {
        album_cover_ = frame;
        return;
    }
    
    video_widget_->SetFallbackImage(frame);
}

void MainWindow::OnPositionChanged(int position_ms, int duration_ms) {
    // 如果播放器已停止，忽略位置更新
    if (player_ && player_->GetState() == model::PlayerState::Stopped) {
        return;
    }
    
    QSignalBlocker blocker(seek_slider_);
    seek_slider_->setRange(0, duration_ms);
    seek_slider_->setValue(position_ms);
    
    time_label_->setText(QString("%1 / %2")
        .arg(theme::font::formatTime(position_ms))
        .arg(theme::font::formatTime(duration_ms)));
}

void MainWindow::OnError(const QString& message) {
    QMessageBox::critical(this, tr("错误"), message);
    status_bar_->showMessage(tr("错误: %1").arg(message));
}

void MainWindow::OnPlaybackFinished() {
    OnStop();
    statusBar()->showMessage(tr("播放完成"));
}

void MainWindow::OnStreamStatsUpdate(const analyzer::StreamStats& stats) {
    // 更新状态栏显示关键信息
    QString status = QString("FPS: %1 | 码率: %2 Kbps | 关键帧: %3")
        .arg(stats.current_fps, 0, 'f', 1)
        .arg(stats.current_bitrate_bps / 1000)
        .arg(stats.key_frame_count);
    status_bar_->showMessage(status);

    // 更新叠加层 FPS 信息
    last_overlay_fps_ = QString("%1 fps").arg(stats.current_fps, 0, 'f', 1);
    ui::VideoOverlayInfo overlay;
    overlay.is_playing = (player_ && player_->GetState() == model::PlayerState::Playing);
    overlay.has_video = !audio_only_mode_ && !showing_raw_image_;
    overlay.fps = last_overlay_fps_;
    overlay.resolution = last_overlay_resolution_;
    overlay.codec = last_overlay_codec_;
    overlay.status = (player_ && player_->GetState() == model::PlayerState::Playing) 
        ? tr("播放中") : tr("已暂停");
    video_widget_->SetOverlayInfo(overlay);
}

void MainWindow::OnHistogramUpdate(const analyzer::HistogramData& hist) {
    // 直方图数据已由分析面板处理，这里可以做其他处理
}

void MainWindow::OnMediaModeChanged(bool has_video) {
    audio_only_mode_ = !has_video;
    audio_level_history_.clear();
    spectrum_history_.clear();
    audio_vis_last_render_ms_ = -1;
    audio_vis_smoothed_ = 0.0;
    audio_vis_target_ = 0.0;
    latest_spectrum_bins_.clear();
    latest_waveform_points_.clear();
    smoothed_spectrum_bins_.clear();
    if (audio_only_mode_) {
        // 保留已有的专辑封面，不清除 album_cover_
        video_widget_->Clear();
    } else {
        album_cover_ = QImage();
    }

    // 更新叠加层
    ui::VideoOverlayInfo overlay;
    overlay.is_playing = (player_ && player_->GetState() == model::PlayerState::Playing);
    overlay.has_video = has_video && !showing_raw_image_;
    overlay.fps = last_overlay_fps_;
    overlay.status = (player_ && player_->GetState() == model::PlayerState::Playing)
        ? tr("播放中") : tr("已暂停");
    video_widget_->SetOverlayInfo(overlay);
    video_widget_->SetCenterPlayButtonVisible(!overlay.is_playing && overlay.has_video);
}

void MainWindow::OnAudioLevelReady(double level, double timestamp_seconds) {
    if (!audio_only_mode_) {
        return;
    }

    audio_vis_target_ = std::clamp(level, 0.0, 1.0);
    if (!audio_vis_timer_.isValid()) {
        audio_vis_timer_.start();
        audio_vis_last_render_ms_ = -1;
    }

    static constexpr qint64 kFrameIntervalMs = 33;
    const qint64 now_ms = audio_vis_timer_.elapsed();
    if (audio_vis_last_render_ms_ >= 0 && (now_ms - audio_vis_last_render_ms_) < kFrameIntervalMs) {
        return;
    }

    RenderAudioVisualization(timestamp_seconds);
}

void MainWindow::OnAudioVisualizationForDisplay(const model::AudioVisualizationFrame& frame) {
    if (!audio_only_mode_) return;
    latest_spectrum_bins_ = frame.spectrum_bins;
    latest_waveform_points_ = frame.waveform_points;

    // Smooth spectrum with fast attack, slow decay for persistence
    const int n = static_cast<int>(latest_spectrum_bins_.size());
    if (smoothed_spectrum_bins_.size() != n) {
        smoothed_spectrum_bins_.resize(n);
        smoothed_spectrum_bins_.fill(0.0);
    }
    for (int i = 0; i < n; ++i) {
        const double target = latest_spectrum_bins_[i];
        const double alpha = (target > smoothed_spectrum_bins_[i]) ? 0.7 : 0.15;
        smoothed_spectrum_bins_[i] += (target - smoothed_spectrum_bins_[i]) * alpha;
    }
}

void MainWindow::RenderAudioVisualization(double timestamp_seconds) {
    const double dt = (audio_vis_last_render_ms_ >= 0)
                          ? (static_cast<double>(audio_vis_timer_.elapsed() - audio_vis_last_render_ms_) / 1000.0)
                          : (0.033);
    audio_vis_last_render_ms_ = audio_vis_timer_.elapsed();

    // Smoothed overall level
    const double tau_attack = 0.04;
    const double tau_release = 0.18;
    const double tau = (audio_vis_target_ > audio_vis_smoothed_) ? tau_attack : tau_release;
    const double alpha = 1.0 - std::exp(-dt / std::max(1e-6, tau));
    audio_vis_smoothed_ += (audio_vis_target_ - audio_vis_smoothed_) * std::clamp(alpha, 0.0, 1.0);

    const int w = std::max(1, video_widget_->width());
    const int h = std::max(1, video_widget_->height());

    QImage img(w, h, QImage::Format_ARGB32);

    // --- Background: radial gradient pulsing with audio ---
    {
        QPainter bg(&img);
        bg.setRenderHint(QPainter::Antialiasing, true);
        const int pulse = static_cast<int>(audio_vis_smoothed_ * 30);
        const double cx = w / 2.0;
        const double cy = h * 0.38;
        const double radius = std::max(w, h) * 0.8;
        QRadialGradient rg(cx, cy, radius);
        rg.setColorAt(0.0, QColor(20 + pulse, 15 + pulse / 2, 50 + pulse));
        rg.setColorAt(0.5, QColor(10, 8, 28));
        rg.setColorAt(1.0, QColor(4, 3, 12));
        bg.fillRect(0, 0, w, h, rg);
    }

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // --- Album cover ---
    const int cover_size = std::clamp(std::min(w, h) * 38 / 100, 80, 380);
    const int cover_cx = w / 2;
    const int cover_cy = h * 38 / 100;
    const int cover_x = cover_cx - cover_size / 2;
    const int cover_y = cover_cy - cover_size / 2;

    if (!album_cover_.isNull()) {
        QImage scaled = album_cover_.scaled(cover_size, cover_size,
                                            Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation);
        const int sx = cover_x + (cover_size - scaled.width()) / 2;
        const int sy = cover_y + (cover_size - scaled.height()) / 2;

        // Glow behind cover, pulsing with audio
        const int glow_r = 15 + static_cast<int>(audio_vis_smoothed_ * 25);
        const int glow_a = 50 + static_cast<int>(audio_vis_smoothed_ * 100);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(100, 160, 255, glow_a));
        painter.drawRoundedRect(sx - glow_r / 2, sy - glow_r / 2,
                                scaled.width() + glow_r, scaled.height() + glow_r, 18, 18);

        // Rounded cover
        QPainterPath clip;
        clip.addRoundedRect(QRect(sx, sy, scaled.width(), scaled.height()), 12, 12);
        painter.save();
        painter.setClipPath(clip);
        painter.drawImage(sx, sy, scaled);
        painter.restore();

        // Border
        painter.setPen(QPen(QColor(255, 255, 255, 35), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(sx, sy, scaled.width(), scaled.height(), 12, 12);

        // Reflection
        const int refl_h = std::min(scaled.height() / 4, 50);
        if (refl_h > 0) {
            QImage ref = scaled.mirrored(false, true).copy(0, 0, scaled.width(), refl_h);
            painter.setOpacity(0.12);
            painter.drawImage(sx, sy + scaled.height() + 3, ref);
            QLinearGradient fg(0, sy + scaled.height() + 3, 0, sy + scaled.height() + 3 + refl_h);
            fg.setColorAt(0.0, QColor(0, 0, 0, 0));
            fg.setColorAt(1.0, QColor(0, 0, 0, 255));
            painter.setOpacity(1.0);
            painter.fillRect(sx, sy + scaled.height() + 3, ref.width(), refl_h, fg);
        }
    } else {
        // Placeholder
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 40, 65, 200));
        painter.drawRoundedRect(cover_x, cover_y, cover_size, cover_size, 12, 12);
        painter.setPen(QColor(100, 120, 160, 140));
        QFont f; f.setPixelSize(cover_size / 3);
        painter.setFont(f);
        painter.drawText(QRect(cover_x, cover_y, cover_size, cover_size),
                         Qt::AlignCenter, QStringLiteral("\u266B"));
    }

    // --- Circular spectrum bars around cover ---
    const int n_bins = static_cast<int>(smoothed_spectrum_bins_.size());
    if (n_bins > 0) {
        const double cx = w / 2.0;
        const double cy = cover_cy;
        const double inner_r = cover_size / 2.0 + 18;
        const double max_bar_len = cover_size * 0.42;

        const int bar_count = std::min(n_bins, 64);
        for (int i = 0; i < bar_count; ++i) {
            double val = std::clamp(smoothed_spectrum_bins_[i] * 8.0, 0.0, 1.0);
            // Apply logarithmic perception: boost quiet frequencies
            val = std::pow(val, 0.6);
            const double bar_len = val * max_bar_len;
            if (bar_len < 2.0) continue;

            const double angle = -M_PI / 2.0 + 2.0 * M_PI * i / bar_count;
            const double cos_a = std::cos(angle);
            const double sin_a = std::sin(angle);

            const double x1 = cx + inner_r * cos_a;
            const double y1 = cy + inner_r * sin_a;
            const double x2 = cx + (inner_r + bar_len) * cos_a;
            const double y2 = cy + (inner_r + bar_len) * sin_a;

            // Gradient color along the bar: base color to tip color
            const double hue = 190.0 + (static_cast<double>(i) / bar_count) * 170.0;
            QColor c_base = QColor::fromHsvF(std::fmod(hue / 360.0, 1.0), 0.55, 0.85, 0.75);
            QColor c_tip  = QColor::fromHsvF(std::fmod(hue / 360.0, 1.0), 0.75, 1.0, 0.95);

            QLinearGradient bar_grad(x1, y1, x2, y2);
            bar_grad.setColorAt(0.0, c_base);
            bar_grad.setColorAt(1.0, c_tip);

            QPen pen(QBrush(bar_grad), 3.2, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(pen);
            painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
        }
    }

    // --- Real waveform at bottom area ---
    const int wf_n = static_cast<int>(latest_waveform_points_.size());
    if (wf_n > 2) {
        const int wf_top = cover_cy + cover_size / 2 + cover_size * 0.5;
        const int wf_bot = h - 10;
        const int wf_h = wf_bot - wf_top;
        if (wf_h > 20) {
            const double mid_y = wf_top + wf_h / 2.0;
            const double amp = wf_h * 0.42;

            // Draw filled waveform area
            QPainterPath wf_path;
            wf_path.moveTo(0.0, mid_y);
            for (int i = 0; i < wf_n; ++i) {
                const double x = static_cast<double>(i) / (wf_n - 1) * w;
                const double y = mid_y - latest_waveform_points_[i] * amp;
                wf_path.lineTo(x, y);
            }
            wf_path.lineTo(static_cast<double>(w), mid_y);
            wf_path.lineTo(0.0, mid_y);

            QLinearGradient wf_fill(0, wf_top, 0, wf_bot);
            const double hue_shift = audio_vis_smoothed_ * 30.0;
            wf_fill.setColorAt(0.0, QColor::fromHsvF((220.0 + hue_shift) / 360.0, 0.6, 0.95, 0.35));
            wf_fill.setColorAt(0.5, QColor::fromHsvF((260.0 + hue_shift) / 360.0, 0.5, 0.7, 0.2));
            wf_fill.setColorAt(1.0, QColor::fromHsvF((220.0 + hue_shift) / 360.0, 0.6, 0.95, 0.35));
            painter.setPen(Qt::NoPen);
            painter.setBrush(wf_fill);
            painter.drawPath(wf_path);

            // Draw waveform line on top
            QPainterPath wf_line;
            for (int i = 0; i < wf_n; ++i) {
                const double x = static_cast<double>(i) / (wf_n - 1) * w;
                const double y = mid_y - latest_waveform_points_[i] * amp;
                if (i == 0) wf_line.moveTo(x, y);
                else wf_line.lineTo(x, y);
            }
            QPen line_pen(QColor::fromHsvF((200.0 + hue_shift) / 360.0, 0.5, 1.0, 0.8));
            line_pen.setWidthF(1.8);
            painter.setPen(line_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(wf_line);

            // Mirror waveform (below center)
            QPainterPath wf_mirror;
            wf_mirror.moveTo(0.0, mid_y);
            for (int i = 0; i < wf_n; ++i) {
                const double x = static_cast<double>(i) / (wf_n - 1) * w;
                const double y = mid_y + latest_waveform_points_[i] * amp;
                wf_mirror.lineTo(x, y);
            }
            wf_mirror.lineTo(static_cast<double>(w), mid_y);
            wf_mirror.lineTo(0.0, mid_y);

            QLinearGradient mirror_fill(0, wf_top, 0, wf_bot);
            mirror_fill.setColorAt(0.0, QColor::fromHsvF((280.0 + hue_shift) / 360.0, 0.5, 0.85, 0.18));
            mirror_fill.setColorAt(1.0, QColor::fromHsvF((300.0 + hue_shift) / 360.0, 0.4, 0.6, 0.08));
            painter.setPen(Qt::NoPen);
            painter.setBrush(mirror_fill);
            painter.drawPath(wf_mirror);

            // Center line
            painter.setPen(QPen(QColor(255, 255, 255, 25), 1.0));
            painter.drawLine(QPointF(0, mid_y), QPointF(w, mid_y));
        }
    } else {
        // Fallback: simple level bars if no waveform data yet
        static constexpr size_t kMaxHistory = 120;
        if (audio_level_history_.size() >= kMaxHistory) audio_level_history_.pop_front();
        audio_level_history_.push_back(audio_vis_smoothed_);

        const int n = static_cast<int>(audio_level_history_.size());
        const int bar_area_y = cover_cy + cover_size / 2 + cover_size * 0.5;
        const int bar_area_h = h - bar_area_y - 10;
        if (bar_area_h > 10 && n > 1) {
            const double bar_w = static_cast<double>(w) / n;
            for (int i = 0; i < n; ++i) {
                const double v = std::clamp(audio_level_history_[static_cast<size_t>(i)], 0.0, 1.0);
                const int amp = static_cast<int>(v * (bar_area_h * 0.8));
                if (amp < 1) continue;
                const int x0 = static_cast<int>(i * bar_w);
                const int bw = std::max(1, static_cast<int>(bar_w) - 1);
                const int my = bar_area_y + bar_area_h / 2;
                const double hue = 180.0 + (static_cast<double>(i) / n) * 120.0;
                QLinearGradient g(x0, my - amp, x0, my + amp);
                g.setColorAt(0.0, QColor::fromHsvF(hue / 360.0, 0.6, 0.95, 0.9));
                g.setColorAt(1.0, QColor::fromHsvF(hue / 360.0, 0.4, 0.7, 0.4));
                painter.setPen(Qt::NoPen);
                painter.setBrush(g);
                painter.drawRoundedRect(x0, my - amp, bw, amp * 2, 1.5, 1.5);
            }
        }
    }

    // --- Info text ---
    painter.setPen(QColor(200, 200, 220, 140));
    QFont info_font;
    info_font.setPixelSize(12);
    painter.setFont(info_font);
    painter.drawText(QRect(10, 6, w - 20, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QString("t=%1s  level=%2")
                         .arg(timestamp_seconds, 0, 'f', 3)
                         .arg(audio_vis_smoothed_, 0, 'f', 3));

    video_widget_->SetFallbackImage(img);
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
