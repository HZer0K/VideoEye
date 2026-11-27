// 「格式转换」页的参数联动测试。
//
// 这一页真正的风险不是崩溃，而是**联动错了还看不出来**：换了容器却没换扩展名、
// 纯音频容器上还留着 H.264、流复制时缩放下拉还亮着 —— 用户会照着错的下拉去点，
// 直到 ffmpeg 报错才知道不对。所以这里全部按"控件当前状态"断言，不 new 主窗口、
// 不启动 ffmpeg（源探测在 editingFinished 才触发，测试里只 setText 不会起进程）。
//
// 命令拼装的规则本身在 FormatConversionTests 里已经逐条钉过，这里只验证页面确实
// 用了那份结果。

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QTemporaryDir>

#include "core/ffmpeg/FormatConversionPlan.h"
#include "ui/conversion_panel/ConversionPanel.h"

namespace {

using videoeye::ffmpeg::ConversionAudioCodec;
using videoeye::ffmpeg::ConversionContainer;
using videoeye::ffmpeg::ConversionFrameRate;
using videoeye::ffmpeg::ConversionGifLoop;
using videoeye::ffmpeg::ConversionPixelFormat;
using videoeye::ffmpeg::ConversionQuality;
using videoeye::ffmpeg::ConversionScale;
using videoeye::ffmpeg::ConversionVideoCodec;
using videoeye::ffmpeg::ConversionVideoRateMode;
using videoeye::ui::ConversionPanel;

QApplication* EnsureApp() {
    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app == nullptr) {
        static int argc = 1;
        static char arg0[] = "test_conversion_panel";
        static char* argv[] = {arg0, nullptr};
        static QApplication* created = new QApplication(argc, argv);
        app = created;
    }
    // QSettings 需要有组织名 / 应用名才有一个真实的落盘位置；测试用独立名字，
    // 既不污染正式版（VideoEye/VideoEye），也让参数持久化用例有个干净的 arena。
    app->setOrganizationName(QStringLiteral("VideoEye"));
    app->setApplicationName(QStringLiteral("VideoEyeTest"));
    return app;
}

class ConversionPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(EnsureApp(), nullptr);
        ASSERT_TRUE(dir_.isValid());
        // 参数会在析构时落盘：不清掉的话，上一个用例选过的容器 / 编码会被下一个
        // 用例的页面恢复，断言"默认值"就会随机失败。
        QSettings().remove(QStringLiteral("conversion"));
        panel_ = new ConversionPanel();
        container_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionContainer"));
        video_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionVideoCodec"));
        audio_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionAudioCodec"));
        scale_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionScale"));
        fps_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionFrameRate"));
        quality_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionQuality"));
        rate_mode_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionRateMode"));
        video_bitrate_ = panel_->findChild<QSpinBox*>(QStringLiteral("ConversionVideoBitrate"));
        crf_ = panel_->findChild<QSpinBox*>(QStringLiteral("ConversionCrf"));
        preset_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionPreset"));
        pixel_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionPixelFormat"));
        loop_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionGifLoop"));
        extra_ = panel_->findChild<QLineEdit*>(QStringLiteral("ConversionExtraArgs"));
        input_ = panel_->findChild<QLineEdit*>(QStringLiteral("ConversionInputPath"));
        output_ = panel_->findChild<QLineEdit*>(QStringLiteral("ConversionOutputPath"));
        command_ = panel_->findChild<QPlainTextEdit*>(QStringLiteral("ConversionCommand"));
        start_ = panel_->findChild<QPushButton*>(QStringLiteral("ConversionStart"));
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(video_, nullptr);
        ASSERT_NE(audio_, nullptr);
        ASSERT_NE(scale_, nullptr);
        ASSERT_NE(fps_, nullptr);
        ASSERT_NE(quality_, nullptr);
        ASSERT_NE(rate_mode_, nullptr);
        ASSERT_NE(video_bitrate_, nullptr);
        ASSERT_NE(crf_, nullptr);
        ASSERT_NE(preset_, nullptr);
        ASSERT_NE(pixel_, nullptr);
        ASSERT_NE(loop_, nullptr);
        ASSERT_NE(extra_, nullptr);
        ASSERT_NE(input_, nullptr);
        ASSERT_NE(output_, nullptr);
        ASSERT_NE(command_, nullptr);
        ASSERT_NE(start_, nullptr);
    }

    void TearDown() override {
        delete panel_;
        panel_ = nullptr;
        // 持久化用例会把选择写进测试专用的 QSettings 作用域，这里收尾清掉。
        QSettings().remove(QStringLiteral("conversion"));
    }

    void SelectContainer(ConversionContainer container) {
        const int index = container_->findData(static_cast<int>(container));
        ASSERT_GE(index, 0);
        container_->setCurrentIndex(index);
    }

    void SelectVideo(ConversionVideoCodec codec) {
        const int index = video_->findData(static_cast<int>(codec));
        ASSERT_GE(index, 0) << "当前容器的视频下拉里没有这一项";
        video_->setCurrentIndex(index);
    }

    bool VideoListContains(ConversionVideoCodec codec) const {
        for (int i = 0; i < video_->count(); ++i) {
            if (video_->itemData(i).toInt() == static_cast<int>(codec)) {
                return true;
            }
        }
        return false;
    }

    QString CommandText() const {
        return command_->toPlainText();
    }

    QTemporaryDir dir_;
    ConversionPanel* panel_ = nullptr;
    QComboBox* container_ = nullptr;
    QComboBox* video_ = nullptr;
    QComboBox* audio_ = nullptr;
    QComboBox* scale_ = nullptr;
    QComboBox* fps_ = nullptr;
    QComboBox* quality_ = nullptr;
    QComboBox* rate_mode_ = nullptr;
    QSpinBox* video_bitrate_ = nullptr;
    QSpinBox* crf_ = nullptr;
    QComboBox* preset_ = nullptr;
    QComboBox* pixel_ = nullptr;
    QComboBox* loop_ = nullptr;
    QLineEdit* extra_ = nullptr;
    QLineEdit* input_ = nullptr;
    QLineEdit* output_ = nullptr;
    QPlainTextEdit* command_ = nullptr;
    QPushButton* start_ = nullptr;
};

