// 滤镜串拆解（-vf / -af / -filter_complex）的解析用例。
//
// 这一层的价值全在"切对"：切错不会崩，只会把 `select=eq(pict_type,I)` 当成两个滤镜，
// 或者在 -filter_complex 里把标签错当成滤镜名，然后给用户看一堆编出来的解释。
// 因此用例覆盖三类：
//   1. 正常切分（单链 / 多链 / 多输入标签 / @实例名 / 无参滤镜）；
//   2. 已知失真源（上游把成对引号吃掉后，参数里的逗号）——要合并回一段并标记 ambiguous；
//   3. 硬伤（括号不配对）——必须 parse_ok=false，而不是"尽力拆一点"。
//
// 能力清单在每个用例里先 Clear()：HasFilter 在未探测时返回 true（不冤枉任何滤镜），
// 想让"不可用"路径变红就必须显式注入一份清单。

#include <gtest/gtest.h>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegFilterGraph.h"

using videoeye::ffmpeg::DescribeFilterGraph;
using videoeye::ffmpeg::FfmpegCapabilityCache;
using videoeye::ffmpeg::FfmpegFilterGraph;
using videoeye::ffmpeg::ParseFilterGraph;

namespace {

FfmpegFilterGraph ParseSimple(const QString& text) {
    FfmpegCapabilityCache::Instance().Clear();
    return ParseFilterGraph(text, false);
}

FfmpegFilterGraph ParseComplex(const QString& text) {
    FfmpegCapabilityCache::Instance().Clear();
    return ParseFilterGraph(text, true);
}

} // namespace

TEST(FfmpegFilterGraphTests, SingleChainSplitsEveryNode) {
    const auto graph = ParseSimple(QStringLiteral("scale=1280:-2,format=yuv420p"));

    ASSERT_TRUE(graph.parse_ok) << graph.error.toStdString();
    ASSERT_EQ(1u, graph.chains.size());
    const auto& nodes = graph.chains.front().nodes;
    ASSERT_EQ(2u, nodes.size());

    EXPECT_EQ(QStringLiteral("scale"), nodes.at(0).name);
    EXPECT_EQ(QStringLiteral("1280:-2"), nodes.at(0).arguments);
    EXPECT_TRUE(nodes.at(0).known);
    EXPECT_EQ(QStringLiteral("缩放"), nodes.at(0).title);

    EXPECT_EQ(QStringLiteral("format"), nodes.at(1).name);
    EXPECT_EQ(QStringLiteral("yuv420p"), nodes.at(1).arguments);
}

TEST(FfmpegFilterGraphTests, FilterWithoutArgumentsHasEmptyArguments) {
    const auto graph = ParseSimple(QStringLiteral("hflip,scale=1280:-2"));

    ASSERT_TRUE(graph.parse_ok);
    ASSERT_EQ(2u, graph.chains.front().nodes.size());
    EXPECT_EQ(QStringLiteral("hflip"), graph.chains.front().nodes.at(0).name);
    EXPECT_TRUE(graph.chains.front().nodes.at(0).arguments.isEmpty());
    EXPECT_FALSE(graph.chains.front().nodes.at(0).known); // 字典没收录，但不是错误
}

TEST(FfmpegFilterGraphTests, InstanceNameIsSeparatedFromFilterName) {
    const auto graph = ParseSimple(QStringLiteral("scale@big=640:360"));

    ASSERT_TRUE(graph.parse_ok);
    ASSERT_EQ(1u, graph.chains.front().nodes.size());
    const auto& node = graph.chains.front().nodes.front();
    EXPECT_EQ(QStringLiteral("scale"), node.name);
    EXPECT_EQ(QStringLiteral("big"), node.instance);
    EXPECT_EQ(QStringLiteral("640:360"), node.arguments);
    EXPECT_TRUE(node.known); // 查字典用 name，不带 @instance
}

