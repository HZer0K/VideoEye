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
#include "core/domain/model/RecoveredSample.h"

namespace videoeye {
namespace model {

// ---- 修复目标模式 ----
enum class RepairMode {
    PreserveBitstream, // 保码流：只修封装 / 索引 / 时间轴，不改动编解码数据
    RecoverContent,    // 救内容：允许转码或重建，优先保住可感知内容
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

// ---- 跳段恢复点的证据来源 ----
//
// 「恢复点」不能只写一句自由文本：seek 成功不等于内容可解码，索引也许根本不可用。
// 每段恢复必须自证恢复点是怎么找到的，强度按枚举顺序递增：
//   Unknown       旧记录 / 未标注
//   AvSeekFrame   依赖 demuxer 索引 seek —— 索引损坏时这条路直接失效
//   MoofResync    容器级重同步：损坏盒链之后找到可信 moof / trun / mdat
//   BitstreamRap  码流级重同步：在载荷窗口内扫 NAL / AU 找到随机访问点
//   TrialDecodeOk 候选点经短窗口试解码确认能建立有效解码状态（最强）
// 只有 TrialDecodeOk 允许报告声称「该点之后可解码」；其余只能声称「找到了候选」。
enum class RepairRecoveryEvidence {
    Unknown,
    AvSeekFrame,
    MoofResync,
    BitstreamRap,
    TrialDecodeOk,
};

// ---- 产物容器 ----
//
// 选择产物封装格式。Auto 沿用源文件扩展名（mov/m4v 保留，其它统一 mp4）。
// ⚠️ MPEG-TS 要求 H.264/HEVC 使用 Annex B 码流，执行器会应用
// h264_mp4toannexb / hevc_mp4toannexb 比特流过滤器；此时产物载荷与源不一致，
// 不能再声称"码流载荷未改动"，验证会跳过载荷哈希比对并在报告里标注。
enum class RepairOutputContainer {
    Auto,
    Mp4,
    Mkv,
    Ts,
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
const char* ToString(RepairRecoveryEvidence value);
const char* ToString(RepairOutputContainer value);
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
const char* DisplayName(RepairRecoveryEvidence value);
const char* DisplayName(RepairOutputContainer value);
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

// ---- 转码保真属性清单（阶段 6 / 切片 J）----
//
// S08 把解码帧重新编码成 8-bit yuv420p。源侧的「像素位深 / 色彩标记 / 旋转
// 矩阵 / HDR 元数据 / 帧尺寸变化」不会跟着画面一起过去；旧实现既不继承也不
// 报告，产物能播，报告里却没有任何一条说明它丢了什么。这个清单只记**实测**
// 到的属性，不做「源大概率是 SDR」这类推断。
//
// 口径（反向守护）：inspected=false = 这次根本没探测过（保码流路径 / 转码
// 未完成）。此时列表必须为空、all_properties_preserved 恒 false —— 空表不能
// 冒充「没有损失」。
struct RepairTranscodeFidelity {
    bool inspected = false;
    std::string source_pixel_format;
    std::string output_pixel_format;
    int source_bit_depth = 0;
    int output_bit_depth = 0;
    int source_width = 0;
    int source_height = 0;
    std::string color_primaries;
    std::string color_transfer;
    std::string color_space;
    std::string color_range;
    std::string chroma_location;
    std::string sample_aspect_ratio;
    double rotation_degrees = 0.0;
    bool has_display_matrix = false;
    bool has_hdr_metadata = false;
    // 与首帧尺寸不同的帧数（逐帧实测，不是容器声明）。
    uint64_t resolution_changed_frames = 0;
    std::vector<std::string> preserved_properties;
    std::vector<std::string> lost_properties;
    // 只有「探测过 + 丢失项为空」才允许为 true。重编码的代际画质损失由
    // video_reencoded / perceptual_fidelity 单独表达，不混进这个结论。
    bool all_properties_preserved = false;
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
    // 阶段 6 / 切片 J：S08 重编码前后「源侧属性」逐项实测结论。
    // 非转码路径保持 inspected=false，导出时必须如实写出「未探测」。
    RepairTranscodeFidelity transcode;

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

