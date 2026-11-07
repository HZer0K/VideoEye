// 文件结构图谱页（切片 1）的页面级 / 数据变换测试。
//
// 为什么单独测这一页：它把一棵通用结构树投影成字节布局，最容易出的不是崩溃，
// 而是"错了也看不出来" —— 递归少走一层、区间算成等宽、结果被裁剪却没标注、
// 空 CSV 看起来像"这个文件真没有节点"。所以这里钉住四件事：
//   1) 节点按深度优先前序展开，层级与 offset/size 区间正确；
//   2) 页头恰好一个「启用分析」+ 一个「导出」，且同一行、入口在左；
//   3) 换文件 / 失败 / 关开关都不能谎报成功，结果也不能被悄悄丢掉；
//   4) 绘制预算与 CSV 的反向守护（无事实不得产表）。
//
// 只构造控件、不渲染到屏幕（offscreen），不触发导出按钮（会弹 QFileDialog 模态框）。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/model/ContainerStructureInfo.h"
#include "tests/support/WidgetLayoutScan.h"
#include "ui/analysis_panel/ContainerStructurePage.h"
#include "ui/analysis_panel/FileStructureMapPage.h"

using videoeye::model::ContainerElement;
using videoeye::model::ContainerFormat;
using videoeye::model::ContainerStructureResult;
using videoeye::model::MediaPlaylistInfo;
using videoeye::model::Mp4Sample;
using videoeye::model::Mp4TrackSampleTable;
using videoeye::model::SegmentInfo;
using videoeye::ui::ContainerStructurePage;
using videoeye::ui::FileStructureMapPage;
namespace detail = videoeye::ui::structure_map_detail;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_file_structure_map_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

ContainerElement Element(const std::string& name, quint64 offset, quint64 size, int depth,
                         const std::string& value = {}) {
    ContainerElement e;
    e.name = name;
    e.type = "Box";
    e.offset = offset;
    e.size = size;
    e.depth = depth;
    e.value = value;
    return e;
}

// 4 个顶层 box，moov 带两层子结构，mdat 占绝大部分字节。
ContainerStructureResult MakeMapResult() {
    ContainerStructureResult r;
    r.valid = true;
    r.format = ContainerFormat::MP4;
    r.format_name = "MP4";
    r.file_path = "map_sample.mp4";
    r.summary = "MP4，2 条流";

    ContainerElement moov = Element("moov", 32, 4096, 0);
    ContainerElement trak = Element("trak", 40, 2048, 1, "video");
    ContainerElement stbl = Element("stbl", 48, 1024, 2);
    trak.children.push_back(stbl);
    moov.children.push_back(trak);
    moov.children.push_back(Element("mvhd", 2088, 108, 1));

    r.element_tree.push_back(Element("ftyp", 0, 32, 0));
    r.element_tree.push_back(moov);
    r.element_tree.push_back(Element("mdat", 4128, 800000, 0));
    r.element_tree.push_back(Element("moof", 804128, 4096, 0));

    r.streams.push_back({0, "video", "avc1", "1280x720"});
    r.streams.push_back({1, "audio", "mp4a", "48000Hz"});
    r.metadata["title"] = "map sample";
    return r;
}

QCheckBox* FindFeatureToggle(QWidget* page) {
    for (QCheckBox* box : page->findChildren<QCheckBox*>()) {
        if (box->text() == QStringLiteral("启用分析"))
            return box;
    }
    return nullptr;
}

} // namespace

TEST(FileStructureMapPageTest, HeaderHasExactlyOneEntryAndOneExportOnTheSameRow) {
    EnsureApp();
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    auto* page = new FileStructureMapPage(true, &host);
    layout->addWidget(page);
    host.resize(900, 600);
    host.show();
    QApplication::processEvents();
    QApplication::processEvents();

    int entries = 0;
    for (QCheckBox* box : page->findChildren<QCheckBox*>()) {
        if (box->text() == QStringLiteral("启用分析"))
            ++entries;
    }
    EXPECT_EQ(entries, 1) << "页头只应有一个「启用分析」";
    EXPECT_EQ(page->findChildren<QCheckBox*>().size(), 1) << "本页没有其它勾选框";
    const auto exports = page->findChildren<QPushButton*>(QStringLiteral("ExportMenuButton"));
    EXPECT_EQ(exports.size(), 1);
    EXPECT_EQ(page->findChildren<QLabel*>(QStringLiteral("PageTitle")).size(), 1);
    ASSERT_NE(page->findChild<QWidget*>(QStringLiteral("StructureMapCanvas")), nullptr);

    QCheckBox* toggle = FindFeatureToggle(page);
    ASSERT_NE(toggle, nullptr);
    ASSERT_EQ(exports.size(), 1);
    const QPoint entry_pos = toggle->mapTo(page, QPoint(0, 0));
    const QPoint export_pos = exports.front()->mapTo(page, QPoint(0, 0));
    EXPECT_LE(std::abs(entry_pos.y() - export_pos.y()), 6);
    EXPECT_LT(entry_pos.x(), export_pos.x());
}

TEST(StructureMapModelTest, CollectNodesKeepsDepthFirstOrderAndByteIntervals) {
    const std::vector<detail::MapNode> nodes = detail::CollectNodes(MakeMapResult(), 2, 512);
    ASSERT_EQ(nodes.size(), 7u) << "4 个顶层 + moov 的 2 个子 + 1 个孙节点，递归少一层就会少节点";

    const std::vector<std::string> expected_names = {"ftyp", "moov", "trak", "stbl", "mvhd", "mdat", "moof"};
    const std::vector<int> expected_depths = {0, 0, 1, 2, 1, 0, 0};
    for (size_t i = 0; i < nodes.size(); ++i) {
        EXPECT_EQ(nodes[i].name.toStdString(), expected_names[i]) << "节点顺序变了（index " << i << "）";
        EXPECT_EQ(nodes[i].depth, expected_depths[i]);
        EXPECT_GE(nodes[i].x0, 0.0);
        EXPECT_LE(nodes[i].x1, 1.0);
        EXPECT_LE(nodes[i].x0, nodes[i].x1);
    }

    // 顶层按真实字节偏移：mdat 在 moov 之后、占绝大部分宽度，moof 在最后。
    EXPECT_GT(nodes[5].x0, nodes[1].x0);
    EXPECT_GT(nodes[5].x1, nodes[1].x1);
    EXPECT_GT(nodes[6].x0, nodes[5].x0);
    EXPECT_GT(nodes[5].x1 - nodes[5].x0, 0.9) << "mdat 应占 ~99% 的字节跨度";
    // 子层必须落在父区间内，不能画到父矩形外面。
    EXPECT_GE(nodes[2].x0, nodes[1].x0);
    EXPECT_LE(nodes[2].x1, nodes[1].x1);
    EXPECT_GE(nodes[3].x0, nodes[2].x0);
    EXPECT_LE(nodes[3].x1, nodes[2].x1);
}

TEST(StructureMapModelTest, NodesWithoutByteLayoutFallBackToEqualWidth) {
    ContainerStructureResult r = MakeMapResult();
    for (ContainerElement& top : r.element_tree) {
        top.offset = 0;
        top.size = 0;
    }
    const std::vector<detail::MapNode> nodes = detail::CollectNodes(r, 2, 512);
    ASSERT_EQ(nodes.size(), 7u);
    EXPECT_DOUBLE_EQ(nodes[0].x0, 0.0);
    EXPECT_GT(nodes[5].x1, nodes[5].x0) << "没有 size 也不能塌成 0 宽";
    EXPECT_GT(nodes[0].x1, nodes[0].x0);
}

TEST(FileStructureMapPageTest, SetResultFeedsCanvasAndReportsSkeletonStatus) {
    EnsureApp();
    FileStructureMapPage page(true);
    page.SetResult(MakeMapResult());
    // 几何判据只扫可见控件：不 show 就会退化成空操作（测了个寂寞）。
    page.resize(900, 600);
    page.show();
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_TRUE(page.HasData());
    EXPECT_TRUE(page.result().valid);
    EXPECT_EQ(page.NodeCount(), 7);
    EXPECT_TRUE(page.DrawingEnabled());
    const QString status = page.StatusText();
    EXPECT_TRUE(status.contains(QStringLiteral("MP4"))) << status.toStdString();
    EXPECT_TRUE(status.contains(QStringLiteral("2 条流"))) << status.toStdString();
    EXPECT_TRUE(status.contains(QStringLiteral("7 个节点"))) << status.toStdString();
    EXPECT_TRUE(status.contains(QStringLiteral("结构骨架"))) << status.toStdString();
    EXPECT_FALSE(status.contains(QStringLiteral("裁剪"))) << status.toStdString();

    // 有数据态也要满足通用几何判据：控件都进布局、没有停在 100x30 @(0,0)。
    std::vector<std::string> findings;
    videoeye_test::CollectSiblingOverlaps(&page, "file_structure_map", findings);
    videoeye_test::CollectDefaultGeometry(&page, "file_structure_map", findings);
    EXPECT_TRUE(findings.empty());
}

TEST(FileStructureMapPageTest, ClearFailureAndToggleNeverPretendSuccess) {
    EnsureApp();
    FileStructureMapPage page(true);
    page.SetResult(MakeMapResult());
    ASSERT_TRUE(page.HasData());

    QCheckBox* toggle = FindFeatureToggle(&page);
    ASSERT_NE(toggle, nullptr);
    toggle->setChecked(false);
    EXPECT_FALSE(page.DrawingEnabled());
    EXPECT_EQ(page.NodeCount(), 7) << "关开关只是不绘制，已收到的结果不能丢";
    toggle->setChecked(true);
    EXPECT_TRUE(page.DrawingEnabled());

    page.SetError(QStringLiteral("boom"));
    EXPECT_FALSE(page.HasData());
    EXPECT_EQ(page.NodeCount(), 0);
    EXPECT_TRUE(page.StatusText().contains(QStringLiteral("boom"))) << page.StatusText().toStdString();

    ContainerStructureResult invalid;
    invalid.valid = false;
    invalid.error_message = "bad box";
    page.SetResult(invalid);
    EXPECT_FALSE(page.HasData());
    EXPECT_TRUE(page.StatusText().contains(QStringLiteral("bad box"))) << page.StatusText().toStdString();

    page.Clear();
    EXPECT_FALSE(page.HasData());
    EXPECT_EQ(page.NodeCount(), 0);
    EXPECT_TRUE(page.StatusText().contains(QStringLiteral("打开媒体文件"))) << page.StatusText().toStdString();
}

TEST(StructureMapModelTest, CsvCoversEveryNodeAndQuotesCommas) {
    ContainerStructureResult r = MakeMapResult();
    r.element_tree[1].children[0].children[0].value = "a,b\"c";
    const QByteArray csv = detail::BuildStructureCsv(r);
    ASSERT_FALSE(csv.isEmpty());
    EXPECT_EQ(csv.count('\n'), 8) << "表头 1 行 + 7 个节点";
    EXPECT_TRUE(csv.contains("name,type,offset,size,depth,value"));
    EXPECT_TRUE(csv.contains("\"a,b\"\"c\"")) << "含逗号 / 引号的值必须正确转义";
    EXPECT_TRUE(csv.contains("\"ftyp\",\"Box\",0,32,0,")) << csv.toStdString();
    EXPECT_TRUE(csv.contains("\"mdat\",\"Box\",4128,800000,0,")) << csv.toStdString();

    // 反向守护：无事实不得产表 —— 空 CSV 分不清"真没节点"和"没跑 / 失败 / 被取消"。
    EXPECT_TRUE(detail::BuildStructureCsv(ContainerStructureResult{}).isEmpty());
    ContainerStructureResult failed;
    failed.valid = false;
    EXPECT_TRUE(detail::BuildStructureCsv(failed).isEmpty());
    // 更要紧的一种：分析失败 / 被取消时，结构树里可能还留着部分节点。
    // 这时也绝不能把它们当成"有效结构"导出成一张看起来正常的空表。
    ContainerStructureResult failed_with_nodes = MakeMapResult();
    failed_with_nodes.valid = false;
    EXPECT_TRUE(detail::BuildStructureCsv(failed_with_nodes).isEmpty()) << "失败结果里残留的节点不能冒充成功结论";
    ContainerStructureResult empty_but_valid;
    empty_but_valid.valid = true;
    EXPECT_TRUE(detail::BuildStructureCsv(empty_but_valid).isEmpty());
}

