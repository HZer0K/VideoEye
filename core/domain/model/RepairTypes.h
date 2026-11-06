#pragma once

// 视频修复模块的领域值类型。
//
// 这一层只放"结果与契约"，不做任何 IO：修复引擎（core/repair）产出这里定义的类型，
// 报告层（core/reporting）与界面（ui/repair_panel）只消费它们。
//
// 设计原则（与 docs/VIDEO_REPAIR_MODULE_DESIGN.md 对齐）：
//   * 证据与结论分离：DamageFinding 只描述"在哪里、测到了什么"，
//     业务结论（可修 / 不可修）由 RepairDiagnosis / RepairPlan / RepairResult 承载；
//   * 定位必须可解释：优先给精确字节区间，拿不到时降级为有界 / 估算并写明来源，
//     不允许把"猜出来的位置"写成精确位置；
//   * 保真度单独建模：是否重编码、是否插入合成帧、时间轴是否被改写都必须显式记录，
//     不能用一句"验证通过"掩盖。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/model/DiagnosticIssue.h"

namespace videoeye {
namespace model {

// ---- 修复目标模式 ----
enum class RepairMode {
    PreserveBitstream,     // 保码流：只修封装 / 索引 / 时间轴，不改动编解码数据
    RecoverContent,        // 救内容：允许转码或重建，优先保住可感知内容
    PlaybackCompatibility, // 播放兼容：为指定播放器 / 分发场景做封装或编码调整
};

// ---- 损坏区间的时间轴处理策略 ----
//
// 「保留原时间间隔」与「删除损坏区间后连续播放」是两种语义不同的产物：
// 前者保留源时间轴（中间有空洞），后者主动改写时间戳让播放连续。
// 不能用一个隐式默认把两者混在一起，否则报告无法解释时间轴到底动了什么。
enum class RepairGapPolicy {
    PreserveOriginalTimeline, // 保留原始时间间隔（默认）
    MergeContiguous,          // 删除损坏区间，把后续片段平移到前一段末尾
};

// ---- 业务状态（终态之外的"修复结果"语义）----
enum class RepairBusinessStatus {
    Repaired,             // 修复完成，且指定验证通过、未发现内容缺失
    PartiallyRecovered,   // 已产出可读文件，但存在明确的内容缺失或修复边界
    NeedsReference,       // 缺少可补充的配置 / 样片 / 初始化段，需要参考文件才能继续
    Unsupported,          // 已准确识别问题，但当前版本的恢复能力不覆盖
    NoRecoverableContent, // 输入里没有可恢复的媒体内容
    Failed,               // 执行、验证或发布失败
    Canceled,             // 用户取消
    TimedOut,             // 超出预算被停止
};

// ---- 检查状态（每一层单独记账）----
enum class RepairCheckStatus {
    NotRun,       // 未执行（预算不足 / 不适用）
    Passed,       // 已检查且通过
    Failed,       // 已检查且失败
    Inconclusive, // 已检查但证据不足（例如只扫了部分样本）
    Unsupported,  // 当前版本不覆盖这类检查
};

// ---- 诊断层级 ----
enum class DamageLayer {
    File,       // 文件身份 / 大小 / 可读性
    Container,  // 盒结构 / 索引 / 分片
    Track,      // 轨道 / 样本表
    Codec,      // 参数集 / 编码语法
    Decode,     // 实际解码
    Timing,     // 时间戳 / 时长 / 同步
    Redundancy, // 冗余副本 / 双层 / 多描述
};

// ---- 证据强度与精度 ----
enum class DamageConfidence {
    Confirmed, // 可复现的实测证据
    Probable,  // 结构性证据强，但未逐包复现
    Possible,  // 启发式提示，需要后续验证
    Unknown,
};

enum class DamagePrecision {
    Exact,     // 精确字节 / 样本 / 帧位置
    Bounded,   // 已知上下界，但边界未逐条确认
    Estimated, // 由结构推算，可能有偏差
    Unknown,
};

// ---- 候选 / 已执行策略 ----
enum class RepairAction {
    S01_Remux,             // 保留码流重新封装（重建索引）
    S02_TailSalvage,       // 截断尾部后抢救可读前缀
    S03_TimelineRepair,    // 时间轴修复（重复 / 回退 / 断裂）
    S04_SegmentSalvage,    // 多段恢复：跳过损坏区间后继续提取后续可解码内容
    S08_TolerantTranscode, // 容忍式转码
    S09_SingleTrack,       // 单轨降级
};

// ---- 验证等级 ----
enum class RepairVerificationLevel {
    V0_Container,   // 只验证产物可被容器层打开
    V1_FrameSample, // 抽帧验证（首 / 中 / 尾）
    V2_FullDecode,  // 全量解码 + 跳转验证
};

enum class RepairFidelityStatus {
    VerifiedMatch,     // 已核对，码流载荷未被改动
    PartiallyVerified, // 只核对了一部分
    NotVerified,       // 未核对
    Failed,            // 核对发现改动 / 不一致
};

const char* ToString(RepairMode value);
const char* ToString(RepairGapPolicy value);
const char* ToString(RepairBusinessStatus value);
const char* ToString(RepairCheckStatus value);
const char* ToString(DamageLayer value);
const char* ToString(DamageConfidence value);
const char* ToString(DamagePrecision value);
const char* ToString(RepairAction value);
const char* ToString(RepairVerificationLevel value);
const char* ToString(RepairFidelityStatus value);

// 人类可读名称（界面 / 报告用）。与 ToString 分开：后者是稳定的机读标识，
// 前者允许以后改文案而不破坏 JSON 契约。
const char* DisplayName(RepairBusinessStatus value);
const char* DisplayName(RepairAction value);
const char* DisplayName(RepairGapPolicy value);
const char* DisplayName(RepairVerificationLevel value);

// ---- 字节区间（半开区间 [begin, end)）----
struct RepairByteExtent {
    uint64_t begin = 0;
    uint64_t end = 0;
    bool known_end = true; // false = 只知道起点（读不到真实结尾）

