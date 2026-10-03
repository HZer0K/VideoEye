#pragma once

// 分析功能的开关项。
//
// 下放到 domain 的原因: 以前它是 AnalysisPanel 的嵌套枚举（ui::AnalysisPanel::
// AnalysisFeature），于是"播放器该开哪些分析"这件事只能由 UI 表述：MainWindow 里
// 用一个 10 分支的 switch 把面板的枚举逐个翻译成 MediaPlayer 的
// SetXxxAnalysisEnabled()，新增一个播放期分析维度要同时改枚举、改 MediaPlayer、
// 改 MainWindow 三处。
//
// 现在 MediaPlayer 直接认这个枚举（SetAnalysisFeature），UI 只做转发 —— 新增维度
// 只需在这里加一项 + 在 MediaPlayer 的 switch 里加一行。
//
// 值是 UI 侧 QMap 的键，只在运行期使用、不落盘，所以没有稳定性要求；但插新项时
// 请加在末尾，别插在中间，免得影响依赖顺序的代码（目前没有，将来可能有）。

namespace videoeye {
namespace model {

enum class AnalysisFeature {
    Master,             // 全局主开关
    StreamStats,        // 流统计
    VideoFrame,         // 视频帧
    AudioFrame,         // 音频帧
    Packet,             // 数据包
    Event,              // 分析事件
    SyncSample,         // 音视频同步
    Timeline,           // 时间线
    ContainerStructure, // 文件结构分析
    Macroblock,         // 宏块分析 (运动矢量/块统计)
    SceneChange,        // 场景切换检测 (镜头边界)
    Diagnostics,        // 诊断与报告 (全文件扫描 + QC 规则引擎)
    VisualDefect        // 画面质量 (黑场/冻结/马赛克/模糊/闪烁/曝光/色偏/梳齿/黑边)
};

}  // namespace model
}  // namespace videoeye