TEST(FfmpegFilterGraphTests, ComplexGraphKeepsChainsAndLabelsApart) {
    const auto graph = ParseComplex(QStringLiteral("[0:v]scale=1280:-2[v];[0:a]volume=2[a]"));

    ASSERT_TRUE(graph.parse_ok) << graph.error.toStdString();
    ASSERT_EQ(2u, graph.chains.size());

    EXPECT_EQ(QStringList{QStringLiteral("0:v")}, graph.chains.at(0).input_labels);
    EXPECT_EQ(QStringList{QStringLiteral("v")}, graph.chains.at(0).output_labels);
    ASSERT_EQ(1u, graph.chains.at(0).nodes.size());
    EXPECT_EQ(QStringLiteral("scale"), graph.chains.at(0).nodes.front().name);

    EXPECT_EQ(QStringList{QStringLiteral("0:a")}, graph.chains.at(1).input_labels);
    EXPECT_EQ(QStringList{QStringLiteral("a")}, graph.chains.at(1).output_labels);
    EXPECT_EQ(QStringLiteral("volume"), graph.chains.at(1).nodes.front().name);
    EXPECT_EQ(QStringLiteral("2"), graph.chains.at(1).nodes.front().arguments);
}

TEST(FfmpegFilterGraphTests, MultipleInputLabelsAreAllKept) {
    const auto graph = ParseComplex(QStringLiteral("[0:v][1:v]overlay=W-w-10:10[out]"));

    ASSERT_TRUE(graph.parse_ok);
    ASSERT_EQ(1u, graph.chains.size());
    EXPECT_EQ((QStringList{QStringLiteral("0:v"), QStringLiteral("1:v")}), graph.chains.front().input_labels);
    EXPECT_EQ(QStringList{QStringLiteral("out")}, graph.chains.front().output_labels);
    EXPECT_EQ(QStringLiteral("overlay"), graph.chains.front().nodes.front().name);
}

TEST(FfmpegFilterGraphTests, CommaInsideArgumentIsMergedBackNotSplit) {
    // 上游把 select='eq(pict_type,I)' 的引号吃掉了，这个逗号看起来像滤镜分隔符。
    // 正确行为：认出来 `I)` 不是滤镜名，并回上一段，并标记 ambiguous。
    const auto graph = ParseSimple(QStringLiteral("select=eq(pict_type,I)"));

    ASSERT_TRUE(graph.parse_ok) << graph.error.toStdString();
    ASSERT_EQ(1u, graph.chains.front().nodes.size());
    EXPECT_EQ(QStringLiteral("select"), graph.chains.front().nodes.front().name);
    EXPECT_EQ(QStringLiteral("eq(pict_type,I)"), graph.chains.front().nodes.front().arguments);
    EXPECT_TRUE(graph.ambiguous);
}

TEST(FfmpegFilterGraphTests, EscapedCommaDoesNotSplit) {
    const auto graph = ParseSimple(QStringLiteral("drawtext=text=a\\,b"));

    ASSERT_TRUE(graph.parse_ok);
    ASSERT_EQ(1u, graph.chains.front().nodes.size());
    EXPECT_EQ(QStringLiteral("drawtext"), graph.chains.front().nodes.front().name);
    EXPECT_TRUE(graph.chains.front().nodes.front().arguments.contains(QStringLiteral("a\\,b")));
    EXPECT_FALSE(graph.ambiguous);
}

TEST(FfmpegFilterGraphTests, UnbalancedBracketIsAnErrorNotABestGuess) {
    const auto graph = ParseSimple(QStringLiteral("[0:vscale=1280:-2"));

    EXPECT_FALSE(graph.parse_ok);
    EXPECT_FALSE(graph.error.isEmpty());
}

TEST(FfmpegFilterGraphTests, SemicolonInVideoFilterChainIsNoted) {
    // 实测：-vf "scale=64:64;null" 报 Error initializing a simple filtergraph
    const auto graph = ParseSimple(QStringLiteral("scale=1;unsharp=2"));

    ASSERT_TRUE(graph.parse_ok);
    ASSERT_EQ(2u, graph.chains.size());
    ASSERT_FALSE(graph.notes.isEmpty());
    EXPECT_TRUE(graph.notes.first().contains(QStringLiteral("Error initializing a simple filtergraph")));
}