    bool Valid() const {
        return known_end ? end >= begin : begin > 0;
    }
    uint64_t SizeBytes() const {
        return known_end ? (end >= begin ? end - begin : 0) : 0;
    }
};

// ---- 问题定位 ----
//
// 所有字段都可缺省：拿不到就留空，绝不填一个"看起来像"的值。
struct DamageLocation {
    std::optional<RepairByteExtent> bytes; // 文件内字节区间

    std::optional<int> stream_index;  // FFmpeg 流下标
    std::optional<uint32_t> track_id; // 容器轨道 id
    std::optional<uint64_t> packet_ordinal;
    std::optional<uint64_t> sample_ordinal;
    std::optional<uint64_t> frame_ordinal;
    std::optional<uint64_t> item_count; // 同类受影响的样本 / 包 / 帧总数

    std::string structure_path; // 如 "moov/trak/mdia/minf/stbl/stsz"
    std::string field_name;     // 如 "sample_size"

    std::optional<int64_t> pts;
    std::optional<int64_t> dts;
    std::optional<int> time_base_num;
    std::optional<int> time_base_den;

    std::optional<double> source_seconds;    // 源时间基下的位置
    std::optional<double> estimated_seconds; // 由结构推算
    std::optional<double> output_seconds;    // 产物内位置

    DamagePrecision precision = DamagePrecision::Unknown;
    std::string provenance; // 证据来源，如 "isobmff.stsz/stsc vs file_size"

    bool HasTime() const {
        return source_seconds.has_value() || estimated_seconds.has_value() || output_seconds.has_value();
    }
};

// ---- 单条损伤证据 ----
struct DamageFinding {
    std::string code;     // 稳定机读码，如 "container.moov_missing"
    std::string detector; // 产出它的检测器
    DamageLayer layer = DamageLayer::File;
    IssueSeverity severity = IssueSeverity::Warning;
    DamageLocation location;
    DamageConfidence confidence = DamageConfidence::Possible;

    std::string evidence;       // 测到了什么（数值 / 边界 / 计数）
    std::string hypothesis;     // 结构性推断（可为空）
    std::string cause_issue_id; // 关联的上游证据 id（如果有）