TEST(FileStructureMapPageTest, NodeBudgetIsEnforcedAndShownInStatus) {
    EnsureApp();
    ContainerStructureResult r;
    r.valid = true;
    r.format_name = "MP4";
    for (int i = 0; i < 700; ++i)
        r.element_tree.push_back(Element("box" + std::to_string(i), static_cast<quint64>(i) * 16, 16, 0));

    EXPECT_EQ(detail::CollectNodes(r, 2, 512).size(), 512u);

    FileStructureMapPage page(true);
    page.SetResult(r);
    EXPECT_EQ(page.NodeCount(), 512);
    EXPECT_TRUE(page.StatusText().contains(QStringLiteral("裁剪"))) << page.StatusText().toStdString();
}

// ---------------------------------------------------------------------------
// 切片 2：均衡布局 / 全深度 / 视窗变换 / 命中 / 搜索 / SVG / 页面交互
// ---------------------------------------------------------------------------

namespace {

detail::MapGeometry TestGeometry() {
    return {40, 38, 800, 26, 3};
}

// 一条 6 层链，用来钉住"深度预算真的放开了"与 parent 面包屑。
ContainerStructureResult MakeDeepResult() {
    ContainerStructureResult r;
    r.valid = true;
    r.format = ContainerFormat::MKV;
    r.format_name = "MKV";
    ContainerElement chain = Element("leaf", 0, 8, 5, "payload");
    for (int depth = 4; depth >= 0; --depth) {
        ContainerElement parent = Element("level" + std::to_string(depth), 0, 128, depth);
        parent.children.push_back(chain);
        chain = parent;
    }
    r.element_tree.push_back(chain);
    return r;
}

} // namespace

TEST(StructureMapModelTest, BalancedLayoutKeepsSmallBoxesVisibleWithoutChangingTopology) {
    const ContainerStructureResult r = MakeMapResult();
    const auto by_bytes = detail::CollectNodes(r, 20, 4000, detail::MapLayoutMode::ByteProportional);
    const auto balanced = detail::CollectNodes(r, 20, 4000, detail::MapLayoutMode::Balanced);
    ASSERT_EQ(balanced.size(), 7u);
    ASSERT_EQ(by_bytes.size(), balanced.size());

    // 两种口径只是宽度不同：名字 / 层级 / 父子关系必须逐项一致，
    // 否则切换视图等于换了一棵树，用户没法把两张图对起来。
    for (size_t i = 0; i < balanced.size(); ++i) {
        EXPECT_EQ(balanced[i].name, by_bytes[i].name) << "index " << i;
        EXPECT_EQ(balanced[i].depth, by_bytes[i].depth) << "index " << i;
        EXPECT_EQ(balanced[i].parent, by_bytes[i].parent) << "index " << i;
        EXPECT_GE(balanced[i].x0, 0.0);
        EXPECT_LE(balanced[i].x1, 1.0);
    }

    // 切片 1 的病：ftyp 只有 0.004% 宽，肉眼完全看不见。
    const double byte_ftyp = by_bytes[0].x1 - by_bytes[0].x0;
    const double balanced_ftyp = balanced[0].x1 - balanced[0].x0;
    EXPECT_LT(byte_ftyp, 0.001);
    EXPECT_GE(balanced_ftyp, 0.05) << "均衡布局必须给最小盒子留出可见份额";
    EXPECT_GT(by_bytes[5].x1 - by_bytes[5].x0, 0.9) << "字节占比视图仍应如实反映 mdat 独占";
    EXPECT_LT(balanced[5].x1 - balanced[5].x0, 0.85) << "均衡布局要把载荷压下来，给元数据腾位置";
    // 子层依旧落在父区间内，不能因为压缩就画到父矩形外面。
    EXPECT_GE(balanced[2].x0, balanced[1].x0);
    EXPECT_LE(balanced[3].x1, balanced[2].x1);
}

TEST(StructureMapModelTest, FullDepthIsCollectedAndParentChainBuildsBreadcrumb) {
    const ContainerStructureResult r = MakeDeepResult();
    EXPECT_EQ(detail::TreeMaxDepth(r), 5);
    EXPECT_EQ(detail::FileSpan(r), 128u);

    const auto all = detail::CollectNodes(r, 20, 4000, detail::MapLayoutMode::Balanced);
    ASSERT_EQ(all.size(), 6u) << "6 层链必须完整展开，少走一层就少一个节点";
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(all[i].depth, i);
        EXPECT_EQ(all[i].parent, i - 1);
    }
    EXPECT_EQ(detail::BuildNodePath(all, 5).toStdString(), "level0 › level1 › level2 › level3 › level4 › leaf");
    EXPECT_EQ(detail::BuildNodePath(all, -1), QString());

    const auto shallow = detail::CollectNodes(r, 2, 4000, detail::MapLayoutMode::Balanced);
    ASSERT_EQ(shallow.size(), 3u);
    for (const auto& node : shallow)
        EXPECT_LE(node.depth, 2);
}

TEST(StructureMapModelTest, ViewportKeepsZoomAnchorAndStaysInsideFile) {
    const detail::MapViewport full;
    EXPECT_DOUBLE_EQ(full.ToScreen(0.5), 0.5);
    EXPECT_DOUBLE_EQ(full.FromScreen(0.5), 0.5);

    const detail::MapViewport zoomed = detail::ZoomAt(full, 0.5, 2.0);
    EXPECT_DOUBLE_EQ(zoomed.scale, 2.0);
    EXPECT_DOUBLE_EQ(zoomed.origin, 0.25);
    EXPECT_DOUBLE_EQ(zoomed.ToScreen(0.5), 0.5) << "锚点在缩放前后必须停在同一屏幕位置";
    EXPECT_LE(zoomed.Right(), 1.0 + 1e-9);

    const detail::MapViewport back = detail::ZoomAt(zoomed, 0.5, 0.5);
    EXPECT_DOUBLE_EQ(back.scale, 1.0);
    EXPECT_DOUBLE_EQ(back.origin, 0.0);

    const detail::MapViewport fit = detail::FitInterval(full, 0.3, 0.5);
    EXPECT_DOUBLE_EQ(fit.scale, 5.0);
    EXPECT_DOUBLE_EQ(fit.ToScreen(0.3), 0.0);
    EXPECT_DOUBLE_EQ(fit.ToScreen(0.5), 1.0);
    EXPECT_LE(detail::FitInterval(full, 0.5, 0.5000001).scale, 256.0);

    // 越界输入一律夹回来，不能让 origin 把整张图推出画布。
    const detail::MapViewport wild = detail::ClampViewport({8.0, 42.0});
    EXPECT_LE(wild.Right(), 1.0 + 1e-9);
    EXPECT_GE(wild.origin, 0.0);
}

TEST(StructureMapModelTest, LodBudgetGrowsWithCanvasButIsAlwaysBounded) {
    const detail::MapLod small = detail::ComputeLod(600, 400, 8);
    const detail::MapLod large = detail::ComputeLod(1400, 900, 8);
    EXPECT_GE(large.rows, small.rows);
    EXPECT_GE(large.max_nodes, small.max_nodes);
    EXPECT_GE(small.max_nodes, 512);
    EXPECT_LE(large.max_nodes, 4000);
    EXPECT_LE(small.rows, 12);
    EXPECT_EQ(small.max_depth, small.rows - 1);

    const detail::MapLod tiny = detail::ComputeLod(10, 10, 8);
    EXPECT_EQ(tiny.rows, 1);
    EXPECT_GE(tiny.max_nodes, 512) << "画布再小也要保住最小预算，否则空态/小窗口下整张图会消失";
    EXPECT_EQ(detail::ComputeLod(800, 600, 0).rows, 1);
}

