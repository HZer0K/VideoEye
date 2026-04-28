#include "MainWindow.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSplitter>
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
#include <algorithm>
#include <QProgressDialog>
#include <QFileInfo>
#include <QFile>
#include <QByteArray>
#include <QRegularExpression>
#include <QtGlobal>
#include <cmath>

namespace videoeye {
namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , player_(nullptr)
    , splitter_(nullptr)
    , bottom_widget_(nullptr)
    , control_group_(nullptr) {
    
    // 创建播放器实例
    player_ = new player::MediaPlayer(this);
    
    SetupUI();
    SetupMenuBar();
    SetupToolBar();
    SetupStatusBar();
    SetupConnections();
    UpdateMinimumWindowSize();
    audio_vis_timer_.start();
    QTimer::singleShot(0, this, [this]() {
        UpdateMinimumWindowSize();
        EnforceSplitterSizes();
    });

    setWindowTitle(tr("VideoEye 2.0 - 视频流分析软件"));
    resize(1200, 800);
    last_geometry_ = geometry();
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
    
    QVBoxLayout* main_layout = new QVBoxLayout(central_widget);
    main_layout->setContentsMargins(0, 0, 0, 0);
    
    splitter_ = new QSplitter(Qt::Vertical, central_widget);
    splitter_->setChildrenCollapsible(false);
    main_layout->addWidget(splitter_);
    
    // 视频显示区域
    video_label_ = new QLabel(splitter_);
    video_label_->setMinimumSize(1, 1);
    video_label_->setMinimumHeight(160);
    video_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    video_label_->setStyleSheet("background-color: black;");
    video_label_->setAlignment(Qt::AlignCenter);
    video_label_->setText(tr("视频显示区域"));
    video_label_->setStyleSheet("background-color: black; color: white; font-size: 20px;");
    
    bottom_widget_ = new QWidget(splitter_);
    bottom_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    QVBoxLayout* bottom_layout = new QVBoxLayout(bottom_widget_);
    bottom_layout->setContentsMargins(6, 6, 6, 6);

    // 控制面板
    control_group_ = new QGroupBox(bottom_widget_);
    control_group_->setTitle(QString());
    control_group_->setFlat(true);
    QHBoxLayout* control_layout = new QHBoxLayout(control_group_);
    
    // 播放控制按钮
    play_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), tr("播放"), control_group_);
    pause_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPause), tr("暂停"), control_group_);
    stop_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), tr("停止"), control_group_);
    prev_frame_button_ = new QPushButton(tr("上一帧"), control_group_);
    next_frame_button_ = new QPushButton(tr("下一帧"), control_group_);
    
    control_layout->addWidget(play_button_);
    control_layout->addWidget(pause_button_);
    control_layout->addWidget(stop_button_);
    control_layout->addWidget(prev_frame_button_);
    control_layout->addWidget(next_frame_button_);
    
    // 进度条
    seek_slider_ = new QSlider(Qt::Horizontal, control_group_);
    seek_slider_->setRange(0, 0);
    control_layout->addWidget(seek_slider_, 1);
    
    // 时间显示
    time_label_ = new QLabel(tr("00:00 / 00:00"), control_group_);
    control_layout->addWidget(time_label_);

    prev_frame_button_->setVisible(false);
    next_frame_button_->setVisible(false);

    control_group_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const int control_h = control_group_->sizeHint().height();
    control_group_->setFixedHeight(control_h);
        
    // 信息面板
    tab_widget_ = new QTabWidget(bottom_widget_);
    tab_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    const int tabbar_h = tab_widget_->tabBar()->sizeHint().height();
    tab_widget_->setMinimumHeight(tabbar_h);
    
    // 流信息标签页
    QWidget* info_tab = new QWidget(tab_widget_);
    QVBoxLayout* info_layout = new QVBoxLayout(info_tab);
    info_text_ = new QTextEdit(info_tab);
    info_text_->setReadOnly(true);
    info_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    info_text_->setLineWrapMode(QTextEdit::NoWrap);
    info_text_->setMinimumHeight(0);
    info_text_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    info_layout->addWidget(info_text_);
    tab_widget_->addTab(info_tab, tr("流信息"));
    
    // 分析面板标签页 (集成分析功能)
    analysis_panel_ = new ui::AnalysisPanel(tab_widget_);
    analysis_panel_->setMinimumHeight(0);
    analysis_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    tab_widget_->addTab(analysis_panel_, tr("分析面板"));
    
    bottom_layout->addWidget(control_group_, 0);
    bottom_layout->addWidget(tab_widget_, 1);

    bottom_widget_->setMinimumHeight(control_h
        + tabbar_h
        + bottom_layout->contentsMargins().top()
        + bottom_layout->contentsMargins().bottom()
        + bottom_layout->spacing());

    splitter_->addWidget(video_label_);
    splitter_->addWidget(bottom_widget_);

    splitter_->setCollapsible(0, false);
    splitter_->setCollapsible(1, false);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({600, 400});

    connect(splitter_, &QSplitter::splitterMoved, this, [this](int, int) {
        if (!splitter_ || !bottom_widget_) {
            return;
        }
        const int bottom_min = bottom_widget_->minimumHeight();
        QList<int> sizes = splitter_->sizes();
        if (sizes.size() != 2) {
            return;
        }
        if (sizes[1] < bottom_min) {
            sizes[0] = std::max(0, sizes[0] - (bottom_min - sizes[1]));
            sizes[1] = bottom_min;
            splitter_->setSizes(sizes);
        }
    });
}