    std::vector<RepairAction> candidate_actions; // 能处理它 / 需要它的策略
    std::vector<std::string> limitations;        // 这条证据不能证明什么
};

// ---- 未实现能力的边界契约 ----
//
// 这类能力最容易被「提供个参考文件就能修」的笼统文案掩盖：用户看不出哪些内容
// 物理上拿不回来、时间轴会不会变、画质会损失什么。所以每条都单独定义前提、
// 不可恢复范围、时间轴变化与质量风险，并附判定依据。
// ⚠️ 只在诊断命中对应证据时挂载 —— 没有事实依据时不得输出「当前不支持」的空壳。
struct RepairCapabilityBoundary {
    std::string code;                             // moov_rebuild / parameter_set_recovery / decodable_gop_transcode
    std::string status;                           // unsupported（当前版本未实现）/ deferred
    std::vector<std::string> preconditions;       // 满足才谈得上「可恢复」
    std::vector<std::string> unrecoverable_scope; // 即使实现也拿不回来的东西
    std::string timeline_impact;                  // 时间轴变化：必然改写 / 需核验 / 未知
    std::vector<std::string> quality_risks;       // 画质、音画同步、语义风险
    std::string evidence;                         // 判定依据（实测或结构论证）
    std::vector<std::string> trigger_findings;    // 本次命中它的诊断证据 code
};

// ---- 覆盖范围 ----
//
// "没发现问题"必须能回答"到底看了多少、跳过了什么、为什么停"。
struct RepairCoverage {
    uint64_t bytes_scanned = 0;
    uint64_t packets_scanned = 0;
    uint64_t frames_scanned = 0;
    double time_scanned_seconds = 0.0;
    bool reached_eof = false;
    bool truncated_by_budget = false;

    std::vector<std::string> skipped_ranges;
    std::vector<std::string> unsupported_items;
    std::vector<std::string> limitations;
};

// ---- 单项检查 ----
struct RepairCheck {
    std::string name;
    DamageLayer layer = DamageLayer::File;
    RepairCheckStatus status = RepairCheckStatus::NotRun;
    std::string detail;
    uint64_t cost_ms = 0;
};

// ---- 输入快照（执行前后比对，防止"修复过程中源文件被改"）----
//
// ⚠️ size 只是最弱的一层证据：同大小改写（原位补丁、勒索软件加密、替换同长片段）
// 完全可能在大小不变的情况下发生。所以除 size 外还要记 mtime 与内容摘要，
// 并把"用什么方法核对的"写进结果，避免把"没发现变化"说成"已证明没变"。
struct RepairInputSnapshot {
    std::string path;
    uint64_t size_bytes = 0;
    int64_t mtime_epoch_seconds = 0;
    std::string signature; // 首个盒 / 探测到的封装特征

    // 内容摘要（FNV-1a 64）：空串表示没有取到，不能当作"内容一致"。
    std::string content_hash;
    // full / head_tail / unavailable：说明摘要的覆盖范围（大文件只取头尾时是弱证据）。
    std::string hash_method;
    uint64_t hashed_bytes = 0;
};

// 源容器逐段声明的样本数（守恒校验基线）。
//
// 只有"确实从结构里数出来"才填：分片序列来自 moof/trun，普通 MP4 来自 stsz。
// 估算值、FFmpeg 探测值一律不写进来。执行后拿它与实际写入包数比对，
// 才能发现"demuxer 在解析阶段静默丢包"这类不会报错的内容缺失。
struct RepairExpectedTrack {
    uint32_t track_id = 0;
    uint64_t sample_count = 0;
    std::string codec_hint;
    std::string provenance;
};

// ---- 逐轨可抢救证据（S09 单轨抢救的判定依据）----
//
// "省略一条轨"必须是来自事实的降级，而不是静默放弃：这里显式记录每条媒体轨
// 是否还有可交付的样本证据、依据是什么，规划层才允许把它从输出里剔除。
struct RepairTrackSalvageInfo {
    uint32_t track_id = 0;
    int input_stream_index = -1;
    std::string kind; // "video" / "audio"
    bool salvageable = true;
    std::string evidence; // 判定依据（越界样本数 / 样本总数 / 未检查）
};

// ---- 诊断结果 ----
struct RepairDiagnosis {
    std::string diagnosis_id;
    std::string request_id;
    std::string source_path;
    RepairInputSnapshot input;

    bool openable = false;        // FFmpeg 能否在常规模式下打开
    bool container_known = false; // 是否识别出受支持的容器
    bool moov_present = false;
    bool fragmented = false;

