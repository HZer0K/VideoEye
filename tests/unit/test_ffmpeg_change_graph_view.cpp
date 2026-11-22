// 「转换图谱」画布的几何 / 命中 / 导出测试。
//
// 自绘控件最容易出的不是崩溃，而是这三件"看起来正常"的事：
//   1) 画的位置和点中的位置不是一回事（点输出节点跳出来的是滤镜的说明）；
//   2) 导出的 SVG 和屏幕上不是同一份（导出空图 / 少一列）；
//   3) 没数据时画一张空图，用户以为"真的什么都没变"。
// 所以用例全部围绕 Visual 列表断言，不去比像素。

#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>

#include "core/ffmpeg/FfmpegChangeGraph.h"
#include "ui/ffmpeg_panel/FfmpegChangeGraphView.h"

namespace {

using videoeye::ffmpeg::ChangeGraph;
using videoeye::ffmpeg::ChangeState;
using videoeye::ffmpeg::FieldChange;
using videoeye::ffmpeg::GraphRow;
using videoeye::ffmpeg::MediaStreamKind;
using videoeye::ffmpeg::ProcessKind;
using videoeye::ui::FfmpegChangeGraphCanvas;
using videoeye::ui::FfmpegChangeGraphView;
using namespace videoeye::ui::change_graph_view;

// QWidget 必须在 QApplication 之后构造（gtest_main 只给了一个空 main）。
QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_ffmpeg_change_graph_view";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

FieldChange MakeField(const QString& key, const QString& label, const QString& before, const QString& after,
                      ChangeState state) {
    FieldChange change;
    change.key = key;
    change.label = label;
    change.before = before;
    change.after = after;
    change.state = state;
    return change;
}

ChangeGraph SampleGraph() {
    ChangeGraph graph;
    graph.ok = true;
    graph.target = QStringLiteral("out.mp4");
    graph.output_measured = true;
    graph.container_before = QStringLiteral("mov");
    graph.container_after = QStringLiteral("mov");

    GraphRow video;
    video.kind = MediaStreamKind::Video;
    video.input_index = 0;
    video.output_index = 0;
    video.input_label = QStringLiteral("0:0");
    video.output_label = QStringLiteral("0:0");
    video.input_summary = QStringLiteral("h264 High · 1920×1080 · 29.97 fps");
    video.output_summary = QStringLiteral("hevc Main · 1280×720 · 29.97 fps");
    video.filter_labels << QStringLiteral("scale 1280:-2");
    video.encoder_label = QStringLiteral("libx265");
    video.encoder_kind = ProcessKind::Encoder;
    video.encoder_detail = QStringLiteral("重编码 · crf 28");
    video.state = ChangeState::Changed;
    video.changes.push_back(MakeField(QStringLiteral("codec"), QStringLiteral("编码"), QStringLiteral("h264 High"),
                                      QStringLiteral("hevc Main"), ChangeState::Changed));
    video.changes.push_back(MakeField(QStringLiteral("size"), QStringLiteral("分辨率"), QStringLiteral("1920×1080"),
                                      QStringLiteral("1280×720"), ChangeState::Changed));

    GraphRow audio;
    audio.kind = MediaStreamKind::Audio;
    audio.input_index = 1;
    audio.output_index = 1;
    audio.input_label = QStringLiteral("0:1");
    audio.output_label = QStringLiteral("0:1");
    audio.input_summary = QStringLiteral("aac LC · 48000 Hz · stereo");
    audio.output_summary = QStringLiteral("aac LC · 48000 Hz · stereo");
    audio.encoder_label = QStringLiteral("copy");
    audio.encoder_kind = ProcessKind::Copy;
    audio.state = ChangeState::Unchanged;
    audio.changes.push_back(MakeField(QStringLiteral("codec"), QStringLiteral("编码"), QStringLiteral("aac LC"),
                                      QStringLiteral("aac LC"), ChangeState::Unchanged));

    graph.rows.push_back(video);
    graph.rows.push_back(audio);
    graph.container_changes.push_back(MakeField(QStringLiteral("duration"), QStringLiteral("时长"),
                                                QStringLiteral("1:23.4"), QStringLiteral("1:23.4"),
                                                ChangeState::Unchanged));
    return graph;
}

std::vector<int> IndicesOfKind(const std::vector<Visual>& visuals, VisualKind kind) {
    std::vector<int> out;
    for (size_t i = 0; i < visuals.size(); ++i) {
        if (visuals.at(i).kind == kind) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

TEST(FfmpegChangeGraphViewTests, BuildsFourColumnsAndOneRowSet) {
    const ChangeGraph graph = SampleGraph();
    const std::vector<Visual> visuals = BuildVisuals(graph, 900, DarkPalette());
    ASSERT_FALSE(visuals.empty());

    EXPECT_EQ(IndicesOfKind(visuals, VisualKind::Header).size(), 4);
    EXPECT_EQ(IndicesOfKind(visuals, VisualKind::InputNode).size(), 2);
    EXPECT_EQ(IndicesOfKind(visuals, VisualKind::FilterNode).size(), 1); // 只有视频行有滤镜
    EXPECT_EQ(IndicesOfKind(visuals, VisualKind::EncoderNode).size(), 2);
    EXPECT_EQ(IndicesOfKind(visuals, VisualKind::OutputNode).size(), 2);
    EXPECT_EQ(IndicesOfKind(visuals, VisualKind::ChangeBadge).size(), 3); // 2 + 1
    EXPECT_EQ(IndicesOfKind(visuals, VisualKind::ContainerBadge).size(), 1);
}

TEST(FfmpegChangeGraphViewTests, HitTestReturnsTheNodeThatWasDrawn) {
    const ChangeGraph graph = SampleGraph();
    const std::vector<Visual> visuals = BuildVisuals(graph, 900, DarkPalette());

    // 每一个节点 / 标签的中心点都必须命中它自己 —— 这就是"画的和点的是不是一回事"。
    for (size_t i = 0; i < visuals.size(); ++i) {
        const Visual& visual = visuals.at(i);
        if (visual.kind == VisualKind::Edge || visual.kind == VisualKind::Header) {
            continue;
        }
        const int hit = VisualAt(visuals, visual.rect.center());
        EXPECT_EQ(hit, static_cast<int>(i)) << "kind=" << static_cast<int>(visual.kind) << " row=" << visual.row;
    }
}

TEST(FfmpegChangeGraphViewTests, ClickingABadgeDescribesThatField) {
    const ChangeGraph graph = SampleGraph();
    const std::vector<Visual> visuals = BuildVisuals(graph, 900, DarkPalette());
    const std::vector<int> badges = IndicesOfKind(visuals, VisualKind::ChangeBadge);
    ASSERT_EQ(badges.size(), 3u);

    const QString codec_text = DescribeVisual(graph, visuals.at(static_cast<size_t>(badges.at(0))));
    EXPECT_TRUE(codec_text.contains(QStringLiteral("编码")));
    EXPECT_TRUE(codec_text.contains(QStringLiteral("h264 High")));

    const QString size_text = DescribeVisual(graph, visuals.at(static_cast<size_t>(badges.at(1))));
    EXPECT_TRUE(size_text.contains(QStringLiteral("分辨率")));
    EXPECT_TRUE(size_text.contains(QStringLiteral("1280×720")));
}

TEST(FfmpegChangeGraphViewTests, DropAndUnprobedRowsReadDifferently) {
    ChangeGraph graph = SampleGraph();
    graph.rows.at(0).output_index = -1;
    graph.rows.at(0).output_label.clear();
    graph.rows.at(0).output_summary.clear();
    graph.rows.at(0).state = ChangeState::Removed;
    graph.rows.at(0).encoder_kind = ProcessKind::Drop;
    graph.rows.at(0).encoder_label = QStringLiteral("丢弃");

    const std::vector<Visual> visuals = BuildVisuals(graph, 900, DarkPalette());
    bool found = false;
    for (const Visual& visual : visuals) {
        if (visual.kind == VisualKind::Placeholder && visual.row == 0 &&
            visual.title.contains(QStringLiteral("输出里没有"))) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "被丢弃的流不能画成一个空白输出节点";

    // 没探测到输出时，占位文案必须说"未探测"，而不是"没有这一路"。
    ChangeGraph unprobed = SampleGraph();
    unprobed.output_measured = false;
    unprobed.rows.at(0).output_index = -1;
    const std::vector<Visual> unprobed_visuals = BuildVisuals(unprobed, 900, DarkPalette());
    bool unprobed_hint = false;
    for (const Visual& visual : unprobed_visuals) {
        if (visual.kind == VisualKind::Placeholder && visual.title == QStringLiteral("未探测")) {
            unprobed_hint = true;
        }
    }
    EXPECT_TRUE(unprobed_hint);
}

TEST(FfmpegChangeGraphViewTests, FolderLongChangeListIntoOneBadge) {
    ChangeGraph graph = SampleGraph();
    GraphRow& video = graph.rows.at(0);
    video.changes.clear();
    for (int i = 0; i < 6; ++i) {
        video.changes.push_back(MakeField(QStringLiteral("k%1").arg(i), QStringLiteral("字段%1").arg(i),
                                          QStringLiteral("旧"), QStringLiteral("新"), ChangeState::Changed));
    }
    const std::vector<Visual> visuals = BuildVisuals(graph, 900, DarkPalette());
    // 上限是 3 行：2 个真变化 + 1 个"还有 N 项"
    EXPECT_EQ(VisibleBadgeCount(graph, 0), 3);
    const std::vector<int> badges = IndicesOfKind(visuals, VisualKind::ChangeBadge);
    ASSERT_GE(badges.size(), 3u);
    bool folded = false;
    for (const Visual& visual : visuals) {
        if (visual.kind == VisualKind::ChangeBadge && visual.change_index < 0) {
            folded = true;
            EXPECT_TRUE(visual.title.contains(QStringLiteral("还有")));
        }
    }
    EXPECT_TRUE(folded);
}

TEST(FfmpegChangeGraphViewTests, SvgExportContainsTheSameNodesAsTheScreen) {
    const ChangeGraph graph = SampleGraph();
    const std::vector<Visual> visuals = BuildVisuals(graph, 900, DarkPalette());
    const QByteArray svg = BuildSvg(visuals, QSize(900, 400), QStringLiteral("转换图谱"), DarkPalette());
    ASSERT_FALSE(svg.isEmpty());
    const QString text = QString::fromUtf8(svg);
    EXPECT_TRUE(text.contains(QStringLiteral("<svg")));
    EXPECT_TRUE(text.contains(QStringLiteral("scale 1280:-2")));
    EXPECT_TRUE(text.contains(QStringLiteral("libx265")));
    EXPECT_TRUE(text.contains(QStringLiteral("1920×1080")));

    // 空图不产文件 —— "导出成功但一片空白"比失败更糟
    EXPECT_TRUE(BuildSvg({}, QSize(900, 400), QStringLiteral("x"), DarkPalette()).isEmpty());
}

TEST(FfmpegChangeGraphViewTests, WidgetShowsSummaryAndDetailWithoutGraphicsStack) {
    ASSERT_NE(EnsureApp(), nullptr);
    FfmpegChangeGraphView view;
    view.resize(900, 420);

    EXPECT_FALSE(view.HasGraph());
    EXPECT_FALSE(view.EmptyHintText().isEmpty());
    EXPECT_TRUE(view.ExportSvg().isEmpty());

    view.SetGraphs({SampleGraph()});
    EXPECT_TRUE(view.HasGraph());
    EXPECT_TRUE(view.EmptyHintText().isEmpty());
    EXPECT_TRUE(view.SummaryText().contains(QStringLiteral("处变化")));
    EXPECT_GT(view.VisualCount(), 0);
    EXPECT_FALSE(view.ExportSvg().isEmpty());

    // 失败图谱（ok=false）要给出原因，而不是一张空画布
    videoeye::ffmpeg::ChangeGraph broken;
    broken.ok = false;
    broken.note = QStringLiteral("命令结构不完整（有参数归属不明），图谱可能失准");
    view.SetGraphs({broken});
    EXPECT_EQ(view.EmptyHintText(), broken.note);
    EXPECT_EQ(view.VisualCount(), 0);
}

TEST(FfmpegChangeGraphViewTests, MultiOutputSwitchesBetweenGraphs) {
    ASSERT_NE(EnsureApp(), nullptr);
    ChangeGraph second = SampleGraph();
    second.target = QStringLiteral("out2.mkv");
    FfmpegChangeGraphView view;
    view.resize(900, 420);
    view.SetGraphs({SampleGraph(), second});
    EXPECT_EQ(view.GraphCount(), 2);
    EXPECT_EQ(view.CurrentIndex(), 0);
    view.SetCurrentIndex(1);
    EXPECT_EQ(view.CurrentIndex(), 1);
}

TEST(FfmpegChangeGraphViewTests, ElideKeepsTextInsideItsColumn) {
    const QString long_text = QStringLiteral("分辨率 1920×1080 → 1280×720，附带一串很长的说明文字用来撑爆格子");
    const QString elided = ElideToWidth(long_text, 80);
    EXPECT_TRUE(elided.endsWith(QStringLiteral("…")));
    EXPECT_LT(elided.size(), long_text.size());
    EXPECT_EQ(ElideToWidth(QStringLiteral("短"), 200), QStringLiteral("短"));
}

TEST(FfmpegChangeGraphViewTests, BadgeNeverLetsTheStateWordOverrunTheText) {
    ChangeGraph graph = SampleGraph();
    // 容器级来一条长到必须截的（真实数据长这样：「流数 1 视频 / 1 音频 / 1 字幕 → …」）
    graph.container_changes.push_back(MakeField(QStringLiteral("stream_count"), QStringLiteral("流数"),
                                                QStringLiteral("1 视频 / 1 音频 / 1 字幕"),
                                                QStringLiteral("1 视频 / 1 音频 / 0 字幕"), ChangeState::Changed));

    const std::vector<Visual> visuals = BuildVisuals(graph, 640, DarkPalette());
    int checked = 0;
    for (const Visual& visual : visuals) {
        const bool is_badge = visual.kind == VisualKind::ChangeBadge || visual.kind == VisualKind::ContainerBadge;
        if (!is_badge || visual.badge.isEmpty()) {
            continue;
        }
        // 一条徽标里挤两段文字：左边「字段 a → b」，右边状态词。宽度不够只能截左边，
        // 不能让两段叠在一起 —— 早先按「正文宽 + 18」算，渲染出来是 "1:23.4未变"。
        EXPECT_LE(BadgeMinWidth(visual.title, visual.badge), visual.rect.width())
            << "title=" << visual.title.toStdString() << " state=" << visual.badge.toStdString();
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

TEST(FfmpegChangeGraphViewTests, CanvasSitsBelowTheHeaderAndSummary) {
    ASSERT_NE(EnsureApp(), nullptr);
    FfmpegChangeGraphView view;
    view.resize(900, 520);
    view.SetGraphs({SampleGraph()});
    view.show();
    QCoreApplication::processEvents();

    // 画布是独立子控件，不能自己铺满整个 View —— 那样列标题会被头部文字压住
    auto* canvas = view.findChild<FfmpegChangeGraphCanvas*>();
    ASSERT_NE(canvas, nullptr);
    int summary_bottom = -1;
    for (QLabel* label : view.findChildren<QLabel*>()) {
        if (label->text() == view.SummaryText()) {
            summary_bottom = label->geometry().bottom();
        }
    }
    ASSERT_GE(summary_bottom, 0);
    EXPECT_GT(canvas->geometry().top(), summary_bottom);
    EXPECT_GT(canvas->width(), 0);
    EXPECT_GE(canvas->minimumHeight(), kNodeHeight);
}

} // namespace
