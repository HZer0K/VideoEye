// VisualDefectPage（分析面板「画面质量」页）的接缝测试。
//
// 为什么值得单独测：这个页面是 12 个面板组件里唯一同时具备
//   ① UI 控件 <-> model::VisualDefectOptions 的**双向**往返、
//   ② 采样档位枚举 -> 下拉项 userData 的**一对一映射**、
//   ③ 播放期回调只在 _pending 上置脏、由面板定时器到点才重绘
// 三条链的组件。这三条里任何一条断了都不会崩，只会静默地用错参数或停在旧界面 ——
// 属于评审 P3「架构承诺的组件可单独测试只兑现了一部分」里最该补的一块。
//
// 具体盯三件事：
//   1. 选项往返：构造函数按入参铺控件（setValue / setChecked），之后控件才是唯一真源。
//      谁要是哪天把 setValue 改成写死默认值，「面板持有的选项」和「界面上显示的」就会
//      分家 —— 用户看到的档位和实际送进分析器的档位是两回事。
//   2. 档位映射：preset 下拉四项的 userData 必须与 VisualSamplingPreset 逐一对上，
//      并且切档位要把 sample_fps / analysis_width 的显式覆盖清掉（档位自带采样率，
//      不清的话两个来源会打架，谁生效取决于遍历顺序）。
//   3. 刷新节拍：AppendDefect / ApplyStats 只置脏，FlushPending 才重绘；
//      换文件 ResetAll 要连表格与脏标记一起清。
//
// 用 offscreen 平台跑，不需要显示器；只构造控件、不渲染，所以直接编这三个页面的源。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTableWidget>

#include <vector>

#include "core/domain/model/QualityMetric.h"
#include "core/domain/model/VisualDefect.h"
#include "core/domain/model/VisualDefectOptions.h"
#include "ui/analysis_panel/VisualDefectPage.h"

using videoeye::model::ActivePictureArea;
using videoeye::model::VisualDefect;
using videoeye::model::VisualDefectOptions;
using videoeye::model::VisualDefectSeverity;
using videoeye::model::VisualDefectType;
using videoeye::model::VisualSamplingPreset;
using videoeye::ui::VisualDefectPage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_visual_defect_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 「选项被改」的接收器。OptionsChanged 带一整份 options，所以连发几次也能逐条比对。
struct OptionsRecorder {
    std::vector<VisualDefectOptions> emitted;

    void Push(const VisualDefectOptions& options) { emitted.push_back(options); }
};

struct BoolRecorder {
    std::vector<bool> calls;

    void Push(bool enabled) { calls.push_back(enabled); }
};

struct SeekRecorder {
    std::vector<double> calls;

    void Push(double seconds) { calls.push_back(seconds); }
};

QComboBox* PresetCombo(VisualDefectPage& page) {
    return page.findChild<QComboBox*>(QStringLiteral("VisualDefectPreset"));
}

QDoubleSpinBox* BlurSpin(VisualDefectPage& page) {
    return page.findChild<QDoubleSpinBox*>(QStringLiteral("VisualDefectBlurThreshold"));
}

QDoubleSpinBox* FreezeSpin(VisualDefectPage& page) {
    return page.findChild<QDoubleSpinBox*>(QStringLiteral("VisualDefectFreezeThreshold"));
}

QCheckBox* RgbCheck(VisualDefectPage& page) {
    return page.findChild<QCheckBox*>(QStringLiteral("VisualDefectCaptureRgb"));
}

QCheckBox* FeatureToggle(VisualDefectPage& page) {
    return page.findChild<QCheckBox*>(QStringLiteral("VisualDefectEnable"));
}