    std::vector<RepairCheck> checks;
    std::vector<DamageFinding> findings;
    RepairCoverage coverage;
    std::vector<RepairExpectedTrack> expected_tracks; // 源侧样本数（守恒基线）
    // 逐轨可抢救证据：只有跑过样本边界检查的轨道才会填。
    std::vector<RepairTrackSalvageInfo> track_salvage;
    // 本次诊断命中的「当前版本未实现能力」边界契约；只在有证据时填充。
    std::vector<RepairCapabilityBoundary> capability_boundaries;

    std::vector<std::string> blocking_reasons; // 当前能力无法继续的原因
    std::vector<RepairAction> candidate_actions;
    std::string summary;
};

// ---- 计划 ----
struct RepairActionPlan {
    RepairAction action = RepairAction::S01_Remux;
    std::string description;
    bool available = true;
    std::string unavailable_reason;
    std::string expected_effect;
    std::string risk;
    // 不可用时指向诊断里的能力边界契约 code，让「为什么做不到」可追溯，
    // 而不是一句「当前版本未实现」。
    std::string capability_boundary;
    std::vector<RepairAction> depends_on;
};

struct RepairPlan {
    std::string plan_id;
    std::string diagnosis_id;
    RepairMode mode = RepairMode::PreserveBitstream;
    bool feasible = false;
    std::string infeasible_reason;

    std::vector<RepairActionPlan> actions;
    std::vector<std::string> global_preconditions;
    std::string expected_output_extension;

    // 执行边界（由诊断推出）：超过这个时间的内容已被确认不可读。
    std::optional<double> salvage_end_seconds;

    // 由诊断判定为「没有可交付样本」而必须在输出里剔除的输入流 / 轨道。
    // 只有用户允许单轨抢救（RepairRequest::allow_single_track）时才会非空。
    std::vector<int> dropped_input_streams;
    std::vector<uint32_t> dropped_track_ids;
};

// ---- 动作执行记录 ----
struct RepairActionExecution {
    RepairAction action = RepairAction::S01_Remux;
    bool attempted = false;
    bool succeeded = false;
    std::string detail;
    std::string error;

    uint64_t packets_read = 0;
    uint64_t packets_written = 0;
    uint64_t bytes_written = 0;
    std::optional<double> output_start_seconds;
    std::optional<double> output_end_seconds;

    // 输入侧参与核对的视频包载荷哈希（用于证明"码流载荷未被改动"）
    std::optional<uint64_t> input_video_payload_hash;
    uint64_t input_video_packet_count = 0;
};

// ---- 保真度 ----
struct RepairFidelity {
    RepairFidelityStatus video = RepairFidelityStatus::NotVerified;
    RepairFidelityStatus audio = RepairFidelityStatus::NotVerified;

    bool video_reencoded = false;
    bool audio_reencoded = false;
    bool synthetic_frames_inserted = false;
    uint64_t synthetic_frame_count = 0;
    // timeline_modified 只表示「引擎是否主动重写时间戳」，不等于「产物时间轴相对源未变」：
    // 借用参考初始化段时，未核验的 mdhd 时基会悄悄改变播放时长。
    bool timeline_modified = false;
    // unchanged / modified / unknown。默认 unknown —— 没有证据时不能先声称没改写。
    std::string timeline_status = "unknown";
    std::string timeline_note;

    // complete_for_selected_checks / partial / unknown / no_content
    std::string content_completeness = "unknown";
    std::optional<double> original_content_recovery_ratio;

    // 感知保真度：没有参考样片时只能是 unknown_without_reference
    std::string perceptual_fidelity = "unknown_without_reference";
    std::vector<std::string> limitations;

