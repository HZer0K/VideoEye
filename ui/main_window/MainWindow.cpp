#include "MainWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QApplication>
#include <QStyle>

namespace videoeye {
namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , player_(nullptr) {
    
    // 创建播放器实例
    player_ = new player::MediaPlayer(this);
    
    SetupUI();
    SetupMenuBar();
    SetupToolBar();
    SetupStatusBar();
    SetupConnections();
    
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
    
    QVBoxLayout* main_layout = new QVBoxLayout(central_widget);
    
    // 视频显示区域
    video_label_ = new QLabel(this);
    video_label_->setMinimumSize(800, 450);
    video_label_->setStyleSheet("background-color: black;");
    video_label_->setAlignment(Qt::AlignCenter);
    video_label_->setText(tr("视频显示区域"));
    video_label_->setStyleSheet("background-color: black; color: white; font-size: 20px;");
    main_layout->addWidget(video_label_);
    
    // 控制面板
    QGroupBox* control_group = new QGroupBox(tr("播放控制"), this);
    QHBoxLayout* control_layout = new QHBoxLayout(control_group);
    
    // 播放控制按钮
    play_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), tr("播放"), this);
    pause_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPause), tr("暂停"), this);
    stop_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), tr("停止"), this);
    
    control_layout->addWidget(play_button_);
    control_layout->addWidget(pause_button_);
    control_layout->addWidget(stop_button_);
    
    // 进度条
    seek_slider_ = new QSlider(Qt::Horizontal, this);
    seek_slider_->setRange(0, 0);
    control_layout->addWidget(seek_slider_, 1);
    
    // 时间显示
    time_label_ = new QLabel(tr("00:00 / 00:00"), this);
    control_layout->addWidget(time_label_);
    
    main_layout->addWidget(control_group);
    
    // 信息面板
    tab_widget_ = new QTabWidget(this);
    
    // 流信息标签页
    QWidget* info_tab = new QWidget();
    QVBoxLayout* info_layout = new QVBoxLayout(info_tab);
    info_text_ = new QTextEdit(info_tab);
    info_text_->setReadOnly(true);
    info_layout->addWidget(info_text_);
    tab_widget_->addTab(info_tab, tr("流信息"));
    
    // 分析结果标签页
    QWidget* analysis_tab = new QWidget();
    QVBoxLayout* analysis_layout = new QVBoxLayout(analysis_tab);
    QLabel* analysis_label = new QLabel(tr("分析结果将在此显示"), analysis_tab);
    analysis_label->setAlignment(Qt::AlignCenter);
    analysis_layout->addWidget(analysis_label);
    tab_widget_->addTab(analysis_tab, tr("分析结果"));
    
    main_layout->addWidget(tab_widget_);
    
    // 底部信息栏
    info_label_ = new QLabel(tr("就绪"), this);
    main_layout->addWidget(info_label_);
}

void MainWindow::SetupMenuBar() {
    menu_bar_ = menuBar();
    
    // 文件菜单
    QMenu* file_menu = menu_bar_->addMenu(tr("文件"));
    file_menu->addAction(tr("打开文件"), this, &MainWindow::OnOpenFile, QKeySequence::Open);
    file_menu->addAction(tr("打开URL"), this, &MainWindow::OnOpenURL, QKeySequence("Ctrl+U"));
    file_menu->addSeparator();
    file_menu->addAction(tr("退出"), this, &MainWindow::OnExit, QKeySequence::Quit);
    
    // 播放菜单
    QMenu* play_menu = menu_bar_->addMenu(tr("播放"));
    play_menu->addAction(tr("播放"), this, &MainWindow::OnPlay, QKeySequence::MediaPlay);
    play_menu->addAction(tr("暂停"), this, &MainWindow::OnPause, QKeySequence::MediaPause);
    play_menu->addAction(tr("停止"), this, &MainWindow::OnStop, QKeySequence::MediaStop);
    
    // 分析菜单
    QMenu* analysis_menu = menu_bar_->addMenu(tr("分析"));
    analysis_menu->addAction(tr("流分析"), this, &MainWindow::OnStreamAnalysis);
    analysis_menu->addAction(tr("人脸检测"), this, &MainWindow::OnFaceDetection);
    analysis_menu->addAction(tr("直方图分析"), this, &MainWindow::OnHistogramAnalysis);
    
    // 帮助菜单
    QMenu* help_menu = menu_bar_->addMenu(tr("帮助"));
    help_menu->addAction(tr("关于"), this, []() {
        QMessageBox::about(nullptr, QObject::tr("关于"),
                          QObject::tr("VideoEye 2.0\n现代化的视频流分析软件"));
    });
}

