#pragma once

// 「共用同一次全文件扫描」页面的窄接口。
//
// 码率与 GOP / 音频 QC / 色彩与 HDR / 字幕与辅助数据 / 流媒体包 / 编码参数集
// 共用诊断页发起的那一次扫描，于是面板有几处必须同时作用到它们：换文件时同步
// 导出用路径、扫描起止一起切扫描态、扫描中一起显示进度。以前每处各写一份 if，
// 加一页就要补一份；现在页面实现这个接口，面板持一份注册表循环调用。
//
// 只收编各页真正共有的方法 —— 结果分发（各页 SetResult 签名不同）与意图信号
// （ScanRequested / CancelRequested / RefreshRequested 是页面自己的信号）仍由
// 面板逐页接线，接口窄到"面板需要的生命周期动作"为止。只有 SetScanActive 是
// 必须实现的（"扫描中按钮要禁用、终态要复位"对所有共用页都成立）；其余给默认
// 空实现 —— 没有该需求的页面不必为了满足接口写一堆空函数。

#include <QString>

namespace videoeye {
struct AnalysisOptions;

namespace ui {

class ScanClient {
public:
    virtual ~ScanClient() = default;

    // 导出 CSV 的默认文件名用当前文件路径拼，面板换文件时同步一次。
    // 默认不存 —— 只有需要按文件路径起默认名的页面才覆盖。
    virtual void SetSourcePath(const QString& path) { (void)path; }

    // 扫描开始 / 终态一起切扫描态。三种终态（完成/取消/失败）都必须复位，
    // 否则页面卡在扫描中：取消按钮还亮着、"开始分析"永久禁用。
    virtual void SetScanActive(bool active) = 0;

    // 共用扫描的进度显示。默认不显示 —— 没有进度条的页面不必覆盖；
    // 进度本身已在质量诊断页集中展示，不必每页再复制一条。
    virtual void SetProgress(int percent) { (void)percent; }
    virtual void SetProgressFormat(const QString& format) { (void)format; }

    // 面板发起扫描前调用：把本页 UI 上的选项并入全局 AnalysisOptions。
    // 默认不并 —— 没有独立选项的页面不必实现。
    virtual void FillScanOptions(AnalysisOptions& options) { (void)options; }
};

} // namespace ui
} // namespace videoeye