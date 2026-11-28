#include "core/reporting/ReferenceQualityExporter.h"

#include "core/reporting/QcReportExporterInternal.h"
#include "infrastructure/serialization/Json.h"

#include <cmath>
#include <sstream>
#include <string>
#include <utility>

namespace videoeye {
namespace reporting {

using detail::CsvField;
using detail::Fixed;
using detail::SingleLine;
using detail::WriteUtf8File;

namespace {

// 逐帧行数上限不在这里设：frames 就是调用方的结果，导出不做截断。
// 截断只发生在界面表格上（用户看不过来），报告必须完整 —— 否则"最差帧"会随
// 显示上限前后不一。

// 数值列：CSV 里 NaN 写空串（"没有这个值"），±inf 写 inf / -inf。
//
// 这里刻意**不**走 CsvField：数字列不加引号，而且以 '-' 开头的负数不能被当成
// 公式前置单引号（"-18.5" 读回来就不是数了）。
std::string Number(double value, int precision) {
    if (std::isnan(value))
        return std::string();
    if (std::isinf(value))
        return value > 0 ? "inf" : "-inf";
    return Fixed(value, precision);
}

// "#<节>,<键>,<值>,<键>,<值>,..." 的行拼装（与 QcReportExporter 的 CSV 同一约定）。
// Text 走 CsvField（含公式注入中和），Number 只接受已经格式化好的数字串。
struct MetaRow {
    std::ostringstream& out;
    MetaRow(std::ostringstream& stream, const char* section) : out(stream) {
        out << '#' << section;
    }
    MetaRow& Text(const char* key, const std::string& value) {
        out << ',' << CsvField(key) << ',' << CsvField(SingleLine(value));
        return *this;
    }
    MetaRow& Number(const char* key, const std::string& value) {
        out << ',' << CsvField(key) << ',' << value;
        return *this;
    }
    MetaRow& Flag(const char* key, bool value) {
        return Text(key, value ? "是" : "否");
    }
};

void AppendMeta(std::ostringstream& body, const model::ReferenceQualityResult& result,
                const ReferenceQualityExportInput& input) {
    // 这一行是给"双击打开"的人看的说明，不含半角逗号，整行落在一个单元格里。
    body << "#VideoEye 参考帧质量对比（CSV）｜ 以 # 开头的行是元数据，读表时用 comment='#' 跳过即可；"
            "表头之后每行一帧\n";

    MetaRow report(body, "报告");
    report.Text("参考文件", input.reference_path)
        .Text("待测文件", input.distorted_path)
        .Flag("有效", result.valid)
        .Flag("已取消", result.cancelled)
        .Text("错误", result.error_message);
    body << "\n";

    // 参数原样落盘：没有这些，"同一对文件两次导出为什么不一样"永远说不清。
    // 结束时间 -1 表示"到参考视频末尾"，与 ReferenceQualityOptions 的约定一致。
    const model::ReferenceQualityOptions& options = result.options;
    MetaRow params(body, "参数");
    params.Number("起始(s)", Fixed(options.start_seconds, 3))
        .Number("结束(s)", Fixed(options.end_seconds, 3))
        .Number("时间偏移(s)", Fixed(options.offset_seconds, 3))
        .Number("对齐容差(s)", Fixed(options.alignment_tolerance_seconds, 3))
        .Number("最大帧数", std::to_string(options.max_frames));
    body << "\n";

    const model::ReferenceQualitySummary& summary = result.summary;
    MetaRow counts(body, "统计");
    counts.Number("参考帧数", std::to_string(summary.reference_sample_count))
        .Number("待测帧数", std::to_string(summary.distorted_sample_count))
        .Number("参考时长(s)", Fixed(summary.reference_duration_seconds, 3))
        .Number("待测时长(s)", Fixed(summary.distorted_duration_seconds, 3))
        .Number("目标帧数", std::to_string(summary.target_count))
        .Number("已比较", std::to_string(summary.compared_frames))
        .Number("失败", std::to_string(summary.failed_frames))
        .Number("跳过", std::to_string(summary.skipped_frames))
        .Number("无损", std::to_string(summary.lossless_frames))
        .Number("覆盖率(%)", Fixed(summary.coverage * 100.0, 1));
    body << "\n";

    MetaRow metrics(body, "指标");
    metrics.Number("平均PSNR(dB)", Number(summary.mean_psnr_db, 3))
        .Number("最低PSNR(dB)", Number(summary.min_psnr_db, 3))
        .Number("最低PSNR帧", std::to_string(summary.min_psnr_frame_index))
        .Number("平均SSIM", Number(summary.mean_ssim, 4))
        .Number("最低SSIM", Number(summary.min_ssim, 4))
        .Number("最低SSIM帧", std::to_string(summary.min_ssim_frame_index))
        .Number("最大对齐偏差(s)", Fixed(summary.max_alignment_delta_seconds, 3))
        .Flag("截断", summary.truncated)
        .Flag("时长不一致", summary.duration_mismatch);
    body << "\n";
}

JsonValue BuildOptionsObject(const model::ReferenceQualityOptions& options) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("start_seconds", JsonValue(options.start_seconds));
    object.Set("end_seconds", JsonValue(options.end_seconds));
    object.Set("offset_seconds", JsonValue(options.offset_seconds));
    object.Set("alignment_tolerance_seconds", JsonValue(options.alignment_tolerance_seconds));
    object.Set("max_frames", JsonValue(options.max_frames));
    return object;
}

JsonValue BuildSummaryObject(const model::ReferenceQualitySummary& summary) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("reference_duration_seconds", JsonValue(summary.reference_duration_seconds));
    object.Set("distorted_duration_seconds", JsonValue(summary.distorted_duration_seconds));
    object.Set("reference_sample_count", JsonValue(summary.reference_sample_count));
    object.Set("distorted_sample_count", JsonValue(summary.distorted_sample_count));
    object.Set("target_count", JsonValue(summary.target_count));
    object.Set("compared_frames", JsonValue(summary.compared_frames));
    object.Set("failed_frames", JsonValue(summary.failed_frames));
    object.Set("skipped_frames", JsonValue(summary.skipped_frames));
    object.Set("lossless_frames", JsonValue(summary.lossless_frames));
    object.Set("coverage", JsonValue(summary.coverage));
    // NaN / +inf 由 JsonValue 序列化成 null（JSON 没有这两个字面量）。
    object.Set("mean_psnr_db", JsonValue(summary.mean_psnr_db));
    object.Set("mean_ssim", JsonValue(summary.mean_ssim));
    object.Set("min_psnr_db", JsonValue(summary.min_psnr_db));
    object.Set("min_ssim", JsonValue(summary.min_ssim));
    object.Set("min_psnr_frame_index", JsonValue(summary.min_psnr_frame_index));
    object.Set("min_ssim_frame_index", JsonValue(summary.min_ssim_frame_index));
    object.Set("max_alignment_delta_seconds", JsonValue(summary.max_alignment_delta_seconds));
    object.Set("truncated", JsonValue(summary.truncated));
    object.Set("duration_mismatch", JsonValue(summary.duration_mismatch));
    // "全部无损"时 mean_psnr_db 是 +inf，序列化成 null 后与"没算出来"同形，
    // 所以另落一个布尔值把这件事说死（lossless_frames == compared_frames 也可推，
    // 但下游不该为了判断这件事去算两个计数）。
    object.Set("all_lossless",
               JsonValue(summary.compared_frames > 0 && summary.lossless_frames == summary.compared_frames));
    return object;
}

JsonValue BuildFrameObject(const model::ReferenceFrameMetric& frame) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("index", JsonValue(frame.index));
    object.Set("timestamp_seconds", JsonValue(frame.timestamp_seconds));
    object.Set("reference_frame_index", JsonValue(frame.reference_frame_index));
    object.Set("distorted_frame_index", JsonValue(frame.distorted_frame_index));
    object.Set("reference_timestamp_seconds", JsonValue(frame.reference_timestamp_seconds));
    object.Set("distorted_timestamp_seconds", JsonValue(frame.distorted_timestamp_seconds));
    object.Set("alignment_delta_seconds", JsonValue(frame.alignment_delta_seconds));
    object.Set("width", JsonValue(frame.width));
    object.Set("height", JsonValue(frame.height));
    object.Set("psnr_db", JsonValue(frame.psnr_db));
    object.Set("ssim", JsonValue(frame.ssim));
    object.Set("lossless", JsonValue(std::isinf(frame.psnr_db) && frame.psnr_db > 0.0));
    object.Set("valid", JsonValue(frame.valid));
    object.Set("error_message", JsonValue(frame.error_message));
    return object;
}

} // namespace

