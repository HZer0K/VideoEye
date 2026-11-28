#pragma once

// 参考帧质量对比（双视频 PSNR / SSIM）结果的报告导出。
//
// 为什么单独成一个导出器，而不是像码率 / 色彩那样在页面里写 QTextStream：
//   * 逐帧明细动辄几千行，"界面怎么显示"和"报告怎么写"是两件事，后者不该占着
//     UI 线程的页面代码；
//   * CSV 与 JSON 必须同口径（同一份 frames、同一套"算不出来"的写法），两处各写
//     一遍迟早漂成"界面 32.5 dB、报告里是空单元格"；
//   * 入参出参都是 std::string，可以脱离 Qt 单测
//     （见 tests/unit/test_reference_quality_exporter.cpp）。
//
// 数值口径（与 core/domain/model/ReferenceQualityResult.h 一致）:
//   * NaN  = 没有这个值（没对上 / 比较失败）→ CSV 空单元、JSON null
//   * +inf = 无损（两帧逐像素一致）→ CSV "inf"、JSON null + lossless=true
//     JSON 规范里没有 Infinity，所以逐帧另留一个 lossless 布尔值 —— 否则"无损"
//     与"没算出来"在 JSON 里长得一模一样。
//
// CSV 约定与 QcReportExporter 相同：**行首为 '#' 的行是元数据**（pandas 用
// comment='#' 直接拿到纯表），表头之后每行一帧。

#include <string>

#include "core/domain/model/ReferenceQualityResult.h"

namespace videoeye {
namespace reporting {

// 报告里要记的两个输入路径。结果模型刻意不带路径 —— 它只描述测量值。
struct ReferenceQualityExportInput {
    std::string reference_path;
    std::string distorted_path;
};

class ReferenceQualityExporter {
public:
    // UTF-8 + BOM（Excel 双击打开中文列不乱码）。打不开文件返回 false。
    static bool ExportCsv(const std::string& path, const model::ReferenceQualityResult& result,
                          const ReferenceQualityExportInput& input = ReferenceQualityExportInput());

    // UTF-8，两空格缩进（可直接喂 jq / 下游脚本）。打不开文件返回 false。
    static bool ExportJson(const std::string& path, const model::ReferenceQualityResult& result,
                           const ReferenceQualityExportInput& input = ReferenceQualityExportInput());
};

} // namespace reporting
} // namespace videoeye
