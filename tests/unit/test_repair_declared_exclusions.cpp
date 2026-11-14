#include <gtest/gtest.h>

// ============================================================
// 阶段 5 / 切片 L：「本次明确未恢复项」的结论层守护
//
// 路线图里写过十几次「明确不做」，但报告过去一个字都不提。空白会被默认
// 读成「没有问题」，与「未测量态静默省略」是同一类错误结论。这一组用例盯住：
//   * 静态目录每条必须有完整六个维度（防止以后只加 code 的空壳条目）；
//   * 只有结构化触发事实成立才挂载，没事实不得把全局免责声明糊到本次运行；
//   * 未探测 / 已补齐 / 未测量 / 预检早退这些反向状态不许伪造命中；
//   * available=false / 零命中 / 有命中三态在 JSON / TXT / HTML 里同源显式。
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairCapabilityContracts.h"
#include "core/repair/RepairDeclaredExclusions.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"

namespace {

using namespace videoeye;
using namespace videoeye::model;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_exclusions_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 已经跑过诊断、且时基有源侧证据的基线：它本身不该命中任何「明确未恢复项」。
RepairResult EvaluatedBaseline() {
    RepairResult result;
    RepairCheck check;
    check.name = "container.open";
    result.diagnosis.checks.push_back(check);
    result.timeline_basis.mode = "container_pts";
    result.timeline_basis.constant = true;
    return result;
}

RepairActionExecution AttemptedExecution(RepairAction action = RepairAction::S04_SegmentSalvage) {
    RepairActionExecution execution;
    execution.action = action;
    execution.attempted = true;
    execution.succeeded = true;
    return execution;
}

std::vector<std::string> Codes(const RepairDeclaredExclusions& declared) {
    std::vector<std::string> codes;
    for (const auto& item : declared.items)
        codes.push_back(item.code);
    return codes;
}

bool HasCode(const RepairDeclaredExclusions& declared, const char* code) {
    const auto codes = Codes(declared);
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

const RepairDeclaredExclusion* FindItem(const RepairDeclaredExclusions& declared, const char* code) {
    for (const auto& item : declared.items) {
        if (item.code == code)
            return &item;
    }
    return nullptr;
}

struct ReportBundle {
    bool ok = false;
    std::string error;
    std::string json;
    std::string text;
    std::string html;
    JsonValue root;
};

ReportBundle WriteReports(const RepairResult& result, const std::string& tag) {
    ReportBundle bundle;
    const std::string dir = MakeTempDir(tag);
    const auto written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    if (!written.AllOk()) {
        bundle.error = written.error;
        return bundle;
    }
    bundle.json = ReadText(dir + "/r.json");
    bundle.text = ReadText(dir + "/r.txt");
    bundle.html = ReadText(dir + "/r.html");
    std::string error;
    if (!JsonParse(bundle.json, bundle.root, &error)) {
        bundle.error = error;
        return bundle;
    }
    bundle.ok = true;
    return bundle;
}

std::vector<std::string> JsonCodes(const JsonValue& root) {
    std::vector<std::string> codes;
    const JsonValue* node = root.Find("declared_exclusions");
    if (!node || !node->IsObject())
        return codes;
    const JsonValue* items = node->Find("items");
    if (!items || !items->IsArray())
        return codes;
    for (const auto& item : items->Items()) {
        const JsonValue* code = item.Find("code");
        if (code)
            codes.push_back(code->StringValueOr());
    }
    return codes;
}

bool ConclusionAbsent(const ReportBundle& bundle, const std::string& code) {
    return bundle.text.find(code) == std::string::npos && bundle.html.find(code) == std::string::npos;
}

// 覆盖守护：目录是「做了才有用、坏了看不出来」的机制，必须单独钉死完整性。
TEST(RepairDeclaredExclusions, CatalogCoversEveryDimension) {
    const auto& catalog = repair::AllDeclaredExclusions();
    ASSERT_GE(catalog.size(), 8u) << "少一条明确不做的项目就少一处结论层事实";
    std::set<std::string> seen;
    for (const auto& item : catalog) {
        EXPECT_FALSE(item.code.empty());
        EXPECT_FALSE(item.title.empty());
        EXPECT_FALSE(item.scope.empty());
        EXPECT_FALSE(item.applies_to.empty());
        EXPECT_FALSE(item.consequence.empty());
        EXPECT_FALSE(item.evidence.empty()) << item.code;
        EXPECT_EQ(item.scope, "version_capability") << item.code;
        EXPECT_EQ(item.status, "not_attempted") << item.code;
        EXPECT_TRUE(item.trigger_facts.empty()) << item.code << " 静态模板不许自带触发事实";
        EXPECT_TRUE(seen.insert(item.code).second) << "code 重复: " << item.code;
    }
}

// 切片 O：静态契约文案只描述「策略」，不得把某个具体编码器写成既成事实。
//
// 本项目锁定的 FFmpeg 是 LGPL 变体（--disable-libx264/--disable-libx265），
// 「视频 libx264 重编码」这句话在本构建里恒为假，而同一份报告的限制项里
// 印的是「编码器=libopenh264」—— 读者会得到两个互相矛盾的答案。
// 静态文本允许说「首选 libx264」「本构建不含 libx264」，但不允许说
// 「<编码器名> 重编码」（那是实测事实，只能由 fidelity.transcode 渲染）。
TEST(RepairDeclaredExclusions, StaticTextNeverStatesEncoderAsFact) {
    const std::vector<std::string> encoder_names = {"libx264", "libx265", "libopenh264"};
    const auto scan = [&encoder_names](const std::string& field, const std::string& text) {
        for (const auto& name : encoder_names) {
            const std::string phrase = name + " 重编码";
            EXPECT_EQ(text.find(phrase), std::string::npos)
                << field << " 把编码器当成了既成事实: " << phrase << " | " << text;
        }
    };

    for (const auto& item : repair::AllDeclaredExclusions()) {
        scan(item.code + ".title", item.title);
        scan(item.code + ".applies_to", item.applies_to);
        scan(item.code + ".consequence", item.consequence);
        scan(item.code + ".evidence", item.evidence);
    }
    for (const auto& boundary : repair::AllCapabilityBoundaries()) {
        scan(boundary.code + ".timeline_impact", boundary.timeline_impact);
        scan(boundary.code + ".evidence", boundary.evidence);
        for (const auto& scope : boundary.unrecoverable_scope)
            scan(boundary.code + ".unrecoverable_scope", scope);
        for (const auto& risk : boundary.quality_risks)
            scan(boundary.code + ".quality_risks", risk);
    }
}

TEST(RepairDeclaredExclusions, NoFactsProducesEvaluatedEmptyList) {
    const auto declared = repair::BuildDeclaredExclusions(EvaluatedBaseline());
    EXPECT_TRUE(declared.available) << "评估过但零命中 ≠ 根本没评估";
    EXPECT_TRUE(declared.unavailable_reason.empty());
    EXPECT_TRUE(declared.items.empty()) << "没有事实依据时不得输出能力声明";
    ASSERT_GE(declared.limitations.size(), 2u);
    EXPECT_NE(declared.limitations[0].find("不代表已覆盖"), std::string::npos);
    EXPECT_NE(declared.limitations[1].find("不能把"), std::string::npos);
}

TEST(RepairDeclaredExclusions, PreRepairEarlyExitDoesNotCarryGlobalDisclaimers) {
    const RepairResult result; // 预检失败：连诊断 checks 都没有
    const auto declared = repair::BuildDeclaredExclusions(result);
    EXPECT_TRUE(declared.available);
    EXPECT_TRUE(declared.items.empty()) << "请求预检就失败时，谈「本次没做什么」没有事实基础";
    const auto all = std::string("诊断 / 执行之前结束");
    EXPECT_NE(std::find_if(declared.limitations.begin(), declared.limitations.end(),
                           [&all](const std::string& text) { return text.find(all) != std::string::npos; }),
              declared.limitations.end());
}

// 表驱动 + 完整期望集合：多挂一条 / 少挂一条都会判红，正反守护同时成立。
// 特别注意 mode 默认就是 "unknown" —— 它不能单独构成触发事实。
TEST(RepairDeclaredExclusions, TriggerMatrixMatchesExactlyOnStructuredFacts) {
    struct Case {
        const char* tag;
        std::function<void(RepairResult&)> mutate;
        std::vector<std::string> expected;
    };
    const std::vector<Case> cases = {
        {"baseline", [](RepairResult&) {}, {}},
        {"attempted_execution",
         [](RepairResult& r) { r.executions.push_back(AttemptedExecution()); },
         {"edit_list_not_rebuilt"}},
        {"vfr_declared",
         [](RepairResult& r) { r.timeline_basis.vfr_declared = true; },
         {"vfr_frame_durations_not_recovered"}},
        {"unknown_mode_without_execution", [](RepairResult& r) { r.timeline_basis.mode = "unknown"; }, {}},
        {"unknown_mode_with_execution",
         [](RepairResult& r) {
             r.timeline_basis.mode = "unknown";
             r.executions.push_back(AttemptedExecution());
         },
         {"edit_list_not_rebuilt", "vfr_frame_durations_not_recovered"}},
        // 切片 M：参考样片辅助重建（C/H/I）借的是参考片的 stts/mdhd，源侧 moov
        // 已丢失。它必须挂 VFR 声明，但不能串到普通 container_pts 路径。
        {"reference_borrowed_mode",
         [](RepairResult& r) { r.timeline_basis.mode = "reference_borrowed"; },
         {"vfr_frame_durations_not_recovered"}},
        {"reference_borrowed_with_execution",
         [](RepairResult& r) {
             r.timeline_basis.mode = "reference_borrowed";
             r.executions.push_back(AttemptedExecution());
         },
         {"edit_list_not_rebuilt", "vfr_frame_durations_not_recovered"}},
        {"container_pts_does_not_trigger_vfr", [](RepairResult& r) { r.timeline_basis.mode = "container_pts"; }, {}},
        {"display_order_analyzed",
         [](RepairResult& r) { r.display_order.analyzed = true; },
         {"original_display_offsets_not_restored"}},
        {"display_order_unmeasured", [](RepairResult& r) { r.display_order.analyzed = false; }, {}},
        {"video_reencoded",
         [](RepairResult& r) { r.fidelity.video_reencoded = true; },
         {"input_timeline_span_not_reconstructed", "synthetic_content_not_inserted",
          "gop_local_reencode_not_attempted"}},
        {"synthetic_content_enabled",
         [](RepairResult& r) {
             r.fidelity.video_reencoded = true;
             r.fidelity.synthetic_frames_inserted = true;
         },
         {"input_timeline_span_not_reconstructed", "gop_local_reencode_not_attempted"}},
        // 切片 W：音频补过静音也不再适用「本次没有插入合成内容」—— 否则同一份报告
        // 会同时写「补入 12288 采样合成静音」和「本次没有插入合成静音」。
        {"audio_silence_inserted",
         [](RepairResult& r) {
             r.fidelity.video_reencoded = true;
             RepairSyntheticAudioFill fill;
             fill.inspected = true;
             fill.coding_mode = "transcode";
             fill.input_stream_index = 1;
             fill.samples_inserted = 12288;
             fill.seconds_inserted = 0.279;
             r.fidelity.synthetic_audio.push_back(fill);
         },
         {"input_timeline_span_not_reconstructed", "gop_local_reencode_not_attempted"}},
        // 反向：探到音频空洞但一个采样都没补（用户没开 / 复制轨），这条声明仍然成立。
        {"audio_gap_detected_not_filled",
         [](RepairResult& r) {
             r.fidelity.video_reencoded = true;
             RepairSyntheticAudioFill fill;
             fill.inspected = true;
             fill.coding_mode = "copy";
             fill.input_stream_index = 1;
             fill.gaps_detected = 1;
             r.fidelity.synthetic_audio.push_back(fill);
         },
         {"input_timeline_span_not_reconstructed", "synthetic_content_not_inserted",
          "gop_local_reencode_not_attempted"}},
        {"transcode_inspected_no_loss", [](RepairResult& r) { r.fidelity.transcode.inspected = true; }, {}},
        {"transcode_inspected_with_loss",
         [](RepairResult& r) {
             r.fidelity.transcode.inspected = true;
             r.fidelity.transcode.lost_properties = {"旋转矩阵", "HDR 静态元数据"};
         },
         {"transcode_properties_not_inherited"}},
        {"transcode_not_inspected_with_loss_text",
         [](RepairResult& r) {
             r.fidelity.transcode.inspected = false;
             r.fidelity.transcode.lost_properties = {"旋转矩阵"};
         },
         {}},
        {"fragmented", [](RepairResult& r) { r.diagnosis.fragmented = true; }, {"fragment_resync_not_attempted"}},
        {"fully_triggered",
         [](RepairResult& r) {
             r.executions.push_back(AttemptedExecution(RepairAction::S08_TolerantTranscode));
             r.timeline_basis.mode = "unknown";
             r.timeline_basis.vfr_declared = true;
             r.display_order.analyzed = true;
             r.display_order.ctts_written = true;
             r.fidelity.video_reencoded = true;
             r.fidelity.transcode.inspected = true;
             r.fidelity.transcode.lost_properties = {"旋转矩阵"};
             r.diagnosis.fragmented = true;
         },
         {"edit_list_not_rebuilt", "vfr_frame_durations_not_recovered", "original_display_offsets_not_restored",
          "input_timeline_span_not_reconstructed", "synthetic_content_not_inserted",
          "transcode_properties_not_inherited", "fragment_resync_not_attempted", "gop_local_reencode_not_attempted"}},
    };
    for (const auto& c : cases) {
        RepairResult result = EvaluatedBaseline();
        c.mutate(result);
        const auto declared = repair::BuildDeclaredExclusions(result);
        EXPECT_TRUE(declared.available) << c.tag;
        auto actual = Codes(declared);
        auto expected = c.expected;
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(actual, expected) << c.tag << " 命中集合与结构化事实不一致";
    }
}

TEST(RepairDeclaredExclusions, FreeTextNeverFabricatesTriggerFacts) {
    RepairResult result = EvaluatedBaseline();
    result.summary = "elst 不重建；VFR 逐帧时长未恢复；分片重同步未做；旋转矩阵丢失";
    result.error = "decodable_gop_transcode";
    result.fidelity.timeline_note = "fragmented=true";
    result.diagnosis.coverage.limitations.push_back("display_order.analyzed=true");
    EXPECT_TRUE(repair::BuildDeclaredExclusions(result).items.empty())
        << "触发只能看结构化字段，不许从 summary / error / limitations 反推";
}

TEST(RepairDeclaredExclusions, TriggerFactsAreStructuredFieldValues) {
    RepairResult result = EvaluatedBaseline();
    result.executions.push_back(AttemptedExecution(RepairAction::S08_TolerantTranscode));
    result.timeline_basis.mode = "unknown";
    result.timeline_basis.vfr_declared = true;
    result.display_order.analyzed = true;
    result.display_order.ctts_written = true;
    result.fidelity.video_reencoded = true;
    result.fidelity.transcode.inspected = true;
    result.fidelity.transcode.lost_properties = {"旋转矩阵"};
    result.diagnosis.fragmented = true;

    const auto declared = repair::BuildDeclaredExclusions(result);
    ASSERT_EQ(declared.items.size(), 8u);
    const std::vector<std::string> allowed_prefixes = {
        "executions[].",
        "timeline_basis.",
        "display_order.",
        "fidelity.video_",
        "fidelity.synthetic_frames_inserted=",
        "fidelity.synthetic_audio[].",
        "fidelity.transcode.",
        "diagnosis.fragmented=",
    };
    for (const auto& item : declared.items) {
        ASSERT_FALSE(item.trigger_facts.empty()) << item.code;
        for (const auto& fact : item.trigger_facts) {
            EXPECT_NE(fact.find('='), std::string::npos) << item.code << " 触发事实必须是 字段=值";
            const bool known =
                std::any_of(allowed_prefixes.begin(), allowed_prefixes.end(),
                            [&fact](const std::string& prefix) { return fact.compare(0, prefix.size(), prefix) == 0; });
            EXPECT_TRUE(known) << item.code << " 触发事实来源不明: " << fact;
        }
    }
    const auto* transcode = FindItem(declared, "transcode_properties_not_inherited");
    ASSERT_NE(transcode, nullptr);
    EXPECT_NE(std::find(transcode->trigger_facts.begin(), transcode->trigger_facts.end(),
                        "fidelity.transcode.lost_properties[]=旋转矩阵"),
              transcode->trigger_facts.end());
}

// 切片 T：S08 只重建一条视频链 —— 这条能力边界必须能被「未保留的第二视频轨」
// 触发，但不能被「容器拒绝音频」或「源侧已丢弃」触发（那是别人的决定，不是
// 我们的能力缺口）。
TEST(RepairDeclaredExclusions, ExtraTracksNotRebuiltOnlyTriggersOnOurOwnDrops) {
    RepairResult result = EvaluatedBaseline();
    result.executions.push_back(AttemptedExecution(RepairAction::S08_TolerantTranscode));
    result.fidelity.video_reencoded = true;

    RepairTranscodeTrackDecision extra_video;
    extra_video.input_stream_index = 1;
    extra_video.kind = "video";
    extra_video.source_codec = "h264";
    extra_video.action = "drop";
    extra_video.reason_code = "extra_video_single_chain";
    extra_video.text = "流 #1（video, h264）→ 未保留：第二视频轨";
    result.fidelity.transcode.track_decisions.push_back(extra_video);

    const auto with_extra = repair::BuildDeclaredExclusions(result);
    const auto* item = FindItem(with_extra, "extra_tracks_not_rebuilt");
    ASSERT_NE(item, nullptr) << "第二视频轨被丢弃了，却没有对应的能力声明";
    ASSERT_FALSE(item->trigger_facts.empty());
    const bool mentions_dropped_track =
        std::any_of(item->trigger_facts.begin(), item->trigger_facts.end(),
                    [](const std::string& fact) { return fact.find("第二视频轨") != std::string::npos; });
    EXPECT_TRUE(mentions_dropped_track) << item->trigger_facts[0];

    // 反向守护（切片 V 之后分成两支）：
    //   * 用户自己关掉了音频重编码 → 别人的决定，不算我们的能力缺口；
    //   * 容器拒绝且没有一条能过试写头的重编码路 → 这正是我们的能力缺口，必须列。
    auto exclusion_for = [](const std::string& reason_code) {
        RepairResult result = EvaluatedBaseline();
        result.executions.push_back(AttemptedExecution(RepairAction::S08_TolerantTranscode));
        result.fidelity.video_reencoded = true;
        RepairTranscodeTrackDecision rejected;
        rejected.input_stream_index = 1;
        rejected.kind = "audio";
        rejected.action = "drop";
        rejected.reason_code = reason_code;
        rejected.text = "流 #1（audio, flac）→ 未保留：" + reason_code;
        result.fidelity.transcode.track_decisions.push_back(rejected);
        return FindItem(repair::BuildDeclaredExclusions(result), "extra_tracks_not_rebuilt");
    };
    EXPECT_TRUE(exclusion_for("audio_drop_reencode_disabled") == nullptr)
        << "用户关掉重编码是别人的决定，不能算成「我们不重建」的能力缺口";
    EXPECT_TRUE(exclusion_for("stream_marked_discard") == nullptr) << "源侧标记丢弃同理";
    EXPECT_TRUE(exclusion_for("audio_drop_no_fitting_encoder") != nullptr)
        << "容器拒绝且没有可用重编码目标 = 我们的能力缺口，必须列进声明";
}

TEST(RepairDeclaredExclusions, ReferenceBorrowedBasisIsAnExplicitStructuredFact) {
    RepairResult result = EvaluatedBaseline();
    result.timeline_basis.mode = "reference_borrowed";
    const auto declared = repair::BuildDeclaredExclusions(result);
    ASSERT_TRUE(declared.available);
    const auto* item = FindItem(declared, "vfr_frame_durations_not_recovered");
    ASSERT_NE(item, nullptr);
    EXPECT_NE(
        std::find(item->trigger_facts.begin(), item->trigger_facts.end(), "timeline_basis.mode=reference_borrowed"),
        item->trigger_facts.end())
        << "触发事实必须是 字段=值，不许写成「参考辅助路径」这类自由文本";
    EXPECT_EQ(item->status, "not_attempted") << "本片只标注口径，不实际恢复逐帧时长";

    // 反向守护：没有该结构化事实（保码流 / S08）时，连自由文本写着「时基来自
    // 参考样片」也不得挂这条，防止新触发条件被放宽成「看着像就挂」。
    RepairResult plain = EvaluatedBaseline();
    plain.timeline_basis.mode = "container_pts";
    plain.reference_assist.used = false;
    plain.fidelity.timeline_note = "时基来自参考样片";
    EXPECT_EQ(FindItem(repair::BuildDeclaredExclusions(plain), "vfr_frame_durations_not_recovered"), nullptr);
}

TEST(RepairDeclaredExclusions, ThreeFormatsShareSameCodeSet) {
    RepairResult result = EvaluatedBaseline();
    result.executions.push_back(AttemptedExecution());
    result.diagnosis.fragmented = true;
    result.declared_exclusions = repair::BuildDeclaredExclusions(result);
    const auto bundle = WriteReports(result, "homogeneous");
    ASSERT_TRUE(bundle.ok) << bundle.error;

    const auto json_codes = JsonCodes(bundle.root);
    ASSERT_EQ(json_codes.size(), 2u);
    for (const auto& code : json_codes) {
        EXPECT_NE(bundle.text.find(code), std::string::npos) << code << " 缺 TXT 行";
        EXPECT_NE(bundle.html.find(code), std::string::npos) << code << " 缺 HTML 行";
    }
    // 未触发的模板不许出现在任何格式里（反向守护）。
    for (const auto& item : repair::AllDeclaredExclusions()) {
        if (std::find(json_codes.begin(), json_codes.end(), item.code) != json_codes.end())
            continue;
        EXPECT_TRUE(ConclusionAbsent(bundle, item.code)) << item.code << " 无事实却出行";
    }
    const JsonValue* node = bundle.root.Find("declared_exclusions");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->Find("available")->BoolValueOr(false));
    const JsonValue* items = node->Find("items");
    ASSERT_NE(items, nullptr);
    for (const auto& item : items->Items()) {
        EXPECT_FALSE(item.Find("title")->StringValueOr().empty());
        EXPECT_FALSE(item.Find("scope")->StringValueOr().empty());
        EXPECT_EQ(item.Find("status")->StringValueOr(), "not_attempted");
        EXPECT_FALSE(item.Find("applies_to")->StringValueOr().empty());
        EXPECT_FALSE(item.Find("consequence")->StringValueOr().empty());
        EXPECT_FALSE(item.Find("evidence")->StringValueOr().empty());
        EXPECT_GT(item.Find("trigger_facts")->Size(), 0u);
    }
    for (const auto& body : {bundle.text, bundle.html}) {
        EXPECT_NE(body.find("本次明确未恢复项"), std::string::npos);
        EXPECT_NE(body.find("评估状态"), std::string::npos);
        EXPECT_NE(body.find("命中条数"), std::string::npos);
        EXPECT_NE(body.find("清单限制"), std::string::npos);
        EXPECT_NE(body.find("不代表已覆盖"), std::string::npos);
    }
}

TEST(RepairDeclaredExclusions, UnavailableStateIsExplicitInAllFormats) {
    RepairResult result = EvaluatedBaseline();
    result.declared_exclusions.available = false;
    result.declared_exclusions.unavailable_reason = "引擎没有评估本次明确未恢复项";
    const auto bundle = WriteReports(result, "unavailable");
    ASSERT_TRUE(bundle.ok) << bundle.error;

    const JsonValue* node = bundle.root.Find("declared_exclusions");
    ASSERT_NE(node, nullptr);
    EXPECT_FALSE(node->Find("available")->BoolValueOr(true));
    EXPECT_EQ(node->Find("unavailable_reason")->StringValueOr(), "引擎没有评估本次明确未恢复项");
    EXPECT_TRUE(JsonCodes(bundle.root).empty());
    for (const auto& body : {bundle.text, bundle.html}) {
        EXPECT_NE(body.find("本次明确未恢复项"), std::string::npos);
        EXPECT_NE(body.find("未评估"), std::string::npos) << body;
        EXPECT_NE(body.find("不等于没有明确未恢复项"), std::string::npos) << body;
    }
    for (const auto& item : repair::AllDeclaredExclusions())
        EXPECT_TRUE(ConclusionAbsent(bundle, item.code)) << item.code << " 未评估态不得列具体条目";
}

TEST(RepairDeclaredExclusions, EmptyHitStateIsExplicitInAllFormats) {
    RepairResult result = EvaluatedBaseline();
    result.declared_exclusions = repair::BuildDeclaredExclusions(result);
    ASSERT_TRUE(result.declared_exclusions.items.empty());
    const auto bundle = WriteReports(result, "empty_hits");
    ASSERT_TRUE(bundle.ok) << bundle.error;

    const JsonValue* node = bundle.root.Find("declared_exclusions");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->Find("available")->BoolValueOr(false));
    EXPECT_EQ(node->Find("items")->Size(), 0u);
    for (const auto& body : {bundle.text, bundle.html}) {
        EXPECT_NE(body.find("本次明确未恢复项"), std::string::npos);
        EXPECT_NE(body.find("评估状态"), std::string::npos);
        EXPECT_NE(body.find("已评估"), std::string::npos);
        EXPECT_NE(body.find("本次未触发任何明确未恢复项"), std::string::npos) << body;
        EXPECT_NE(body.find("清单限制"), std::string::npos);
    }
}

} // namespace