TEST_F(ConversionPanelTest, DefaultsToMp4AutoAndBlocksStartWithoutInput) {
    EXPECT_EQ(container_->currentData().toInt(), static_cast<int>(ConversionContainer::Mp4));
    EXPECT_EQ(video_->currentData().toInt(), static_cast<int>(ConversionVideoCodec::Auto));
    EXPECT_EQ(audio_->currentData().toInt(), static_cast<int>(ConversionAudioCodec::Auto));
    // 没输入就没有可执行的命令，开始按钮必须是灰的（否则点下去只会看到错误日志）。
    EXPECT_FALSE(start_->isEnabled());
    EXPECT_TRUE(CommandText().contains(QStringLiteral("输入")));
}

TEST_F(ConversionPanelTest, WebmDropsH264FromCodecList) {
    SelectContainer(ConversionContainer::Webm);

    // WebM 的规范不收 H.264：这一项不该出现，否则用户会选中它然后被 core 拒掉。
    EXPECT_FALSE(VideoListContains(ConversionVideoCodec::H264));
    EXPECT_TRUE(VideoListContains(ConversionVideoCodec::Vp9));
    EXPECT_TRUE(VideoListContains(ConversionVideoCodec::Av1));
    EXPECT_TRUE(VideoListContains(ConversionVideoCodec::Copy));
}

TEST_F(ConversionPanelTest, AudioOnlyContainerLocksVideoAndKeepsItsAudio) {
    SelectContainer(ConversionContainer::Mp3);

    // 纯音频容器上视频没有可选项：下拉被禁用，只剩"自动 / 不要视频"。
    EXPECT_FALSE(video_->isEnabled());
    EXPECT_EQ(video_->count(), 2);
    EXPECT_EQ(audio_->currentData().toInt(), static_cast<int>(ConversionAudioCodec::Auto));

    input_->setText(dir_.filePath(QStringLiteral("a.mp4")));
    // 命令里必须是 -vn，不能出现 -c:v（mp3 容器装不下视频，ffmpeg 会整条失败）。
    EXPECT_TRUE(CommandText().contains(QStringLiteral("-vn"))) << CommandText().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("libmp3lame"))) << CommandText().toStdString();
    EXPECT_FALSE(CommandText().contains(QStringLiteral("-c:v")));
}

TEST_F(ConversionPanelTest, ContainerChangeRewritesOutputExtension) {
    const QString input = dir_.filePath(QStringLiteral("clip.mp4"));
    input_->setText(input);
    ASSERT_TRUE(output_->text().endsWith(QStringLiteral(".mp4"))) << output_->text().toStdString();

    SelectContainer(ConversionContainer::Mkv);
    // 输出名是程序自动生成的时候必须跟着容器换扩展名 —— 改了容器还存成 .mp4
    // 是最常见的"转完打不开"。
    EXPECT_TRUE(output_->text().endsWith(QStringLiteral(".mkv"))) << output_->text().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("libx264"))) << CommandText().toStdString();
}

