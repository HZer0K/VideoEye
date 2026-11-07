#include "ui/analysis_panel/AnalysisPageFactory.h"

#include <QFrame>
#include <QVBoxLayout>

#include "core/domain/model/QcReport.h"
#include "infrastructure/logging/Logger.h"

// 建页需要完整类型（工厂头只做前向声明，见那里的说明）。
#include "ui/analysis_panel/AudioQcPage.h"
#include "ui/analysis_panel/BitrateGopPage.h"
#include "ui/analysis_panel/ColorHdrPage.h"
#include "ui/analysis_panel/ContainerStructurePage.h"
#include "ui/analysis_panel/DiagnosticsPage.h"
#include "ui/analysis_panel/EventTimelineView.h"
#include "ui/analysis_panel/FileStructureMapPage.h"
#include "ui/analysis_panel/FramePacketView.h"
#include "ui/analysis_panel/MacroblockView.h"
#include "ui/analysis_panel/SceneChangePage.h"
#include "ui/analysis_panel/StreamOverviewView.h"
#include "ui/analysis_panel/SubtitleAuxPage.h"
#include "ui/analysis_panel/VisualDefectPage.h"
#include "ui/bitstream_panel/BitstreamPanel.h"
#include "ui/reporting_panel/ReportingPanel.h"
#include "ui/streaming_panel/StreamingPanel.h"

namespace videoeye {
namespace ui {
namespace {

// 视图内部的开关编号 -> 全局 AnalysisFeature。
//
// 每个视图各有一张这样的表（播放统计页 4 项：流统计 / 视频帧 / 音频帧 / 数据包、
// 事件时间轴 3 项、宏块 1 项）。以前是几段 switch 分写在各个 SetupXxx 里，加一项要
// 记得改两处（读 + 写）且两处必须一致 —— 写错一边就会出现"界面上开着、数据被过滤掉"。
// 现在表只有一份，读与写共用。
using FeatureTable = std::vector<model::AnalysisFeature>;

template <typename ViewT>
void InjectIndexedHooks(ViewT* view, const PageBuildDeps& deps, FeatureTable table) {
    view->SetFeatureHooks(
        [deps, table](int view_feature) {
            if (view_feature < 0 || view_feature >= static_cast<int>(table.size()))
                return true;
            return deps.is_feature_on(table[view_feature]);
        },
        [deps, table](int view_feature, bool enabled) {
            if (view_feature < 0 || view_feature >= static_cast<int>(table.size()))
                return;
            deps.set_feature(table[view_feature], enabled);
        });
}

// 单个页面的建页函数。签名统一，才能按 id 查表分派。
//
// 以前这里是一条 14 个 else-if 的分发链：加一页要往下补一截，漏补不会有任何
// 编译期或测试期提示（只在运行时 LOG_ERROR 后 continue）。现在"id -> 建页函数"
// 是一张静态表，缺项由 BuildAnalysisPages 统一报出，新增页面只需加一行。
using PageBuilder = QWidget* (*)(AnalysisPages& pages, const PageBuildDeps& deps, const QString& path);

// 「播放统计与帧包」：顶部流概览（固定高度）+ 分隔条 + 底部帧/包明细（可伸展）。
QWidget* BuildPlaybackStatsPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.stream_overview = new StreamOverviewView(deps.parent);
    pages.frame_packet = new FramePacketView(deps.parent);
    pages.frame_packet->setMinimumHeight(360);

    InjectIndexedHooks(pages.stream_overview, deps,
                       {model::AnalysisFeature::StreamStats, model::AnalysisFeature::VideoFrame,
                        model::AnalysisFeature::AudioFrame, model::AnalysisFeature::Packet});

    // GOP 摘要由「帧/包明细」从解码帧 pict_type 推导产出，「流概览」区拿它填
    // 「最大GOP大小」并画「GOP 帧数分布」曲线。两个视图互不认识，只在建页处连一次
    // —— 直接互相持有指针会让两者无法单独构造 / 单独测试。
    //
    // 捕获两个裸指针而不是 pages 指针：AnalysisPages 是按值返回给面板的结构体，
    // 捕获它的地址会在返回后悬空；控件指针本身是稳定的。
    StreamOverviewView* overview = pages.stream_overview;
    FramePacketView* frames = pages.frame_packet;
    QObject::connect(frames, &FramePacketView::GopSummariesChanged, deps.signal_context,
                     [overview, frames]() { overview->SetGopSummaries(frames->GopSummaries()); });