TEST(FfmpegFilterGraphTests, LabeledChainsInVideoFilterAreAccepted) {
    // 实测：-vf "[0:v]scale=64:64[a];[a]null[b]" 能正常跑 —— 有标签连接就不该提示
    const auto graph = ParseSimple(QStringLiteral("[0:v]scale=64:64[a];[a]null[b]"));

    ASSERT_TRUE(graph.parse_ok) << graph.error.toStdString();
    ASSERT_EQ(2u, graph.chains.size());
    EXPECT_TRUE(graph.notes.isEmpty());
}

TEST(FfmpegFilterGraphTests, UnavailableFilterIsReported) {
    FfmpegCapabilityCache::Instance().Clear();
    FfmpegCapabilityCache::Instance().SetFilters({QStringLiteral("scale")});

    const auto graph = ParseFilterGraph(QStringLiteral("scale=1280:-2,unsharp=5:5"), false);
    ASSERT_TRUE(graph.parse_ok);
    EXPECT_EQ(QStringList{QStringLiteral("unsharp")}, graph.UnavailableFilters());

    FfmpegCapabilityCache::Instance().Clear();
}

TEST(FfmpegFilterGraphTests, UnknownFilterIsNotAnUnavailableFilter) {
    // 字典没收录 ≠ 当前 ffmpeg 没有。没探测过能力清单时不能报「不可用」。
    FfmpegCapabilityCache::Instance().Clear();

    const auto graph = ParseFilterGraph(QStringLiteral("somebrandnewfilter=1"), false);
    ASSERT_TRUE(graph.parse_ok);
    EXPECT_TRUE(graph.UnavailableFilters().isEmpty());
}

TEST(FfmpegFilterGraphTests, EmptyTextGivesEmptyGraphAndEmptyDescription) {
    const auto graph = ParseSimple(QString());

    EXPECT_TRUE(graph.parse_ok);
    EXPECT_TRUE(graph.chains.empty());
    EXPECT_TRUE(DescribeFilterGraph(graph).isEmpty());
}

TEST(FfmpegFilterGraphTests, DescribeSingleChainListsNodesInOrder) {
    const auto graph = ParseSimple(QStringLiteral("scale=1280:-2,unsharp=5:5"));
    const QString text = DescribeFilterGraph(graph);

    EXPECT_TRUE(text.contains(QStringLiteral("2 个滤镜"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("scale 1280:-2（缩放）"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("unsharp 5:5（字典里没有这个滤镜）"))) << text.toStdString();
    // 顺序不能反：滤镜链是有执行顺序的
    EXPECT_LT(text.indexOf(QStringLiteral("scale")), text.indexOf(QStringLiteral("unsharp")));
}

TEST(FfmpegFilterGraphTests, DescribeComplexGraphShowsLabelsAndChains) {
    const auto graph = ParseComplex(QStringLiteral("[0:v]scale=1280:-2[v];[0:a]volume=2[a]"));
    const QString text = DescribeFilterGraph(graph);

    EXPECT_TRUE(text.contains(QStringLiteral("2 条链"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("链 1：[0:v] → scale 1280:-2（缩放） → [v]"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("链 2：[0:a] → volume 2（音量调整） → [a]"))) << text.toStdString();
}

TEST(FfmpegFilterGraphTests, DescribeMarksUnavailableFilterInline) {
    FfmpegCapabilityCache::Instance().Clear();
    FfmpegCapabilityCache::Instance().SetFilters({QStringLiteral("scale")});

    const auto graph = ParseFilterGraph(QStringLiteral("scale=1,unsharp=2"), false);
    const QString text = DescribeFilterGraph(graph);
    EXPECT_TRUE(text.contains(QStringLiteral("unsharp 2（当前这个 ffmpeg 里没有）"))) << text.toStdString();

    FfmpegCapabilityCache::Instance().Clear();
}
