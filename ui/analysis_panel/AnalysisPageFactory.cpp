#include "ui/analysis_panel/AnalysisPageFactory.h"

#include <QFrame>
#include <QVBoxLayout>

#include "core/domain/model/QcReport.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace ui {
namespace {

// 视图内部的开关编号 -> 全局 AnalysisFeature。
//
// 三个视图各有一张这样的表（流概览 1 项、帧/包 3 项、事件时间轴 3 项、宏块 1 项）。
// 以前是四段 switch 分写在四个 SetupXxx 里，加一项要记得改两处（读 + 写）且两处
// 必须一致 —— 写错一边就会出现"界面上开着、数据被过滤掉"。现在表只有一份，
// 读与写共用。
using FeatureTable = std::vector<model::AnalysisFeature>;

template <typename ViewT>
void InjectIndexedHooks(ViewT* view, const PageBuildDeps& deps, FeatureTable table) {
    view->SetFeatureHooks(
        [deps, table](int view_feature) {
            if (view_feature < 0 || view_feature >= static_cast<int>(table.size())) return true;
            return deps.is_feature_on(table[view_feature]);
        },
        [deps, table](int view_feature, bool enabled) {
            if (view_feature < 0 || view_feature >= static_cast<int>(table.size())) return;
            deps.set_feature(table[view_feature], enabled);
        });
}

// 「播放统计与帧包」：顶部流概览（固定高度）+ 分隔条 + 底部帧/包明细（可伸展）。
QWidget* BuildPlaybackStatsPage(AnalysisPages* pages, const PageBuildDeps& deps) {
    pages->stream_overview = new StreamOverviewView(deps.parent);
    pages->frame_packet = new FramePacketView(deps.parent);
    pages->frame_packet->setMinimumHeight(360);

    InjectIndexedHooks(pages->stream_overview, deps, {model::AnalysisFeature::StreamStats});
    InjectIndexedHooks(pages->frame_packet, deps,
                       {model::AnalysisFeature::VideoFrame, model::AnalysisFeature::AudioFrame,
                        model::AnalysisFeature::Packet});

    // GOP 摘要由「帧/包明细」从解码帧 pict_type 推导产出，「流概览」区拿它填
    // 「最大GOP大小」并画「GOP 帧数分布」曲线。两个视图互不认识，只在建页处连一次
    // —— 直接互相持有指针会让两者无法单独构造 / 单独测试。
    //
    // 捕获两个裸指针而不是 pages 指针：AnalysisPages 是按值返回给面板的结构体，
    // 捕获它的地址会在返回后悬空；控件指针本身是稳定的。
    StreamOverviewView* overview = pages->stream_overview;
    FramePacketView* frames = pages->frame_packet;
    QObject::connect(frames, &FramePacketView::GopSummariesChanged, deps.signal_context,
                     [overview, frames]() {
                         overview->SetGopSummaries(frames->GopSummaries());
                     });

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

}  // namespace

AnalysisPages BuildAnalysisPages(const PageBuildDeps& deps) {
    AnalysisPages pages;

    auto text = [&deps](const char* source) -> QString {
        return deps.translate ? deps.translate(source) : QString::fromUtf8(source);
    };
    const QString path = deps.current_path ? deps.current_path() : QString();

    // 按目录顺序建页 —— 顺序即外部 stack 的页序号，改顺序改这张表（AnalysisPageCatalog）。
    for (const AnalysisPageDescriptor& entry : AnalysisPageCatalog()) {
        const QString id = QString::fromUtf8(entry.id);
        QWidget* page_widget = nullptr;

        if (id == QStringLiteral("playback_stats_packets")) {
            page_widget = BuildPlaybackStatsPage(&pages, deps);
        } else if (id == QStringLiteral("event_timeline")) {
            pages.event_timeline = new EventTimelineView(deps.parent);
            InjectIndexedHooks(pages.event_timeline, deps,
                               {model::AnalysisFeature::Event, model::AnalysisFeature::SyncSample,
                                model::AnalysisFeature::Timeline});
            page_widget = pages.event_timeline;
        } else if (id == QStringLiteral("file_structure")) {
            pages.container = new ContainerStructurePage(
                deps.is_feature_on(model::AnalysisFeature::ContainerStructure), deps.parent);
            page_widget = pages.container;
        } else if (id == QStringLiteral("motion_vectors")) {
            pages.macroblock = new MacroblockView(deps.parent);
            InjectIndexedHooks(pages.macroblock, deps, {model::AnalysisFeature::Macroblock});
            page_widget = pages.macroblock;
        } else if (id == QStringLiteral("scene_change")) {
            pages.scene_change = new SceneChangePage(
                deps.is_feature_on(model::AnalysisFeature::SceneChange), deps.parent);
            page_widget = pages.scene_change;
        } else if (id == QStringLiteral("visual_quality")) {
            pages.visual_defect = new VisualDefectPage(
                deps.is_feature_on(model::AnalysisFeature::VisualDefect),
                deps.visual_defect_options ? deps.visual_defect_options()
                                          : model::VisualDefectOptions(),
                deps.parent);
            page_widget = pages.visual_defect;
        } else if (id == QStringLiteral("bitrate_gop")) {
            pages.bitrate_gop = new BitrateGopPage(deps.parent);
            pages.bitrate_gop->SetSourcePath(path);
            page_widget = pages.bitrate_gop;
        } else if (id == QStringLiteral("audio_qc")) {
            pages.audio_qc = new AudioQcPage(deps.parent);
            pages.audio_qc->SetSourcePath(path);
            page_widget = pages.audio_qc;
        } else if (id == QStringLiteral("color_hdr")) {
            pages.color_hdr = new ColorHdrPage(deps.parent);
            pages.color_hdr->SetSourcePath(path);
            page_widget = pages.color_hdr;
        } else if (id == QStringLiteral("codec_parameters")) {
            pages.bitstream_params = new BitstreamPanel(deps.parent);
            pages.bitstream_params->setMinimumHeight(560);
            page_widget = pages.bitstream_params;
        } else if (id == QStringLiteral("streaming_packets")) {
            pages.streaming = new StreamingPanel(deps.parent);
            pages.streaming->setMinimumHeight(560);
            page_widget = pages.streaming;
        } else if (id == QStringLiteral("subtitle_aux")) {
            pages.subtitle_aux = new SubtitleAuxPage(deps.parent);
            page_widget = pages.subtitle_aux;
        } else if (id == QStringLiteral("quality_diagnostics")) {
            pages.diagnostics = new DiagnosticsPage(deps.parent);
            pages.diagnostics->SetSourcePath(path);
            page_widget = pages.diagnostics;
        } else if (id == QStringLiteral("report_batch_qc")) {
            pages.reporting = new ReportingPanel(deps.parent);
            page_widget = pages.reporting;
        } else {
            // 目录里有这一页、工厂却没建 —— 加页时只改了表忘了补分支。
            // 静默跳过会造成"侧栏有条目、stack 里是空页"，而且下标会整体错位，
            // 是最难查的一类。宁可在这里喊出来。
            LOG_ERROR("分析页目录里的 id 没有对应的建页分支: " + id.toStdString());
            continue;
        }

        deps.add_page(page_widget, text(entry.title), id, QString::fromUtf8(entry.group),
                      entry.order, (entry.scope[0] != '\0') ? text(entry.scope) : QString());
    }

    return pages;
}

QMap<QString, PageBadge> CollectPageBadges(const AnalysisPages& pages, bool scanning) {
    QMap<QString, PageBadge> badges;

    auto add = [&badges, scanning](const QString& page_id, int count, int level) {
        if (count <= 0 && !scanning) return;
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
    if (pages.bitrate_gop) add(QStringLiteral("bitrate_gop"), pages.bitrate_gop->IssueCount(), 1);
    if (pages.audio_qc) add(QStringLiteral("audio_qc"), pages.audio_qc->IssueCount(), 1);
    if (pages.color_hdr) add(QStringLiteral("color_hdr"), pages.color_hdr->IssueCount(), 1);
    if (pages.streaming) {
        add(QStringLiteral("streaming_packets"), pages.streaming->IssueCount(), 1);
    }
    if (pages.bitstream_params) {
        add(QStringLiteral("codec_parameters"), pages.bitstream_params->IssueCount(), 1);
    }
    if (pages.subtitle_aux) add(QStringLiteral("subtitle_aux"), 0, 0);

    // 画面质量是播放期累积的缺陷记录，不随全文件扫描走，但同样值得一个计数徽标
    if (pages.visual_defect) {
        const int count = static_cast<int>(pages.visual_defect->DefectRecords().size());
        if (count > 0) add(QStringLiteral("visual_quality"), count, 1);
    }
    return badges;
}

}  // namespace ui
}  // namespace videoeye