    // R03：参考段借来的参数必须逐项列出；时基没有源侧证据时不能写"兼容"。
    // verified / unverified / rejected / not_applicable
    std::string reference_compatibility = "not_applicable";
    std::vector<std::string> borrowed_parameters;
    // R11：被省略的轨道 / 元数据 / 章节，逐项列出而不是静默丢掉。
    std::vector<std::string> omitted_items;
};

// ---- 逐轨样本守恒（R07）----
//
// "全轨合计 + 统一容差"会让一条大轨道把小轨道整轨丢失掩盖过去：1000/951
// 也能算出 95% 的"通过"。所以基线必须按轨道逐条比对。样本和包不总是 1:1
// （访问单元可能被拆分或合并），差值要显式记成关系，不能静默当容差消化掉。
struct RepairTrackConservation {
    uint32_t track_id = 0;
    int input_stream_index = -1;
    int output_stream_index = -1;
    std::string kind; // "video" / "audio"
    uint64_t expected_samples = 0;
    uint64_t packets_written = 0;
    // conserved / loss / excess / unmapped / unknown
    std::string status;
    // one_to_one / split / merged / unknown
    std::string relation;
    std::string note;
};

// ---- 逐轨验证目标（R04）----
//
// "保留轨道"是执行器实际写进产物的轨；"要求验证轨道"是必须逐轨给出解码结论的轨。
// 用户 request.target_streams 不再被诊断层拿去选一条主轨、执行层却复制全部轨，
// 而是显式映射到输出流下标：第二视频轨 / 音频轨损坏不再被整体 Passed 盖住。
struct RepairTrackVerificationTarget {
    uint32_t track_id = 0;
    int input_stream_index = -1;
    int output_stream_index = -1;
    std::string kind; // "video" / "audio" / "attached_pic"
    bool required = false;
    // R04 覆盖守护：产物里真实存在该轨，但执行器没有把它写进「保留轨 → 输出流」契约。
    // 验证器不能擅自替这份契约背书，只能留痕并让结论变成不确定。
    bool coverage_gap = false;
    std::string note;
};

// ---- 逐轨验证结果（R04）----
//
// "验证器固定挑第一条视频轨"时，第二视频轨 / 音频轨损坏会被整体 Passed 盖住。
// 覆盖范围是结果的一部分：verified=false 明确表示这一轨没有实际解码过。
struct RepairTrackVerification {
    uint32_t track_id = 0;
    int output_stream_index = -1;
    std::string kind; // "video" / "audio" / "attached_pic" / "unknown"
    RepairCheckStatus status = RepairCheckStatus::NotRun;
    bool verified = false; // 是否真的跑过解码
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t errors = 0;
    uint64_t corrupt_packets = 0; // R05：逐轨也要能看到损坏包，不能只汇总到全局
    uint64_t corrupt_frames = 0;
    uint64_t error_recovery_frames = 0;
    uint64_t valid_frames = 0;
    double duration_seconds = 0.0;
    std::string scope;
    std::string detail;
};

// ---- 验证结果 ----
struct RepairVerification {
    RepairVerificationLevel level = RepairVerificationLevel::V0_Container;
    RepairCheckStatus status = RepairCheckStatus::NotRun;

    std::vector<RepairCheck> checks;
    uint64_t decoded_frames = 0;
    uint64_t decode_errors = 0;
    // R05：API 返回成功但帧被标记损坏 / 解码器做了错误隐藏，都不等于"画面干净"。
    // 这几项分开记账，不能揉进一个 decode_errors。
    uint64_t corrupt_packets = 0;
    uint64_t corrupt_frames = 0;
    uint64_t error_recovery_frames = 0;
    uint64_t valid_frames = 0;
    // R05：帧级损坏标记是「解码器能力」，不是「画面干净度」的全集。
    // observed（至少一帧带标记）/ not_observed（解出帧但没有任何标记）/ not_run（未解码）。
    // ⚠️ not_observed 不能推出画面干净：项目锁定 FFmpeg 8.1 的可用解码器上
    // AV_FRAME_FLAG_CORRUPT 实测恒 0，字节损坏只走 API 错误 / decode_error_flags。
    std::string frame_marker_evidence = "not_run";
    uint64_t marked_frames = 0;                        // 实测带标记帧数 = corrupt_frames + error_recovery_frames
    std::vector<std::string> frame_marker_limitations; // 恒 0 的成因，必须随结论一起输出
    uint64_t seek_checks = 0;
    uint64_t seek_passes = 0;
    std::string decode_scope; // "video#0, audio#1, all packets"