VisualDefect MakeDefect(int id, double start, double end) {
    VisualDefect d;
    d.id = id;
    d.type = VisualDefectType::Blur;
    d.severity = VisualDefectSeverity::Warning;
    d.start_seconds = start;
    d.end_seconds = end;
    d.start_frame = 0;
    d.end_frame = 0;
    d.description = "模糊";
    return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 构造：入参里的非档位字段应当原样留下
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, ConstructorKeepsIncomingFieldValues) {
    EnsureApp();

    VisualDefectOptions opts;
    opts.preset = VisualSamplingPreset::Fine;
    opts.blur_threshold = 3.5;
    opts.freeze_diff = 0.08;
    opts.capture_rgb = false;
    opts.detect_blur = false;
    opts.sample_fps = 25.0;
    opts.analysis_width = 320;

    VisualDefectPage page(true, opts);

    // 构造只按入参铺控件，不反过来覆写 options（档位除外，见下一个用例）
    EXPECT_DOUBLE_EQ(3.5, page.options().blur_threshold);
    EXPECT_DOUBLE_EQ(0.08, page.options().freeze_diff);
    EXPECT_FALSE(page.options().capture_rgb);
    EXPECT_FALSE(page.options().detect_blur);
    EXPECT_DOUBLE_EQ(25.0, page.options().sample_fps);
    EXPECT_EQ(320, page.options().analysis_width);
}

// ---------------------------------------------------------------------------
// 2. 构造：控件初值来自入参（不是写死的默认值）
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, ControlsAreInitializedFromIncomingOptions) {
    EnsureApp();

    VisualDefectOptions opts;
    opts.blur_threshold = 3.5;
    opts.freeze_diff = 0.08;
    opts.capture_rgb = false;

    VisualDefectPage page(true, opts);
    QComboBox* combo = PresetCombo(page);
    QDoubleSpinBox* blur = BlurSpin(page);
    QDoubleSpinBox* freeze = FreezeSpin(page);
    QCheckBox* rgb = RgbCheck(page);

    ASSERT_TRUE(combo != nullptr);
    ASSERT_TRUE(blur != nullptr);
    ASSERT_TRUE(freeze != nullptr);
    ASSERT_TRUE(rgb != nullptr);

    EXPECT_DOUBLE_EQ(3.5, blur->value());
    EXPECT_DOUBLE_EQ(0.08, freeze->value());
    EXPECT_FALSE(rgb->isChecked());
    // 档位控件停在组合框的默认项（索引 1）而不是跟着 options_ 走 —— 这条单独在下一个
    // 用例里讲，那是本文件唯一"界面与生效值会分家"的地方
    EXPECT_EQ(1, combo->currentIndex());
}

// ---------------------------------------------------------------------------
// 3. 档位只铺控件、不回写 options：界面显示与生效值分家（先把实测行为钉住）
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, IncomingPresetIsKeptButComboStaysOnItsDefaultEntry) {
    EnsureApp();

    VisualDefectOptions opts;
    opts.preset = VisualSamplingPreset::OfflineFull;
    OptionsRecorder rec;

    VisualDefectPage page(true, opts);
    QObject::connect(&page, &VisualDefectPage::OptionsChanged,
                     [&rec](const VisualDefectOptions& options) { rec.Push(options); });

    QComboBox* combo = PresetCombo(page);
    ASSERT_TRUE(combo != nullptr);

    // 实测两件事，都是**反直觉**的：
    //   1. options_ 里保留调用方传进来的档位 —— 构造不回写控件状态；
    //   2. 下拉却停在默认项（索引 1 = "标准 (2 帧/秒)"），因为 setCurrentIndex(1) 那行
    //      写在 connect() 之前：既没触发 OnOptionChanged，也没人把 options_ 同步成控件。
    // 结果就是「界面显示标准」与「实际送进分析器的是离线全帧」分家。
    // 面板目前只建一次页面、传的都是自己持有的那一份（默认也是标准），所以感知不到；
    // 一旦将来页面需要重建（切文件 / 恢复上次配置），或者有人传了非默认档位，
    // 用户看到的档位就会是错的。这里先把现状钉住 —— 修法在 docs/ARCHITECTURE.md §5.4。
    EXPECT_EQ(VisualSamplingPreset::OfflineFull, page.options().preset);
    EXPECT_EQ(1, combo->currentIndex());
    // 构造期不 emit（setCurrentIndex 排在 connect 前面），"改控件"的用例都以空基线起步
    EXPECT_TRUE(rec.emitted.empty());
}

