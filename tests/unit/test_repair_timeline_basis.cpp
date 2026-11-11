// 切片 F：时基依据（VFR 声明 + 显式推定帧率）的正反例回归。
//
// 期望值来源（**不**是把实现输出抄一遍）：
//   * 帧率分数 → ticks：25 fps 就是每帧 1/25 s；用 timescale=1000、delta=40 表达，
//     两者严格成比例。30000/1001 必须保持 1001/30000，不许被折成 0.0333666…
//     —— 折了以后三十秒就差半帧，而报告里写着"时基无误"。
//   * 无 VUI 的源：按规矩**必须**拒绝；只有显式给了帧率才允许重建，并且
//     结论里要写明"帧率来自用户输入"、timeline_status 只能是 modified。
//   * fixed_frame_rate_flag=0 的源：按名义帧率重建，但 ctts 一律不写
//     （逐帧时长没有证据，写 ctts 等于按错的时长压缩时间轴）。
//
// 语料为什么是手写的：仓库锁定的预编译 FFmpeg 没有 libx264，现场生成不出
// "有 B 帧且 SPS 带 VUI" 的码流；而"SPS 没有 VUI"这种源更没有现成样本。
// 手写码流只用于结构断言（timescale / stts / ctts / 报告字段），
// 不含可解码图像数据。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairPlainMp4Rebuilder.h"
#include "core/reporting/RepairReportExporter.h"

#include "H264TestBitstream.h"

namespace {

namespace fs = std::filesystem;
namespace model = videoeye::model;
using videoeye::IsobmffFile;
using videoeye::IsobmffParser;
using videoeye::repair::ComputeTimelineTicks;
using videoeye::repair::RepairPlainMp4Rebuilder;
using videoeye::repair::RepairTimelineAssumption;
using videoeye::repair::RepairTimelineTicks;

std::string MakeTempDir(const std::string& tag) {
    const fs::path dir =
        fs::temp_directory_path() /
        ("videoeye_timeline_" + tag + "_" +
         std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(tag + std::to_string(::rand())))));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

RepairTimelineAssumption Assumption(uint32_t num, uint32_t den) {
    RepairTimelineAssumption assumption;
    assumption.num = num;
    assumption.den = den;
    return assumption;
}

} // namespace

// ============================================================
// 帧率分数 → mdhd ticks：必须是精确换算，溢出必须报错
// ============================================================
TEST(RepairTimelineBasis, TicksKeepExactRatioForIntegerRates) {
    RepairTimelineTicks ticks;
    std::string error;
    ASSERT_TRUE(ComputeTimelineTicks(25, 1, 0, &ticks, &error)) << error;
    // 25 fps：timescale 放大到 1000，每帧 40 tick，40/1000 == 1/25。
    EXPECT_EQ(ticks.timescale, 1000u);
    EXPECT_EQ(ticks.sample_delta, 40u);

    ASSERT_TRUE(ComputeTimelineTicks(50, 1, 0, &ticks, &error)) << error;
    EXPECT_EQ(ticks.timescale, 1000u);
    EXPECT_EQ(ticks.sample_delta, 20u);

    // timescale 已经 >= 1000 时不再放大：30000/1001 原样保留。
    ASSERT_TRUE(ComputeTimelineTicks(30000, 1001, 0, &ticks, &error)) << error;
    EXPECT_EQ(ticks.timescale, 30000u);
    EXPECT_EQ(ticks.sample_delta, 1001u);

    ASSERT_TRUE(ComputeTimelineTicks(24000, 1001, 0, &ticks, &error)) << error;
    EXPECT_EQ(ticks.timescale, 24000u);
    EXPECT_EQ(ticks.sample_delta, 1001u);
}

TEST(RepairTimelineBasis, TicksRejectIncompleteFractions) {
    RepairTimelineTicks ticks;
    std::string error;
    EXPECT_FALSE(ComputeTimelineTicks(0, 1, 0, &ticks, &error));
    EXPECT_NE(error.find("num/den"), std::string::npos) << error;
    EXPECT_FALSE(ComputeTimelineTicks(25, 0, 0, &ticks, &error));
    EXPECT_NE(error.find("num/den"), std::string::npos) << error;
}