    // 音画同步：比较首条视频轨与首条音频轨的展示时间范围。
    // start/end delta 是两端绝对偏移；drift 是两端偏移的变化量，才是真正随时间累积的漂移。
    bool av_sync_checked = false;
    double av_sync_start_delta_seconds = 0.0;
    double av_sync_end_delta_seconds = 0.0;
    double av_sync_drift_seconds = 0.0;
    // not_run / not_applicable / within_tolerance / drift
    std::string av_sync_status = "not_run";

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
    // 恢复点来源。Unknown 只用于没有恢复点的首段与旧记录，不许当成「已核验」。
    RepairRecoveryEvidence recovery_evidence = RepairRecoveryEvidence::Unknown;
    std::string recovery_point; // 恢复点描述（人类可读，含定位参数）
};

// ---- 损失区间（执行期实测）----
//
// ⚠️ 与 diagnosis.coverage.skipped_ranges 不是一回事：后者是「没敢扫的内容类别」
// 的文字说明（如 "mdat 载荷未逐字节扫描"），这里是源时间轴上确实没进入产物的区间。
// 它由 recovered_segments 派生，不另起一份真值表；没有其它副本时 recoverable=false，
// 绝不能把合成 / 补帧写成「已恢复」。
// 参考样片辅助重建的记账（切片 C）。
// 「借了什么、怎么核验的、哪些结论是推断」必须与恢复结果同进三格式报告：
// 只报「已修复」而不说参数来自另一份文件，等于把推断伪装成实测。
struct RepairReferenceAssist {
    bool used = false;
    std::string reference_name;
    std::string compatibility; // verified / unverified / rejected
    std::string compatibility_reason;
    std::vector<std::string> borrowed_parameters;
    std::string layout_evidence; // 布局线索如何在损坏文件里被逐块自证
    std::string timescale_check;
    std::vector<std::string> limitations;
};

struct RepairLostRange {
    double input_start_seconds = 0.0;
    double input_end_seconds = 0.0;
    std::optional<uint64_t> byte_start; // 拿不到字节边界就留空，不填 0
    std::optional<uint64_t> byte_end;
    std::string reason;   // 稳定机读码，如 "container.damaged_segment_skipped"
    std::string evidence; // 实测依据（跳过秒数 / 恢复点）
    bool recoverable = false;

    double DurationSeconds() const {
        return input_end_seconds > input_start_seconds ? input_end_seconds - input_start_seconds : 0.0;
    }
};

// ---- 逐轨覆盖与音画共同区间（切片 G）----
//
// 全局 `lost_ranges` 只能说明「源时间轴上有一段没进产物」，说明不了「是哪条轨缺的」。
// 一条轨的索引失效时另一条轨可能全程连续；音画两条轨的缺口起点也常常差几百毫秒。
// 只报全局秒数会把「音频其实完整」说成「音画都缺了」。
//
// 这一块由样本账本的**流式统计**导出，不读明细缓存 —— 明细超过上限就变成抽样，
// 拿它算区间会把「没缓存到的样本」误判成丢帧。所有计数都是全量精确值。
//
// 区间口径：全部是**输入时间轴**秒数（与 `lost_ranges` 同一口径），不是产物时间轴。
// 开了 MergeContiguous 时产物时间轴会被平移，两张表不能直接相减。
//
// 判据说明（缺口语义）：样本按写入顺序推进，重排帧（B 帧）的 PTS 会略早于已见最大值，
// 此时不产生间隔（不贡献缺口）。因此缺口起点可能被高估至多一个重排深度，
// 方向是「少报」而不是「多报」。
struct RepairTrackGap {
    double start_seconds = 0.0;
    double end_seconds = 0.0;