void MainWindow::UpdateMinimumWindowSize() {
    const int video_min = video_label_ ? video_label_->minimumHeight() : 160;
    int bottom_min = 0;
    if (bottom_widget_) {
        bottom_min = bottom_widget_->minimumHeight();
        if (bottom_min <= 0) {
            bottom_min = bottom_widget_->minimumSizeHint().height();
        }
    }

    int bars = 0;
    if (menuBar()) {
        bars += menuBar()->sizeHint().height();
    }
    if (tool_bar_) {
        bars += tool_bar_->sizeHint().height();
    }
    if (statusBar()) {
        bars += statusBar()->sizeHint().height();
    }

    int min_height = bars + bottom_min + video_min + 20;

    int min_width = 0;
    if (control_group_) {
        min_width = control_group_->minimumSizeHint().width();
        if (min_width < control_group_->sizeHint().width()) {
            min_width = control_group_->sizeHint().width();
        }
    }
    if (min_width < 900) {
        min_width = 900;
    }

    setMinimumSize(min_width, min_height);
    if (auto* cw = centralWidget()) {
        cw->setMinimumWidth(min_width);
        cw->setMinimumHeight(min_height);
    }
    if (splitter_) {
        splitter_->setMinimumWidth(min_width);
        splitter_->setMinimumHeight(min_height);
    }
}

void MainWindow::EnforceSplitterSizes() {
    if (!splitter_ || !bottom_widget_) {
        return;
    }
    const int bottom_min = bottom_widget_->minimumHeight();
    const int video_min = video_label_ ? video_label_->minimumHeight() : 160;
    QList<int> sizes = splitter_->sizes();
    if (sizes.size() != 2) {
        return;
    }
    bool changed = false;
    if (sizes[1] < bottom_min) {
        sizes[1] = bottom_min;
        changed = true;
    }
    if (sizes[0] < video_min) {
        sizes[0] = video_min;
        changed = true;
    }
    if (changed) {
        splitter_->setSizes(sizes);
    }
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    UpdateMinimumWindowSize();
    EnforceSplitterSizes();
    last_geometry_ = geometry();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    EnforceSplitterSizes();

    if (enforcing_geometry_) {
        last_geometry_ = geometry();
        return;
    }

    const QSize min_sz = minimumSize();
    QRect g = geometry();

    int target_w = g.width();
    int target_h = g.height();
    int target_x = g.x();
    int target_y = g.y();

    const QRect old_g = last_geometry_.isValid() ? last_geometry_ : g;

    const bool left_moved = g.left() != old_g.left();
    const bool right_moved = g.right() != old_g.right();
    const bool top_moved = g.top() != old_g.top();
    const bool bottom_moved = g.bottom() != old_g.bottom();

    const bool anchor_right = left_moved && !right_moved;
    const bool anchor_bottom = top_moved && !bottom_moved;

    if (target_w < min_sz.width()) {
        target_w = min_sz.width();
        if (anchor_right) {
            target_x = g.right() - target_w + 1;
        }
    }

    if (target_h < min_sz.height()) {
        target_h = min_sz.height();
        if (anchor_bottom) {
            target_y = g.bottom() - target_h + 1;
        }
    }

    if (target_w != g.width() || target_h != g.height() || target_x != g.x() || target_y != g.y()) {
        enforcing_geometry_ = true;
        setGeometry(target_x, target_y, target_w, target_h);
        enforcing_geometry_ = false;
        g = geometry();
    }

    last_geometry_ = g;
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    if (!enforcing_geometry_) {
        last_geometry_ = geometry();
    }
}