TEST(RepairTimelineBasis, TicksRejectDurationBeyond32Bit) {
    RepairTimelineTicks ticks;
    std::string error;
    // 25 fps → 每帧 40 tick，(2^32-1)/40 ≈ 1.07 亿个样本。给两亿个必须报错，
    // 不能悄悄截断成 32 位时长（那样长片会被播放器当短片）。
    EXPECT_FALSE(ComputeTimelineTicks(25, 1, 200000000ull, &ticks, &error));
    EXPECT_NE(error.find("32 位"), std::string::npos) << error;
    EXPECT_NE(error.find("mdhd version 1"), std::string::npos) << error;

    // samples == 0 表示"还不知道"，此时跳过上限检查（SPS 解析阶段就是这样）。
    EXPECT_TRUE(ComputeTimelineTicks(25, 1, 0, &ticks, &error)) << error;
}

// ============================================================
// 没有 VUI 的源：默认拒绝；给了显式帧率才重建，且结论必须写清楚来源
// ============================================================
TEST(RepairTimelineBasis, RejectsMissingVuiWithoutAssumption) {
    const std::string dir = MakeTempDir("no_vui_reject");
    const std::string source = dir + "/no_vui.mp4";
    H264TestSpsOptions options;
    options.with_vui = false;
    ASSERT_TRUE(H264TestWriteAvccMp4(source, options)) << "无法写出自建码流";

    const auto probe = RepairPlainMp4Rebuilder::Probe(source, nullptr);
    EXPECT_FALSE(probe.attemptable) << "没有 VUI 也没有显式帧率时不得声称可重建";
    EXPECT_NE(probe.reason.find("帧率证据"), std::string::npos) << probe.reason;
    EXPECT_NE(probe.reason.find("显式帧率"), std::string::npos)
        << "拒绝原因必须告诉用户怎么补救，否则只能看到一句无解的错误：" << probe.reason;
    EXPECT_EQ(probe.timeline_basis.mode, "unknown");
}

TEST(RepairTimelineBasis, RebuildsWithExplicitFrameRateWhenVuiMissing) {
    const std::string dir = MakeTempDir("no_vui_assume");
    const std::string source = dir + "/no_vui.mp4";
    H264TestSpsOptions options;
    options.with_vui = false;
    ASSERT_TRUE(H264TestWriteAvccMp4(source, options, 2, false)) << "无法写出自建码流";

    const RepairTimelineAssumption assumption = Assumption(25, 1);
    const auto probe = RepairPlainMp4Rebuilder::Probe(source, nullptr, nullptr, 0, &assumption);
    ASSERT_TRUE(probe.attemptable) << probe.reason;
    EXPECT_EQ(probe.timeline_basis.mode, "user_input");
    EXPECT_EQ(probe.timeline_basis.frame_rate, "25/1");

    const std::string out = dir + "/rebuilt.mp4";
    const auto rebuilt = RepairPlainMp4Rebuilder::Rebuild(source, out, nullptr, nullptr, 0, &assumption);
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error;
    EXPECT_EQ(rebuilt.timeline_basis.mode, "user_input");
    EXPECT_EQ(rebuilt.timeline_basis.frame_rate, "25/1");
    EXPECT_TRUE(rebuilt.timeline_basis.constant);

    // 结论里必须有一句"这是用户给的，不是源侧证据"。
    bool says_user_input = false;
    for (const auto& limitation : rebuilt.timeline_basis.limitations) {
        if (limitation.find("显式输入") != std::string::npos)
            says_user_input = true;
    }
    EXPECT_TRUE(says_user_input) << "时基来自用户输入却没有写进限制项";

    // 产物时基必须是 1000/40，不是随便什么能播放的值。
    IsobmffFile product;
    ASSERT_TRUE(IsobmffParser::Parse(out, product)) << product.error_message;
    ASSERT_EQ(product.tracks.size(), 1u);
    EXPECT_EQ(product.tracks[0].media_timescale, 1000u);
    ASSERT_TRUE(product.tracks[0].has_stts);
    ASSERT_FALSE(product.tracks[0].stts.empty());
    EXPECT_EQ(product.tracks[0].stts[0].sample_delta, 40u);
}