TEST_F(ConversionPanelTest, OutputExtensionMismatchBlocksStart) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));
    ASSERT_TRUE(output_->text().endsWith(QStringLiteral(".mp4")));

    // 手改输出名、扩展名仍然是 .mp4 才合法；改成 .mkv 则命令预览直接报"扩展名不一致"。
    output_->setText(dir_.filePath(QStringLiteral("clip.mkv")));
    EXPECT_TRUE(CommandText().contains(QStringLiteral("扩展名"))) << CommandText().toStdString();
    EXPECT_FALSE(start_->isEnabled());

    output_->setText(dir_.filePath(QStringLiteral("clip.mp4")));
    EXPECT_FALSE(CommandText().contains(QStringLiteral("扩展名"))) << CommandText().toStdString();
}

TEST_F(ConversionPanelTest, CopyVideoDisablesScaleAndDropsFilter) {
    const QString input = dir_.filePath(QStringLiteral("clip.mp4"));
    input_->setText(input);
    ASSERT_TRUE(scale_->isEnabled());

    SelectVideo(ConversionVideoCodec::Copy);
    // -vf 与 -c:v copy 互斥，两者同时存在 ffmpeg 会直接拒绝整条命令。
    EXPECT_FALSE(scale_->isEnabled());
    EXPECT_FALSE(CommandText().contains(QStringLiteral("-vf"))) << CommandText().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("copy"))) << CommandText().toStdString();
    // 缩放下拉被锁住时，值也必须回到"保持原样"，否则界面显示与实际命令不一致。
    EXPECT_EQ(scale_->currentData().toInt(), static_cast<int>(ConversionScale::Keep));
}

TEST_F(ConversionPanelTest, BitrateModeSwitchesControlAndCommand) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));
    ASSERT_TRUE(quality_->isEnabled());
    ASSERT_FALSE(video_bitrate_->isEnabled());

    const int index = rate_mode_->findData(static_cast<int>(ConversionVideoRateMode::Bitrate));
    ASSERT_GE(index, 0);
    rate_mode_->setCurrentIndex(index);

    // 按码率模式下 CRF 不再是旋钮，命令里也必须只有 -b:v。
    EXPECT_FALSE(quality_->isEnabled());
    EXPECT_TRUE(video_bitrate_->isEnabled());
    EXPECT_TRUE(CommandText().contains(QStringLiteral("-b:v"))) << CommandText().toStdString();
    EXPECT_FALSE(CommandText().contains(QStringLiteral("-crf"))) << CommandText().toStdString();
}

TEST_F(ConversionPanelTest, PresetControlCoversSoftwareVideoEncoders) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));

    SelectContainer(ConversionContainer::Webm); // Auto → VP9
    // VP9 / AV1 不认 -preset，但同样有"速度 / 压缩率"取舍：换算成 -cpu-used。
    EXPECT_TRUE(preset_->isEnabled());
    EXPECT_FALSE(CommandText().contains(QStringLiteral("-preset"))) << CommandText().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("-cpu-used"))) << CommandText().toStdString();

    SelectContainer(ConversionContainer::Mkv);
    SelectVideo(ConversionVideoCodec::H264);
    EXPECT_TRUE(preset_->isEnabled());
    EXPECT_TRUE(CommandText().contains(QStringLiteral("-preset"))) << CommandText().toStdString();
    EXPECT_FALSE(CommandText().contains(QStringLiteral("-cpu-used"))) << CommandText().toStdString();

    SelectContainer(ConversionContainer::Avi);
    SelectVideo(ConversionVideoCodec::Mpeg4);
    EXPECT_FALSE(preset_->isEnabled());
}

TEST_F(ConversionPanelTest, TenBitIsAppliedForH264AndRefusedForMpeg4) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));
    const int ten = pixel_->findData(static_cast<int>(ConversionPixelFormat::Yuv420p10));
    ASSERT_GE(ten, 0);

    // 默认 MP4 + Auto(H.264)：10-bit 可用，命令里是 yuv420p10le。
    pixel_->setCurrentIndex(ten);
    EXPECT_EQ(pixel_->currentData().toInt(), static_cast<int>(ConversionPixelFormat::Yuv420p10));
    EXPECT_TRUE(CommandText().contains(QStringLiteral("yuv420p10le"))) << CommandText().toStdString();

    // AVI + MPEG-4 只有 8-bit：选了也要被页面拉回 8-bit，不能生成必然失败的命令。
    SelectContainer(ConversionContainer::Avi);
    SelectVideo(ConversionVideoCodec::Mpeg4);
    pixel_->setCurrentIndex(ten);
    EXPECT_EQ(pixel_->currentData().toInt(), static_cast<int>(ConversionPixelFormat::Yuv420p));
    EXPECT_FALSE(CommandText().contains(QStringLiteral("yuv420p10le"))) << CommandText().toStdString();
}