void MainWindow::SetupMenuBar() {
    menu_bar_ = menuBar();
    
    // 文件菜单
    QMenu* file_menu = menu_bar_->addMenu(tr("文件"));
    file_menu->addAction(tr("打开文件"), QKeySequence::Open, this, &MainWindow::OnOpenFile);
    file_menu->addAction(tr("打开URL"), QKeySequence("Ctrl+U"), this, &MainWindow::OnOpenURL);
    export_frames_action_ = file_menu->addAction(tr("导出视频帧..."), this, &MainWindow::OnExportVideoFrames);
    file_menu->addSeparator();
    file_menu->addAction(tr("退出"), QKeySequence::Quit, this, &MainWindow::OnExit);
    
    // 分析菜单
    QMenu* analysis_menu = menu_bar_->addMenu(tr("分析"));
    stream_analysis_action_ = analysis_menu->addAction(tr("流分析"));
    stream_analysis_action_->setCheckable(true);
    stream_analysis_action_->setChecked(false);
    
    frame_analysis_action_ = analysis_menu->addAction(tr("视频帧分析"));
    frame_analysis_action_->setCheckable(true);
    frame_analysis_action_->setChecked(false);
    
    histogram_action_ = analysis_menu->addAction(tr("直方图"));
    histogram_action_->setCheckable(true);
    histogram_action_->setChecked(false);
    histogram_action_->setEnabled(false);
    
    face_detection_action_ = analysis_menu->addAction(tr("人脸检测"));
    face_detection_action_->setCheckable(true);
    face_detection_action_->setChecked(false);
    face_detection_action_->setEnabled(false);

    connect(stream_analysis_action_, &QAction::toggled, this, [this](bool enabled) {
        if (!player_) {
            return;
        }
        player_->EnableAnalysis(enabled);
        histogram_action_->setEnabled(enabled);
        face_detection_action_->setEnabled(enabled);

        if (!enabled) {
            player_->SetHistogramEnabled(false);
            player_->SetFaceDetectionEnabled(false);
            statusBar()->showMessage(tr("流分析已禁用"));
            return;
        }

        player_->SetHistogramEnabled(histogram_action_->isChecked());
        player_->SetFaceDetectionEnabled(face_detection_action_->isChecked());
        statusBar()->showMessage(tr("流分析已启用"));
    });

    connect(frame_analysis_action_, &QAction::toggled, this, [this](bool enabled) {
        if (!player_) {
            return;
        }
        player_->SetFrameTypeAnalysisEnabled(enabled);
        statusBar()->showMessage(enabled ? tr("视频帧分析已启用") : tr("视频帧分析已禁用"));
    });

    connect(histogram_action_, &QAction::toggled, this, [this](bool enabled) {
        if (!player_) {
            return;
        }
        if (enabled && !stream_analysis_action_->isChecked()) {
            stream_analysis_action_->setChecked(true);
        }
        player_->SetHistogramEnabled(enabled && stream_analysis_action_->isChecked());
        statusBar()->showMessage(enabled ? tr("直方图分析已启用") : tr("直方图分析已禁用"));
    });

    connect(face_detection_action_, &QAction::toggled, this, [this](bool enabled) {
        if (!player_) {
            return;
        }
        if (enabled && !stream_analysis_action_->isChecked()) {
            stream_analysis_action_->setChecked(true);
        }
        player_->SetFaceDetectionEnabled(enabled && stream_analysis_action_->isChecked());
        statusBar()->showMessage(enabled ? tr("人脸检测已启用") : tr("人脸检测已禁用"));
    });
    
    // 帮助菜单
    QMenu* help_menu = menu_bar_->addMenu(tr("帮助"));
    help_menu->addAction(tr("关于"), this, []() {
        QMessageBox::about(nullptr, QObject::tr("关于"),
                          QObject::tr("VideoEye 2.0\n现代化的视频流分析软件"));
    });
}