    double DurationSeconds() const {
        return end_seconds > start_seconds ? end_seconds - start_seconds : 0.0;
    }
};

struct RepairTrackCoverage {
    int output_stream_index = -1;
    uint32_t track_id = 0;
    std::string kind;  // "video" / "audio" / "other"
    std::string codec; // 输入侧编码名，拿不到留空
    uint64_t samples = 0;
    // 该轨有 N 个样本没有时间戳：它们既不算覆盖也不算缺口，必须单独说清楚。
    uint64_t samples_without_timestamp = 0;
    bool span_known = false;
    double first_seconds = 0.0;
    double last_seconds = 0.0;
    // 缺口判据的基准：该轨样本间隔直方图众数桶的估算值（近似，不是实测中位数）。
    double typical_interval_seconds = 0.0;
    // 以下两项是全量精确值，与缺口明细是否截断无关。
    uint64_t gap_count = 0;
    double gap_seconds = 0.0;
    std::vector<RepairTrackGap> gaps;
    bool gaps_truncated = false;
};

struct RepairTrackCoverageSummary {
    bool available = false;
    // 扫描走到源末尾（未被取消 / 超时 / 读写失败打断）。false 时逐轨覆盖只是前缀，
    // 不能当成「整段没内容」的证据。
    bool complete = false;
    std::string unavailable_reason;
    std::vector<RepairTrackCoverage> tracks;
    // 所有轨 span 的交集 —— 这才是「音画同时在播」的区间。
    bool common_span_known = false;
    double common_start_seconds = 0.0;
    double common_end_seconds = 0.0;
    std::string common_span_note;
    std::vector<std::string> limitations;
};

// ---- 显示顺序证据（切片 E）----
//
// 容器只给解码序；B 帧重排后的显示序只能从码流 POC 推。这一块记录"推没推、
// 依据是什么、推到什么程度"，与 ctts 实际有没有写进产物分开落字段：
// `analyzed` 是"跑过测量"，`available` 是"测量给出了可用结论"，
// `ctts_written` 是"产物里真的写了 ctts" —— 三者不能互相冒充。
struct RepairDisplayOrderSummary {
    bool analyzed = false;
    bool available = false;
    std::string unavailable_reason;
    std::string codec;        // "h264" / "hevc"
    std::string order_source; // "poc" / "display_rank" / "order_hint"
    std::string basis;        // 推导依据
    // verified / derived / unknown
    std::string confidence = "unknown";
    // 帧时长证据（ctts 的单位来源）
    std::string duration_source = "none";
    uint32_t timescale = 0;
    uint32_t sample_delta = 0;
    uint64_t frames = 0;
    uint64_t frames_observed = 0;
    bool truncated = false;
    uint64_t rap_segments = 0;
    uint64_t reordered_frames = 0;
    uint64_t max_reorder = 0;
    bool ctts_required = false;
    // 产物里是否真的写了 ctts（只有 available && ctts_required && 条数守恒才为 true）。
    bool ctts_written = false;
    std::vector<std::string> limitations;
};

// ---- 时基（每帧时长）依据（切片 F）----
//
// 时间轴上的所有秒数都来自"一帧多长"。这个值要么有源侧证据（SPS VUI 的
// timing_info、容器 stts、ADTS 采样率），要么是用户显式给的，要么就不存在。
// 三种情况必须分开落字段，不能合成一个"可用/不可用"：
//   * `mode`         —— "vui_timing" / "user_input" / "container_pts" /
//                        "adts_sample_rate" / "unknown"
//   * `frame_rate`   —— "30000/1001" 这类精确分数；拿不到就是 "unknown"，
//                        不从 mode 反推，也不写近似小数
//   * `constant`     —— 每个样本的时长能否当常数用（变帧率下不成立）
//   * `vfr_declared` —— 码流自己声明了非恒定帧率（SPS VUI fixed_frame_rate_flag=0）。
//                        此时 `constant` 仍然可以为 true（VUI 给的是名义值），
//                        但"逐帧时长可验证"这件事是假的，报告必须说出来。
struct RepairTimelineBasis {
    std::string mode = "unknown";
    std::string frame_rate = "unknown";
    bool constant = false;
    bool vfr_declared = false;
    std::string detail;
    std::vector<std::string> limitations;
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
    // 完全缺失时间戳的包中，实际按标称帧时长合成并写入产物的数量。
    uint64_t synthesized_timestamps = 0;
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
    // 产物实际容器（auto / mp4 / mov / mkv / ts）：与请求的 output_container 区分，
    // Auto 模式下真正落盘的容器由源扩展名决定。
    std::string output_container;

    std::vector<RepairActionExecution> executions;
    std::vector<RepairTrackConservation> track_conservation;
    // 样本层实测摘要：上三层计数的事实源。available=false 表示本次执行没有
    // 接入样本账本（例如尚未改造的转码路径），不能当成「零样本」。
    RepairSampleLedgerSummary sample_ledger;
    std::vector<std::string> sample_conservation_problems;
    // S04 / S03 的实测记录：报告必须能解释保留了哪些区间、时间轴改了什么。
    std::vector<RepairRecoveredSegment> recovered_segments;
    // S04 跳过的源时间区间：由 recovered_segments 派生，供报告逐段说明损失。
    std::vector<RepairLostRange> lost_ranges;
    // 切片 G：逐轨覆盖 / 逐轨缺口 / 音画共同覆盖区间。available=false 表示本次
    // 执行没有接入样本账本（例如转码或参考辅助重建路径），不能当成「零缺口」。
    RepairTrackCoverageSummary track_coverage;
    RepairTimelineRepair timeline_repair;
    // 参考样片辅助重建（切片 C）：借了哪些解码配置、布局线索如何在源侧自证。
    RepairReferenceAssist reference_assist;
    // 显示顺序证据（切片 E）：POC 实测结论与 ctts 是否真写进产物。
    RepairDisplayOrderSummary display_order;
    // 切片 F：时间轴秒数的来源。`mode == "unknown"` 表示这次没有拿到时基证据，
    // 报告不能说"时间轴没问题"。
    RepairTimelineBasis timeline_basis;
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