TEST(RepairTimelineBasis, FallsBackToAssumptionWhenTimingInfoIsInvalid) {
    const std::string dir = MakeTempDir("bad_timing");
    const std::string source = dir + "/bad_timing.mp4";
    H264TestSpsOptions options;
    options.zero_timing = true; // VUI 有 timing_info，但 num_units_in_tick = 0
    ASSERT_TRUE(H264TestWriteAvccMp4(source, options, 1, false)) << "无法写出自建码流";

    // 没有显式帧率：无效的 timing_info 和"没有 VUI"一样，都不能当证据用。
    EXPECT_FALSE(RepairPlainMp4Rebuilder::Probe(source, nullptr).attemptable);

    const RepairTimelineAssumption assumption = Assumption(30, 1);
    const std::string out = dir + "/rebuilt.mp4";
    const auto rebuilt = RepairPlainMp4Rebuilder::Rebuild(source, out, nullptr, nullptr, 0, &assumption);
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error;
    EXPECT_EQ(rebuilt.timeline_basis.mode, "user_input");
    EXPECT_EQ(rebuilt.timeline_basis.frame_rate, "30/1");
}

// ============================================================
// VFR 声明：按名义帧率重建，但绝不写 ctts
// ============================================================
TEST(RepairTimelineBasis, VfrDeclarationSuppressesCtts) {
    const std::string dir = MakeTempDir("vfr");
    const std::string source = dir + "/vfr.mp4";
    H264TestSpsOptions options;
    options.fixed_frame_rate = false; // 码流自己说"我不是恒定帧率"
    ASSERT_TRUE(H264TestWriteAvccMp4(source, options, 2, true)) << "无法写出自建码流";

    const std::string out = dir + "/rebuilt.mp4";
    const auto rebuilt = RepairPlainMp4Rebuilder::Rebuild(source, out, nullptr);
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error;
    EXPECT_TRUE(rebuilt.timeline_basis.vfr_declared);
    EXPECT_EQ(rebuilt.timeline_basis.frame_rate, "25/1");
    EXPECT_FALSE(rebuilt.display_order.ctts_written) << "逐帧时长没有证据时写 ctts 就是把整条时间轴算错";
    EXPECT_FALSE(rebuilt.display_order.ctts_required);

    bool says_vfr = false;
    for (const auto& limitation : rebuilt.timeline_basis.limitations) {
        if (limitation.find("非恒定帧率") != std::string::npos)
            says_vfr = true;
    }
    EXPECT_TRUE(says_vfr) << "声明了 VFR 却没在时基限制里说";

    // 反过来：显示序确实推出来了（POC 有效），但不能因为"没写 ctts"
    // 就说成"显示序与解码序一致"—— 那是把"不知道"说成"没问题"。
    EXPECT_TRUE(rebuilt.display_order.available) << rebuilt.display_order.unavailable_reason;
    for (const auto& limitation : rebuilt.display_order.limitations)
        EXPECT_EQ(limitation.find("显示序与解码序一致"), std::string::npos) << limitation;

    IsobmffFile product;
    ASSERT_TRUE(IsobmffParser::Parse(out, product)) << product.error_message;
    ASSERT_EQ(product.tracks.size(), 1u);
    EXPECT_FALSE(product.tracks[0].has_ctts);
}

TEST(RepairTimelineBasis, FractionalVuiRateKeepsExactFraction) {
    const std::string dir = MakeTempDir("fractional");
    const std::string source = dir + "/ntsc.mp4";
    H264TestSpsOptions options;
    options.num_units_in_tick = 1001;
    options.time_scale = 60000; // 29.97 fps
    ASSERT_TRUE(H264TestWriteAvccMp4(source, options, 1, false)) << "无法写出自建码流";

    const auto probe = RepairPlainMp4Rebuilder::Probe(source, nullptr);
    ASSERT_TRUE(probe.attemptable) << probe.reason;
    EXPECT_EQ(probe.timeline_basis.mode, "vui_timing");
    // 报告里必须是约分后的精确分数，不能是 29.97。
    EXPECT_EQ(probe.timeline_basis.frame_rate, "30000/1001");

    const std::string out = dir + "/rebuilt.mp4";
    const auto rebuilt = RepairPlainMp4Rebuilder::Rebuild(source, out, nullptr);
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error;
    IsobmffFile product;
    ASSERT_TRUE(IsobmffParser::Parse(out, product)) << product.error_message;
    ASSERT_EQ(product.tracks.size(), 1u);
    EXPECT_EQ(product.tracks[0].media_timescale, 60000u);
    ASSERT_FALSE(product.tracks[0].stts.empty());
    EXPECT_EQ(product.tracks[0].stts[0].sample_delta, 2002u);
}