TEST(StructureMapModelTest, HitTestUsesExactlyTheSameGeometryAsDrawing) {
    const auto nodes = detail::CollectNodes(MakeMapResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    const detail::MapGeometry geom = TestGeometry();
    const detail::MapViewport view;
    ASSERT_EQ(nodes.size(), 7u);

    for (size_t i = 0; i < nodes.size(); ++i) {
        const QRect box = detail::NodeRect(nodes[i], geom, view, 0);
        EXPECT_GT(box.width(), 0);
        EXPECT_EQ(detail::NodeAt(nodes, geom, view, box.center(), 0), static_cast<int>(i))
            << "绘制矩形中心必须命中自己（index " << i << "）";
    }
    EXPECT_EQ(detail::NodeAt(nodes, geom, view, QPoint(geom.left - 5, geom.top + 5), 0), -1);
    EXPECT_EQ(detail::NodeAt(nodes, geom, view, QPoint(geom.left + 10, geom.top - 5), 0), -1);
    EXPECT_EQ(detail::NodeAt(nodes, geom, view, QPoint(geom.left + 10, geom.top + geom.rows * geom.row_height), 0), -1);

    // 垂直滚动到第 3 层后，L0/L1 的绘制与命中都得一起滚走。
    const QRect stbl = detail::NodeRect(nodes[3], geom, view, 0);
    EXPECT_EQ(detail::NodeAt(nodes, geom, view, QPoint(stbl.center().x(), geom.top + 8), 2), 3);
    EXPECT_EQ(detail::NodeAt(nodes, geom, view, QPoint(geom.left + 8, geom.top + 8), 5), -1);
}

TEST(StructureMapModelTest, SearchMatchesNameTypeValueAndNeverMatchesEmpty) {
    const auto nodes = detail::CollectNodes(MakeMapResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    EXPECT_TRUE(detail::MatchNodes(nodes, QString()).isEmpty());
    EXPECT_TRUE(detail::MatchNodes(nodes, QStringLiteral("   ")).isEmpty());
    EXPECT_EQ(detail::MatchNodes(nodes, QStringLiteral("MDA")), (QVector<int>{5})) << "大小写不敏感";
    EXPECT_EQ(detail::MatchNodes(nodes, QStringLiteral("box")).size(), 7) << "type 也应可搜";
    EXPECT_EQ(detail::MatchNodes(nodes, QStringLiteral("video")), (QVector<int>{2})) << "value 也应可搜";
    EXPECT_TRUE(detail::MatchNodes(nodes, QStringLiteral("不存在的节点")).isEmpty());
}

TEST(StructureMapModelTest, SvgSnapshotMirrorsDrawnNodesAndRejectsEmptyResults) {
    const auto nodes = detail::CollectNodes(MakeMapResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    const detail::MapGeometry geom = TestGeometry();
    const QByteArray svg =
        detail::BuildStructureSvg(nodes, geom, detail::MapViewport{}, 0, detail::MapLayoutMode::Balanced, false);
    ASSERT_FALSE(svg.isEmpty());
    EXPECT_TRUE(svg.contains("<svg"));
    EXPECT_TRUE(svg.contains("layout=balanced"));
    EXPECT_TRUE(svg.contains("truncated=no"));
    EXPECT_TRUE(svg.contains("mdat"));
    EXPECT_GE(svg.count("<rect"), 8) << "背景 + 7 个节点";

    const QByteArray truncated =
        detail::BuildStructureSvg(nodes, geom, detail::MapViewport{}, 0, detail::MapLayoutMode::ByteProportional, true);
    EXPECT_TRUE(truncated.contains("layout=byte-proportional"));
    EXPECT_TRUE(truncated.contains("truncated=yes"));

    // 反向守护：没有事实就不产图，别让"分析失败"看起来像一次成功的空导出。
    EXPECT_TRUE(detail::BuildStructureSvg({}, geom, detail::MapViewport{}, 0, detail::MapLayoutMode::Balanced, false)
                    .isEmpty());
    EXPECT_TRUE(
        detail::BuildStructureSvg(nodes, geom, detail::MapViewport{}, 99, detail::MapLayoutMode::Balanced, false)
            .isEmpty())
        << "滚动到没有任何可见节点的层时也不能产出只有背景的空图";
}

TEST(StructureMapModelTest, DescribeNodeReportsRealOffsetsSizesAndSpan) {
    const auto nodes = detail::CollectNodes(MakeMapResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    const quint64 span = detail::FileSpan(MakeMapResult());
    EXPECT_EQ(span, 808224u);
    const QString text = detail::DescribeNode(nodes[1], span);
    EXPECT_TRUE(text.contains(QStringLiteral("moov"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("0x20"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("4.00 KiB"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("4096"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("L0"))) << text.toStdString();
    EXPECT_EQ(detail::BuildNodePath(nodes, 3).toStdString(), "moov › trak › stbl");
}

TEST(FileStructureMapPageTest, ToolbarKeepsSingleHeaderAndExposesInteractionState) {
    EnsureApp();
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    auto* page = new FileStructureMapPage(true, &host);
    layout->addWidget(page);
    host.resize(1000, 700);
    host.show();
    QApplication::processEvents();
    QApplication::processEvents();

    // 页头契约不能因为加了工具条而退化：整页仍只有一个勾选框、一个导出。
    EXPECT_EQ(page->findChildren<QCheckBox*>().size(), 1);
    EXPECT_EQ(page->findChildren<QPushButton*>(QStringLiteral("ExportMenuButton")).size(), 1);
    for (const char* name : {"StructureMapToolbar", "StructureMapLayoutMode", "StructureMapDepth", "StructureMapSearch",
                             "StructureMapHitCount", "StructureMapDetail", "StructureMapCanvas"})
        EXPECT_NE(page->findChild<QWidget*>(QString::fromLatin1(name)), nullptr) << name;

    page->SetResult(MakeMapResult());
    EXPECT_EQ(page->LayoutMode(), detail::MapLayoutMode::Balanced);
    EXPECT_EQ(page->NodeCount(), 7);
    EXPECT_DOUBLE_EQ(page->ViewScale(), 1.0);

    page->SetLayoutMode(detail::MapLayoutMode::ByteProportional);
    EXPECT_EQ(page->LayoutMode(), detail::MapLayoutMode::ByteProportional);
    EXPECT_EQ(page->NodeCount(), 7) << "切布局只改宽度，不能改拓扑";

    page->SetSearchQuery(QStringLiteral("mdat"));
    EXPECT_EQ(page->SearchQuery(), QStringLiteral("mdat"));
    EXPECT_EQ(page->HitCount(), 1);

    page->SelectNode(1);
    EXPECT_EQ(page->SelectedNode(), 1);
    EXPECT_TRUE(page->DetailText().contains(QStringLiteral("moov"))) << page->DetailText().toStdString();
    EXPECT_TRUE(page->DetailText().contains(QStringLiteral("0x20"))) << page->DetailText().toStdString();

    page->FocusNode(1);
    EXPECT_GT(page->ViewScale(), 1.0);
    page->ResetView();
    EXPECT_DOUBLE_EQ(page->ViewScale(), 1.0);

    page->Clear();
    EXPECT_EQ(page->SelectedNode(), -1);
    EXPECT_EQ(page->HitCount(), 0);
    EXPECT_FALSE(page->DetailText().contains(QStringLiteral("moov")));

    std::vector<std::string> findings;
    videoeye_test::CollectSiblingOverlaps(page, "file_structure_map", findings);
    videoeye_test::CollectDefaultGeometry(page, "file_structure_map", findings);
    EXPECT_TRUE(findings.empty()) << [&] {
        std::string joined;
        for (const auto& f : findings)
            joined += f + "\n";
        return joined;
    }();
}

TEST(FileStructureMapPageTest, DeepTreeIsFlaggedInsteadOfSilentlyDropped) {
    EnsureApp();
    FileStructureMapPage page(true);
    page.SetResult(MakeDeepResult());
    EXPECT_EQ(detail::TreeMaxDepth(page.result()), 5);
    EXPECT_GT(page.NodeCount(), 0);

    // 超过硬深度上限时状态里必须出现裁剪字样，不能拿"结构骨架"糊过去。
    ContainerStructureResult deep;
    deep.valid = true;
    deep.format_name = "MKV";
    ContainerElement chain = Element("leaf", 0, 8, 0);
    for (int i = 0; i < 25; ++i) {
        ContainerElement parent = Element("level" + std::to_string(i), 0, 16, 0);
        parent.children.push_back(chain);
        chain = parent;
    }
    deep.element_tree.push_back(chain);
    page.SetResult(deep);
    EXPECT_GT(detail::TreeMaxDepth(page.result()), 20);
    EXPECT_TRUE(page.StatusText().contains(QStringLiteral("深度已裁剪"))) << page.StatusText().toStdString();
}

// ---------------------------------------------------------------------------
// 切片 3：三视图几何 / 时间轴包带 / 完整性五维 / JSON / 跨页联动
// ---------------------------------------------------------------------------

namespace {

const QRect kPlotArea(40, 38, 800, 400);

// 4 个样本、10 秒、1000 时基的 MP4 样本表。cts 落在 0 / 2.5 / 5 / 7.5 秒，
// 大小 1000/2000/3000/4000，前两个为关键帧 —— 分桶结果可以逐项核对。
ContainerStructureResult MakeSampleResult() {
    ContainerStructureResult r = MakeMapResult();
    r.mp4_samples.valid = true;
    Mp4TrackSampleTable track;
    track.track_id = 1;
    track.type = "video";
    track.codec = "avc1";
    track.media_timescale = 1000;
    track.media_duration = 10000;
    track.stsz_sample_count = 4;
    for (int i = 0; i < 4; ++i) {
        Mp4Sample sample;
        sample.index = static_cast<uint32_t>(i);
        sample.cts = i * 2500;
        sample.dts = i * 2500;
        sample.duration = 2500;
        sample.size = static_cast<uint32_t>(1000 * (i + 1));
        sample.keyframe = (i == 0 || i == 2);
        track.samples.push_back(sample);
    }
    r.mp4_samples.tracks.push_back(track);
    return r;
}

ContainerStructureResult MakeStreamingResult() {
    ContainerStructureResult r = MakeMapResult();
    r.format = ContainerFormat::HLS;
    r.format_name = "HLS";
    r.streaming_package.valid = true;
    MediaPlaylistInfo playlist;
    for (int i = 0; i < 3; ++i) {
        SegmentInfo segment;
        segment.start_seconds = i * 2.0;
        segment.duration_seconds = 2.0;
        segment.has_duration = true;
        segment.exists = true;
        segment.probed = true;
        segment.file_size_bytes = 1000 * (i + 1);
        playlist.segments.push_back(segment);
    }
    r.streaming_package.playlists.push_back(playlist);
    return r;
}

} // namespace

TEST(StructureMapModelTest, IcicleVisualsUseExactlyNodeRectGeometry) {
    const auto nodes = detail::CollectNodes(MakeMapResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    const detail::MapGeometry geom = TestGeometry();
    const detail::MapViewport view;
    const auto visuals = detail::BuildIcicleVisuals(nodes, geom, view, 0);
    ASSERT_EQ(visuals.size(), nodes.size());
    for (size_t i = 0; i < visuals.size(); ++i) {
        EXPECT_EQ(visuals[i].node_index, static_cast<int>(i));
        EXPECT_EQ(visuals[i].rect, detail::NodeRect(nodes[i], geom, view, 0)) << "index " << i;
        EXPECT_EQ(visuals[i].track_index, -1);
    }
    int hit = detail::VisualAt(visuals, visuals[2].rect.center());
    ASSERT_GE(hit, 0);
    EXPECT_EQ(visuals[hit].node_index, 2);
    EXPECT_EQ(detail::VisualAt(visuals, QPoint(2, 2)), -1);
}

TEST(StructureMapModelTest, TreemapConservesAreaKeepsChildrenInsideAndIsHitTestable) {
    const auto nodes = detail::CollectNodes(MakeMapResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    const auto visuals = detail::BuildTreemapVisuals(nodes, kPlotArea, 4000);
    ASSERT_FALSE(visuals.empty());
    EXPECT_EQ(visuals.size(), nodes.size()) << "压缩权重后每个节点都该有可见矩形，不能被 mdat 挤没";

    // 顶层矩形必须把绘图区拼满（面积守恒），且不互相重叠。
    long long top_area = 0;
    long long mdat_area = 0;
    std::vector<QRect> tops;
    for (const auto& visual : visuals) {
        ASSERT_GE(visual.node_index, 0);
        EXPECT_TRUE(kPlotArea.contains(visual.rect)) << "矩形画出绘图区";
        if (nodes[visual.node_index].depth != 0)
            continue;
        const long long area = static_cast<long long>(visual.rect.width()) * visual.rect.height();
        top_area += area;
        if (nodes[visual.node_index].name == QStringLiteral("mdat"))
            mdat_area = area;
        tops.push_back(visual.rect);
    }
    EXPECT_EQ(top_area, static_cast<long long>(kPlotArea.width()) * kPlotArea.height());
    EXPECT_LT(static_cast<double>(mdat_area) / top_area, 0.9)
        << "载荷不该独占到把元数据压成 1px（切片 1 的病不能在树图里复发）";
    ASSERT_EQ(tops.size(), 4u);
    for (size_t i = 0; i < tops.size(); ++i)
        for (size_t j = i + 1; j < tops.size(); ++j)
            EXPECT_FALSE(tops[i].intersects(tops[j])) << "顶层矩形重叠 " << i << "/" << j;

    // 子层必须嵌套在父矩形内，不能因为递归切分画到父外面。
    QRect moov, trak, stbl;
    for (const auto& visual : visuals) {
        const QString name = nodes[visual.node_index].name;
        if (name == QStringLiteral("moov"))
            moov = visual.rect;
        else if (name == QStringLiteral("trak"))
            trak = visual.rect;
        else if (name == QStringLiteral("stbl"))
            stbl = visual.rect;
    }
    ASSERT_FALSE(moov.isEmpty());
    ASSERT_FALSE(trak.isEmpty());
    ASSERT_FALSE(stbl.isEmpty());
    EXPECT_TRUE(moov.contains(trak));
    EXPECT_TRUE(trak.contains(stbl));

    // 父矩形左上角（子区域内缩之后）必须能命中父本身，而不是被子的矩形抢走。
    const int hit = detail::VisualAt(visuals, QPoint(moov.left() + 1, moov.top() + 1));
    ASSERT_GE(hit, 0);
    EXPECT_EQ(nodes[visuals[hit].node_index].name, QStringLiteral("moov"));
}

TEST(StructureMapModelTest, VisualBudgetAndSvgGuardRejectEmptyViews) {
    const auto nodes = detail::CollectNodes(MakeMapResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    EXPECT_TRUE(detail::BuildTreemapVisuals({}, kPlotArea, 4000).empty());
    EXPECT_TRUE(detail::BuildTreemapVisuals(nodes, QRect(0, 0, 4, 4), 4000).empty());
    EXPECT_LE(detail::BuildTreemapVisuals(nodes, kPlotArea, 3).size(), 3u);
    EXPECT_TRUE(detail::BuildVisualsSvg({}, QSize(880, 470), detail::MapViewKind::Treemap, false).isEmpty());
    const auto small_visuals = detail::BuildTreemapVisuals(nodes, kPlotArea, 3);
    EXPECT_TRUE(detail::BuildVisualsSvg(small_visuals, QSize(3, 3), detail::MapViewKind::Treemap, false).isEmpty());

    const auto visuals = detail::BuildTreemapVisuals(nodes, kPlotArea, 4000);
    const QByteArray svg = detail::BuildVisualsSvg(visuals, QSize(880, 470), detail::MapViewKind::Treemap, true);
    ASSERT_FALSE(svg.isEmpty());
    EXPECT_TRUE(svg.contains("view=treemap"));
    EXPECT_TRUE(svg.contains("truncated=yes"));
    EXPECT_TRUE(svg.contains("mdat"));
    EXPECT_GE(svg.count("<rect"), static_cast<int>(nodes.size())) << "背景之外每个节点都要有矩形";
}

TEST(StructureMapModelTest, PacketBandsAggregateRealSamplesIntoTimeBins) {
    const detail::PacketBandData bands = detail::BuildPacketBands(MakeSampleResult(), 10);
    EXPECT_TRUE(bands.available);
    EXPECT_TRUE(bands.source.contains(QStringLiteral("样本表")));
    EXPECT_FALSE(bands.sampled);
    EXPECT_DOUBLE_EQ(bands.duration_seconds, 10.0);
    EXPECT_EQ(bands.tracks.size(), 1u);
    ASSERT_EQ(bands.tracks[0].bins.size(), 10u);
    EXPECT_EQ(bands.TotalCount(), 4u);
    EXPECT_EQ(bands.BinCount(), 10);
    EXPECT_TRUE(bands.tracks[0].has_bytes);

    quint64 bytes = 0, count = 0, keyframes = 0;
    for (const auto& bin : bands.tracks[0].bins) {
        bytes += bin.bytes;
        count += bin.count;
        keyframes += bin.keyframes;
    }
    EXPECT_EQ(bytes, 10000u);
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(keyframes, 2u);
    EXPECT_EQ(bands.tracks[0].bins[0].bytes, 1000u);
    EXPECT_EQ(bands.tracks[0].bins[2].bytes, 2000u);
    EXPECT_EQ(bands.tracks[0].bins[5].bytes, 3000u);
    EXPECT_EQ(bands.tracks[0].bins[7].bytes, 4000u);

    const auto visuals = detail::BuildPacketVisuals(bands, kPlotArea);
    ASSERT_EQ(visuals.size(), 10u);
    for (size_t i = 0; i < visuals.size(); ++i) {
        EXPECT_EQ(visuals[i].node_index, -1);
        EXPECT_EQ(visuals[i].track_index, 0);
        EXPECT_EQ(visuals[i].bin_index, static_cast<int>(i));
        EXPECT_GT(visuals[i].bar_height, 0);
        EXPECT_FALSE(visuals[i].bar_height > visuals[i].rect.height());
    }
    const int hit = detail::VisualAt(visuals, visuals[3].rect.center());
    EXPECT_EQ(hit, 3) << "整列都应可命中，空桶也要能点开详情";
    EXPECT_TRUE(detail::DescribeBandBin(bands, 0, 0).contains(QStringLiteral("时间桶详情")));
    EXPECT_TRUE(detail::DescribeBandBin(bands, -1, 0).isEmpty());
    EXPECT_TRUE(detail::DescribeBandBin(bands, 0, 999).isEmpty());
}

TEST(StructureMapModelTest, PacketBandsNeverInventDataAndDeclareCoverage) {
    // 没有样本表 / 分片清单：宁可空着，也别画一条"平均密度带"。
    const detail::PacketBandData none = detail::BuildPacketBands(MakeMapResult(), 32);
    EXPECT_FALSE(none.available);
    EXPECT_TRUE(none.source.contains(QStringLiteral("没有可用")));
    EXPECT_TRUE(detail::BuildPacketVisuals(none, kPlotArea).empty());
    ContainerStructureResult failed;
    failed.valid = false;
    EXPECT_FALSE(detail::BuildPacketBands(failed, 32).available);

    // 缺时基时不能把"按样本数均分"冒充时间轴。
    ContainerStructureResult no_timescale = MakeSampleResult();
    no_timescale.mp4_samples.tracks[0].media_timescale = 0;
    const detail::PacketBandData untimed = detail::BuildPacketBands(no_timescale, 10);
    EXPECT_FALSE(untimed.available);
    EXPECT_TRUE(untimed.source.contains(QStringLiteral("时基")));

    // 样本表被截断必须如实标注为抽样。
    ContainerStructureResult truncated = MakeSampleResult();
    truncated.mp4_samples.tracks[0].samples_truncated = true;
    const detail::PacketBandData sampled = detail::BuildPacketBands(truncated, 10);
    EXPECT_TRUE(sampled.sampled);
    EXPECT_TRUE(sampled.coverage_note.contains(QStringLiteral("截断")));

    // 流媒体清单：按分片起点分桶，并区分"抽查"与"全探测"。
    const detail::PacketBandData streaming = detail::BuildPacketBands(MakeStreamingResult(), 10);
    EXPECT_TRUE(streaming.available);
    EXPECT_TRUE(streaming.source.contains(QStringLiteral("清单")));
    EXPECT_DOUBLE_EQ(streaming.duration_seconds, 6.0);
    EXPECT_EQ(streaming.TotalCount(), 3u);
    EXPECT_FALSE(streaming.partially_probed);
    EXPECT_TRUE(streaming.tracks[0].has_bytes);

    ContainerStructureResult unprobed = MakeStreamingResult();
    unprobed.streaming_package.playlists[0].segments[0].probed = false;
    EXPECT_TRUE(detail::BuildPacketBands(unprobed, 10).partially_probed);
}

TEST(StructureMapModelTest, CoverageKeepsFiveDimensionsSeparateAndNeverClaimsPayloadCompleteness) {
    const detail::MapCoverage clean = detail::BuildCoverage(MakeMapResult(), false, false);
    EXPECT_TRUE(clean.analyzed);
    EXPECT_TRUE(clean.conclusion_complete);
    EXPECT_FALSE(clean.window_limited);
    EXPECT_FALSE(clean.sampled);
    EXPECT_FALSE(clean.truncated);
    EXPECT_TRUE(clean.error_reason.isEmpty());
    EXPECT_TRUE(clean.SummaryText().contains(QStringLiteral("结构解析成功")));
    EXPECT_TRUE(clean.SummaryText().contains(QStringLiteral("结构骨架")));

    ContainerStructureResult windowed = MakeMapResult();
    windowed.diagnostics.mkv.analyzed = true;
    windowed.diagnostics.mkv.scan_complete = false;
    const detail::MapCoverage window = detail::BuildCoverage(windowed, false, false);
    EXPECT_TRUE(window.window_limited);
    EXPECT_FALSE(window.sampled);
    EXPECT_FALSE(window.truncated);
    EXPECT_FALSE(window.conclusion_complete);
    EXPECT_TRUE(window.SummaryText().contains(QStringLiteral("窗口")));

    ContainerStructureResult sampled = MakeSampleResult();
    sampled.mp4_samples.tracks[0].samples_truncated = true;
    const detail::MapCoverage sample = detail::BuildCoverage(sampled, false, false);
    EXPECT_TRUE(sample.sampled);
    EXPECT_FALSE(sample.window_limited);
    EXPECT_FALSE(sample.conclusion_complete);

    const detail::MapCoverage clipped = detail::BuildCoverage(MakeMapResult(), true, true);
    EXPECT_TRUE(clipped.truncated);
    EXPECT_FALSE(clipped.sampled);
    EXPECT_FALSE(clipped.conclusion_complete);
    EXPECT_TRUE(clipped.SummaryText().contains(QStringLiteral("预算裁剪")));
    EXPECT_TRUE(clipped.SummaryText().contains(QStringLiteral("深度")));

    ContainerStructureResult failed;
    failed.valid = false;
    failed.error_message = "boom";
    const detail::MapCoverage error = detail::BuildCoverage(failed, false, false);
    EXPECT_FALSE(error.analyzed);
    EXPECT_FALSE(error.conclusion_complete);
    EXPECT_EQ(error.error_reason, QStringLiteral("boom"));
    EXPECT_TRUE(error.SummaryText().contains(QStringLiteral("boom")));

    // HLS/DASH 节点没有 size/offset，这件事本身也要落进事实，别让等宽看起来像实测。
    ContainerStructureResult hls = MakeStreamingResult();
    const detail::MapCoverage manifest = detail::BuildCoverage(hls, false, false);
    EXPECT_TRUE(manifest.SummaryText().contains(QStringLiteral("无 size")));
}

TEST(StructureMapModelTest, JsonCarriesCoverageAndRejectsResultsWithoutFacts) {
    const ContainerStructureResult r = MakeSampleResult();
    const detail::MapCoverage coverage = detail::BuildCoverage(r, true, false);
    const QByteArray json = detail::BuildStructureJson(r, coverage);
    ASSERT_FALSE(json.isEmpty());
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    ASSERT_FALSE(doc.isNull());
    const QJsonObject root = doc.object();
    EXPECT_EQ(root.value(QStringLiteral("schema")).toString(), QStringLiteral("videoeye.structure-map/1"));
    EXPECT_EQ(root.value(QStringLiteral("element_count")).toInt(), 7);
    EXPECT_EQ(root.value(QStringLiteral("file_span")).toDouble(), 808224.0);
    EXPECT_EQ(root.value(QStringLiteral("elements")).toArray().size(), 7);
    EXPECT_EQ(root.value(QStringLiteral("streams")).toArray().size(), 2);
    EXPECT_EQ(root.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("title")).toString(),
              QStringLiteral("map sample"));
    const QJsonObject cov = root.value(QStringLiteral("coverage")).toObject();
    EXPECT_TRUE(cov.value(QStringLiteral("analyzed")).toBool());
    EXPECT_FALSE(cov.value(QStringLiteral("structure_complete")).toBool()) << "节点被裁剪就不能报完整";
    EXPECT_TRUE(cov.value(QStringLiteral("truncated")).toBool());
    EXPECT_GT(cov.value(QStringLiteral("facts")).toArray().size(), 2);
    EXPECT_FALSE(cov.value(QStringLiteral("sampled")).toBool());

    // 含引号 / 反斜杠 / 换行的值必须能原样往返，手写 JSON 最容易在这里坏掉。
    ContainerStructureResult escaped = MakeMapResult();
    escaped.element_tree[1].children[0].children[0].value = "a\"b\\c\n";
    const QJsonDocument roundtrip =
        QJsonDocument::fromJson(detail::BuildStructureJson(escaped, detail::BuildCoverage(escaped, false, false)));
    ASSERT_FALSE(roundtrip.isNull());
    bool found = false;
    for (const QJsonValue& value : roundtrip.object().value(QStringLiteral("elements")).toArray()) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("name")).toString() == QStringLiteral("stbl") &&
            item.value(QStringLiteral("value")).toString() == QString::fromUtf8("a\"b\\c\n"))
            found = true;
    }
    EXPECT_TRUE(found);

    // 反向守护：无效 / 空树一律不产文件，空 JSON 自证不了"没跑"和"没问题"。
    EXPECT_TRUE(detail::BuildStructureJson(ContainerStructureResult{}, coverage).isEmpty());
    ContainerStructureResult failed = MakeMapResult();
    failed.valid = false;
    EXPECT_TRUE(detail::BuildStructureJson(failed, coverage).isEmpty());
    ContainerStructureResult empty_valid;
    empty_valid.valid = true;
    EXPECT_TRUE(detail::BuildStructureJson(empty_valid, coverage).isEmpty());
}

TEST(FileStructureMapPageTest, ThreeViewsSwitchWithoutBreakingHeaderContract) {
    EnsureApp();
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    auto* page = new FileStructureMapPage(true, &host);
    layout->addWidget(page);
    host.resize(1000, 700);
    host.show();
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_EQ(page->findChildren<QCheckBox*>().size(), 1) << "加视图切换不能多出勾选框";
    EXPECT_EQ(page->findChildren<QPushButton*>(QStringLiteral("ExportMenuButton")).size(), 1);
    for (const char* name : {"StructureMapViewKind", "StructureMapCoverage", "StructureMapLayoutMode",
                             "StructureMapDepth", "StructureMapSearch", "StructureMapCanvas"})
        EXPECT_NE(page->findChild<QWidget*>(QString::fromLatin1(name)), nullptr) << name;

    page->SetResult(MakeSampleResult());
    EXPECT_EQ(page->ViewKind(), detail::MapViewKind::Icicle);
    EXPECT_GT(page->VisualCount(), 0);
    EXPECT_TRUE(page->Coverage().analyzed);
    EXPECT_TRUE(page->CoverageText().contains(QStringLiteral("完整性")));
    EXPECT_TRUE(page->Bands().available);
    QComboBox* layout_combo = page->findChild<QComboBox*>(QStringLiteral("StructureMapLayoutMode"));
    ASSERT_NE(layout_combo, nullptr);
    EXPECT_TRUE(layout_combo->isEnabled());

    page->SetViewKind(detail::MapViewKind::Treemap);
    EXPECT_EQ(page->ViewKind(), detail::MapViewKind::Treemap);
    EXPECT_GT(page->VisualCount(), 0);
    EXPECT_FALSE(layout_combo->isEnabled()) << "树图视图下不该留着无效的布局下拉";

    page->SetViewKind(detail::MapViewKind::PacketBands);
    EXPECT_EQ(page->ViewKind(), detail::MapViewKind::PacketBands);
    EXPECT_GT(page->VisualCount(), 0);
    EXPECT_EQ(page->VisualCount(), page->Bands().BinCount());

    page->SetViewKind(detail::MapViewKind::Icicle);
    EXPECT_TRUE(layout_combo->isEnabled());
    EXPECT_GT(page->VisualCount(), 0);

    std::vector<std::string> findings;
    videoeye_test::CollectSiblingOverlaps(page, "file_structure_map", findings);
    videoeye_test::CollectDefaultGeometry(page, "file_structure_map", findings);
    EXPECT_TRUE(findings.empty()) << [&] {
        std::string joined;
        for (const auto& f : findings)
            joined += f + "\n";
        return joined;
    }();
}

TEST(FileStructureMapPageTest, JumpToTreeSignalCarriesNodeIdentityNotRowNumber) {
    EnsureApp();
    FileStructureMapPage page(true);
    page.SetResult(MakeMapResult());
    // 本测试目标没链 Qt6::Test（没有 QSignalSpy），用普通 connect 捕获即可。
    int calls = 0;
    QString captured_path;
    quint64 captured_offset = 0;
    quint64 captured_size = 0;
    QObject::connect(&page, &FileStructureMapPage::StructureNodeJumpRequested,
                     [&](const QString& path, quint64 offset, quint64 size) {
                         ++calls;
                         captured_path = path;
                         captured_offset = offset;
                         captured_size = size;
                     });

    page.SelectNode(3); // moov › trak › stbl
    page.RequestJumpToTree();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(captured_path.toStdString(), "moov › trak › stbl");
    EXPECT_EQ(captured_offset, 48ull);
    EXPECT_EQ(captured_size, 1024ull);

    // 没选中节点时不能发一个指向空节点的跳转。
    page.SelectNode(-1);
    page.RequestJumpToTree();
    EXPECT_EQ(calls, 1);
}

TEST(ContainerStructurePageTest, SelectElementLocatesByOffsetSizeOrFallsBackToBreadcrumb) {
    EnsureApp();
    ContainerStructurePage page(true);
    page.SetResult(MakeMapResult());
    QTreeWidget* tree = page.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);

    EXPECT_TRUE(page.SelectElement(QStringLiteral("moov › trak › stbl"), 48, 1024));
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->text(0), QStringLiteral("stbl"));

    // 身份对不上但路径存在（HLS 清单节点没有 offset/size）时按面包屑回退。
    EXPECT_TRUE(page.SelectElement(QStringLiteral("moov › trak › stbl"), 999999, 888));
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->text(0), QStringLiteral("stbl"));

    EXPECT_FALSE(page.SelectElement(QStringLiteral("不存在的节点"), 1, 1));
    page.Clear();
    EXPECT_FALSE(page.SelectElement(QStringLiteral("moov › trak › stbl"), 48, 1024));
}

// ---------------------------------------------------------------------------
// 切片 4：逐格式画像 / 扫描窗口 / 第四视图 / EBML 字节轴 / 大文件预算
// ---------------------------------------------------------------------------

namespace {

constexpr qint64 kMib = 1024LL * 1024LL;

// 4 GiB 的 MKV，但元素树只扫了前 32 MiB；体检是另一遍全文件元素头扫描。
ContainerStructureResult MakeEbmlResult() {
    ContainerStructureResult r;
    r.valid = true;
    r.format = ContainerFormat::MKV;
    r.format_name = "MKV";
    r.element_tree.push_back(Element("EBML", 0, 64, 0));
    ContainerElement segment = Element("Segment", 64, 4ULL * 1024 * 1024 * 1024 - 64, 0);
    segment.children.push_back(Element("Info", 64, 128, 1));
    segment.children.push_back(Element("Tracks", 192, 256, 1));
    segment.children.push_back(Element("Cluster", 448, 4096, 1));
    r.element_tree.push_back(segment);

    r.ebml_detail.valid = true;
    r.ebml_detail.doc_type = "matroska";
    r.ebml_detail.file_bytes = 4096LL * kMib;
    r.ebml_detail.scanned_bytes = 32LL * kMib;
    r.ebml_detail.scan_truncated = true;
    r.ebml_detail.total_clusters = 900;
    r.ebml_detail.block_nodes_truncated = true;
    videoeye::model::EbmlTrackInfo track;
    r.ebml_detail.tracks.push_back(track);
    for (int i = 0; i < 6; ++i) {
        videoeye::model::EbmlBlockSummary block;
        block.track_number = (i % 2) + 1;
        block.block_offset = 512 + static_cast<uint64_t>(i) * 4096;
        block.data_size = 1000 * (i + 1);
        block.keyframe = (i % 3 == 0);
        r.ebml_detail.blocks.push_back(block);
    }
    r.diagnostics.mkv.analyzed = true;
    r.diagnostics.mkv.scan_complete = true;
    r.diagnostics.mkv.clusters = 900;
    r.diagnostics.mkv.has_cues = false;
    return r;
}

// TS 结构树：只有 root 有文件大小，Program / ES / PES 采样节点 size 全 0。
ContainerStructureResult MakeTsResult() {
    ContainerStructureResult r;
    r.valid = true;
    r.format = ContainerFormat::MPEG_TS;
    r.format_name = "MPEG-TS";
    ContainerElement root = Element("MPEG-TS Stream", 0, 1880000, 0);
    ContainerElement program = Element("Program 1", 0, 0, 1);
    ContainerElement es = Element("PID 0x0100 - H.264", 0, 0, 2);
    es.children.push_back(Element("Video PES", 1880, 0, 3));
    program.children.push_back(es);
    root.children.push_back(program);
    r.element_tree.push_back(root);

    r.transport.analyzed = true;
    r.transport.applicable = true;
    r.transport.packet_size = 188;
    r.transport.scanned_packets = 1000;
    r.transport.estimated_packets = 10000;
    r.transport.scan_complete = false;
    r.transport.programs = 1;
    r.transport.streams = 3;
    r.transport.services = 1;
    r.transport.has_pat = true;
    r.transport.has_pmt = true;
    r.transport.continuity_errors = 7;
    r.transport.pes_payload_scanned = true;
    r.transport.pes_streams_checked = 2;
    r.transport.idr_count = 12;
    return r;
}

// 清单层级：所有节点都没有 offset / size（真实 HLS/DASH 结构树就是这个形状）。
ContainerStructureResult MakeManifestResult() {
    ContainerStructureResult r = MakeStreamingResult();
    r.format = ContainerFormat::HLS;
    r.format_name = "HLS";
    for (ContainerElement& top : r.element_tree) {
        top.offset = 0;
        top.size = 0;
        top.children.clear();
    }
    r.element_tree[0].children.push_back(Element("Variant #0", 0, 0, 1));
    r.element_tree[1].children.push_back(Element("Media Playlist", 0, 0, 1));
    return r;
}

ContainerStructureResult MakeAviResult() {
    ContainerStructureResult r;
    r.valid = true;
    r.format = ContainerFormat::AVI;
    r.format_name = "AVI";
    r.element_tree.push_back(Element("RIFF", 0, 1024, 0));
    r.element_tree.push_back(Element("LIST movi", 1024, 900000, 0));
    r.diagnostics.avi.analyzed = true;
    r.diagnostics.avi.scan_complete = false;
    r.diagnostics.avi.idx1_entries = 1234;
    r.diagnostics.avi.idx1_bad_offsets = 2;
    return r;
}

bool HasFact(const detail::FormatProfile& profile, const QString& label, const QString& value_part = {}) {
    for (const detail::FormatFact& fact : profile.facts) {
        if (fact.label == label && (value_part.isEmpty() || fact.value.contains(value_part)))
            return true;
    }
    return false;
}

} // namespace

TEST(StructureMapFormatTest, ProfileIdentifiesFamiliesWithoutInventingFacts) {
    const detail::FormatProfile mp4 = detail::BuildFormatProfile(MakeMapResult());
    EXPECT_EQ(mp4.family, detail::MapFormatFamily::Isobmff);
    EXPECT_TRUE(mp4.has_byte_layout);
    EXPECT_FALSE(mp4.window_limited);
    EXPECT_FALSE(mp4.has_precise_window);
    EXPECT_TRUE(mp4.coverage_notes.isEmpty()) << "干净的 MP4 不该凭空多出完整性告警";
    EXPECT_TRUE(mp4.structure_note.contains(QStringLiteral("offset")));
    EXPECT_TRUE(HasFact(mp4, QStringLiteral("顶层元素"), QStringLiteral("4")));
    EXPECT_TRUE(HasFact(mp4, QStringLiteral("样本表"), QStringLiteral("未产出")))
        << "没有样本表就要写「未产出」，不能默认成 0 轨 / 0 样本看起来像真统计过";

    const detail::FormatProfile invalid = detail::BuildFormatProfile(ContainerStructureResult{});
    EXPECT_EQ(invalid.family, detail::MapFormatFamily::Unknown);
    EXPECT_TRUE(invalid.facts.empty());
    EXPECT_FALSE(invalid.has_byte_layout);
    EXPECT_TRUE(detail::BuildFormatVisuals(invalid, kPlotArea).empty());

    const detail::FormatProfile hls = detail::BuildFormatProfile(MakeManifestResult());
    EXPECT_EQ(hls.family, detail::MapFormatFamily::Manifest);
    EXPECT_FALSE(hls.has_byte_layout) << "清单节点没有可比较的 size";
    EXPECT_TRUE(hls.structure_note.contains(QStringLiteral("size / offset")));
    EXPECT_TRUE(HasFact(hls, QStringLiteral("清单声明 / 已探测")));

    ContainerStructureResult raw = MakeMapResult();
    raw.format = ContainerFormat::RAW_ES;
    raw.element_tree.clear();
    const detail::FormatProfile raw_profile = detail::BuildFormatProfile(raw);
    EXPECT_EQ(raw_profile.family, detail::MapFormatFamily::RawElementary);
    EXPECT_FALSE(raw_profile.has_byte_layout);
    EXPECT_TRUE(raw_profile.coverage_notes.join(QString()).contains(QStringLiteral("裸流")));
}

TEST(StructureMapFormatTest, EbmlWindowIsMeasuredAndFullFileDiagnosticsAreSeparated) {
    const ContainerStructureResult r = MakeEbmlResult();
    const detail::FormatProfile profile = detail::BuildFormatProfile(r);
    EXPECT_EQ(profile.family, detail::MapFormatFamily::Ebml);
    EXPECT_TRUE(profile.has_byte_layout);
    EXPECT_TRUE(profile.window_limited);
    EXPECT_TRUE(profile.has_precise_window);
    EXPECT_NEAR(profile.window_fraction, 32.0 / 4096.0, 1e-9);
    EXPECT_TRUE(profile.window_label.contains(QStringLiteral("32.00 MiB")));
    EXPECT_TRUE(profile.window_label.contains(QStringLiteral("4.00 GiB")));
    EXPECT_TRUE(HasFact(profile, QStringLiteral("已扫描 / 文件大小")));
    EXPECT_TRUE(HasFact(profile, QStringLiteral("扫描方式"), QStringLiteral("元素头顺序扫描")))
        << "全文件体检必须和结构树窗口分开说，不能混成一句「已扫描」";

    bool has_window_note = false;
    bool has_diagnostic_note = false;
    for (const QString& note : profile.coverage_notes) {
        has_window_note = has_window_note || note.contains(QStringLiteral("片尾元素"));
        has_diagnostic_note = has_diagnostic_note || note.contains(QStringLiteral("全文件元素头扫描"));
    }
    EXPECT_TRUE(has_window_note);
    EXPECT_TRUE(has_diagnostic_note);

    const detail::MapCoverage coverage = detail::BuildCoverage(r, false, false);
    EXPECT_TRUE(coverage.window_limited);
    EXPECT_FALSE(coverage.conclusion_complete);
    EXPECT_TRUE(coverage.SummaryText().contains(QStringLiteral("32.00 MiB")));
    EXPECT_TRUE(coverage.SummaryText().contains(QStringLiteral("全文件元素头扫描")))
        << "格式画像里的口径事实必须合进完整性条，不能只画在概览卡片上";
}

TEST(StructureMapFormatTest, TsProfileUsesOnlyPersistedTransportFactsAndDisablesAreaView) {
    const ContainerStructureResult r = MakeTsResult();
    const detail::FormatProfile profile = detail::BuildFormatProfile(r);
    EXPECT_EQ(profile.family, detail::MapFormatFamily::TransportStream);
    EXPECT_FALSE(profile.has_byte_layout) << "只有 root 有 size，Program/ES/PES 全是 0";
    EXPECT_FALSE(detail::ViewSupported(profile, detail::MapViewKind::Treemap));
    EXPECT_TRUE(detail::ViewUnsupportedReason(profile, detail::MapViewKind::Treemap).contains(QStringLiteral("TS")));
    EXPECT_TRUE(detail::ViewSupported(profile, detail::MapViewKind::Icicle));
    EXPECT_TRUE(detail::ViewSupported(profile, detail::MapViewKind::FormatOverview));

    EXPECT_TRUE(HasFact(profile, QStringLiteral("包长"), QStringLiteral("188 B")));
    EXPECT_TRUE(HasFact(profile, QStringLiteral("已扫 / 预估包数"), QStringLiteral("1000 / 10000")));
    EXPECT_TRUE(HasFact(profile, QStringLiteral("节目 / ES / 服务"), QStringLiteral("1 / 3 / 1")));
    EXPECT_TRUE(HasFact(profile, QStringLiteral("失步 / CC 错误 / 加扰"), QStringLiteral("7")));
    EXPECT_TRUE(HasFact(profile, QStringLiteral("载荷级扫描"), QStringLiteral("2 条 ES")));
    EXPECT_TRUE(profile.window_limited);
    EXPECT_NEAR(profile.window_fraction, 0.1, 1e-9);
    EXPECT_TRUE(profile.window_label.contains(QStringLiteral("按固定包长换算")));

    const detail::MapCoverage coverage = detail::BuildCoverage(r, false, false);
    EXPECT_TRUE(coverage.window_limited);
    EXPECT_TRUE(coverage.SummaryText().contains(QStringLiteral("采样汇总")));

    // 没跑过传输层扫描：只能写「未运行 / 适用但未产出」，不能编出 0 个错误的结论。
    ContainerStructureResult unscanned = MakeTsResult();
    unscanned.transport = videoeye::model::TransportStreamInfo{};
    const detail::FormatProfile none = detail::BuildFormatProfile(unscanned);
    EXPECT_TRUE(HasFact(none, QStringLiteral("扫描状态"), QStringLiteral("未运行")));
    EXPECT_FALSE(none.has_precise_window);
}

TEST(StructureMapFormatTest, LegacyWindowAndUnusableByteViewsAreDeclared) {
    const detail::FormatProfile avi = detail::BuildFormatProfile(MakeAviResult());
    EXPECT_EQ(avi.family, detail::MapFormatFamily::LegacyBinary);
    EXPECT_TRUE(avi.window_limited);
    EXPECT_FALSE(avi.has_precise_window) << "只有 scan_complete=false 时不能画一条假的分界线";
    EXPECT_TRUE(avi.coverage_notes.join(QString()).contains(QStringLiteral("AVI 结构扫描未到文件尾")));
    EXPECT_TRUE(HasFact(avi, QStringLiteral("idx1 条目 / 坏偏移"), QStringLiteral("2")));

    const detail::FormatProfile hls = detail::BuildFormatProfile(MakeManifestResult());
    EXPECT_FALSE(detail::ViewSupported(hls, detail::MapViewKind::Treemap));
    EXPECT_TRUE(detail::ViewUnsupportedReason(hls, detail::MapViewKind::Treemap).contains(QStringLiteral("size")));
    EXPECT_TRUE(detail::ViewSupported(hls, detail::MapViewKind::PacketBands));
}

TEST(StructureMapFormatTest, FormatOverviewCardsAreHitTestableAndDecorationsAreNot) {
    const detail::FormatProfile profile = detail::BuildFormatProfile(MakeTsResult());
    const auto visuals = detail::BuildFormatVisuals(profile, kPlotArea);
    ASSERT_FALSE(visuals.empty());
    int cards = 0;
    int titles = 0;
    for (size_t i = 0; i < visuals.size(); ++i) {
        const detail::MapVisual& visual = visuals[i];
        EXPECT_TRUE(kPlotArea.contains(visual.rect)) << "卡片画出绘图区 " << i;
        if (visual.decorative) {
            ++titles;
            EXPECT_EQ(visual.fact_index, -1);
        } else {
            ASSERT_GE(visual.fact_index, 0);
            EXPECT_LT(visual.fact_index, static_cast<int>(profile.facts.size()));
            ++cards;
            EXPECT_EQ(detail::VisualAt(visuals, visual.rect.center()), static_cast<int>(i))
                << "卡片中心必须命中自己（index " << i << "）";
        }
    }
    EXPECT_GT(cards, 0);
    EXPECT_GT(titles, 0);
    // 装饰标题不参与命中：点它的正中间不该返回它自己。
    for (size_t i = 0; i < visuals.size(); ++i) {
        if (!visuals[i].decorative)
            continue;
        EXPECT_NE(detail::VisualAt(visuals, visuals[i].rect.center()), static_cast<int>(i));
    }

    const QByteArray svg =
        detail::BuildVisualsSvg(visuals, QSize(880, 470), detail::MapViewKind::FormatOverview, false);
    ASSERT_FALSE(svg.isEmpty());
    EXPECT_TRUE(svg.contains("view=format-overview"));
    EXPECT_TRUE(detail::BuildVisualsSvg({}, QSize(880, 470), detail::MapViewKind::FormatOverview, false).isEmpty());

    EXPECT_TRUE(detail::DescribeFormatFact(profile, 0).contains(QStringLiteral("实测值")));
    EXPECT_TRUE(detail::DescribeFormatFact(profile, 0).contains(profile.facts[0].label));
    EXPECT_TRUE(detail::DescribeFormatFact(profile, -1).isEmpty());
    EXPECT_TRUE(detail::DescribeFormatFact(profile, 9999).isEmpty());
}

TEST(StructureMapFormatTest, WindowOverlayFollowsViewportAndNeverBlocksNodes) {
    const detail::FormatProfile profile = detail::BuildFormatProfile(MakeEbmlResult());
    const detail::MapGeometry geom = TestGeometry();
    const auto overlay = detail::BuildWindowOverlay(profile, geom, detail::MapViewport{});
    ASSERT_EQ(overlay.size(), 1u);
    EXPECT_TRUE(overlay[0].decorative);
    EXPECT_EQ(overlay[0].fact_index, -1);
    EXPECT_NEAR(static_cast<double>(overlay[0].rect.left()), geom.left + profile.window_fraction * geom.width, 1.0);
    EXPECT_EQ(overlay[0].rect.right(), geom.left + geom.width - 1);

    // 缩放后边界跟着视窗走，不能钉在第一次的像素位置。
    const auto zoomed = detail::BuildWindowOverlay(profile, geom, detail::MapViewport{2.0, 0.0});
    ASSERT_EQ(zoomed.size(), 1u);
    EXPECT_GT(zoomed[0].rect.left(), overlay[0].rect.left());

    // 叠加层放进 visuals 后，节点矩形仍然能被命中（decorative 不挡）。
    const auto nodes = detail::CollectNodes(MakeEbmlResult(), 20, 4000, detail::MapLayoutMode::Balanced);
    std::vector<detail::MapVisual> visuals = detail::BuildIcicleVisuals(nodes, geom, detail::MapViewport{}, 0);
    visuals.insert(visuals.begin(), overlay.begin(), overlay.end());
    ASSERT_FALSE(visuals.empty());
    for (const detail::MapVisual& visual : visuals) {
        if (visual.decorative)
            continue;
        const int hit = detail::VisualAt(visuals, visual.rect.center());
        ASSERT_GE(hit, 0);
        EXPECT_EQ(visuals[static_cast<size_t>(hit)].node_index, visual.node_index);
    }

    // 没有精确窗口 / 窗口覆盖全文件时不画任何东西。
    EXPECT_TRUE(
        detail::BuildWindowOverlay(detail::BuildFormatProfile(MakeMapResult()), geom, detail::MapViewport{}).empty());
    EXPECT_TRUE(
        detail::BuildWindowOverlay(detail::BuildFormatProfile(MakeTsResult()), geom, detail::MapViewport{50.0, 0.0})
            .empty());
}

TEST(StructureMapFormatTest, EbmlBlockBandsUseByteAxisAndNeverPretendToBeATimeAxis) {
    const ContainerStructureResult r = MakeEbmlResult();
    const detail::PacketBandData bands = detail::BuildPacketBands(r, 16);
    ASSERT_TRUE(bands.available);
    EXPECT_EQ(bands.axis, detail::MapBandAxis::ByteOffset);
    EXPECT_EQ(bands.axis_label, QStringLiteral("文件字节偏移"));
    EXPECT_DOUBLE_EQ(bands.axis_max, static_cast<double>(r.ebml_detail.file_bytes));
    EXPECT_TRUE(bands.source.contains(QStringLiteral("EBML")));
    EXPECT_TRUE(bands.source.contains(QStringLiteral("不是时间轴")));
    EXPECT_TRUE(bands.coverage_note.contains(QStringLiteral("相对 Cluster")));
    EXPECT_TRUE(bands.sampled) << "块摘要有上限 / 树内块节点被裁剪，必须标成抽样";
    EXPECT_EQ(bands.tracks.size(), 2u) << "按 track_number 分轨";
    EXPECT_EQ(bands.TotalCount(), 6u);
    EXPECT_EQ(bands.BinCount(), 32);

    quint64 bytes = 0;
    for (const detail::PacketBandTrack& track : bands.tracks) {
        for (const detail::PacketBandBin& bin : track.bins)
            bytes += bin.bytes;
    }
    EXPECT_EQ(bytes, 21000u) << "1000+2000+...+6000 一个都不能丢";

    const auto visuals = detail::BuildPacketVisuals(bands, kPlotArea);
    ASSERT_FALSE(visuals.empty());
    EXPECT_EQ(detail::VisualAt(visuals, visuals.front().rect.center()), 0);
    const QString detail_text = detail::DescribeBandBin(bands, 0, 0);
    EXPECT_TRUE(detail_text.contains(QStringLiteral("字节区间详情")));
    EXPECT_TRUE(detail_text.contains(QStringLiteral("文件字节偏移")));
    EXPECT_TRUE(detail_text.contains(QStringLiteral("不适用"))) << "块摘要没有异常字段，别显示一个假的 0";

    // 反向守护：没有块摘要就回落成「没有可用数据」，axis 不能被上次的结果污染。
    ContainerStructureResult no_blocks = r;
    no_blocks.ebml_detail.blocks.clear();
    const detail::PacketBandData none = detail::BuildPacketBands(no_blocks, 16);
    EXPECT_FALSE(none.available);
    EXPECT_EQ(none.axis, detail::MapBandAxis::TimeSeconds);
    EXPECT_TRUE(none.source.contains(QStringLiteral("没有可用")));

    // 真实时间轴仍是时间轴，别被 EBML 分支带偏。
    const detail::PacketBandData samples = detail::BuildPacketBands(MakeSampleResult(), 10);
    EXPECT_EQ(samples.axis, detail::MapBandAxis::TimeSeconds);
    EXPECT_DOUBLE_EQ(samples.axis_max, 10.0);
    EXPECT_TRUE(detail::DescribeBandBin(samples, 0, 0).contains(QStringLiteral("时间桶详情")));
}

TEST(StructureMapFormatTest, JsonCarriesTheSameFormatProfileAsTheView) {
    const ContainerStructureResult r = MakeEbmlResult();
    const QByteArray json = detail::BuildStructureJson(r, detail::BuildCoverage(r, false, false));
    ASSERT_FALSE(json.isEmpty());
    const QJsonObject profile =
        QJsonDocument::fromJson(json).object().value(QStringLiteral("format_profile")).toObject();
    ASSERT_FALSE(profile.isEmpty());
    EXPECT_EQ(profile.value(QStringLiteral("family")).toInt(), static_cast<int>(detail::MapFormatFamily::Ebml));
    EXPECT_TRUE(profile.value(QStringLiteral("window_limited")).toBool());
    EXPECT_TRUE(profile.value(QStringLiteral("has_precise_window")).toBool());
    EXPECT_NEAR(profile.value(QStringLiteral("window_fraction")).toDouble(), 32.0 / 4096.0, 1e-9);
    EXPECT_GT(profile.value(QStringLiteral("facts")).toArray().size(), 5);
    EXPECT_FALSE(profile.value(QStringLiteral("coverage_notes")).toArray().isEmpty());
}

TEST(FileStructureMapPageTest, FourthViewExposesProfileAndKeepsHeaderContract) {
    EnsureApp();
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    auto* page = new FileStructureMapPage(true, &host);
    layout->addWidget(page);
    host.resize(1000, 700);
    host.show();
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_EQ(page->findChildren<QCheckBox*>().size(), 1);
    EXPECT_EQ(page->findChildren<QPushButton*>(QStringLiteral("ExportMenuButton")).size(), 1);
    QComboBox* views = page->findChild<QComboBox*>(QStringLiteral("StructureMapViewKind"));
    ASSERT_NE(views, nullptr);
    ASSERT_EQ(views->count(), 4);

    page->SetResult(MakeEbmlResult());
    EXPECT_EQ(page->Profile().family, detail::MapFormatFamily::Ebml);
    EXPECT_TRUE(page->Profile().window_limited);
    EXPECT_TRUE(page->Coverage().window_limited);
    EXPECT_TRUE(page->CoverageText().contains(QStringLiteral("窗口")));

    page->SetViewKind(detail::MapViewKind::FormatOverview);
    EXPECT_EQ(page->ViewKind(), detail::MapViewKind::FormatOverview);
    EXPECT_GT(page->VisualCount(), 0);
    page->SelectFact(0);
    EXPECT_EQ(page->SelectedFact(), 0);
    EXPECT_TRUE(page->DetailText().contains(page->Profile().facts[0].label)) << page->DetailText().toStdString();

    page->Clear();
    EXPECT_EQ(page->SelectedFact(), -1);
    EXPECT_TRUE(page->Profile().facts.empty());

    std::vector<std::string> findings;
    videoeye_test::CollectSiblingOverlaps(page, "file_structure_map", findings);
    videoeye_test::CollectDefaultGeometry(page, "file_structure_map", findings);
    EXPECT_TRUE(findings.empty());
}

TEST(FileStructureMapPageTest, UnsupportedViewsAreDisabledAndSilentlyFallBack) {
    EnsureApp();
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    auto* page = new FileStructureMapPage(true, &host);
    layout->addWidget(page);
    host.resize(1000, 700);
    host.show();
    QApplication::processEvents();

    page->SetResult(MakeTsResult());
    EXPECT_FALSE(page->ViewSupported(detail::MapViewKind::Treemap));
    EXPECT_TRUE(page->ViewUnsupportedNote().contains(QStringLiteral("TS")));
    EXPECT_TRUE(page->CoverageText().contains(QStringLiteral("采样汇总")));

    QComboBox* views = page->findChild<QComboBox*>(QStringLiteral("StructureMapViewKind"));
    ASSERT_NE(views, nullptr);
    auto* model = qobject_cast<QStandardItemModel*>(views->model());
    ASSERT_NE(model, nullptr);
    ASSERT_NE(model->item(static_cast<int>(detail::MapViewKind::Treemap)), nullptr);
    EXPECT_FALSE(model->item(static_cast<int>(detail::MapViewKind::Treemap))->isEnabled());

    // 直接调接口也要回退，不留下「以为切了、其实画不出来」的状态。
    page->SetViewKind(detail::MapViewKind::Treemap);
    EXPECT_EQ(page->ViewKind(), detail::MapViewKind::Icicle);
    EXPECT_EQ(views->currentIndex(), static_cast<int>(detail::MapViewKind::Icicle));

    // 没有可比较的 size 时，「字节占比」也被禁用并自动回到均衡布局。
    page->SetLayoutMode(detail::MapLayoutMode::ByteProportional);
    EXPECT_EQ(page->LayoutMode(), detail::MapLayoutMode::Balanced);

    // 换成正常 MP4 后置灰解除。
    page->SetResult(MakeMapResult());
    EXPECT_TRUE(page->ViewSupported(detail::MapViewKind::Treemap));
    EXPECT_TRUE(model->item(static_cast<int>(detail::MapViewKind::Treemap))->isEnabled());
    page->SetViewKind(detail::MapViewKind::Treemap);
    EXPECT_EQ(page->ViewKind(), detail::MapViewKind::Treemap);
}

TEST(StructureMapFormatTest, LargeTreeStaysWithinDrawBudgets) {
    QElapsedTimer timer;
    timer.start();
    ContainerStructureResult r;
    r.valid = true;
    r.format = ContainerFormat::MP4;
    r.format_name = "MP4";
    for (int i = 0; i < 5000; ++i) {
        ContainerElement top = Element("box" + std::to_string(i), static_cast<quint64>(i) * 4096, 4096, 0);
        for (int j = 0; j < 3; ++j)
            top.children.push_back(
                Element("child" + std::to_string(j), static_cast<quint64>(i) * 4096 + j * 64, 64, 1));
        r.element_tree.push_back(std::move(top));
    }

    // 20000 个节点：收集预算必须封顶，多要一个用于判裁剪也不能失控。
    const auto nodes =
        detail::CollectNodes(r, detail::kMaxTreeDepth, detail::kMaxMapNodes, detail::MapLayoutMode::Balanced);
    EXPECT_EQ(nodes.size(), static_cast<size_t>(detail::kMaxMapNodes));
    EXPECT_EQ(detail::CollectNodes(r, detail::kMaxTreeDepth, detail::kMaxMapNodes + 1).size(),
              static_cast<size_t>(detail::kMaxMapNodes + 1));

    const auto treemap = detail::BuildTreemapVisuals(nodes, kPlotArea, detail::kMaxMapNodes);
    EXPECT_LE(treemap.size(), static_cast<size_t>(detail::kMaxMapNodes));
    for (const detail::MapVisual& visual : treemap)
        EXPECT_TRUE(kPlotArea.contains(visual.rect));

    const detail::FormatProfile profile = detail::BuildFormatProfile(r);
    EXPECT_LE(detail::BuildFormatVisuals(profile, kPlotArea).size(), profile.facts.size() + 4);

    // 20 万个样本：桶数仍由预算决定，不能按样本数建矩形。
    ContainerStructureResult samples = MakeSampleResult();
    samples.mp4_samples.tracks[0].samples.clear();
    for (int i = 0; i < 200000; ++i) {
        Mp4Sample sample;
        sample.index = static_cast<uint32_t>(i);
        sample.cts = i * 25;
        sample.dts = i * 25;
        sample.duration = 25;
        sample.size = 512;
        samples.mp4_samples.tracks[0].samples.push_back(sample);
    }
    const detail::PacketBandData bands = detail::BuildPacketBands(samples, 160);
    EXPECT_TRUE(bands.available);
    EXPECT_EQ(bands.BinCount(), 160);
    EXPECT_EQ(bands.TotalCount(), 200000u);
    EXPECT_LE(detail::BuildPacketVisuals(bands, kPlotArea).size(), 160u);

    // 宽松上限只用来拦「指数级退化」，不是精确的性能基线（CI 机器可能更慢）。
    EXPECT_LT(timer.elapsed(), 15000) << "纯内存投影不该退化到秒级以上";
}

// ---------------------------------------------------------------------------
// 流字节分布（用户问题：「#0 video / #1 audio 占满整行，数据不该在头部之后吗」）
// ---------------------------------------------------------------------------
// 真实 MP4：ftyp(0..32) + moov(32..4128) 是头部，mdat(4128..804128) 里按时间
// 交错存放 video / audio 样本。旧版把 2 条流按画布宽度等分横铺，读起来是
// 「video 占前半文件、audio 占后半文件」—— 这里钉死「按实测 offset 画」。
namespace {

ContainerStructureResult MakeInterleavedResult() {
    ContainerStructureResult r = MakeMapResult();
    r.mp4_samples.valid = true;
    r.mp4_samples.file_size = 808224;
    r.mp4_samples.first_mdat_offset = 4128;
    r.mp4_samples.first_mdat_size = 800000;
    r.mp4_samples.moov_before_mdat = true;

    Mp4TrackSampleTable video;
    video.track_id = 1;
    video.type = "video";
    video.codec = "avc1";
    video.media_timescale = 1000;
    video.media_duration = 10000;
    video.stsz_sample_count = 4;
    const quint64 video_offsets[] = {4128, 102128, 202128, 302128};
    for (int i = 0; i < 4; ++i) {
        Mp4Sample sample;
        sample.index = static_cast<uint32_t>(i);
        sample.offset = video_offsets[i];
        sample.size = 50000;
        sample.cts = i * 2500;
        sample.dts = i * 2500;
        sample.duration = 2500;
        sample.keyframe = (i % 2 == 0);
        video.samples.push_back(sample);
    }
    Mp4TrackSampleTable audio;
    audio.track_id = 2;
    audio.type = "audio";
    audio.codec = "mp4a";
    audio.media_timescale = 48000;
    audio.media_duration = 480000;
    audio.stsz_sample_count = 4;
    const quint64 audio_offsets[] = {52128, 152128, 252128, 352128};
    for (int i = 0; i < 4; ++i) {
        Mp4Sample sample;
        sample.index = static_cast<uint32_t>(i);
        sample.offset = audio_offsets[i];
        sample.size = 8000;
        sample.cts = i * 12000;
        sample.dts = i * 12000;
        sample.duration = 12000;
        audio.samples.push_back(sample);
    }
    r.mp4_samples.tracks.push_back(video);
    r.mp4_samples.tracks.push_back(audio);
    return r;
}

const QRect kStreamBandArea(40, 400, 800, 30);

// 模拟真实文件的每轨展开上限：video 全展开，audio 声明 40 个只展开前 4 个。
// q.mp4 实测 video 20000/22593、audio 20000/42362 —— 音频包更密，
// 同样上限只覆盖半份文件；右侧空白是「未展开」，不是「没有音频」。
ContainerStructureResult MakeTruncatedStreamResult() {
    ContainerStructureResult r = MakeInterleavedResult();
    r.mp4_samples.tracks[1].stsz_sample_count = 40;
    r.mp4_samples.tracks[1].samples_truncated = true;
    return r;
}

// 某条轨第一个 / 最后一个有样本的桶；没有样本返回 -1。
std::pair<int, int> NonEmptyBinRange(const detail::StreamByteRow& row) {
    int first = -1;
    int last = -1;
    for (size_t i = 0; i < row.bins.size(); ++i) {
        if (row.bins[i].count == 0)
            continue;
        if (first < 0)
            first = static_cast<int>(i);
        last = static_cast<int>(i);
    }
    return {first, last};
}

} // namespace

TEST(FileStructureMapPageTest, StreamByteRowsUseRealSampleOffsetsAfterHeaders) {
    const ContainerStructureResult r = MakeInterleavedResult();
    const detail::StreamByteRows rows = detail::BuildStreamByteRows(r, 32);

    ASSERT_TRUE(rows.available);
    ASSERT_EQ(rows.rows.size(), 2u);
    EXPECT_EQ(rows.bins, 32);
    EXPECT_EQ(rows.file_span, detail::FileSpan(r));
    EXPECT_EQ(rows.TotalCount(), 8u);
    EXPECT_EQ(rows.out_of_range_samples, 0u);
    EXPECT_EQ(rows.source.contains(QStringLiteral("stco")), true);

    // 用户的问题：样本数据确实在头部（ftyp / moov）之后，落在 mdat 区间里。
    EXPECT_GE(rows.min_offset, r.mp4_samples.first_mdat_offset);
    EXPECT_LT(rows.min_offset, r.mp4_samples.first_mdat_offset + r.mp4_samples.first_mdat_size);
    EXPECT_GT(rows.max_offset, rows.min_offset);
    EXPECT_LE(rows.max_offset, r.mp4_samples.file_size);

    // 两轨交错，而不是「video 一整块 + audio 一整块」前后排列。
    const auto video_range = NonEmptyBinRange(rows.rows[0]);
    const auto audio_range = NonEmptyBinRange(rows.rows[1]);
    ASSERT_GE(video_range.first, 0);
    ASSERT_GE(audio_range.first, 0);
    EXPECT_LT(audio_range.first, video_range.second);
    EXPECT_LT(video_range.first, audio_range.second);
    EXPECT_EQ(rows.rows[0].type, "video");
    EXPECT_EQ(rows.rows[1].type, "audio");
    EXPECT_EQ(rows.rows[0].total_bytes, 200000u);
    EXPECT_EQ(rows.rows[1].total_bytes, 32000u);
}

TEST(FileStructureMapPageTest, StreamByteVisualsOnlyDrawBinsThatHaveSamples) {
    const detail::StreamByteRows rows = detail::BuildStreamByteRows(MakeInterleavedResult(), 32);
    const auto visuals = detail::BuildStreamByteVisuals(rows, kStreamBandArea, detail::MapViewport{}, 14);
    ASSERT_FALSE(visuals.empty());
    // 8 个样本最多点亮 8 个桶；等分横铺的旧画法会产出 2 个铺满半幅的矩形。
    EXPECT_LE(visuals.size(), 8u);
    EXPECT_LT(visuals.size(), static_cast<size_t>(rows.bins) * rows.rows.size());
    for (const detail::MapVisual& visual : visuals) {
        EXPECT_GE(visual.stream_row, 0);
        EXPECT_GE(visual.stream_bin, 0);
        EXPECT_EQ(visual.node_index, -1);
        EXPECT_TRUE(kStreamBandArea.contains(visual.rect)) << "矩形画出流带";
        EXPECT_GT(visual.bar_height, 0);
        // 绝不铺满整行：单个桶矩形远窄于绘图区宽度。
        EXPECT_LT(visual.rect.width(), kStreamBandArea.width() / 4);
    }

    // 第一个 video 样本 offset=4128 → bin 0，左边缘必须贴着绘图区左端；
    // 第一个 audio 样本 offset=52128 → bin 2，左边缘按文件跨度换算。
    EXPECT_EQ(visuals.front().rect.left(), kStreamBandArea.left());
    int audio_left = -1;
    for (const detail::MapVisual& visual : visuals) {
        if (visual.stream_row == 1) {
            audio_left = visual.rect.left();
            break;
        }
    }
    const int expected_audio_left =
        kStreamBandArea.left() + static_cast<int>(std::lround(kStreamBandArea.width() * 2.0 / rows.bins));
    EXPECT_EQ(audio_left, expected_audio_left);

    // 缩放后同一批桶跟着视窗移动，屏幕外的不建矩形。
    const auto zoomed = detail::BuildStreamByteVisuals(rows, kStreamBandArea, detail::MapViewport{4.0, 0.0}, 14);
    EXPECT_LT(zoomed.size(), visuals.size()) << "放大后屏幕外的桶必须被裁掉";
    for (const detail::MapVisual& visual : zoomed)
        EXPECT_TRUE(kStreamBandArea.contains(visual.rect));

    // 导出与屏幕共用同一份矩形；结构图不能再被标成包带视图。
    const QByteArray svg = detail::BuildVisualsSvg(visuals, QSize(880, 470), detail::MapViewKind::Icicle, false);
    ASSERT_FALSE(svg.isEmpty());
    EXPECT_TRUE(svg.contains("view=structure-map"));
    EXPECT_GE(svg.count("<rect"), static_cast<int>(visuals.size()));
}

TEST(FileStructureMapPageTest, NoSampleOffsetsProducesNoStreamBand) {
    // 没有展开的样本表：流只是逻辑轨道，一个矩形都不许画，也不许产详情。
    const detail::StreamByteRows plain = detail::BuildStreamByteRows(MakeMapResult(), 32);
    EXPECT_FALSE(plain.available);
    EXPECT_TRUE(plain.rows.empty());
    EXPECT_TRUE(plain.coverage_note.contains(QStringLiteral("不绘制")));
    EXPECT_TRUE(detail::BuildStreamByteVisuals(plain, kStreamBandArea, detail::MapViewport{}, 14).empty());
    EXPECT_TRUE(detail::DescribeStreamByteBin(plain, 0, 0).isEmpty());

    ContainerStructureResult failed = MakeInterleavedResult();
    failed.valid = false;
    EXPECT_FALSE(detail::BuildStreamByteRows(failed, 32).available);

    ContainerStructureResult no_tracks = MakeInterleavedResult();
    no_tracks.mp4_samples.tracks.clear();
    EXPECT_FALSE(detail::BuildStreamByteRows(no_tracks, 32).available);
}

TEST(FileStructureMapPageTest, StreamByteRowsReportTruncationAndOutOfRange) {
    ContainerStructureResult r = MakeInterleavedResult();
    r.mp4_samples.tracks[0].samples_truncated = true;
    const detail::StreamByteRows truncated = detail::BuildStreamByteRows(r, 32);
    ASSERT_TRUE(truncated.available);
    EXPECT_TRUE(truncated.rows[0].truncated);
    EXPECT_TRUE(truncated.coverage_note.contains(QStringLiteral("截断")));
    EXPECT_TRUE(detail::DescribeStreamByteBin(truncated, 0, 0).contains(QStringLiteral("截断")));

    r.mp4_samples.tracks[1].samples[0].offset = 900000; // 越过文件跨度
    const detail::StreamByteRows out_of_range = detail::BuildStreamByteRows(r, 32);
    EXPECT_EQ(out_of_range.out_of_range_samples, 1u);
    EXPECT_TRUE(out_of_range.coverage_note.contains(QStringLiteral("越过")));
    // 越界样本不能凭空消失：仍计入总样本数，只是被归入末端桶。
    EXPECT_EQ(out_of_range.TotalCount(), 8u);
}

TEST(FileStructureMapPageTest, TruncatedTrackMarksUnexpandedTailAsUnknownNotEmpty) {
    const ContainerStructureResult r = MakeTruncatedStreamResult();
    const detail::StreamByteRows rows = detail::BuildStreamByteRows(r, 32);
    ASSERT_TRUE(rows.available);
    ASSERT_EQ(rows.rows.size(), 2u);
    EXPECT_TRUE(rows.any_truncated);
    EXPECT_EQ(rows.rows[0].unexpanded_samples, 0u);
    EXPECT_EQ(rows.rows[1].declared_samples, 40u);
    EXPECT_EQ(rows.rows[1].expanded_samples, 4u);
    EXPECT_EQ(rows.rows[1].unexpanded_samples, 36u);
    EXPECT_EQ(rows.rows[1].last_expanded_end, 360128u);
    EXPECT_GT(rows.rows[1].expanded_x1, 0.0);
    EXPECT_LT(rows.rows[1].expanded_x1, 1.0);
    EXPECT_TRUE(rows.truncation_note.contains(QStringLiteral("#1 4/40")));
    EXPECT_TRUE(rows.coverage_note.contains(QStringLiteral("未展开")));
    EXPECT_TRUE(rows.coverage_note.contains(QStringLiteral("不代表没有数据")));

    const auto visuals = detail::BuildStreamByteVisuals(rows, kStreamBandArea, detail::MapViewport{}, 14);
    int hatched = 0;
    for (const detail::MapVisual& visual : visuals) {
        if (!visual.hatched)
            continue;
        ++hatched;
        EXPECT_EQ(visual.stream_row, 1);
        EXPECT_EQ(visual.stream_bin, -1);
        EXPECT_TRUE(visual.decorative) << "未展开区不能抢走桶的命中";
        EXPECT_TRUE(visual.label.contains(QStringLiteral("未展开")));
        EXPECT_TRUE(visual.label.contains(QStringLiteral("36")));
        EXPECT_GT(visual.rect.left(), kStreamBandArea.left() + kStreamBandArea.width() / 4);
        EXPECT_EQ(visual.rect.right(), kStreamBandArea.right());
        EXPECT_TRUE(kStreamBandArea.contains(visual.rect));
    }
    EXPECT_EQ(hatched, 1) << "只给被截断的音频轨画未展开区";

    // 导出与屏幕同源：SVG 里也必须能看出右侧是未展开，而不是空白。
    const QByteArray svg = detail::BuildVisualsSvg(visuals, QSize(880, 470), detail::MapViewKind::Icicle, false);
    ASSERT_FALSE(svg.isEmpty());
    EXPECT_TRUE(svg.contains(QStringLiteral("未展开").toUtf8()));

    // 结构 JSON 同样带出每轨声明/展开数，下游不会把截断密度当成全文件。
    const QJsonObject sample_table =
        QJsonDocument::fromJson(detail::BuildStructureJson(r, detail::BuildCoverage(r, false, false)))
            .object()
            .value(QStringLiteral("sample_table"))
            .toObject();
    ASSERT_FALSE(sample_table.isEmpty());
    const QJsonArray tracks = sample_table.value(QStringLiteral("tracks")).toArray();
    ASSERT_EQ(tracks.size(), 2);
    EXPECT_EQ(tracks.at(1).toObject().value(QStringLiteral("unexpanded_samples")).toInt(), 36);
    EXPECT_EQ(tracks.at(0).toObject().value(QStringLiteral("unexpanded_samples")).toInt(), 0);
}

TEST(FileStructureMapPageTest, FullyExpandedTracksHaveNoUnexpandedOverlay) {
    // 反向守护：没有截断就不许画斜纹，否则「未展开」会变成与事实无关的装饰。
    const detail::StreamByteRows rows = detail::BuildStreamByteRows(MakeInterleavedResult(), 32);
    EXPECT_FALSE(rows.any_truncated);
    EXPECT_TRUE(rows.truncation_note.isEmpty());
    for (const detail::StreamByteRow& row : rows.rows)
        EXPECT_EQ(row.unexpanded_samples, 0u);

    const auto visuals = detail::BuildStreamByteVisuals(rows, kStreamBandArea, detail::MapViewport{}, 14);
    EXPECT_LE(visuals.size(), 8u);
    for (const detail::MapVisual& visual : visuals) {
        EXPECT_FALSE(visual.hatched);
        EXPECT_FALSE(visual.decorative);
        EXPECT_GE(visual.stream_bin, 0);
    }
}

TEST(FileStructureMapPageTest, PageWiresStreamByteRowsAndClearsThemOnFileChange) {
    EnsureApp();
    FileStructureMapPage page(true);
    page.SetResult(MakeInterleavedResult());

    const detail::StreamByteRows rows = page.StreamRows();
    ASSERT_TRUE(rows.available);
    EXPECT_GE(rows.min_offset, 4128u);
    // 页面默认走「均衡布局」，必须把样本投影到 mdat 的布局区间，而不是留在最左端。
    EXPECT_GT(NonEmptyBinRange(rows.rows[0]).first, 0) << "页面漏传布局投影";
    page.SelectStreamBin(0, 0);
    EXPECT_TRUE(page.DetailText().contains(QStringLiteral("样本字节区间")));

    // 截断结果接进页面后同样要能看到「未展开」，不能只剩一条变短的色带。
    page.SetResult(MakeTruncatedStreamResult());
    ASSERT_TRUE(page.StreamRows().any_truncated);
    ASSERT_EQ(page.StreamRows().rows.size(), 2u);
    EXPECT_EQ(page.StreamRows().rows[1].unexpanded_samples, 36u);

    // 换到没有样本偏移的文件：底部必须回到「不绘制」，旧文件的色带不能留着。
    page.SetResult(MakeMapResult());
    EXPECT_FALSE(page.StreamRows().available);
    EXPECT_FALSE(page.DetailText().contains(QStringLiteral("样本字节区间")));
    page.Clear();
    EXPECT_FALSE(page.StreamRows().available);
}

TEST(FileStructureMapPageTest, BalancedLayoutProjectsStreamBinsAwayFromHeaders) {
    const ContainerStructureResult r = MakeInterleavedResult();
    const auto balanced_nodes = detail::CollectNodes(r, 2, 512, detail::MapLayoutMode::Balanced);
    const auto projection = detail::BuildLayoutProjection(balanced_nodes);
    ASSERT_TRUE(projection.valid());

    const detail::MapNode* mdat = nullptr;
    for (const detail::MapNode& node : balanced_nodes) {
        if (node.depth == 0 && node.name == QStringLiteral("mdat")) {
            mdat = &node;
            break;
        }
    }
    ASSERT_NE(mdat, nullptr);
    const double first_sample = 4128.0 / static_cast<double>(detail::FileSpan(r));
    // 样本偏移投影后必须落进 mdat 的布局区间，而不是留在头部那一段。
    EXPECT_GE(projection.Project(first_sample), mdat->x0 - 1e-6);
    EXPECT_LE(projection.Project(first_sample), mdat->x1 + 1e-6);
    const detail::MapLayoutProjection identity;
    EXPECT_GT(projection.Project(first_sample), identity.Project(first_sample))
        << "均衡布局必须真的改变坐标，否则下面的对齐断言等于没测";
    double prev = -1.0;
    for (int i = 0; i <= 32; ++i) {
        const double v = projection.Project(i / 32.0);
        EXPECT_GE(v, 0.0);
        EXPECT_LE(v, 1.0);
        EXPECT_GE(v, prev - 1e-9) << "投影必须单调，否则样本会落错 box";
        prev = v;
    }

    // 分桶跟着布局走：均衡布局下样本不再落在最左侧的头部桶里。
    const auto identity_rows = detail::BuildStreamByteRows(r, 32);
    const auto balanced_rows = detail::BuildStreamByteRows(r, 32, projection);
    ASSERT_TRUE(identity_rows.available);
    ASSERT_TRUE(balanced_rows.available);
    EXPECT_EQ(balanced_rows.TotalCount(), identity_rows.TotalCount());
    const int identity_first = NonEmptyBinRange(identity_rows.rows[0]).first;
    const int balanced_first = NonEmptyBinRange(balanced_rows.rows[0]).first;
    ASSERT_GE(identity_first, 0);
    EXPECT_GT(balanced_first, identity_first);

    // 桶是布局分段，详情只能报实测字节范围，不能由「桶宽 × file_span」反推。
    const QString text = detail::DescribeStreamByteBin(balanced_rows, 0, balanced_first);
    EXPECT_TRUE(text.contains(QStringLiteral("实测字节范围")));
    EXPECT_TRUE(text.contains(detail::FormatBytes(4128)));
}