    // 下方四个明细子页的导出并入页头那一个「导出」菜单：一个页面只留一个导出入口，
    // 而不是顶部流概览一个、每张子表再各一个。
    overview->AddExportAction(
        StreamOverviewView::tr("视频帧明细 CSV"), [frames]() { frames->ExportVideoFrameCsv(); },
        StreamOverviewView::tr("导出视频帧明细表（帧类型 / 时间 / PTS / GOP 归属）"));
    overview->AddExportAction(
        StreamOverviewView::tr("GOP 摘要 CSV"), [frames]() { frames->ExportGopCsv(); },
        StreamOverviewView::tr("导出按解码帧类型推导的 GOP 分段统计"));
    overview->AddExportAction(
        StreamOverviewView::tr("音频帧明细 CSV"), [frames]() { frames->ExportAudioFrameCsv(); },
        StreamOverviewView::tr("导出音频帧明细表（样本数 / 采样率 / 声道 / 字节）"));
    overview->AddExportAction(
        StreamOverviewView::tr("数据包明细 CSV"), [frames]() { frames->ExportPacketCsv(); },
        StreamOverviewView::tr("导出包明细表（流索引 / 类型 / PTS / DTS / 大小）"));

    QWidget* page = new QWidget(deps.parent);
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(overview);

    QFrame* sep = new QFrame(page);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    layout->addWidget(frames, 1);
    return page;
}

QWidget* BuildEventTimelinePage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.event_timeline = new EventTimelineView(deps.parent);
    InjectIndexedHooks(
        pages.event_timeline, deps,
        {model::AnalysisFeature::Event, model::AnalysisFeature::SyncSample, model::AnalysisFeature::Timeline});
    return pages.event_timeline;
}

QWidget* BuildFileStructurePage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.container =
        new ContainerStructurePage(deps.is_feature_on(model::AnalysisFeature::ContainerStructure), deps.parent);
    return pages.container;
}

// 「文件结构图谱」：与容器结构页共用 ContainerStructure 这个 feature 的初值，
// 但开关只 gate 本页绘制（见 FileStructureMapPage.h 的说明），不往面板发信号。
QWidget* BuildFileStructureMapPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.structure_map =
        new FileStructureMapPage(deps.is_feature_on(model::AnalysisFeature::ContainerStructure), deps.parent);
    return pages.structure_map;
}

QWidget* BuildMotionVectorsPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.macroblock = new MacroblockView(deps.parent);
    InjectIndexedHooks(pages.macroblock, deps, {model::AnalysisFeature::Macroblock});
    return pages.macroblock;
}

QWidget* BuildSceneChangePage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.scene_change = new SceneChangePage(deps.is_feature_on(model::AnalysisFeature::SceneChange), deps.parent);
    return pages.scene_change;
}

QWidget* BuildVisualQualityPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.visual_defect = new VisualDefectPage(
        deps.is_feature_on(model::AnalysisFeature::VisualDefect),
        deps.visual_defect_options ? deps.visual_defect_options() : model::VisualDefectOptions(), deps.parent);
    return pages.visual_defect;
}

QWidget* BuildBitrateGopPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString& path) {
    pages.bitrate_gop = new BitrateGopPage(deps.parent);
    pages.bitrate_gop->SetSourcePath(path);
    return pages.bitrate_gop;
}

QWidget* BuildAudioQcPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString& path) {
    pages.audio_qc = new AudioQcPage(deps.parent);
    pages.audio_qc->SetSourcePath(path);
    return pages.audio_qc;
}

QWidget* BuildColorHdrPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString& path) {
    pages.color_hdr = new ColorHdrPage(deps.parent);
    pages.color_hdr->SetSourcePath(path);
    return pages.color_hdr;
}

QWidget* BuildCodecParametersPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.bitstream_params = new BitstreamPanel(deps.parent);
    pages.bitstream_params->setMinimumHeight(560);
    return pages.bitstream_params;
}

QWidget* BuildStreamingPacketsPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.streaming = new StreamingPanel(deps.parent);
    pages.streaming->setMinimumHeight(560);
    return pages.streaming;
}

QWidget* BuildSubtitleAuxPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.subtitle_aux = new SubtitleAuxPage(deps.parent);
    return pages.subtitle_aux;
}

QWidget* BuildQualityDiagnosticsPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString& path) {
    pages.diagnostics = new DiagnosticsPage(deps.parent);
    pages.diagnostics->SetSourcePath(path);
    return pages.diagnostics;
}

QWidget* BuildReportBatchQcPage(AnalysisPages& pages, const PageBuildDeps& deps, const QString&) {
    pages.reporting = new ReportingPanel(deps.parent);
    return pages.reporting;
}

struct PageBuilderEntry {
    const char* id;
    PageBuilder build;
};