// ============================================================
// 引擎接线：参数预检 + 结论落进结果与三格式报告
// ============================================================
TEST(RepairTimelineBasis, EngineRejectsIncompleteAssumption) {
    const std::string dir = MakeTempDir("incomplete");
    const std::string source = dir + "/no_vui.mp4";
    H264TestSpsOptions options;
    options.with_vui = false;
    ASSERT_TRUE(H264TestWriteAvccMp4(source, options)) << "无法写出自建码流";

    model::RepairRequest request;
    request.source_path = source;
    request.output_directory = dir + "/out";
    request.assumed_frame_rate_num = 25; // 只给分子

    const model::RepairResult result = videoeye::repair::RepairEngine::Run(request, nullptr, nullptr);
    EXPECT_EQ(result.status, model::RepairBusinessStatus::Failed);
    EXPECT_NE(result.error.find("显式帧率参数不完整"), std::string::npos) << result.error;
    EXPECT_FALSE(result.output_published) << "参数写错时不能发布任何产物";
}

TEST(RepairTimelineBasis, EngineReportsAssumedBasisAndMarksTimelineModified) {
    const std::string dir = MakeTempDir("engine");
    const std::string source = dir + "/no_vui.mp4";
    H264TestSpsOptions options;
    options.with_vui = false;
    ASSERT_TRUE(H264TestWriteAvccMp4(source, options, 2, false)) << "无法写出自建码流";

    model::RepairRequest request;
    request.source_path = source;
    request.output_directory = dir + "/out";
    request.verification_level = model::RepairVerificationLevel::V0_Container;
    request.assumed_frame_rate_num = 25;
    request.assumed_frame_rate_den = 1;
    request.write_report = true;

    const model::RepairResult result = videoeye::repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_EQ(result.timeline_basis.mode, "user_input");
    EXPECT_EQ(result.timeline_basis.frame_rate, "25/1");
    // 时基是推出来的，时间轴就不能标成 unchanged —— 否则报告会替用户断言
    // "播放速度没变"，而每一秒都是按推定帧率算的。
    EXPECT_EQ(result.fidelity.timeline_status, "modified");
    EXPECT_TRUE(result.fidelity.timeline_modified);

    const auto written =
        videoeye::reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string json = ReadText(dir + "/r.json");
    EXPECT_NE(json.find("\"timeline_basis\""), std::string::npos) << "JSON 缺时基依据";
    EXPECT_NE(json.find("\"user_input\""), std::string::npos) << "JSON 没写时基来源";
    EXPECT_NE(json.find("\"frame_rate\""), std::string::npos);
    for (const char* name : {"/r.txt", "/r.html"}) {
        const std::string body = ReadText(dir + name);
        EXPECT_NE(body.find("时基依据"), std::string::npos) << name << " 缺时基依据";
        EXPECT_NE(body.find("用户显式帧率"), std::string::npos) << name << " 没写时基来源";
    }
}

TEST(RepairTimelineBasis, ReportDeclaresVfrInAllFormats) {
    // 直接构造结果：这里验的是"结论怎么落盘"，不是引擎流程。
    const std::string dir = MakeTempDir("report_vfr");
    model::RepairResult result;
    result.result_id = "result-vfr";
    result.request_id = "req-vfr";
    result.timeline_basis.mode = "vui_timing";
    result.timeline_basis.frame_rate = "30000/1001";
    result.timeline_basis.constant = true;
    result.timeline_basis.vfr_declared = true;
    result.timeline_basis.detail = "SPS VUI timing_info";
    result.timeline_basis.limitations.push_back("码流声明非恒定帧率（fixed_frame_rate_flag=0）");

    const auto written =
        videoeye::reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;

    const std::string json = ReadText(dir + "/r.json");
    EXPECT_NE(json.find("\"vfr_declared\""), std::string::npos) << "JSON 缺 VFR 声明";
    EXPECT_NE(json.find("\"frame_rate\""), std::string::npos) << "JSON 缺帧率";
    EXPECT_NE(json.find("30000/1001"), std::string::npos) << "JSON 没写精确分数帧率";

    for (const char* name : {"/r.txt", "/r.html"}) {
        const std::string body = ReadText(dir + name);
        EXPECT_NE(body.find("码流声明非恒定帧率"), std::string::npos) << name;
        EXPECT_NE(body.find("30000/1001"), std::string::npos) << name;
    }
}
