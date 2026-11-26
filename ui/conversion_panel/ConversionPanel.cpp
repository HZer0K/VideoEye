#include "ui/conversion_panel/ConversionPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandPreflight.h"
#include "core/ffmpeg/FfmpegFailureDiagnostics.h"
#include "core/ffmpeg/FfmpegToolLocator.h"
#include "ui/main_window/media_file_filter.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {
namespace {

QString ColorStyle(const char* color) {
    return QStringLiteral("color:%1").arg(QString::fromLatin1(color));
}

QString FormatEta(qint64 eta_ms) {
    const qint64 seconds = eta_ms / 1000;
    if (seconds < 60) {
        return QCoreApplication::translate("ConversionPanel", "%1 秒").arg(seconds);
    }
    return QCoreApplication::translate("ConversionPanel", "%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
}

/// 设置页的来源下拉：值就是枚举本身（存 int，读回时 static_cast），
/// 不用另立一套字符串标识 —— 标识与枚举对不上时最难查。
template <typename Enum>
void AddEnumItem(QComboBox* combo, const QString& text, Enum value) {
    combo->addItem(text, static_cast<int>(value));
}

template <typename Enum>
Enum CurrentEnum(const QComboBox* combo) {
    return static_cast<Enum>(combo->currentData().toInt());
}

/// 这个列表里有没有"真能选"的项。只看条数会误判：纯音频容器的视频列表是
/// {自动, 不要视频} 两项，但它们都不是可选的视频编码，下拉该锁住。
bool HasRealVideoChoice(const std::vector<ffmpeg::ConversionVideoCodec>& values) {
    for (const auto value : values) {
        if (value != ffmpeg::ConversionVideoCodec::Auto && value != ffmpeg::ConversionVideoCodec::None) {
            return true;
        }
    }
    return false;
}

bool HasRealAudioChoice(const std::vector<ffmpeg::ConversionAudioCodec>& values) {
    for (const auto value : values) {
        if (value != ffmpeg::ConversionAudioCodec::Auto && value != ffmpeg::ConversionAudioCodec::None) {
            return true;
        }
    }
    return false;
}

} // namespace

ConversionPanel::ConversionPanel(QWidget* parent) : QWidget(parent) {
    settings_ = new QSettings(this);
    BuildUi();

    runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(runner_, &ffmpeg::FfmpegProcessRunner::OutputLine, this, &ConversionPanel::OnRunOutput);
    connect(runner_, &ffmpeg::FfmpegProcessRunner::Finished, this, &ConversionPanel::OnRunFinished);

    // 源探测用**独立的 runner**：与转换进程共用一套会在"转换还没跑完"时被探测的
    // 输出行污染（两边的 stderr 混在一起，进度条会乱跳）。
    probe_runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(probe_runner_, &ffmpeg::FfmpegProcessRunner::OutputLine, this, &ConversionPanel::OnProbeOutput);
    connect(probe_runner_, &ffmpeg::FfmpegProcessRunner::Finished, this, &ConversionPanel::OnProbeFinished);

    // 探测只是读文件头：网络流 / 坏文件可能卡很久，10 秒没结果就当没探到。
    probe_timer_ = new QTimer(this);
    probe_timer_->setSingleShot(true);
    connect(probe_timer_, &QTimer::timeout, this, &ConversionPanel::OnProbeTimeout);

    // 编码器清单也用独立 runner：它只打印 `-encoders`，和源探测 / 转换互不干扰。
    encoder_runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(encoder_runner_, &ffmpeg::FfmpegProcessRunner::OutputLine, this, &ConversionPanel::OnEncoderListOutput);
    connect(encoder_runner_, &ffmpeg::FfmpegProcessRunner::Finished, this, &ConversionPanel::OnEncoderListFinished);
    encoder_timer_ = new QTimer(this);
    encoder_timer_->setSingleShot(true);
    connect(encoder_timer_, &QTimer::timeout, this, &ConversionPanel::OnEncoderListTimeout);

    // 产物核对：转换成功后再跑一次 `ffmpeg -i <输出>`，读出来的才是真结果。
    verify_runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(verify_runner_, &ffmpeg::FfmpegProcessRunner::OutputLine, this, &ConversionPanel::OnVerifyOutput);
    connect(verify_runner_, &ffmpeg::FfmpegProcessRunner::Finished, this, &ConversionPanel::OnVerifyFinished);
    verify_timer_ = new QTimer(this);
    verify_timer_->setSingleShot(true);
    connect(verify_timer_, &QTimer::timeout, this, &ConversionPanel::OnVerifyTimeout);

    RefreshToolState();
    RefreshCodecChoices();
    RebuildPlan();
}

ConversionPanel::~ConversionPanel() {
    CancelProbe();
    CancelEncoderListProbe();
    CancelVerify();
}

// ===================== UI =====================

void ConversionPanel::BuildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // ---------------- 页头：标题 · 源信息 · 程序状态 ----------------
    // 一行说完三件事：这是什么页、源是什么、手里的 ffmpeg 是哪份。
    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    auto* title = new QLabel(tr("格式转换"), this);
    QFont title_font = title->font();
    title_font.setPointSize(title_font.pointSize() + 4);
    title_font.setBold(true);
    title->setFont(title_font);
    header->addWidget(title);

    source_summary_ = new QLabel(tr("还没有选择源文件"), this);
    source_summary_->setStyleSheet(ColorStyle(theme::color::kTextSecondary));
    header->addWidget(source_summary_, 1);

    tool_label_ = new QLabel(this);
    header->addWidget(tool_label_);
    root->addLayout(header);

    // ---------------- 源文件 ----------------
    auto* input_group = new QGroupBox(tr("源文件"), this);
    auto* input_layout = new QGridLayout(input_group);
    input_layout->setContentsMargins(14, 18, 14, 12);
    input_layout->setHorizontalSpacing(10);
    input_layout->setColumnStretch(1, 1);

    input_layout->addWidget(new QLabel(tr("文件"), input_group), 0, 0);
    input_edit_ = new QLineEdit(input_group);
    input_edit_->setObjectName(QStringLiteral("ConversionInputPath"));
    input_edit_->setPlaceholderText(tr("选择要转换的文件"));
    input_edit_->setClearButtonEnabled(true);
    input_layout->addWidget(input_edit_, 0, 1);
    browse_input_button_ = new QPushButton(tr("浏览…"), input_group);
    input_layout->addWidget(browse_input_button_, 0, 2);
    use_current_button_ = new QPushButton(tr("用当前媒体"), input_group);
    use_current_button_->setEnabled(false);
    use_current_button_->setToolTip(tr("把主窗口当前打开的文件填进来"));
    input_layout->addWidget(use_current_button_, 0, 3);
    root->addWidget(input_group);