void MainWindow::SetupToolBar() {
    tool_bar_ = addToolBar(tr("工具栏"));
    current_media_label_ = new QLabel(tr("未选择媒体"), this);
    current_media_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    current_media_label_->setMinimumWidth(600);
    tool_bar_->addWidget(current_media_label_);
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
                        if (player_) {
                            statusBar()->showMessage(tr("正在终止导出..."));
                            player_->CancelVideoFrameExport();
                        }
                    });
                } else {
                    export_progress_dialog_->setMaximum(total_frames > 0 ? total_frames : 0);
                }
                export_progress_dialog_->setValue(0);
                export_progress_dialog_->setLabelText(tr("正在导出视频帧..."));
                export_progress_dialog_->show();
            });
    
    // 播放器信号连接 - 分析功能 (实时分析)
    connect(player_, &player::MediaPlayer::StreamStatsReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateStreamStats);
    connect(player_, &player::MediaPlayer::HistogramReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateHistogram);
    connect(player_, &player::MediaPlayer::FaceDetectionReady,
            analysis_panel_, &ui::AnalysisPanel::UpdateFaceDetection);
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
    
    // 控件信号连接
    connect(play_button_, &QPushButton::clicked, this, &MainWindow::OnPlay);
    connect(pause_button_, &QPushButton::clicked, this, &MainWindow::OnPause);
    connect(stop_button_, &QPushButton::clicked, this, &MainWindow::OnStop);
    connect(prev_frame_button_, &QPushButton::clicked, this, &MainWindow::OnPrevRawFrame);
    connect(next_frame_button_, &QPushButton::clicked, this, &MainWindow::OnNextRawFrame);
    connect(seek_slider_, &QSlider::valueChanged, this, &MainWindow::OnSeek);
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

        auto info = player_->GetStreamInfo();
        info_text_->setPlainText(QString::fromStdString(info.ToString()));

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

    auto info = player_->GetStreamInfo();
    info_text_->setPlainText(QString::fromStdString(info.ToString()));

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
    info_text_->setPlainText(
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
    video_label_->setPixmap(QPixmap::fromImage(img).scaled(video_label_->size(),
                                                           Qt::KeepAspectRatio,
                                                           Qt::SmoothTransformation));
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
    if (play_button_) {
        play_button_->setEnabled(!raw_mode);
    }
    if (pause_button_) {
        pause_button_->setEnabled(!raw_mode);
    }

    if (raw_mode) {
        QSignalBlocker blocker(seek_slider_);
        seek_slider_->setRange(0, std::max(0, raw_total_frames_ - 1));
        seek_slider_->setValue(raw_current_frame_);
        seek_slider_->setEnabled(raw_total_frames_ > 1);
        time_label_->setText(tr("帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_));
    } else {
        seek_slider_->setEnabled(true);
        time_label_->setText(tr("00:00 / 00:00"));
    }
}

void MainWindow::OnPlay() {
    if (player_) {
        if (showing_raw_image_) {
            statusBar()->showMessage(tr("当前为图像文件，无法播放"));
            return;
        }
        const auto state = player_->GetState();
        if ((state == model::PlayerState::Idle ||
             state == model::PlayerState::Stopped ||
             state == model::PlayerState::Error) &&
            !current_media_url_.isEmpty()) {
            if (!player_->Open(current_media_url_)) {
                statusBar()->showMessage(tr("打开失败: %1").arg(current_media_url_));
                return;
            }
        }
        player_->Play();
    }
}

void MainWindow::OnPause() {
    player_->Pause();
}

void MainWindow::OnStop() {
    player_->Stop();
    video_label_->clear();
    video_label_->setText(tr("视频显示区域"));
    video_label_->setStyleSheet("background-color: black; color: white; font-size: 20px;");
    audio_level_history_.clear();
    audio_only_mode_ = false;
    audio_vis_last_render_ms_ = -1;
    audio_vis_smoothed_ = 0.0;
    audio_vis_target_ = 0.0;
    showing_raw_image_ = false;
    raw_image_path_.clear();
    raw_pixel_format_.clear();
    raw_width_ = 0;
    raw_height_ = 0;
    raw_frame_size_ = 0;
    raw_total_frames_ = 0;
    raw_current_frame_ = 0;
    UpdateRawNavigationState();
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
    player_->Seek(value);
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
            break;
        case model::PlayerState::Error:
            state_text = tr("错误");
            break;
    }
    
    statusBar()->showMessage(state_text);
}

void MainWindow::OnFrameReady(const QImage& frame) {
    QPixmap pixmap = QPixmap::fromImage(frame);
    video_label_->setPixmap(pixmap.scaled(video_label_->size(), 
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
}

void MainWindow::OnPositionChanged(int position_ms, int duration_ms) {
    QSignalBlocker blocker(seek_slider_);
    seek_slider_->setRange(0, duration_ms);
    seek_slider_->setValue(position_ms);
    
    auto format_time = [](int ms) {
        int seconds = ms / 1000;
        int minutes = seconds / 60;
        int hours = minutes / 60;
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes % 60, 2, 10, QChar('0'))
            .arg(seconds % 60, 2, 10, QChar('0'));
    };
    
    time_label_->setText(QString("%1 / %2")
        .arg(format_time(position_ms))
        .arg(format_time(duration_ms)));
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
}

void MainWindow::OnHistogramUpdate(const analyzer::HistogramData& hist) {
    // 直方图数据已由分析面板处理，这里可以做其他处理
}

void MainWindow::OnFaceDetectionUpdate(const std::vector<analyzer::FaceInfo>& faces) {
    // 人脸检测结果已由分析面板处理，这里可以做其他处理
    if (!faces.empty()) {
        statusBar()->showMessage(tr("检测到 %1 张人脸").arg(faces.size()));
    }
}

void MainWindow::OnMediaModeChanged(bool has_video) {
    audio_only_mode_ = !has_video;
    audio_level_history_.clear();
    audio_vis_last_render_ms_ = -1;
    audio_vis_smoothed_ = 0.0;
    audio_vis_target_ = 0.0;
    if (audio_only_mode_) {
        video_label_->clear();
        video_label_->setStyleSheet("background-color: black;");
    }
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

    const double dt = (audio_vis_last_render_ms_ >= 0)
                          ? (static_cast<double>(now_ms - audio_vis_last_render_ms_) / 1000.0)
                          : (static_cast<double>(kFrameIntervalMs) / 1000.0);
    audio_vis_last_render_ms_ = now_ms;

    const double tau_attack = 0.05;
    const double tau_release = 0.20;
    const double tau = (audio_vis_target_ > audio_vis_smoothed_) ? tau_attack : tau_release;
    const double alpha = 1.0 - std::exp(-dt / std::max(1e-6, tau));
    audio_vis_smoothed_ = audio_vis_smoothed_ + (audio_vis_target_ - audio_vis_smoothed_) * std::clamp(alpha, 0.0, 1.0);

    const int w = std::max(1, video_label_->width());
    const int h = std::max(1, video_label_->height());

    static constexpr size_t kMaxHistory = 90;
    if (audio_level_history_.size() >= kMaxHistory) {
        audio_level_history_.pop_front();
    }
    audio_level_history_.push_back(audio_vis_smoothed_);

    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::black);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int mid_y = h / 2;
    painter.setPen(QColor(60, 60, 60));
    painter.drawLine(0, mid_y, w, mid_y);

    const int n = static_cast<int>(audio_level_history_.size());
    if (n > 0) {
        const double bar_w = static_cast<double>(w) / n;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 220, 120));

        for (int i = 0; i < n; ++i) {
            const double v = std::clamp(audio_level_history_[static_cast<size_t>(i)], 0.0, 1.0);
            const int amp = static_cast<int>(v * (h * 0.45));
            const int x0 = static_cast<int>(i * bar_w);
            const int bw = std::max(1, static_cast<int>(bar_w) - 1);
            painter.drawRect(x0, mid_y - amp, bw, amp * 2);
        }
    }

    painter.setPen(QColor(220, 220, 220));
    painter.drawText(QRect(8, 8, w - 16, 24),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QString("t=%1s  level=%2")
                         .arg(timestamp_seconds, 0, 'f', 3)
                         .arg(audio_vis_smoothed_, 0, 'f', 3));

    video_label_->setPixmap(QPixmap::fromImage(img));
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

} // namespace ui
} // namespace videoeye
