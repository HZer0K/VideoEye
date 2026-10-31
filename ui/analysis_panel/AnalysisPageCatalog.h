#pragma once

// 分析页目录：id / 标题 / 侧栏分组 / 组内序号 / 口径。
//
// 为什么要有这一张表
// ------------------
// 页面在外部 QStackedWidget 里的**下标**是硬契约：ctest 里有按 stack 下标取控件的
// 回归（tabs->widget(0/1/3)），侧栏则按 (group, order) 排序与分组。以前这个顺序
// 没有任何一处显式声明 —— 它只是 AnalysisPanel::SetupUI 里十几个 SetupXxx() 的
// 调用次序的**副产品**。改的人看不见这条依赖，测试先红，且红在完全不相干的地方。
//
// 现在顺序是一张数据表，建页方（AnalysisPageFactory）按它循环，侧栏徽标按它的 id
// 归集，测试（tests/unit/test_analysis_page_catalog.cpp）把它钉住。
//
// 只依赖 QString / vector，不碰任何页面组件 —— 所以这张表能脱离 Qt Widgets 与
// 全部页面实现被单独测（不必为了测顺序去链 14 个页面的 .cpp 与它们的依赖图）。

#include <QString>
#include <vector>

namespace videoeye {
namespace ui {

struct AnalysisPageDescriptor {
    const char* id = "";    // 稳定标识（侧栏徽标按它归集，见 CollectPageBadges）
    const char* title = ""; // 未翻译的源文本；由建页方过一遍 tr()
    const char* group = ""; // 侧栏分组 id
    int order = 0;          // 组内排序键
    const char* scope = ""; // 口径说明（"播放期" / "全文件"），空 = 不显示
};

// 顺序 == 外部 stack 的页序号。调整顺序前先看 tests/unit/test_analysis_page_catalog.cpp：
// 那里把这条序列当作契约断言。
//
// 另一条口径与 stack 顺序**无关**：侧栏按 (group, order) 排序。所以同组内的
// 展示次序可以单独调整，不会碰 stack 下标 —— 「全片质量」组就是这么把
// quality_diagnostics 提到首位的：它是这一组的结论页，用户进来先看结论，
// 再按需展开码率 / 音频 / 色彩这些分项（"组内首发"由 order 表达，不看表格行序）。
inline const std::vector<AnalysisPageDescriptor>& AnalysisPageCatalog() {
    static const std::vector<AnalysisPageDescriptor> kCatalog = {
        {"playback_stats_packets", "播放统计与帧包", "playback", 0, "播放期"},
        {"event_timeline", "事件与时间轴", "playback", 4, ""},
        {"file_structure", "文件结构", "file_parse", 0, "全文件"},
        {"motion_vectors", "运动矢量与块分析", "playback", 1, "播放期"},
        {"scene_change", "场景切换", "playback", 2, "播放期"},
        {"visual_quality", "画面质量", "playback", 3, "播放期"},
        {"bitrate_gop", "码率与 GOP", "full_quality", 1, "全文件"},
        {"audio_qc", "音频 QC", "full_quality", 2, "全文件"},
        {"color_hdr", "色彩与 HDR", "full_quality", 3, "全文件"},
        {"codec_parameters", "编码参数集", "file_parse", 1, "全文件"},
        {"streaming_packets", "流媒体包", "file_parse", 2, "全文件"},
        {"subtitle_aux", "字幕与辅助数据", "file_parse", 3, "全文件"},
        {"quality_diagnostics", "质量诊断", "full_quality", 0, "全文件"},
        {"report_batch_qc", "报告与批量 QC", "report_tools", 0, ""},
    };
    return kCatalog;
}

} // namespace ui
} // namespace videoeye
