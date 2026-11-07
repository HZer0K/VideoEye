#pragma once

// 文件结构图谱页（切片 4）：把 ContainerStructureResult 投影成四种互补的大图。
//
// 与「文件结构」页的分工：那边是树 + 表格，负责逐个节点精确查阅；这边只回答
// "整个文件长什么样" —— 顶层布局、占比、层级深度、流分布、时间轴密度。
// 两页消费同一份 ContainerStructureResult，由 AnalysisPanel 在 ContainerStructureReady
// 时分别转发；跨页跳转也不互持指针，一律经 AnalysisPanel -> MainWindow 编排。
//
// 三个视图（structure_map_detail::MapViewKind）：
//   Icicle      层级带，宽度可选「均衡布局 / 字节占比」（切片 1+2）
//   Treemap     按压缩字节权重递归二分的面积图，回答"有哪些盒子、谁大谁小"
//   PacketBands 按已持久化的 MP4 样本表 / 流媒体清单 / EBML 块摘要分桶的密度带
//   FormatOverview 逐格式事实卡片：扫描窗口、TS 传输摘要、清单层级、legacy 体检
//
// ⭐ 不撒谎的五条底线：
//   1) 逐包数据 demux 完就释放了，包带只能来自样本表 / 分片清单 / 块摘要，且必须标注抽样；
//      EBML 块只有相对 Cluster 的 timecode，只能按字节偏移分桶，不能冒充时间轴；
//   2) 结构骨架 ≠ 载荷 / 逐包结论，完整性事实由 BuildCoverage 分五维落，不合成"完成"；
//   3) 无事实不产文件 —— CSV / SVG / JSON 对无效或空结果一律返回空；
//   4) 按实测 size 判断能不能画面积图 / 字节占比；TS 与清单没有可比较的 size，视图直接置灰；
//   5) 流是逻辑轨道、不带 offset/size。底部色带只能来自 Mp4Sample::offset（交错存于 mdat），
//      绝不把 N 条流按画布宽度等分横铺 —— 那会读成「video 占前半、audio 占后半」。
//
// 数据变换、视窗变换、几何与命中都在 StructureMapModel.h 的 structure_map_detail
// 里并公开：自绘最容易出的不是崩溃，而是"画的位置和点中的位置不是一回事"、
// "空结果导出成一份看起来正常的空表"，这些做成纯函数才能被单测直接钉住。

#include <QByteArray>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <vector>

#include "ui/analysis_panel/StructureMapFormat.h"
#include "ui/analysis_panel/StructureMapModel.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;

namespace videoeye {
namespace ui {

class StructureMapCanvas;

class FileStructureMapPage : public QWidget {
    Q_OBJECT

public:
    // feature_checked: 页头「启用分析」的初值（复用容器结构这个 feature 的当前值）。
    // 本页的开关只 gate 本页是否绘制：容器解析在打开文件时已经自动跑完，
    // 关掉开关不会改动全局 feature，也不会丢已收到的结果。
    explicit FileStructureMapPage(bool feature_checked, QWidget* parent = nullptr);

    // 打开文件时容器解析完成 / 失败 / 换文件。三者都只碰本页自己的状态。
    void SetResult(const model::ContainerStructureResult& result);
    void SetError(const QString& message);
    void Clear();

    const model::ContainerStructureResult& result() const {
        return result_;
    }
    // 图上实际画到的节点数（受绘制预算裁剪后的值）。
    int NodeCount() const;
    bool HasData() const;
    bool DrawingEnabled() const;
    QString StatusText() const;

    // 只读状态快照，供页面级单测断言交互结果（不依赖像素）。
    double ViewScale() const;
    structure_map_detail::MapLayoutMode LayoutMode() const;
    structure_map_detail::MapViewKind ViewKind() const;
    int SelectedNode() const;
    int HitCount() const;
    QString DetailText() const;
    QString SearchQuery() const;
    QString CoverageText() const;
    structure_map_detail::MapCoverage Coverage() const;
    structure_map_detail::PacketBandData Bands() const;
    structure_map_detail::FormatProfile Profile() const;
    structure_map_detail::StreamByteRows StreamRows() const;
    int VisualCount() const;
    int SelectedFact() const;
    // 该视图对本格式是否可用（TS / 清单没有可比较的 size，面积图会失真）。
    bool ViewSupported(structure_map_detail::MapViewKind kind) const;
    QString ViewUnsupportedNote() const;

    // 供测试与工具栏共用的动作入口。
    void SetSearchQuery(const QString& query);
    void SelectNode(int index);
    void SelectFact(int index);             // 格式概览卡片（测试与工具条共用）
    void SelectStreamBin(int row, int bin); // 底部流字节分布桶
    void ResetView();
    void FocusNode(int index);
    void SetLayoutMode(structure_map_detail::MapLayoutMode mode);
    void SetViewKind(structure_map_detail::MapViewKind kind);
    void SetVisibleLayers(int layers); // 0 = 全部
    // 把当前选中节点定位到「文件结构」树页。无选中节点时什么都不做。
    void RequestJumpToTree();

signals:
    // 跨页联动：路径 + 偏移 + 大小是身份，不是行号。由 AnalysisPanel 上抛给
    // MainWindow::ShowPageById（它负责同步侧栏高亮），再回调面板选中树节点。
    void StructureNodeJumpRequested(const QString& path, quint64 offset, quint64 size);

private:
    void SetupUi();
    void Refresh();
    void RebuildNodes();
    void RebuildDepthCombo();
    void ApplyViewAvailability();
    void UpdateDetail();
    void UpdateHitLabel();
    void UpdateCoverageUi();
    void OnExportCsv();
    void OnExportSvg();
    void OnExportJson();

    QLabel* title_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* coverage_label_ = nullptr;
    QCheckBox* feature_toggle_ = nullptr;
    QComboBox* view_combo_ = nullptr;
    QComboBox* layout_combo_ = nullptr;
    QComboBox* depth_combo_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    QLabel* hit_label_ = nullptr;
    QLabel* detail_label_ = nullptr;
    StructureMapCanvas* canvas_ = nullptr;

    model::ContainerStructureResult result_;
    std::vector<structure_map_detail::MapNode> nodes_;
    structure_map_detail::PacketBandData bands_;
    structure_map_detail::StreamByteRows stream_rows_;
    structure_map_detail::MapCoverage coverage_;
    structure_map_detail::FormatProfile profile_;
    QVector<int> hits_;
    int hit_cursor_ = -1;
    int selected_ = -1;
    int selected_bin_track_ = -1;
    int selected_bin_index_ = -1;
    int selected_fact_ = -1;
    int selected_stream_row_ = -1;
    int selected_stream_bin_ = -1;
    int visible_layers_ = 0; // 0 = 全部
    int tree_depth_ = 0;
    quint64 file_span_ = 0;
    structure_map_detail::MapLayoutMode mode_ = structure_map_detail::MapLayoutMode::Balanced;
    structure_map_detail::MapViewKind view_kind_ = structure_map_detail::MapViewKind::Icicle;
    bool nodes_truncated_ = false;
    bool depth_truncated_ = false;
};

} // namespace ui
} // namespace videoeye