// ---------------------------------------------------------------------------
// 4. 档位映射：下拉四项必须逐项对上枚举
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, PresetComboMapsEveryEntryToItsEnumValue) {
    EnsureApp();

    VisualDefectPage page(true, VisualDefectOptions());
    OptionsRecorder rec;
    QObject::connect(&page, &VisualDefectPage::OptionsChanged,
                     [&rec](const VisualDefectOptions& options) { rec.Push(options); });

    QComboBox* combo = PresetCombo(page);
    ASSERT_TRUE(combo != nullptr);
    ASSERT_EQ(4, combo->count());

    const VisualSamplingPreset expected[4] = {
        VisualSamplingPreset::Fast,        // 索引 0: 快速 (1 帧/秒)
        VisualSamplingPreset::Standard,    // 索引 1: 标准 (2 帧/秒)
        VisualSamplingPreset::Fine,        // 索引 2: 精细 (5 帧/秒)
        VisualSamplingPreset::OfflineFull, // 索引 3: 离线全帧 (逐帧)
    };

    for (int i = 0; i < combo->count(); ++i) {
        // userData 与枚举必须对上 —— 错位的话界面显示"精细"、分析器拿到的是 OfflineFull
        EXPECT_EQ(static_cast<int>(expected[i]), combo->itemData(i).toInt()) << "item " << i;

        combo->setCurrentIndex(i);
        ASSERT_FALSE(rec.emitted.empty()) << "combo index " << i;
        EXPECT_EQ(expected[i], rec.emitted.back().preset) << "combo index " << i;
    }
}

// ---------------------------------------------------------------------------
// 5. 切档位要清掉显式采样覆盖
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, SwitchingPresetClearsExplicitSampleOverrides) {
    EnsureApp();

    VisualDefectOptions opts;
    opts.sample_fps = 25.0;
    opts.analysis_width = 320;

    VisualDefectPage page(true, opts);
    QComboBox* combo = PresetCombo(page);
    ASSERT_TRUE(combo != nullptr);

    // 构造不清覆盖（只在"切档位"这条路径上清）
    EXPECT_DOUBLE_EQ(25.0, page.options().sample_fps);
    EXPECT_EQ(320, page.options().analysis_width);

    combo->setCurrentIndex(3);
    ASSERT_EQ(3, combo->currentIndex());
    EXPECT_EQ(VisualSamplingPreset::OfflineFull, page.options().preset);
    EXPECT_DOUBLE_EQ(0.0, page.options().sample_fps);
    EXPECT_EQ(0, page.options().analysis_width);
}

// ---------------------------------------------------------------------------
// 6. 阈值 / 缩略图开关：<-> options 往返
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, ThresholdAndThumbOptionsRoundTrip) {
    EnsureApp();

    VisualDefectOptions opts;
    opts.blur_threshold = 3.5;
    opts.freeze_diff = 0.08;
    opts.capture_rgb = false;

    VisualDefectPage page(true, opts);
    OptionsRecorder rec;
    QObject::connect(&page, &VisualDefectPage::OptionsChanged,
                     [&rec](const VisualDefectOptions& options) { rec.Push(options); });

    QDoubleSpinBox* blur = BlurSpin(page);
    QDoubleSpinBox* freeze = FreezeSpin(page);
    QCheckBox* rgb = RgbCheck(page);
    ASSERT_TRUE(blur != nullptr);
    ASSERT_TRUE(freeze != nullptr);
    ASSERT_TRUE(rgb != nullptr);

    const std::size_t before = rec.emitted.size();
    blur->setValue(7.25);
    EXPECT_DOUBLE_EQ(7.25, page.options().blur_threshold);
    EXPECT_EQ(before + 1u, rec.emitted.size());

    freeze->setValue(0.15);
    EXPECT_DOUBLE_EQ(0.15, page.options().freeze_diff);
    EXPECT_EQ(before + 2u, rec.emitted.size());

    rgb->setChecked(true);
    EXPECT_TRUE(page.options().capture_rgb);
    EXPECT_EQ(before + 3u, rec.emitted.size());
}