    connect(browse_input_button_, &QPushButton::clicked, this, &ConversionPanel::OnBrowseInput);
    connect(use_current_button_, &QPushButton::clicked, this, &ConversionPanel::OnUseCurrentFile);
    // 逐字符只更新预览；**探测放到 editingFinished**：否则用户每敲一个字母都会
    // 起一个 ffmpeg 进程去读那个还不存在的路径。
    connect(input_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (auto_output_) {
            output_edit_->setText(ffmpeg::DefaultConversionOutputPath(text.trimmed(), CurrentProfile().container));
        }
        RebuildPlan();
    });
    connect(input_edit_, &QLineEdit::editingFinished, this, [this]() { StartProbe(input_edit_->text().trimmed()); });

    // ---------------- 目标格式 ----------------
    auto* target_group = new QGroupBox(tr("目标格式"), this);
    auto* grid = new QGridLayout(target_group);
    grid->setContentsMargins(14, 18, 14, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    for (int col : {1, 3, 5}) {
        grid->setColumnStretch(col, 1);
    }

    grid->addWidget(new QLabel(tr("容器"), target_group), 0, 0);
    container_combo_ = new QComboBox(target_group);
    container_combo_->setObjectName(QStringLiteral("ConversionContainer"));
    for (const auto container : ffmpeg::AllConversionContainers()) {
        AddEnumItem(container_combo_, ffmpeg::ContainerLabel(container), container);
    }
    container_combo_->setToolTip(tr("输出封装格式。容器决定哪些编码能被装进去（例如 WebM 只收 VP9/AV1）。"));
    grid->addWidget(container_combo_, 0, 1);

    grid->addWidget(new QLabel(tr("视频编码"), target_group), 0, 2);
    video_combo_ = new QComboBox(target_group);
    video_combo_->setObjectName(QStringLiteral("ConversionVideoCodec"));
    grid->addWidget(video_combo_, 0, 3);

    grid->addWidget(new QLabel(tr("音频编码"), target_group), 0, 4);
    audio_combo_ = new QComboBox(target_group);
    audio_combo_->setObjectName(QStringLiteral("ConversionAudioCodec"));
    grid->addWidget(audio_combo_, 0, 5);

    grid->addWidget(new QLabel(tr("画质"), target_group), 1, 0);
    quality_combo_ = new QComboBox(target_group);
    quality_combo_->setObjectName(QStringLiteral("ConversionQuality"));
    AddEnumItem(quality_combo_, tr("高质量"), ffmpeg::ConversionQuality::High);
    AddEnumItem(quality_combo_, tr("平衡（推荐）"), ffmpeg::ConversionQuality::Balanced);
    AddEnumItem(quality_combo_, tr("小体积"), ffmpeg::ConversionQuality::Small);
    AddEnumItem(quality_combo_, tr("自定义 CRF"), ffmpeg::ConversionQuality::Custom);
    quality_combo_->setCurrentIndex(1);
    grid->addWidget(quality_combo_, 1, 1);

    grid->addWidget(new QLabel(tr("CRF"), target_group), 1, 2);
    crf_spin_ = new QSpinBox(target_group);
    crf_spin_->setRange(0, 63); // H.264/HEVC 上限 51、VP9/AV1 上限 63，统一放到 63 再在 core 里夹
    crf_spin_->setValue(23);
    crf_spin_->setEnabled(false);
    crf_spin_->setToolTip(tr("只在「自定义 CRF」时生效。数值越小越清晰、文件越大（H.264 常用 18~28）。"));
    grid->addWidget(crf_spin_, 1, 3);

    grid->addWidget(new QLabel(tr("音频码率"), target_group), 1, 4);
    bitrate_combo_ = new QComboBox(target_group);
    AddEnumItem(bitrate_combo_, tr("自动"), 0);
    for (const int kbps : {96, 128, 192, 256, 320}) {
        AddEnumItem(bitrate_combo_, QStringLiteral("%1 kbps").arg(kbps), kbps);
    }
    grid->addWidget(bitrate_combo_, 1, 5);

    grid->addWidget(new QLabel(tr("缩放"), target_group), 2, 0);
    scale_combo_ = new QComboBox(target_group);
    scale_combo_->setObjectName(QStringLiteral("ConversionScale"));
    AddEnumItem(scale_combo_, tr("保持原样"), ffmpeg::ConversionScale::Keep);
    AddEnumItem(scale_combo_, tr("1080p"), ffmpeg::ConversionScale::P1080);
    AddEnumItem(scale_combo_, tr("720p"), ffmpeg::ConversionScale::P720);
    AddEnumItem(scale_combo_, tr("480p"), ffmpeg::ConversionScale::P480);
    scale_combo_->setToolTip(tr("只约束高度，宽度按比例自动算（保持偶数）。流复制时不可用。"));
    grid->addWidget(scale_combo_, 2, 1);

    grid->addWidget(new QLabel(tr("帧率"), target_group), 2, 2);
    fps_combo_ = new QComboBox(target_group);
    AddEnumItem(fps_combo_, tr("保持原样"), ffmpeg::ConversionFrameRate::Keep);
    AddEnumItem(fps_combo_, QStringLiteral("60 fps"), ffmpeg::ConversionFrameRate::Fps60);
    AddEnumItem(fps_combo_, QStringLiteral("30 fps"), ffmpeg::ConversionFrameRate::Fps30);
    AddEnumItem(fps_combo_, QStringLiteral("24 fps"), ffmpeg::ConversionFrameRate::Fps24);
    grid->addWidget(fps_combo_, 2, 3);

    grid->addWidget(new QLabel(tr("码率模式"), target_group), 2, 4);
    rate_mode_combo_ = new QComboBox(target_group);
    rate_mode_combo_->setObjectName(QStringLiteral("ConversionRateMode"));
    AddEnumItem(rate_mode_combo_, tr("按质量（CRF）"), ffmpeg::ConversionVideoRateMode::Quality);
    AddEnumItem(rate_mode_combo_, tr("按码率"), ffmpeg::ConversionVideoRateMode::Bitrate);
    rate_mode_combo_->setToolTip(
        tr("按质量：画质优先，体积随内容浮动；按码率：体积可预估，适合有上传/带宽上限的交付。"));
    grid->addWidget(rate_mode_combo_, 2, 5);

    grid->addWidget(new QLabel(tr("视频码率"), target_group), 3, 0);
    video_bitrate_spin_ = new QSpinBox(target_group);
    video_bitrate_spin_->setObjectName(QStringLiteral("ConversionVideoBitrate"));
    video_bitrate_spin_->setRange(100, 200000);
    video_bitrate_spin_->setSingleStep(500);
    video_bitrate_spin_->setValue(4000);
    video_bitrate_spin_->setSuffix(tr(" kbps"));
    video_bitrate_spin_->setToolTip(tr("只在「按码率」时生效。目标平均码率：体积 ≈ 码率 × 时长。"));
    grid->addWidget(video_bitrate_spin_, 3, 1);

    grid->addWidget(new QLabel(tr("编码预设"), target_group), 3, 2);
    preset_combo_ = new QComboBox(target_group);
    preset_combo_->setObjectName(QStringLiteral("ConversionPreset"));
    for (const auto preset : ffmpeg::AllConversionPresets()) {
        AddEnumItem(preset_combo_, ffmpeg::PresetLabel(preset), preset);
    }
    preset_combo_->setToolTip(tr("只对 H.264 / H.265 生效：越慢压缩率越高，同画质下文件更小。"));
    grid->addWidget(preset_combo_, 3, 3);

    grid->addWidget(new QLabel(tr("像素格式"), target_group), 3, 4);
    pixel_combo_ = new QComboBox(target_group);
    pixel_combo_->setObjectName(QStringLiteral("ConversionPixelFormat"));
    for (const auto format : ffmpeg::AllConversionPixelFormats()) {
        AddEnumItem(pixel_combo_, ffmpeg::PixelFormatLabel(format), format);
    }
    pixel_combo_->setToolTip(
        tr("8-bit 兼容性最好；10-bit 需要编码器支持（H.264 / H.265 / VP9 / AV1），能减少暗部色带。"));
    grid->addWidget(pixel_combo_, 3, 5);

    grid->addWidget(new QLabel(tr("附加参数"), target_group), 4, 0);
    extra_edit_ = new QLineEdit(target_group);
    extra_edit_->setObjectName(QStringLiteral("ConversionExtraArgs"));
    extra_edit_->setPlaceholderText(tr("可选：追加 ffmpeg 原始参数，例如 -metadata title=\"片名\" -map 0:s?"));
    extra_edit_->setClearButtonEnabled(true);
    extra_edit_->setToolTip(tr("原样追加到输出文件之前，ffmpeg 以最后一个同名选项为准。\n"
                               "不允许 -i / -y / -n / -nostdin，也不能写输入或输出文件路径。"));
    grid->addWidget(extra_edit_, 4, 1, 1, 5);

    auto* output_label = new QLabel(tr("输出文件"), target_group);
    grid->addWidget(output_label, 5, 0);
    output_edit_ = new QLineEdit(target_group);
    output_edit_->setObjectName(QStringLiteral("ConversionOutputPath"));
    output_edit_->setPlaceholderText(tr("留空则与源文件同目录、同名换扩展名"));
    output_edit_->setClearButtonEnabled(true);
    grid->addWidget(output_edit_, 5, 1, 1, 4);
    browse_output_button_ = new QPushButton(tr("另存为…"), target_group);
    grid->addWidget(browse_output_button_, 5, 5);
    root->addWidget(target_group);

    connect(container_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnContainerChanged);
    connect(video_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(audio_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(quality_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(crf_spin_, &QSpinBox::valueChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(rate_mode_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(video_bitrate_spin_, &QSpinBox::valueChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(preset_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(pixel_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(bitrate_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(scale_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(fps_combo_, &QComboBox::currentIndexChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(extra_edit_, &QLineEdit::textChanged, this, &ConversionPanel::OnSettingsChanged);
    // textEdited 只在用户手工输入时发；程序调用 setText 不发 —— auto_output_ 靠这个区分。
    connect(output_edit_, &QLineEdit::textEdited, this, &ConversionPanel::OnOutputEdited);
    connect(output_edit_, &QLineEdit::textChanged, this, &ConversionPanel::OnSettingsChanged);
    connect(browse_output_button_, &QPushButton::clicked, this, &ConversionPanel::OnBrowseOutput);

    // ---------------- 命令预览 ----------------
    // 预览用**真实将执行的 argv**（含引号），不是另拼一份给人看的文案 ——
    // 两套拼法迟早会不一致，而用户正是拿着这一行去判断"到底要转成什么"。
    auto* command_group = new QGroupBox(tr("将要执行的命令"), this);
    auto* command_layout = new QVBoxLayout(command_group);
    command_layout->setContentsMargins(14, 18, 14, 12);
    command_layout->setSpacing(8);
    command_view_ = new QPlainTextEdit(command_group);
    command_view_->setObjectName(QStringLiteral("ConversionCommand"));
    command_view_->setReadOnly(true);
    command_view_->setFont(theme::font::monoFont(9));
    command_view_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    command_view_->setFixedHeight(64);
    command_layout->addWidget(command_view_);

    auto* hint_row = new QHBoxLayout();
    hint_label_ = new QLabel(command_group);
    hint_label_->setWordWrap(true);
    hint_label_->setStyleSheet(ColorStyle(theme::color::kTextSecondary));
    hint_row->addWidget(hint_label_, 1);
    copy_button_ = new QPushButton(tr("复制命令"), command_group);
    hint_row->addWidget(copy_button_);
    command_layout->addLayout(hint_row);
    root->addWidget(command_group);

    connect(copy_button_, &QPushButton::clicked, this, &ConversionPanel::OnCopyCommand);

    // ---------------- 运行 ----------------
    auto* run_group = new QGroupBox(tr("转换"), this);
    auto* run_layout = new QVBoxLayout(run_group);
    run_layout->setContentsMargins(14, 18, 14, 12);
    run_layout->setSpacing(10);

    auto* run_row = new QHBoxLayout();
    run_row->setSpacing(10);
    start_button_ = new QPushButton(tr("开始转换"), run_group);
    start_button_->setObjectName(QStringLiteral("ConversionStart"));
    start_button_->setDefault(true);
    run_row->addWidget(start_button_);
    stop_button_ = new QPushButton(tr("停止"), run_group);
    stop_button_->setEnabled(false);
    run_row->addWidget(stop_button_);

    progress_bar_ = new QProgressBar(run_group);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    run_row->addWidget(progress_bar_, 1);

    status_label_ = new QLabel(tr("就绪"), run_group);
    status_label_->setStyleSheet(ColorStyle(theme::color::kTextSecondary));
    run_row->addWidget(status_label_);
    run_layout->addLayout(run_row);

    log_view_ = new QPlainTextEdit(run_group);
    log_view_->setReadOnly(true);
    log_view_->setFont(theme::font::monoFont(9));
    log_view_->setMaximumBlockCount(4000); // ffmpeg 的进度行按 \r 切，几分钟就上百行
    log_view_->setMinimumHeight(140);
    run_layout->addWidget(log_view_, 1);

    // 只在真的产出文件时出现：失败 / 被停止时点开一个空目录只会让人更困惑。
    result_row_ = new QWidget(run_group);
    auto* result_layout = new QHBoxLayout(result_row_);
    result_layout->setContentsMargins(0, 0, 0, 0);
    result_layout->setSpacing(10);
    open_dir_button_ = new QPushButton(tr("打开输出目录"), result_row_);
    open_media_button_ = new QPushButton(tr("在 VideoEye 中打开"), result_row_);
    result_layout->addWidget(open_dir_button_);
    result_layout->addWidget(open_media_button_);
    result_layout->addStretch(1);
    result_row_->setVisible(false);
    run_layout->addWidget(result_row_);
    root->addWidget(run_group, 1);

    connect(start_button_, &QPushButton::clicked, this, &ConversionPanel::OnStart);
    connect(stop_button_, &QPushButton::clicked, this, &ConversionPanel::OnStop);
    connect(open_dir_button_, &QPushButton::clicked, this, &ConversionPanel::OnOpenOutputDir);
    connect(open_media_button_, &QPushButton::clicked, this, &ConversionPanel::OnOpenMedia);
}

// ===================== 参数与命令预览 =====================

ffmpeg::ConversionProfile ConversionPanel::CurrentProfile() const {
    ffmpeg::ConversionProfile profile;
    profile.container = CurrentEnum<ffmpeg::ConversionContainer>(container_combo_);
    profile.video = CurrentEnum<ffmpeg::ConversionVideoCodec>(video_combo_);
    profile.audio = CurrentEnum<ffmpeg::ConversionAudioCodec>(audio_combo_);
    profile.quality = CurrentEnum<ffmpeg::ConversionQuality>(quality_combo_);
    profile.crf = crf_spin_->value();
    profile.video_rate_mode = CurrentEnum<ffmpeg::ConversionVideoRateMode>(rate_mode_combo_);
    profile.video_bitrate_kbps = video_bitrate_spin_->value();
    profile.preset = CurrentEnum<ffmpeg::ConversionPreset>(preset_combo_);
    profile.pixel_format = CurrentEnum<ffmpeg::ConversionPixelFormat>(pixel_combo_);
    profile.audio_bitrate_kbps = bitrate_combo_->currentData().toInt();
    profile.scale = CurrentEnum<ffmpeg::ConversionScale>(scale_combo_);
    profile.fps = CurrentEnum<ffmpeg::ConversionFrameRate>(fps_combo_);
    profile.extra_arguments = extra_edit_->text();
    return profile;
}

void ConversionPanel::RefreshCodecChoices() {
    const auto container = CurrentEnum<ffmpeg::ConversionContainer>(container_combo_);
    // 记住原选择：换容器时只要新容器也支持，就别把用户选好的编码改掉。
    const int wanted_video = video_combo_->currentData().toInt();
    const int wanted_audio = audio_combo_->currentData().toInt();

    {
        const QSignalBlocker blocker(video_combo_);
        video_combo_->clear();
        for (const auto codec : ffmpeg::SupportedVideoCodecs(container)) {
            AddEnumItem(video_combo_, ffmpeg::VideoCodecLabel(codec), codec);
        }
        const int index = video_combo_->findData(wanted_video);
        video_combo_->setCurrentIndex(index >= 0 ? index : 0);
    }
    {
        const QSignalBlocker blocker(audio_combo_);
        audio_combo_->clear();
        for (const auto codec : ffmpeg::SupportedAudioCodecs(container)) {
            AddEnumItem(audio_combo_, ffmpeg::AudioCodecLabel(codec), codec);
        }
        const int index = audio_combo_->findData(wanted_audio);
        audio_combo_->setCurrentIndex(index >= 0 ? index : 0);
    }

    // 纯音频容器 / GIF：视频与音频编码没有可选项，直接锁住，别让人以为能改。
    video_combo_->setEnabled(HasRealVideoChoice(ffmpeg::SupportedVideoCodecs(container)));
    audio_combo_->setEnabled(HasRealAudioChoice(ffmpeg::SupportedAudioCodecs(container)));
}

void ConversionPanel::RebuildPlan() {
    const QString input = input_edit_->text().trimmed();
    const auto resolved_video = ffmpeg::ResolveVideoCodec(CurrentEnum<ffmpeg::ConversionContainer>(container_combo_),
                                                          CurrentEnum<ffmpeg::ConversionVideoCodec>(video_combo_));
    // 流复制配 -vf 是 ffmpeg 明确禁止的组合：先把缩放/帧率归位到"保持原样"，
    // 再拿参数去算命令。否则界面显示"720p"、命令里却没有 -vf，两边对不上。
    if (resolved_video == ffmpeg::ConversionVideoCodec::Copy) {
        ResetTransformChoices();
    }
    // 预设 / 10-bit 只对部分编码有意义；换了编码还留着旧选项会让界面与命令不一致。
    NormalizeCodecOptions(resolved_video);
    const ffmpeg::ConversionProfile profile = CurrentProfile();
    const ffmpeg::ConversionPlan plan =
        ffmpeg::BuildConversionPlan(profile, input, output_edit_->text().trimmed(), CurrentSourceFacts());

    // 命令预览
    if (plan.ok) {
        command_view_->setPlainText(ffmpeg::FfmpegProcessRunner::BuildDisplayCommand(CurrentProgram(), plan.arguments));
    } else {
        command_view_->setPlainText(plan.error);
    }

    // 编码器是否真的在这份 ffmpeg 里：清单没查过时 HasEncoder 一律返回 true（不下结论），
    // 查到缺失就在预览阶段拦住，不必等它跑到一半才报 "Unknown encoder"。
    QStringList missing_encoders;
    if (plan.ok) {
        auto& caps = ffmpeg::FfmpegCapabilityCache::Instance();
        if (caps.encoders_known()) {
            for (const QString& encoder : ffmpeg::RequiredEncoders(profile)) {
                if (!caps.HasEncoder(encoder)) {
                    missing_encoders << encoder;
                }
            }
        }
    }

    // 提示行：先报"实际会用什么"，再报能跑但有坑的地方
    QStringList notes;
    if (plan.ok) {
        const auto video = ffmpeg::ResolveVideoCodec(profile.container, profile.video);
        const auto audio = ffmpeg::ResolveAudioCodec(profile.container, profile.audio);
        QStringList actual;
        if (video == ffmpeg::ConversionVideoCodec::Copy) {
            actual << tr("视频：流复制");
        } else if (video == ffmpeg::ConversionVideoCodec::None) {
            actual << tr("视频：不带");
        } else {
            actual << tr("视频：%1").arg(ffmpeg::VideoCodecLabel(video));
        }
        if (audio == ffmpeg::ConversionAudioCodec::Copy) {
            actual << tr("音频：流复制");
        } else if (audio == ffmpeg::ConversionAudioCodec::None) {
            actual << tr("音频：不带");
        } else {
            actual << tr("音频：%1").arg(ffmpeg::AudioCodecLabel(audio));
        }
        notes << actual.join(QStringLiteral(" · "));
        notes << plan.warnings;
        if (!missing_encoders.isEmpty()) {
            notes << tr("本机 ffmpeg 缺少编码器：%1 —— 请改选编码，或换一份包含它们的 ffmpeg 构建")
                         .arg(missing_encoders.join(QStringLiteral("、")));
        }
        if (!missing_encoders.isEmpty()) {
            hint_label_->setStyleSheet(ColorStyle(theme::color::kDanger));
        } else {
            hint_label_->setStyleSheet(
                ColorStyle(plan.warnings.isEmpty() ? theme::color::kTextSecondary : theme::color::kWarning));
        }
    } else {
        notes << plan.error;
        hint_label_->setStyleSheet(ColorStyle(theme::color::kDanger));
    }
    hint_label_->setText(notes.join(QStringLiteral("\n")));

    const bool running = runner_ != nullptr && runner_->IsRunning();
    const bool tool_ready = !CurrentProgram().isEmpty();
    start_button_->setEnabled(plan.ok && tool_ready && !running && missing_encoders.isEmpty());
    stop_button_->setEnabled(running);
    // 流复制 / 不要视频时这些参数不会进命令，这里顺手置灰，
    // 比让用户点完再看到红字提示更省事。
    const bool can_transform =
        resolved_video != ffmpeg::ConversionVideoCodec::Copy && resolved_video != ffmpeg::ConversionVideoCodec::None;
    const bool rate_by_bitrate = profile.video_rate_mode == ffmpeg::ConversionVideoRateMode::Bitrate;
    scale_combo_->setEnabled(can_transform && !running);
    fps_combo_->setEnabled(can_transform && !running);
    rate_mode_combo_->setEnabled(can_transform && !running);
    video_bitrate_spin_->setEnabled(can_transform && rate_by_bitrate && !running);
    quality_combo_->setEnabled(can_transform && !rate_by_bitrate && !running);
    crf_spin_->setEnabled(can_transform && !rate_by_bitrate && profile.quality == ffmpeg::ConversionQuality::Custom &&
                          !running);
    preset_combo_->setEnabled(can_transform && ffmpeg::CodecUsesPreset(resolved_video) && !running);
    pixel_combo_->setEnabled(can_transform && !running);
    bitrate_combo_->setEnabled(profile.audio != ffmpeg::ConversionAudioCodec::Copy &&
                               profile.audio != ffmpeg::ConversionAudioCodec::None && !running);
}

void ConversionPanel::OnSettingsChanged() {
    RebuildPlan();
}

void ConversionPanel::ResetTransformChoices() {
    // 用 blocker：这里只是把控件归位，不需要再触发一轮 RebuildPlan（正在里面）。
    const QSignalBlocker scale_blocker(scale_combo_);
    const QSignalBlocker fps_blocker(fps_combo_);
    if (scale_combo_->currentIndex() != 0) {
        scale_combo_->setCurrentIndex(0);
    }
    if (fps_combo_->currentIndex() != 0) {
        fps_combo_->setCurrentIndex(0);
    }
}

void ConversionPanel::NormalizeCodecOptions(ffmpeg::ConversionVideoCodec video) {
    // 同 ResetTransformChoices：只归位，不再触发一轮 RebuildPlan。
    const QSignalBlocker preset_blocker(preset_combo_);
    const QSignalBlocker pixel_blocker(pixel_combo_);
    if (!ffmpeg::CodecUsesPreset(video)) {
        const int index = preset_combo_->findData(static_cast<int>(ffmpeg::ConversionPreset::Medium));
        if (index >= 0 && preset_combo_->currentIndex() != index) {
            preset_combo_->setCurrentIndex(index);
        }
    }
    if (!ffmpeg::CodecSupportsTenBit(video) &&
        CurrentEnum<ffmpeg::ConversionPixelFormat>(pixel_combo_) == ffmpeg::ConversionPixelFormat::Yuv420p10) {
        const int index = pixel_combo_->findData(static_cast<int>(ffmpeg::ConversionPixelFormat::Yuv420p));
        if (index >= 0) {
            pixel_combo_->setCurrentIndex(index);
        }
    }
}

void ConversionPanel::OnContainerChanged() {
    RefreshCodecChoices();
    // 自动生成的输出名要跟着换扩展名；用户自己填过的输出一律不覆盖。
    if (auto_output_) {
        const QString input = input_edit_->text().trimmed();
        output_edit_->setText(
            ffmpeg::DefaultConversionOutputPath(input, CurrentEnum<ffmpeg::ConversionContainer>(container_combo_)));
    }
    RebuildPlan();
}

void ConversionPanel::OnOutputEdited() {
    auto_output_ = false;
    RebuildPlan();
}

// ===================== 源探测 =====================

ffmpeg::ConversionSourceFacts ConversionPanel::CurrentSourceFacts() const {
    ffmpeg::ConversionSourceFacts facts;
    // 探测结果只对"当前输入框里的那个文件"有效：换了文件但还没探完时不能拿旧结果
    // 去判断兼容性（那会把 A 文件的编码当成 B 文件的）。
    if (!source_probe_.parsed || probe_path_ != input_edit_->text().trimmed()) {
        return facts;
    }
    facts.known = true;
    const auto* video = source_probe_.FirstStream(ffmpeg::MediaStreamKind::Video);
    const auto* audio = source_probe_.FirstStream(ffmpeg::MediaStreamKind::Audio);
    // 封面图（attached pic）是 video 流但不参与转码对比，算"没有视频轨"更准。
    facts.has_video = source_probe_.CountPlayableStreams(ffmpeg::MediaStreamKind::Video) > 0;
    facts.has_audio = source_probe_.CountPlayableStreams(ffmpeg::MediaStreamKind::Audio) > 0;
    facts.video_codec = video != nullptr ? video->codec : QString();
    facts.audio_codec = audio != nullptr ? audio->codec : QString();
    facts.audio_track_count = source_probe_.CountPlayableStreams(ffmpeg::MediaStreamKind::Audio);
    facts.subtitle_track_count = source_probe_.CountStreams(ffmpeg::MediaStreamKind::Subtitle);
    facts.duration_seconds = source_probe_.duration_seconds;
    return facts;
}

void ConversionPanel::RefreshSourceSummary() {
    if (probe_path_.isEmpty()) {
        source_summary_->setText(tr("还没有选择源文件"));
        source_summary_->setStyleSheet(ColorStyle(theme::color::kTextSecondary));
        return;
    }
    if (!source_probe_.parsed) {
        source_summary_->setText(
            tr("源：%1（未识别出流信息，实际以转换结果为准）").arg(QFileInfo(probe_path_).fileName()));
        source_summary_->setStyleSheet(ColorStyle(theme::color::kWarning));
        return;
    }

    QStringList parts;
    parts << QFileInfo(probe_path_).fileName();
    if (!source_probe_.container.isEmpty()) {
        parts << source_probe_.container;
    }
    parts << ffmpeg::FormatDurationHuman(source_probe_.duration_seconds);
    if (const auto* video = source_probe_.FirstStream(ffmpeg::MediaStreamKind::Video)) {
        QString video_text = video->codec;
        if (video->has_size()) {
            video_text += QLatin1Char(' ') + ffmpeg::FormatResolutionHuman(video->width, video->height);
        }
        parts << video_text.trimmed();
    }
    if (const auto* audio = source_probe_.FirstStream(ffmpeg::MediaStreamKind::Audio)) {
        QString audio_text = audio->codec;
        if (audio->sample_rate > 0) {
            audio_text += QStringLiteral(" %1 Hz").arg(audio->sample_rate);
        }
        parts << audio_text.trimmed();
    }
    if (source_probe_.file_size_bytes > 0) {
        parts << tr("%1 MB").arg(QString::number(source_probe_.file_size_bytes / 1024.0 / 1024.0, 'f', 1));
    }
    source_summary_->setText(parts.join(QStringLiteral(" · ")));
    source_summary_->setStyleSheet(ColorStyle(theme::color::kTextSecondary));
}

void ConversionPanel::StartProbe(const QString& path) {
    CancelProbe();
    source_probe_ = ffmpeg::MediaProbeSnapshot();
    const QString program = CurrentProgram();
    if (path.isEmpty() || program.isEmpty()) {
        RefreshSourceSummary();
        RebuildPlan();
        return;
    }

    probe_path_ = path;
    probe_buffer_.clear();
    const quint64 generation = ++probe_generation_;
    probe_launch_generation_ = generation;
    probe_timer_->start(10000);
    if (!probe_runner_->Start(program, ffmpeg::BuildMediaProbeArguments(path))) {
        probe_timer_->stop();
        probe_path_.clear();
        RefreshSourceSummary();
    }
}

void ConversionPanel::CancelProbe() {
    if (probe_timer_ != nullptr) {
        probe_timer_->stop();
    }
    // 世代号推进：在飞的回调（含已排队的 Finished）回来时会认不出来，结果直接丢弃。
    ++probe_generation_;
    probe_buffer_.clear();
    probe_path_.clear();
    source_probe_ = ffmpeg::MediaProbeSnapshot();
}

void ConversionPanel::OnProbeOutput(const QString& text, bool) {
    probe_buffer_ += text;
    probe_buffer_ += QLatin1Char('\n');
}

void ConversionPanel::OnProbeFinished(const ffmpeg::FfmpegRunResult&) {
    // `ffmpeg -i <file>` **一定**以退出码 1 结束（"At least one output file must be
    // specified"），所以这里只看解析结果，不看 status。
    if (probe_launch_generation_ != probe_generation_ || probe_path_.isEmpty()) {
        return;
    }
    probe_timer_->stop();
    source_probe_ = ffmpeg::ParseMediaProbe(probe_buffer_, probe_path_);
    ffmpeg::FillLocalFileFacts(&source_probe_);
    RefreshSourceSummary();
    RebuildPlan(); // 源编码已知后，流复制的兼容性提示会变得更准确
}

void ConversionPanel::OnProbeTimeout() {
    if (probe_path_.isEmpty()) {
        return;
    }
    // 超时不是"没结果"而是"这一个文件探不出来"：停掉进程并作废结果，
    // 让页面退回"未知"，而不是挂着等。
    ++probe_generation_;
    probe_buffer_.clear();
    if (probe_runner_ != nullptr) {
        probe_runner_->Stop(0, 0);
    }
    source_probe_ = ffmpeg::MediaProbeSnapshot();
    RefreshSourceSummary();
    RebuildPlan();
}

// ===================== 编码器清单 =====================

void ConversionPanel::StartEncoderListProbe(const QString& program) {
    if (program.isEmpty()) {
        return;
    }
    auto& caps = ffmpeg::FfmpegCapabilityCache::Instance();
    if (caps.encoders_known()) {
        return; // 已经查过（可能是「FFmpeg 命令」页查的），不再起进程
    }
    if (encoder_runner_->IsRunning()) {
        if (encoder_program_ == program) {
            return; // 同一份程序的清单正在查
        }
        // 程序换过：这一轮结果作废，等旧进程退出后在 Finished 回调里按新路径重来。
        ++encoder_generation_;
        encoder_buffer_.clear();
        encoder_program_ = program;
        encoder_runner_->Stop(0, 0);
        return;
    }
    encoder_program_ = program;
    encoder_buffer_.clear();
    const quint64 generation = ++encoder_generation_;
    encoder_launch_generation_ = generation;
    encoder_timer_->start(10000);
    if (!encoder_runner_->Start(program, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")})) {
        encoder_timer_->stop();
        encoder_buffer_.clear();
    }
}

void ConversionPanel::CancelEncoderListProbe() {
    if (encoder_timer_ != nullptr) {
        encoder_timer_->stop();
    }
    ++encoder_generation_;
    encoder_buffer_.clear();
    if (encoder_runner_ != nullptr && encoder_runner_->IsRunning()) {
        // 只是打印清单，没有输出文件要收尾 —— 直接终止，不用给优雅退出的时间。
        encoder_runner_->Stop(0, 0);
    }
}

void ConversionPanel::OnEncoderListOutput(const QString& text, bool) {
    if (encoder_launch_generation_ != encoder_generation_) {
        return;
    }
    encoder_buffer_ += text;
    encoder_buffer_ += QLatin1Char('\n');
}

void ConversionPanel::OnEncoderListFinished(const ffmpeg::FfmpegRunResult& result) {
    if (encoder_launch_generation_ != encoder_generation_) {
        // 被"换程序"作废的一轮：它的清单一个字都不能用，按当前程序重查。
        encoder_buffer_.clear();
        StartEncoderListProbe(CurrentProgram());
        return;
    }
    encoder_timer_->stop();
    if (result.status == ffmpeg::FfmpegRunStatus::Failed || result.status == ffmpeg::FfmpegRunStatus::StartError ||
        result.status == ffmpeg::FfmpegRunStatus::Stopped) {
        // 查不到就当"未知"：HasEncoder 在未知时一律返回 true，绝不因为查不到而误拦。
        encoder_buffer_.clear();
        RebuildPlan();
        return;
    }
    ffmpeg::FfmpegCapabilityCache::Instance().SetEncoders(
        ffmpeg::FfmpegCommandCatalog::ParseEncoderNames(encoder_buffer_));
    encoder_buffer_.clear();
    RebuildPlan();
}

void ConversionPanel::OnEncoderListTimeout() {
    ++encoder_generation_;
    encoder_buffer_.clear();
    if (encoder_runner_ != nullptr) {
        encoder_runner_->Stop(0, 0);
    }
    RebuildPlan();
}

// ===================== 产物核对 =====================

void ConversionPanel::StartVerify(const QString& path) {
    CancelVerify();
    if (path.isEmpty()) {
        return;
    }
    const QString program = CurrentProgram();
    if (program.isEmpty() || !QFileInfo(path).isFile()) {
        return;
    }
    verify_path_ = path;
    verify_buffer_.clear();
    const quint64 generation = ++verify_generation_;
    verify_launch_generation_ = generation;
    verify_timer_->start(10000);
    if (!verify_runner_->Start(program, ffmpeg::BuildMediaProbeArguments(path))) {
        verify_timer_->stop();
        verify_path_.clear();
    }
}

void ConversionPanel::CancelVerify() {
    if (verify_timer_ != nullptr) {
        verify_timer_->stop();
    }
    ++verify_generation_;
    verify_buffer_.clear();
    verify_path_.clear();
    if (verify_runner_ != nullptr && verify_runner_->IsRunning()) {
        verify_runner_->Stop(0, 0);
    }
}

void ConversionPanel::OnVerifyOutput(const QString& text, bool) {
    if (verify_launch_generation_ != verify_generation_) {
        return;
    }
    verify_buffer_ += text;
    verify_buffer_ += QLatin1Char('\n');
}

void ConversionPanel::OnVerifyFinished(const ffmpeg::FfmpegRunResult&) {
    // 与源探测同一口径：`ffmpeg -i` 一定以退出码 1 结束（"At least one output file
    // must be specified"），这里只看解析结果，不看 status。
    if (verify_launch_generation_ != verify_generation_ || verify_path_.isEmpty()) {
        return;
    }
    verify_timer_->stop();
    const ffmpeg::MediaProbeSnapshot snapshot = ffmpeg::ParseMediaProbe(verify_buffer_, verify_path_);
    verify_buffer_.clear();
    ApplyVerification(snapshot);
}

void ConversionPanel::OnVerifyTimeout() {
    if (verify_path_.isEmpty()) {
        return;
    }
    ++verify_generation_;
    verify_buffer_.clear();
    if (verify_runner_ != nullptr) {
        verify_runner_->Stop(0, 0);
    }
    AppendLog(tr("核对：输出文件读不出流信息（可能仍在写入或格式不被支持），已跳过核对"), false);
}

void ConversionPanel::ApplyVerification(const ffmpeg::MediaProbeSnapshot& snapshot) {
    if (!snapshot.parsed) {
        AppendLog(tr("核对：读不出输出文件的流信息，无法确认转换结果"), false);
        return;
    }

    // 先摆事实：盘上到底是一个什么文件。
    QStringList parts;
    if (!snapshot.container.isEmpty()) {
        parts << snapshot.container;
    }
    parts << ffmpeg::FormatDurationHuman(snapshot.duration_seconds);
    if (const auto* video = snapshot.FirstStream(ffmpeg::MediaStreamKind::Video)) {
        QString video_text = video->codec;
        if (video->has_size()) {
            video_text += QLatin1Char(' ') + ffmpeg::FormatResolutionHuman(video->width, video->height);
        }
        parts << video_text.trimmed();
    }
    if (const auto* audio = snapshot.FirstStream(ffmpeg::MediaStreamKind::Audio)) {
        parts << audio->codec;
    }
    AppendLog(tr("实测输出：%1").arg(parts.join(QStringLiteral(" · "))), false);

    const ffmpeg::ConversionVerification verification =
        ffmpeg::VerifyConversionOutput(verify_profile_, verify_source_, snapshot);
    if (!verification.mismatch) {
        status_label_->setText(status_label_->text() + tr(" · 已核对"));
        AppendLog(tr("核对通过：输出编码与分辨率符合目标"), false);
        return;
    }

    for (const QString& note : verification.notes) {
        AppendLog(tr("核对：%1").arg(note), true);
    }
    status_label_->setText(status_label_->text() + tr(" · 发现 %1 处差异").arg(verification.notes.size()));
    status_label_->setStyleSheet(ColorStyle(theme::color::kWarning));
    emit StatusMessage(tr("格式转换完成，但实测结果与目标有差异"));
}

// ===================== 工具 =====================

QString ConversionPanel::CurrentProgram() const {
    return cached_program_;
}

void ConversionPanel::RefreshToolState() {
    const QString configured = settings_->value(QStringLiteral("ffmpeg/toolPath")).toString();
    const ffmpeg::FfmpegToolInfo info = ffmpeg::FfmpegToolLocator::Resolve(configured);
    cached_program_ = info.exists ? info.path : QString();

    if (cached_program_.isEmpty()) {
        tool_label_->setText(tr("未找到 ffmpeg —— 到「FFmpeg 命令」页设置程序路径"));
        tool_label_->setStyleSheet(ColorStyle(theme::color::kDanger));
        tool_label_->setToolTip(tr("这一页调用的是原生 ffmpeg 程序（与分析功能用的内置库不是同一份）。"));
    } else {
        tool_label_->setText(tr("ffmpeg：%1").arg(QFileInfo(cached_program_).fileName()));
        tool_label_->setStyleSheet(ColorStyle(theme::color::kTextSecondary));
        tool_label_->setToolTip(cached_program_ + QStringLiteral("\n") + info.origin);
    }

    // 编码器清单只需要查一次：缓存里已有就不再起进程（在「FFmpeg 命令」页查过会直接复用）。
    if (!cached_program_.isEmpty()) {
        StartEncoderListProbe(cached_program_);
    }
}

// ===================== 运行 =====================

void ConversionPanel::OnStart() {
    if (runner_->IsRunning()) {
        return;
    }
    // 上一次转换的产物的核对可能还在飞：结果属于旧文件，直接作废。
    CancelVerify();
    const QString program = CurrentProgram();
    if (program.isEmpty()) {
        AppendLog(tr("找不到 ffmpeg 程序，无法转换。"), true);
        return;
    }

    const ffmpeg::ConversionProfile profile = CurrentProfile();
    const ffmpeg::ConversionSourceFacts source = CurrentSourceFacts();
    const auto plan =
        ffmpeg::BuildConversionPlan(profile, input_edit_->text().trimmed(), output_edit_->text().trimmed(), source);
    if (!plan.ok) {
        AppendLog(plan.error, true);
        return;
    }

    // 预检只认这三类"跑起来必然失败"的问题；判断不了的结构它会整体跳过。
    for (const auto& issue : ffmpeg::PreflightCommand(plan.arguments)) {
        if (issue.severity != ffmpeg::PreflightSeverity::Error) {
            continue;
        }
        QMessageBox::warning(this, tr("转换前检查未通过"), issue.message);
        return;
    }

    const QString output = plan.arguments.back();
    const QFileInfo output_info(output);
    if (output_info.exists()) {
        // 命令里固定带 -y（否则 ffmpeg 会停在覆盖确认上，进程一直挂着），
        // 所以这一步必须问清楚 —— 这不是"顺手覆盖"，是删掉一个已有文件。
        const auto answer =
            QMessageBox::question(this, tr("输出文件已存在"),
                                  tr("%1\n\n继续转换会覆盖它。确定要继续吗？").arg(QDir::toNativeSeparators(output)),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    } else if (!output_info.absoluteDir().exists()) {
        AppendLog(tr("输出目录不存在：%1").arg(output_info.absolutePath()), true);
        return;
    }

    current_arguments_ = plan.arguments;
    last_output_path_ = output;
    // 核对以"点开始那一刻"的设置为准：转换结束后用户再改下拉，不该影响对旧产物的判断。
    verify_profile_ = profile;
    verify_source_ = source;
    run_stderr_.clear();
    last_run_ok_ = false;
    log_view_->clear();
    progress_.Reset();
    progress_bar_->setRange(0, 0); // 时长还没读到：先显示忙碌，别编一个百分比
    progress_bar_->setValue(0);
    result_row_->setVisible(false);

    for (const QString& warning : plan.warnings) {
        AppendLog(tr("提示：%1").arg(warning), false);
    }
    AppendLog(QStringLiteral("$ ") + ffmpeg::FfmpegProcessRunner::BuildDisplayCommand(program, plan.arguments), false);
    // 为什么不带 -nostdin：停止要靠往 stdin 写 "q" 才让 ffmpeg 自己写完索引（moov）。
    // 加了 -nostdin 就只剩强杀一条路，产物可能打不开。
    AppendLog(tr("(停止会先请 ffmpeg 自己收尾；超时才强制终止)"), false);

    SetRunning(true);
    if (!runner_->Start(program, plan.arguments)) {
        SetRunning(false);
        return;
    }
    status_label_->setText(tr("正在转换…"));
    status_label_->setStyleSheet(ColorStyle(theme::color::kAccent));
    emit StatusMessage(tr("格式转换已开始"));
}

void ConversionPanel::OnStop() {
    if (!runner_->IsRunning()) {
        return;
    }
    AppendLog(tr("(用户请求停止：先让 ffmpeg 收尾写出索引，最多等 3 秒)"), false);
    status_label_->setText(tr("正在停止…"));
    runner_->Stop();
    emit StatusMessage(tr("正在停止格式转换"));
}

void ConversionPanel::SetRunning(bool running) {
    const QList<QWidget*> locked = {input_edit_,          container_combo_,      video_combo_,
                                    audio_combo_,         quality_combo_,        crf_spin_,
                                    rate_mode_combo_,     video_bitrate_spin_,   preset_combo_,
                                    pixel_combo_,         bitrate_combo_,        scale_combo_,
                                    fps_combo_,           extra_edit_,           output_edit_,
                                    browse_input_button_, browse_output_button_, start_button_};
    for (QWidget* widget : locked) {
        if (widget != nullptr) {
            widget->setEnabled(!running);
        }
    }
    stop_button_->setEnabled(running);
    if (!running) {
        // 上面把控件一律解禁；这里再按容器的真实能力收回来（纯音频容器 / GIF 的
        // 编码下拉本来就该锁着），然后按当前参数重新判定哪些控件真的可用。
        RefreshCodecChoices();
        RebuildPlan();
    }
}

void ConversionPanel::OnRunOutput(const QString& text, bool is_error) {
    if (is_error) {
        run_stderr_ += text;
        run_stderr_ += QLatin1Char('\n');
    }
    if (progress_.Feed(text)) {
        ShowProgressLine();
    }
    AppendLog(text, is_error);
}

void ConversionPanel::ShowProgressLine() {
    const ffmpeg::FfmpegProgressSnapshot snapshot = progress_.Snapshot();
    if (snapshot.percent >= 0.0) {
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(static_cast<int>(snapshot.percent));
    } else {
        // 流式输入 / 时长未知：进度条保持忙碌，但下面的文字仍能给出已处理时长。
        progress_bar_->setRange(0, 0);
    }

    QString text = tr("%1 / %2").arg(progress_.position_text(), progress_.duration_text());
    if (snapshot.speed > 0.0) {
        text += tr(" · %1x").arg(snapshot.speed, 0, 'f', 2);
    }
    if (snapshot.eta_ms > 0) {
        text += tr(" · 剩余 %1").arg(FormatEta(snapshot.eta_ms));
    }
    status_label_->setText(text);
}

void ConversionPanel::OnRunFinished(const ffmpeg::FfmpegRunResult& result) {
    SetRunning(false);
    progress_bar_->setRange(0, 100);

    // 「产出」以磁盘为准：命令里写了输出名不等于它真落盘了。
    const QFileInfo output_info(last_output_path_);
    const bool produced = output_info.exists() && output_info.size() > 0;
    last_run_ok_ = result.status == ffmpeg::FfmpegRunStatus::Finished && produced;

    switch (result.status) {
    case ffmpeg::FfmpegRunStatus::Finished:
        if (produced) {
            progress_bar_->setValue(100);
            status_label_->setText(tr("完成 · 用时 %1 秒 · %2 MB")
                                       .arg(result.elapsed_ms / 1000)
                                       .arg(QString::number(output_info.size() / 1024.0 / 1024.0, 'f', 1)));
            status_label_->setStyleSheet(ColorStyle(theme::color::kSuccess));
            AppendLog(tr("转换完成：%1").arg(QDir::toNativeSeparators(last_output_path_)), false);
            emit StatusMessage(tr("格式转换完成"));
        } else {
            // 退出码 0 但文件不在：最典型的是覆盖确认被"q"回答成 no（overwrite_refused）。
            status_label_->setText(tr("已结束，但没有产出文件"));
            status_label_->setStyleSheet(ColorStyle(theme::color::kWarning));
            AppendLog(tr("ffmpeg 正常退出，但目标位置没有生成文件 —— 请检查是否取消了覆盖确认。"), true);
            emit StatusMessage(tr("格式转换已结束，但没有产出文件"));
        }
        break;
    case ffmpeg::FfmpegRunStatus::Stopped:
        status_label_->setText(result.stopped_cleanly() ? tr("已停止（输出已收尾，可播放）")
                                                        : tr("已停止（输出可能不完整）"));
        status_label_->setStyleSheet(ColorStyle(theme::color::kWarning));
        AppendLog(result.stopped_cleanly() ? tr("已停止：ffmpeg 自己收尾完成，已写入的部分可以播放。")
                                           : tr("已停止：进程被强制终止，输出文件可能缺索引而打不开。"),
                  !result.stopped_cleanly());
        emit StatusMessage(tr("格式转换已停止"));
        break;
    case ffmpeg::FfmpegRunStatus::StartError:
        status_label_->setText(tr("启动失败"));
        status_label_->setStyleSheet(ColorStyle(theme::color::kDanger));
        AppendLog(tr("启动失败：%1").arg(result.error_message), true);
        emit StatusMessage(tr("格式转换启动失败"));
        break;
    case ffmpeg::FfmpegRunStatus::Failed: {
        status_label_->setText(tr("转换失败（退出码 %1）").arg(result.exit_code));
        status_label_->setStyleSheet(ColorStyle(theme::color::kDanger));
        // 失败原因在 stderr 几十行里，这里只挑最需要先看的一两条翻成人话；
        // 没命中就什么都不编，让用户自己看上面的原始输出。
        const auto diagnoses = ffmpeg::DiagnoseFailure(run_stderr_);
        int shown = 0;
        for (const auto& diagnosis : diagnoses) {
            AppendLog(tr("可能原因：%1 —— %2").arg(diagnosis.title, diagnosis.suggestion), true);
            if (++shown >= 2) {
                break;
            }
        }
        emit StatusMessage(tr("格式转换失败"));
        break;
    }
    default:
        break;
    }

    // 只有真产出才给"去结果"的入口：失败/停止时这两个按钮只会把人带到空目录。
    result_row_->setVisible(produced);
    open_media_button_->setEnabled(produced);

    // 「退出码 0 + 文件存在」不等于"转对了"：回读一遍产物，把实测编码 / 分辨率 /
    // 时长摆出来；与目标不符时明确提示，而不是让用户自己打开才发现。
    if (last_run_ok_) {
        StartVerify(last_output_path_);
    }
}

void ConversionPanel::AppendLog(const QString& line, bool is_error) {
    if (log_view_ == nullptr) {
        return;
    }
    if (is_error) {
        log_view_->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>")
                                  .arg(QString::fromLatin1(theme::color::kDanger), line.toHtmlEscaped()));
    } else {
        log_view_->appendPlainText(line);
    }
}

// ===================== 动作 =====================

void ConversionPanel::OnBrowseInput() {
    const QString path = QFileDialog::getOpenFileName(this, tr("选择要转换的文件"), QString(), BuildMediaFileFilter());
    if (path.isEmpty()) {
        return;
    }
    auto_output_ = true;
    input_edit_->setText(path); // textChanged 会带出输出名、探测与命令预览
}

void ConversionPanel::OnUseCurrentFile() {
    if (current_media_path_.isEmpty()) {
        return;
    }
    auto_output_ = true;
    input_edit_->setText(current_media_path_);
}

void ConversionPanel::OnBrowseOutput() {
    // 默认名带目标扩展名：用户改了容器却还存成 .mp4 是最常见的"转完打不开"。
    QString suggested = output_edit_->text().trimmed();
    if (suggested.isEmpty()) {
        suggested = ffmpeg::DefaultConversionOutputPath(input_edit_->text().trimmed(),
                                                        CurrentEnum<ffmpeg::ConversionContainer>(container_combo_));
    }
    const QString extension = ffmpeg::ContainerExtension(CurrentEnum<ffmpeg::ConversionContainer>(container_combo_));
    const QString filter = tr("%1 文件 (*.%2);;所有文件 (*)").arg(extension.toUpper(), extension);
    const QString path = QFileDialog::getSaveFileName(this, tr("另存为"), suggested, filter);
    if (path.isEmpty()) {
        return;
    }
    auto_output_ = false;
    output_edit_->setText(path);
}

void ConversionPanel::OnCopyCommand() {
    QApplication::clipboard()->setText(command_view_->toPlainText());
    emit StatusMessage(tr("命令已复制到剪贴板"));
}

void ConversionPanel::OnOpenOutputDir() {
    if (last_output_path_.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(last_output_path_).absolutePath()));
}

void ConversionPanel::OnOpenMedia() {
    if (!last_run_ok_ || last_output_path_.isEmpty()) {
        return;
    }
    emit OpenMediaRequested(last_output_path_);
}

void ConversionPanel::SetCurrentFile(const QString& path) {
    current_media_path_ = path;
    use_current_button_->setEnabled(!path.isEmpty());
    // 只在输入框还空着时自动填：用户可能已经挑好了别的文件，不该被正在播放的媒体顶掉。
    if (!path.isEmpty() && input_edit_->text().trimmed().isEmpty()) {
        auto_output_ = true;
        input_edit_->setText(path);
        StartProbe(path);
    }
}

void ConversionPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (runner_ != nullptr && runner_->IsRunning()) {
        return;
    }
    // 程序路径有可能刚在别处改过；重新解析一次，免得本页一直显示"未找到 ffmpeg"。
    RefreshToolState();
    RebuildPlan();
}

bool ConversionPanel::ConfirmShutdown() {
    if (runner_ == nullptr || !runner_->IsRunning()) {
        return true;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("转换还在进行"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("有一个格式转换正在执行。"));
    box.setInformativeText(tr("直接退出会掐断它 —— 没写完索引的文件（尤其是 mp4/mov）可能打不开。\n\n"
                              "「停止并退出」会先请 ffmpeg 自己收尾（最多等 5 秒），收不住才强杀。"));
    QPushButton* stop_button = box.addButton(tr("停止并退出"), QMessageBox::AcceptRole);
    QPushButton* cancel_button = box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel_button);
    box.exec();
    if (box.clickedButton() != stop_button) {
        return false;
    }

    AppendLog(tr("(关闭窗口：已请 ffmpeg 收尾，最多等 5 秒)"), false);
    const bool stopped = runner_->StopAndWait(5000);
    AppendLog(stopped ? tr("(ffmpeg 已退出，输出的收尾是完整的)")
                      : tr("(ffmpeg 没在宽限期内退出，已被强制终止 —— 输出可能不可用)"),
              !stopped);
    return true;
}

} // namespace ui
} // namespace videoeye