    // R09：V0 只做容器层检查，绝不能把 structure_only=false 的路径和它混为一谈。
    bool structure_only = false;
    std::vector<RepairTrackVerification> per_track;
    std::vector<std::string> timeline_checks; // 逐轨时长 / 时间戳跨度对比（可读明细）
    // R03：时长证据必须结构化，不能只打印成文字行却不参与结论。
    // 错误时基可以「全部帧都解码成功、播放时长却变了一半」。
    double expected_duration_seconds = 0.0; // 源侧 sidx/tfdt 估算，0 = 无证据
    double observed_duration_seconds = 0.0; // 产物逐轨实测（取第一条可比的轨）
    double duration_delta_seconds = 0.0;
    bool timeline_comparable = false; // 两侧都有实测值才叫「比对过」
    bool timeline_mismatch = false;   // 偏差超阈值，必须拉低结论

    std::vector<DamageFinding> residual_findings;
    std::string evidence;
};

// ---- 恢复区间（S04 多段恢复的实测记录）----
//
// 一次修复可能由多个连续可读片段拼成。每个片段都记录输入侧时间范围与产物侧
// 时间范围：只有这样才能解释"为什么这一段保留、为什么另一段被丢弃"，也才能
// 区分「保留原间隔」与「连续拼接」两种产物的时间轴含义。
struct RepairRecoveredSegment {
    uint32_t index = 0;
    double input_start_seconds = 0.0;
    double input_end_seconds = 0.0;
    double output_start_seconds = 0.0;
    double output_end_seconds = 0.0;
    uint64_t packets_written = 0;
    bool resumed_after_damage = false;   // 是否由跳段恢复得到（第一段为 false）
    double skipped_before_seconds = 0.0; // 本段之前被跳过的输入时长
    std::string recovery_point;          // 恢复点依据，如 "av_seek_frame resync"
};

// ---- 时间轴修复记录（S03）----
//
// 只做有证据的修复：重复 / 回退的时间戳会被显式重写并计数；缺失时间戳只统计，
// 不在没有依据时伪造。缺口消除（MergeContiguous）单独标记，避免和"改了播放速度"
// 混为一谈。
struct RepairTimelineRepair {
    bool attempted = false;
    bool gap_removed = false;
    double removed_gap_seconds = 0.0;
    uint64_t timestamp_regressions = 0;
    uint64_t timestamp_duplicates = 0;
    uint64_t missing_timestamps = 0;
    uint64_t rewritten_timestamps = 0;
    // unchanged / adjusted / rebuilt / unknown
    std::string status = "unchanged";
    std::string note;
};

// ---- 产物 ----
struct RepairArtifact {
    std::string role; // "output" / "report_json" / "report_text" / "report_html";
    std::string path;
    std::string mime;
    std::string stored_range; // 如 "bytes [0, N)"
};

// ---- 修复结果 ----
struct RepairResult {
    std::string result_id;
    std::string request_id;
    std::string plan_id;
    std::string diagnosis_id;

    RepairBusinessStatus status = RepairBusinessStatus::Failed;
    std::string summary;
    std::string error;

    RepairDiagnosis diagnosis;
    RepairPlan plan;
    RepairVerification verification;
    RepairFidelity fidelity;

    RepairInputSnapshot input;
    // R08：参考文件同样要快照 —— 诊断读到的初始化段与执行时拼进去的可能不是同一份。
    RepairInputSnapshot reference_input;
    bool reference_snapshot_valid = false;
    std::string input_check_method; // 说明发布前到底核对了什么、有什么局限
    std::string output_path;
    bool output_published = false;

    std::vector<RepairActionExecution> executions;
    std::vector<RepairTrackConservation> track_conservation;
    // S04 / S03 的实测记录：报告必须能解释保留了哪些区间、时间轴改了什么。
    std::vector<RepairRecoveredSegment> recovered_segments;
    RepairTimelineRepair timeline_repair;
    std::vector<RepairArtifact> artifacts;
    std::vector<DamageFinding> residual_findings;

    std::string ffmpeg_version;
    std::string created_at_iso8601;

    // 报告路径由 RepairOutputWriter 统一命名（与产物共享 run id）；报告本身由
    // 表现层用 RepairReportExporter 写入，修复引擎不越层写报告。
    std::string report_json_path;
    std::string report_text_path;
    std::string report_html_path;
};

} // namespace model
} // namespace videoeye