// ---------------------------------------------------------------------------
// 7. 页内「启用检测」开关：初值取自构造入参
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, FeatureToggleFollowsConstructorFlag) {
    EnsureApp();

    BoolRecorder rec;
    {
        VisualDefectPage page(false, VisualDefectOptions());
        QObject::connect(&page, &VisualDefectPage::FeatureToggled,
                     [&rec](bool enabled) { rec.Push(enabled); });
        QCheckBox* toggle = FeatureToggle(page);
        ASSERT_TRUE(toggle != nullptr);
        EXPECT_FALSE(toggle->isChecked());

        toggle->setChecked(true);
        ASSERT_EQ(1u, rec.calls.size());
        EXPECT_TRUE(rec.calls[0]);
    }
    {
        VisualDefectPage page(true, VisualDefectOptions());
        QCheckBox* toggle = FeatureToggle(page);
        ASSERT_TRUE(toggle != nullptr);
        EXPECT_TRUE(toggle->isChecked());
    }
}

// ---------------------------------------------------------------------------
// 8. 播放期回调只置脏，刷新到点才重绘；换文件 ResetAll 全清
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, FlushPendingFlushesOnceAndResetAllClears) {
    EnsureApp();

    VisualDefectPage page(true, VisualDefectOptions());
    ActivePictureArea area;
    area.valid = true;
    area.width = 1920;
    area.height = 1080;

    page.AppendDefect(MakeDefect(1, 1.0, 2.0));
    page.AppendDefect(MakeDefect(2, 3.0, 4.5));
    page.ApplyStats(120, 3, area);

    // 只入队 + 置脏，还没重绘
    EXPECT_TRUE(page.HasPending());

    page.FlushPending();
    EXPECT_FALSE(page.HasPending());

    QTableWidget* table = page.findChild<QTableWidget*>();
    ASSERT_TRUE(table != nullptr);
    EXPECT_EQ(2, table->rowCount());

    // 再刷一次不该翻倍，也不该报错
    page.FlushPending();
    EXPECT_EQ(2, table->rowCount());

    page.ResetAll();
    EXPECT_FALSE(page.HasPending());
    EXPECT_EQ(0, table->rowCount());
}

// ---------------------------------------------------------------------------
// 9. 点缺陷行 -> 请求 seek 到该缺陷起点
// ---------------------------------------------------------------------------

TEST(VisualDefectPageTests, ClickingDefectRowAsksSeekToItsStartSecond) {
    EnsureApp();

    VisualDefectPage page(true, VisualDefectOptions());
    SeekRecorder rec;
    QObject::connect(&page, &VisualDefectPage::SeekRequested,
                     [&rec](double seconds) { rec.Push(seconds); });

    page.AppendDefect(MakeDefect(7, 12.5, 14.0));
    page.FlushPending();

    QTableWidget* table = page.findChild<QTableWidget*>();
    ASSERT_TRUE(table != nullptr);
    ASSERT_EQ(1, table->rowCount());
    EXPECT_TRUE(table->item(0, 0) != nullptr);

    table->cellClicked(0, 0);
    ASSERT_EQ(1u, rec.calls.size());
    EXPECT_DOUBLE_EQ(12.5, rec.calls[0]);

    // 行号越界不该发信号
    table->cellClicked(1, 0);
    EXPECT_EQ(1u, rec.calls.size());
}
