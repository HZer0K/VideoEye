#pragma once

// ============================================================
// 序列级媒体取证（测试专用）
//
// 为什么不能用 ProbeMediaFile 代替：它只回答「能不能打开、有几帧、时长多少」。
// 帧数一致完全可能掩盖「包顺序被换了」「PTS 回退」「解出来的画面不是同一批」——
// 尤其是 bitstream copy 的 remux，写错流映射 / 时基 / 裁剪边界时，帧数、包数、
// 甚至聚合载荷哈希都可能仍然对得上。
//
// 这里把「一条流里到底发生了什么」记成有序事实：
//   * 逐包载荷哈希序列（顺序敏感，能定位第几个包不对）
//   * pts/dts 相邻差序列（允许容器整体平移，但不允许间隔被改写）
//   * dts 单调性（包 = 解码顺序，回退即缺陷）
//   * 显示顺序 PTS 单调性（在**解码输出的帧**上看，而不是包顺序上）
//
// ⚠️ 包顺序里的 PTS 对含 B 帧的流天然不单调（解码顺序 ≠ 显示顺序），
// 拿它当缺陷判据会把正常的 H.264 主流 profile 全部误报。这里保留
// packet 级 pts_monotonic 仅供诊断，真正的判据是 dts_monotonic 与
// display_pts_monotonic（后者从 avcodec_receive_frame 的输出顺序取）。
//   * 全帧解码后逐帧亮度采样的滚动哈希（包一样但画面不一样也能抓到）
//
// 所有对比函数都带「空事实守护」：源或产物没打开、没包、没解出帧时返回差异，
// 而不是返回空列表 —— 否则「什么都没测到」会伪装成「完全一致」。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye_test {

struct PacketFacts {
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 0;
    bool has_pts = false;
    bool has_dts = false;
    int size = 0;
    bool keyframe = false;
    uint64_t payload_hash = 0; // 该包载荷的 FNV-1a 64
};

struct StreamSequence {
    int index = -1;
    std::string codec_type; // video / audio / ...
    std::string codec_name;
    int time_base_num = 0;
    int time_base_den = 0;
    std::vector<PacketFacts> packets;

    bool pts_monotonic = true;         // 包顺序里的 PTS；含 B 帧时天然不单调，只作诊断
    bool dts_monotonic = true;         // 包顺序里的 DTS；这才是解码顺序的硬约束
    bool display_pts_monotonic = true; // 解码输出顺序里的 PTS
    uint64_t display_pts_samples = 0;  // 0 = 没取到显示顺序事实，不得算「一致」
    uint64_t keyframe_count = 0;
    uint64_t decoded_frames = 0;
    std::string decode_error;

    // 逐包 / 逐帧的滚动哈希，十六进制字符串，便于断言里直接打印。
    std::string payload_sequence_hash;
    std::string frame_sequence_hash;

    // 逐帧亮度均值（显示顺序，与 frame_sequence_hash 同一遍解码产出）。
    // 存在的理由：哈希在重编码后必然变化，回答不了「这一帧是黑的还是上一帧」，
    // 而切片 Y（重复帧 / 黑帧）恰恰需要这个判别。空 = 没解码过视频帧。
    std::vector<double> frame_mean_luma;
};

struct MediaSequence {
    bool opened = false;
    std::string error;
    double duration_seconds = 0.0;
    uint64_t total_packets = 0;
    std::vector<StreamSequence> streams;
};

uint64_t Fnv1a64(uint64_t state, const void* data, size_t size);
std::string HexHash(uint64_t value);

// 全包遍历 + 每条视频流全帧解码。max_packets 是「畸形文件无限出包」的护栏，
// 触顶时 opened 仍为 true 但 error 会写明，调用方必须把它当失败。
MediaSequence CaptureMediaSequence(const std::string& path, bool decode_frames = true, uint64_t max_packets = 200000);

// 轻量探测：只读流类型 / 编码名 / 时基，不遍历包、不解码。
// 批量语料在做昂贵的全量取证之前，要靠它按「产品能力边界」预筛
// （MediaExporter 有意丢弃字幕/数据轨，mp4 也承载不了 ProRes 等编码）。
MediaSequence ProbeStreamLayout(const std::string& path);

// 逐流配对：流数 / codec / 包数 / 逐包载荷哈希 / pts-dts 相邻差 / 单调性 /
// 逐帧亮度哈希。空 = 完全一致；否则每条差异都带 stream[i] 前缀，便于定位。
std::vector<std::string> CompareExactSequence(const MediaSequence& expected, const MediaSequence& actual);

// 两侧视频流解出的逐帧亮度序列与帧数完全一致（且确实解出过帧）。
// 跨容器 remux 时 muxer 可能合法地重打包 bitstream（如 MKV 的 Annex-B
// 转成 MP4 的 AVCC 长度前缀），载荷字节会变但画面不该变；调用方只有在
// 除「载荷序列」外没有任何其它差异时才允许据此判为容器适配。
bool FramesIdentical(const MediaSequence& expected, const MediaSequence& actual);

// 区间裁剪：产物的视频包序列必须是源视频包序列的**连续子序列**（按载荷哈希匹配），
// 且产物自身 pts/dts 单调、包数严格少于源（否则「裁剪」没发生，不该算通过）。
std::vector<std::string> CompareSubsequence(const MediaSequence& expected, const MediaSequence& actual);

// 从 INDEX/目录里挑样本时用的判定：至少有一条视频流、有包、且全帧解码出过帧。
bool HasDecodableVideo(const MediaSequence& sequence);

} // namespace videoeye_test