TEST_F(ConversionPanelTest, ExtraArgumentsArePreviewedAndDangerousOnesAreRejected) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));

    extra_->setText(QStringLiteral("-metadata title=\"demo\""));
    EXPECT_TRUE(CommandText().contains(QStringLiteral("title=demo"))) << CommandText().toStdString();

    // -y 会绕过页面的覆盖确认：预览里必须直接显示原因，而不是拼出一条能跑的命令。
    extra_->setText(QStringLiteral("-y"));
    EXPECT_TRUE(CommandText().contains(QStringLiteral("覆盖策略"))) << CommandText().toStdString();
    EXPECT_FALSE(start_->isEnabled());

    extra_->setText(QStringLiteral("not-an-option"));
    EXPECT_TRUE(CommandText().contains(QStringLiteral("以 - 开头"))) << CommandText().toStdString();
}

TEST_F(ConversionPanelTest, InputWithGeneratedOutputAllowsStart) {
    // 输入存在与否不影响命令拼装；start 的可用性还取决于本机有没有 ffmpeg，
    // 所以这里只断言命令已经成形（有输入路径、有输出路径、有编码器）。
    const QString input = dir_.filePath(QStringLiteral("clip.mp4"));
    input_->setText(input);

    EXPECT_FALSE(output_->text().isEmpty());
    EXPECT_TRUE(CommandText().contains(input)) << CommandText().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("-i"))) << CommandText().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("+faststart"))) << CommandText().toStdString();
}

TEST_F(ConversionPanelTest, CustomCrfRangeFollowsVideoCodec) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));
    quality_->setCurrentIndex(quality_->findData(static_cast<int>(ConversionQuality::Custom)));

    // MP4 + Auto → H.264：CRF 0~51。
    EXPECT_EQ(crf_->minimum(), 0);
    EXPECT_EQ(crf_->maximum(), 51);

    // WebM + Auto → VP9：CRF 上限 63。
    SelectContainer(ConversionContainer::Webm);
    EXPECT_EQ(crf_->maximum(), 63);

    // AVI + MPEG-4：走 -q:v 2~31，不是 CRF。
    SelectContainer(ConversionContainer::Avi);
    SelectVideo(ConversionVideoCodec::Mpeg4);
    EXPECT_EQ(crf_->minimum(), 2);
    EXPECT_EQ(crf_->maximum(), 31);
}

TEST_F(ConversionPanelTest, GifEnablesScaleFrameRateAndLoop) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));
    SelectContainer(ConversionContainer::Gif);

    // GIF 的视频编码是"不要视频"（调色板分支），但缩放 / 帧率 / 循环确实生效，不能置灰。
    EXPECT_TRUE(scale_->isEnabled());
    EXPECT_TRUE(fps_->isEnabled());
    EXPECT_TRUE(loop_->isEnabled());

    scale_->setCurrentIndex(scale_->findData(static_cast<int>(ConversionScale::P720)));
    fps_->setCurrentIndex(fps_->findData(static_cast<int>(ConversionFrameRate::Fps24)));
    loop_->setCurrentIndex(loop_->findData(static_cast<int>(ConversionGifLoop::Once)));

    EXPECT_TRUE(CommandText().contains(QStringLiteral("fps=24"))) << CommandText().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("scale=-2:720"))) << CommandText().toStdString();
    EXPECT_TRUE(CommandText().contains(QStringLiteral("-loop -1"))) << CommandText().toStdString();

    // 换回普通容器后循环项必须置灰：它只对 GIF 有意义。
    SelectContainer(ConversionContainer::Mp4);
    EXPECT_FALSE(loop_->isEnabled());
}

TEST_F(ConversionPanelTest, RememberTargetOptionsOnNextLaunch) {
    // 参数持久化：面板析构时把选择落盘，下次打开恢复的是同一套目标参数。
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));
    SelectContainer(ConversionContainer::Mkv);
    SelectVideo(ConversionVideoCodec::H265);
    quality_->setCurrentIndex(quality_->findData(static_cast<int>(ConversionQuality::Small)));

    const int saved_container = container_->currentData().toInt();
    const int saved_video = video_->currentData().toInt();
    const int saved_quality = quality_->currentData().toInt();

    delete panel_;
    panel_ = new ConversionPanel();

    container_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionContainer"));
    video_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionVideoCodec"));
    quality_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionQuality"));
    ASSERT_NE(container_, nullptr);
    ASSERT_NE(video_, nullptr);
    ASSERT_NE(quality_, nullptr);

    EXPECT_EQ(container_->currentData().toInt(), saved_container);
    EXPECT_EQ(video_->currentData().toInt(), saved_video);
    EXPECT_EQ(quality_->currentData().toInt(), saved_quality);
}

} // namespace