bool ReferenceQualityExporter::ExportCsv(const std::string& path, const model::ReferenceQualityResult& result,
                                         const ReferenceQualityExportInput& input) {
    std::ostringstream body;
    body << "\xEF\xBB\xBF"; // UTF-8 BOM，Excel 直接双击打开才不会乱码
    AppendMeta(body, result, input);
    body << "帧序号,参考时间(s),待测时间(s),对齐偏差(s),参考帧号,待测帧号,宽,高,PSNR(dB),SSIM,有效,错误\n";
    for (const model::ReferenceFrameMetric& frame : result.frames) {
        body << frame.index << ',' << Fixed(frame.reference_timestamp_seconds, 3) << ','
             << Fixed(frame.distorted_timestamp_seconds, 3) << ',' << Fixed(frame.alignment_delta_seconds, 3) << ','
             << frame.reference_frame_index << ',' << frame.distorted_frame_index << ',' << frame.width << ','
             << frame.height << ',' << Number(frame.psnr_db, 3) << ',' << Number(frame.ssim, 4) << ','
             << (frame.valid ? "是" : "否") << ',' << CsvField(SingleLine(frame.error_message)) << "\n";
    }
    return WriteUtf8File(path, body.str());
}

bool ReferenceQualityExporter::ExportJson(const std::string& path, const model::ReferenceQualityResult& result,
                                          const ReferenceQualityExportInput& input) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("schema_version", JsonValue(1));
    root.Set("reference_file", JsonValue(input.reference_path));
    root.Set("distorted_file", JsonValue(input.distorted_path));
    root.Set("valid", JsonValue(result.valid));
    root.Set("cancelled", JsonValue(result.cancelled));
    root.Set("error_message", JsonValue(result.error_message));
    root.Set("options", BuildOptionsObject(result.options));
    root.Set("summary", BuildSummaryObject(result.summary));

    JsonValue frames = JsonValue::MakeArray();
    for (const model::ReferenceFrameMetric& frame : result.frames)
        frames.PushBack(BuildFrameObject(frame));
    root.Set("frames", std::move(frames));

    return WriteUtf8File(path, root.ToPrettyString(2) + "\n");
}

} // namespace reporting
} // namespace videoeye
