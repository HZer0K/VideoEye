#pragma once

// 切片 C：参考样片辅助的普通 MP4 moov 重建。
//
// 与 RepairPlainMp4Rebuilder 的区别：
//   * 无参考版只能处理「纯 H.264（AVCC）」或「纯 ADTS AAC」单轨，且要求 mdat
//     里有 in-band SPS/PPS 与 VUI 帧率证据。而真实录制文件把参数集只放在 moov
//     的 avcC/esds 里，删掉 moov 后载荷里只剩 slice 与裸 AAC AU，无参考必然失败。
//   * 这里从同源完整样片**只借解码配置**（stsd 的 sample entry 原始字节、
//     track→codec 映射、ftyp brands、逐轨时基作为待核对值），样本大小 / 偏移 /
//     时间表全部在损坏文件里重新生成。
//
// ⚠️ 第一版格式指纹（不满足一律 unverified + 拒绝，绝不猜）：
//   * 恰好一个 mdat、无 moov、无 moof；
//   * 恰好一条 H.264(avc1) 视频轨 + 一条 AAC(mp4a) 音频轨，4 字节 NAL 长度前缀；
//   * 参考文件是普通 MP4（非分片）；参考可以含 ctts（B 帧重排），但
//     **合成偏移一律不借**：显示顺序由损坏文件自身的码流 POC 实测后写进产物
//     的 ctts（切片 H）。参考只借 stsd 解码配置与待核对的时基；
//   * 两轨 stts 都必须是「可解释的」：音频恒为 1024 采样/帧；视频允许非恒定
//     （真 VFR），此时逐帧时长按参考样本表借用 —— 前提是同源且样本数守恒
//     （扫描阶段已强制相等）。VFR 下不写 ctts，因为 POC 偏移换算需要恒定帧长。
//   * 布局线索（chunk 顺序 + 每 chunk 样本数 + 裸 AAC AU 长度候选）必须在
//     损坏文件的 mdat 里逐块字节守恒自证，视频 AU 边界由 NAL 长度前缀实测。
//
// 裸 AAC AU 没有自描述边界，这是 ISOBMFF 的事实。第一版把参考 stsz 序列当作
// **布局线索候选**（不是真值）：每个音频 chunk 的总长必须与候选长度之和相等、
// 且不得与视频 NAL 链冲突，最终偏移与大小仍在损坏文件里实测。结论标 inferred，
// 并在报告里写明「音频边界不是自描述恢复」。完全自描述的 ADTS 路径留给第二版。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/RecoveredSample.h"
#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

class RepairRunBudget;

// stts 的一段（run-length）：连续 count 个样本、每个 delta tick。
struct RepairReferenceSttsRun {
    uint32_t count = 0;
    uint32_t delta = 0;
};

struct RepairReferenceTrackProfile {
    uint32_t track_id = 0;
    std::string codec;
    std::string handler;
    uint32_t timescale = 0;
    // 恒定轨 = 该轨的样本时长；变帧率轨 = **主 delta**（count 最大的那一段），
    // 只用于展示与兜底，重建按 `stts` 逐帧借用。
    uint32_t sample_delta = 0;
    uint32_t sample_count = 0;
    // 参考片该轨的完整 stts 序列（原样保留 run-length）。切片 I：非常量 stts
    // 不再整份拒绝 —— 逐帧时长表恰恰是 VFR 唯一的源侧证据，参考样片有它。
    std::vector<RepairReferenceSttsRun> stts;
    // 非常量 stts（真 VFR）。前提是参考与损坏文件同源且样本数守恒（ScanSource
    // 已强制相等），所以逐帧借用不会错位；ctts 仍不写，见 profile.limitations。
    bool variable_stts = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint8_t aac_object_type = 0;
    uint8_t aac_sfi = 0;
    // stsd 里第一个 sample entry 的完整字节（含 8 字节 box 头）：唯一允许借用的
    // 解码配置，直接原样写回重建产物的 stsd，不重新序列化 avcC / esds。
    std::vector<uint8_t> sample_entry;
    // 布局线索候选：参考文件该轨的 stsz 序列。仅用于无自描述边界的裸 AAC AU，
    // 必须在损坏文件里逐块守恒验证；视频轨只用来做「线索是否自洽」的对照。
    std::vector<uint32_t> candidate_sizes;
    // 参考片含 ctts（B 帧显示顺序重排）。**只记录、不借用**：合成偏移必须由
    // 损坏文件自身的码流 POC 实测得到（切片 E/G 的链路），照抄参考的 ctts
    // 等于把另一份文件的显示顺序按到本片上。
    bool has_ctts = false;
};

struct RepairReferenceProfile {
    struct ChunkHint {
        int track = -1; // 0 = 视频，1 = 音频
        uint32_t samples = 0;
        // 相对参考 mdat 载荷起点的偏移。删 moov 只让 mdat 整体平移，
        // 相对布局不变；绝对 stco 偏移不能直接借到损坏文件上。
        uint64_t relative_start = 0;
    };

    bool ok = false;
    std::string error;
    std::string compatibility = "unverified";
    std::string compatibility_reason;
    std::string timescale_check;
    std::vector<std::string> borrowed_parameters;
    std::vector<std::string> limitations;
    std::vector<uint8_t> ftyp;
    uint32_t movie_timescale = 0;
    RepairReferenceTrackProfile video;
    RepairReferenceTrackProfile audio;
    // 按参考 mdat 的 chunk 偏移排序得到的全局布局线索。
    std::vector<ChunkHint> chunks;
    std::string layout_fingerprint;
};

struct RepairReferenceRebuildResult {
    bool attempted = false;
    bool ok = false;
    std::string error;
    std::string note;
    uint64_t samples_rebuilt = 0;
    uint32_t rebuilt_tracks = 0;
    uint64_t output_bytes = 0;
    model::RepairSampleLedgerSummary sample_ledger;
    RepairReferenceProfile profile;
    model::RepairReferenceAssist assist;
    // 切片 H：显示顺序实测结论（POC → ctts）。analyzed=false 表示这次没做测量。
    model::RepairDisplayOrderSummary display_order;
};

class RepairReferenceMoovRebuilder {
public:
    // 只读参考文件，产出经过自洽核验的布局线索。不触碰损坏文件，方便独立验收
    // 「不同源 / 时基冲突 / 含 B 帧」等拒绝条件。
    static RepairReferenceProfile ReadProfile(const std::string& reference_path,
                                              const RepairRunBudget* budget = nullptr);

    // 轻量探测：结构 + 参考自洽 + 损坏文件 mdat 能否被布局线索完整解释。
    // 不写任何文件；失败时 reason 可直接进报告。
    struct Probe {
        bool attemptable = false;
        std::string reason;
        std::string codec_summary;
        std::vector<std::string> limitations;
    };
    static Probe ProbeWithReference(const std::string& source_path, const std::string& reference_path,
                                    const std::atomic<bool>* cancel, const RepairRunBudget* budget = nullptr,
                                    std::size_t scan_chunk_bytes = 0);

    // 重建：写出 ftyp + 双轨 moov + mdat 的完整普通 MP4。
    static RepairReferenceRebuildResult Rebuild(const std::string& source_path, const std::string& reference_path,
                                                const std::string& output_path, const std::atomic<bool>* cancel,
                                                const RepairRunBudget* budget = nullptr,
                                                std::size_t scan_chunk_bytes = 0);
};

} // namespace repair
} // namespace videoeye