// id -> 建页函数。AnalysisPageCatalog 决定**顺序与导航元数据**，这张表决定**怎么建**。
// 两张表的 id 必须一一对应 —— BuildAnalysisPages 在查不到表项时会直接报错并跳过。
const PageBuilderEntry kPageBuilders[] = {
    {"playback_stats_packets", &BuildPlaybackStatsPage},
    {"event_timeline", &BuildEventTimelinePage},
    {"file_structure", &BuildFileStructurePage},
    {"motion_vectors", &BuildMotionVectorsPage},
    {"scene_change", &BuildSceneChangePage},
    {"visual_quality", &BuildVisualQualityPage},
    {"bitrate_gop", &BuildBitrateGopPage},
    {"audio_qc", &BuildAudioQcPage},
    {"color_hdr", &BuildColorHdrPage},
    {"codec_parameters", &BuildCodecParametersPage},
    {"streaming_packets", &BuildStreamingPacketsPage},
    {"subtitle_aux", &BuildSubtitleAuxPage},
    {"quality_diagnostics", &BuildQualityDiagnosticsPage},
    {"report_batch_qc", &BuildReportBatchQcPage},
    {"file_structure_map", &BuildFileStructureMapPage},
};

PageBuilder FindPageBuilder(const QString& id) {
    for (const PageBuilderEntry& entry : kPageBuilders) {
        if (id == QLatin1String(entry.id))
            return entry.build;
    }
    return nullptr;
}

} // namespace

AnalysisPages BuildAnalysisPages(const PageBuildDeps& deps) {
    AnalysisPages pages;

    auto text = [&deps](const char* source) -> QString {
        return deps.translate ? deps.translate(source) : QString::fromUtf8(source);
    };
    const QString path = deps.current_path ? deps.current_path() : QString();

    // 按目录顺序建页 —— 顺序即外部 stack 的页序号，改顺序改这张表（AnalysisPageCatalog）。
    for (const AnalysisPageDescriptor& entry : AnalysisPageCatalog()) {
        const QString id = QString::fromUtf8(entry.id);
        const PageBuilder build = FindPageBuilder(id);
        if (!build) {
            // 目录里有这一页、注册表却没登记 —— 加页时只改了表忘了补建页函数。
            // 静默跳过会造成"侧栏有条目、stack 里是空页"，而且下标会整体错位，
            // 是最难查的一类。宁可在这里喊出来。
            LOG_ERROR("分析页目录里的 id 没有对应的建页函数: " + id.toStdString());
            continue;
        }

        QWidget* page_widget = build(pages, deps, path);
        deps.add_page(page_widget, text(entry.title), id, QString::fromUtf8(entry.group), entry.order,
                      (entry.scope[0] != '\0') ? text(entry.scope) : QString());
    }

    return pages;
}

QMap<QString, PageBadge> CollectPageBadges(const AnalysisPages& pages, bool scanning) {
    QMap<QString, PageBadge> badges;

    auto add = [&badges, scanning](const QString& page_id, int count, int level) {
        if (count <= 0 && !scanning)
            return;
        PageBadge info;
        info.issue_count = count;
        info.level = count > 0 ? level : 0;
        info.scanning = scanning;
        badges.insert(page_id, info);
    };

    // 质量诊断: QC 报告问题总数 + 最高严重度（致命>错误>警告）
    if (pages.diagnostics && pages.diagnostics->hasResult()) {
        const model::QcReport& report = pages.diagnostics->qcReport();
        const int count = static_cast<int>(report.issues.size());
        int level = 0;
        if (report.CountBySeverity(model::IssueSeverity::Critical) > 0) {
            level = 3;
        } else if (report.CountBySeverity(model::IssueSeverity::Error) > 0) {
            level = 2;
        } else if (count > 0) {
            level = 1;
        }
        add(QStringLiteral("quality_diagnostics"), count, level);
    } else {
        add(QStringLiteral("quality_diagnostics"), 0, 0);
    }

    // 共用同一次扫描的页面: 各自的异常表行数（没有异常表的页面只标扫描态）
    if (pages.bitrate_gop)
        add(QStringLiteral("bitrate_gop"), pages.bitrate_gop->IssueCount(), 1);
    if (pages.audio_qc)
        add(QStringLiteral("audio_qc"), pages.audio_qc->IssueCount(), 1);
    if (pages.color_hdr)
        add(QStringLiteral("color_hdr"), pages.color_hdr->IssueCount(), 1);
    if (pages.streaming) {
        add(QStringLiteral("streaming_packets"), pages.streaming->IssueCount(), 1);
    }
    if (pages.bitstream_params) {
        add(QStringLiteral("codec_parameters"), pages.bitstream_params->IssueCount(), 1);
    }
    if (pages.subtitle_aux)
        add(QStringLiteral("subtitle_aux"), 0, 0);

    // 画面质量是播放期累积的缺陷记录，不随全文件扫描走，但同样值得一个计数徽标
    if (pages.visual_defect) {
        const int count = static_cast<int>(pages.visual_defect->DefectRecords().size());
        if (count > 0)
            add(QStringLiteral("visual_quality"), count, 1);
    }
    return badges;
}

} // namespace ui
} // namespace videoeye
