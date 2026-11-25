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
#include <QSpinBox>
#include <QStringList>
#include <QTemporaryDir>

#include "core/ffmpeg/FormatConversionPlan.h"
#include "ui/conversion_panel/ConversionPanel.h"

namespace {

using videoeye::ffmpeg::ConversionAudioCodec;
using videoeye::ffmpeg::ConversionContainer;
using videoeye::ffmpeg::ConversionPixelFormat;
using videoeye::ffmpeg::ConversionScale;
using videoeye::ffmpeg::ConversionVideoCodec;
using videoeye::ffmpeg::ConversionVideoRateMode;
using videoeye::ui::ConversionPanel;

QApplication* EnsureApp() {
    if (QCoreApplication::instance() != nullptr) {
        return qobject_cast<QApplication*>(QCoreApplication::instance());
    }
    static int argc = 1;
    static char arg0[] = "test_conversion_panel";
    static char* argv[] = {arg0, nullptr};
    static QApplication* app = new QApplication(argc, argv);
    return app;
}

class ConversionPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(EnsureApp(), nullptr);
        ASSERT_TRUE(dir_.isValid());
        panel_ = new ConversionPanel();
        container_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionContainer"));
        video_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionVideoCodec"));
        audio_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionAudioCodec"));
        scale_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionScale"));
        quality_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionQuality"));
        rate_mode_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionRateMode"));
        video_bitrate_ = panel_->findChild<QSpinBox*>(QStringLiteral("ConversionVideoBitrate"));
        preset_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionPreset"));
        pixel_ = panel_->findChild<QComboBox*>(QStringLiteral("ConversionPixelFormat"));
        extra_ = panel_->findChild<QLineEdit*>(QStringLiteral("ConversionExtraArgs"));
        input_ = panel_->findChild<QLineEdit*>(QStringLiteral("ConversionInputPath"));
        output_ = panel_->findChild<QLineEdit*>(QStringLiteral("ConversionOutputPath"));
        command_ = panel_->findChild<QPlainTextEdit*>(QStringLiteral("ConversionCommand"));
        start_ = panel_->findChild<QPushButton*>(QStringLiteral("ConversionStart"));
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(video_, nullptr);
        ASSERT_NE(audio_, nullptr);
        ASSERT_NE(scale_, nullptr);
        ASSERT_NE(quality_, nullptr);
        ASSERT_NE(rate_mode_, nullptr);
        ASSERT_NE(video_bitrate_, nullptr);
        ASSERT_NE(preset_, nullptr);
        ASSERT_NE(pixel_, nullptr);
        ASSERT_NE(extra_, nullptr);
        ASSERT_NE(input_, nullptr);
        ASSERT_NE(output_, nullptr);
        ASSERT_NE(command_, nullptr);
        ASSERT_NE(start_, nullptr);
    }

    void TearDown() override {
        delete panel_;
        panel_ = nullptr;
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
    QComboBox* quality_ = nullptr;
    QComboBox* rate_mode_ = nullptr;
    QSpinBox* video_bitrate_ = nullptr;
    QComboBox* preset_ = nullptr;
    QComboBox* pixel_ = nullptr;
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

TEST_F(ConversionPanelTest, PresetIsOnlyEnabledForH264AndH265) {
    input_->setText(dir_.filePath(QStringLiteral("clip.mp4")));

    SelectContainer(ConversionContainer::Webm); // Auto → VP9
    EXPECT_FALSE(preset_->isEnabled());
    EXPECT_FALSE(CommandText().contains(QStringLiteral("-preset"))) << CommandText().toStdString();

    SelectContainer(ConversionContainer::Mkv);
    SelectVideo(ConversionVideoCodec::H264);
    EXPECT_TRUE(preset_->isEnabled());
    EXPECT_TRUE(CommandText().contains(QStringLiteral("-preset"))) << CommandText().toStdString();
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

} // namespace