void MainWindow::SetupToolBar() {
    tool_bar_ = addToolBar(tr("工具栏"));
    tool_bar_->addAction(play_button_->text(), this, &MainWindow::OnPlay);
    tool_bar_->addAction(pause_button_->text(), this, &MainWindow::OnPause);
    tool_bar_->addAction(stop_button_->text(), this, &MainWindow::OnStop);
}

void MainWindow::SetupStatusBar() {
    status_bar_ = statusBar();
    status_bar_->showMessage(tr("就绪"));
}

void MainWindow::SetupConnections() {
    // 播放器信号连接
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
    
    // 控件信号连接
    connect(play_button_, &QPushButton::clicked, this, &MainWindow::OnPlay);
    connect(pause_button_, &QPushButton::clicked, this, &MainWindow::OnPause);
    connect(stop_button_, &QPushButton::clicked, this, &MainWindow::OnStop);
    connect(seek_slider_, &QSlider::valueChanged, this, &MainWindow::OnSeek);
}

void MainWindow::OnOpenFile() {
    QString filename = QFileDialog::getOpenFileName(this,
        tr("打开媒体文件"), "",
        tr("媒体文件 (*.mp4 *.avi *.mkv *.flv *.ts *.mp3 *.aac *.wav);;所有文件 (*)"));
    
    if (filename.isEmpty()) {
        return;
    }
    
    if (player_->Open(filename)) {
        info_label_->setText(tr("已打开: %1").arg(filename));
        status_bar_->showMessage(tr("已打开: %1").arg(filename));
        
        // 显示流信息
        auto info = player_->GetStreamInfo();
        info_text_->setText(QString::fromStdString(info.ToString()));
    }
}

void MainWindow::OnOpenURL() {
    bool ok;
    QString url = QInputDialog::getText(this, tr("打开URL"),
                                        tr("输入流媒体URL:"),
                                        QLineEdit::Normal,
                                        "rtmp://", &ok);
    
    if (ok && !url.isEmpty()) {
        if (player_->Open(url)) {
            info_label_->setText(tr("已打开: %1").arg(url));
        }
    }
}

void MainWindow::OnExit() {
    close();
}

void MainWindow::OnPlay() {
    player_->Play();
}

void MainWindow::OnPause() {
    player_->Pause();
}

void MainWindow::OnStop() {
    player_->Stop();
    video_label_->clear();
    video_label_->setText(tr("视频显示区域"));
    video_label_->setStyleSheet("background-color: black; color: white; font-size: 20px;");
}

void MainWindow::OnSeek(int value) {
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
    
    info_label_->setText(state_text);
    status_bar_->showMessage(state_text);
}

void MainWindow::OnFrameReady(const QImage& frame) {
    QPixmap pixmap = QPixmap::fromImage(frame);
    video_label_->setPixmap(pixmap.scaled(video_label_->size(), 
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
}

void MainWindow::OnPositionChanged(int position_ms, int duration_ms) {
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
    info_label_->setText(tr("播放完成"));
}

void MainWindow::OnStreamAnalysis() {
    info_label_->setText(tr("流分析功能开发中..."));
}

void MainWindow::OnFaceDetection() {
    info_label_->setText(tr("人脸检测功能开发中..."));
}

void MainWindow::OnHistogramAnalysis() {
    info_label_->setText(tr("直方图分析功能开发中..."));
}

} // namespace ui
} // namespace videoeye

// main函数
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    // 设置应用程序信息
    app.setApplicationName("VideoEye");
    app.setApplicationVersion("2.0.0");
    app.setOrganizationName("VideoEye Team");
    
    videoeye::ui::MainWindow window;
    window.show();
    
    return app.exec();
}
