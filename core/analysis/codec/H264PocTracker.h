#pragma once

// H.264 帧级 POC 追踪器（"帧级码流结构"的最小闭环）。
//
// 解决的问题：容器只给解码顺序（sample 顺序 + DTS），显示顺序必须从 POC 推。
// 没有 POC 时，工具只能说"这段有 B 帧"（看 slice_type），说不出：
//   * B 帧重排了几帧（POC 回退幅度）
//   * 显示顺序有没有跳变 / 重复（拼接、丢帧、重复封装）
//   * GOP 边界在哪、每段多长
//
// 与 HevcPocTracker 的分工：两者的扫包骨架（Feed / FeedExtradata /
// Finish）与统计汇总（FrameOrderStats）是同一套，只有三处编码语义不同 ——
// NAL 切分语法、slice_header 布局、POC 推导公式。所以是两个类而不是一个
// if/else 大函数：混在一起后每读一处都要先确认"这段是哪个编码的"。
//
// 明确不做（避免用"看起来像"冒充"已验证"）：
//   * 不重建参考帧列表（DPB / MMCO / RPL）。那需要按解码过程维护滑动窗口，
//     且对"这份文件能不能播"这个问题没有增量信息 —— POC 异常已经能报警。
//   * 不支持 pic_order_cnt_type == 1（需要 offset_for_ref_frame 表，实际素材罕见）。
//     遇到时 timeline.available=false 并把原因写清楚，不静默给 0。
//   * field coding（隔行）能算 POC，但**不**据 POC 步长推断"缺帧"
//     （隔行的 POC 步长是 1，逐行是 2，混在一起推会误报）。
//
// 与 ParameterSetTracker 的分工：那个管"参数集变了没有"，这个管"帧顺序对不对"。
// 两者都逐包扫 NAL，但关注点不同，合在一起会让任一方都难读。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ❗ 必须 include media 层的 NalUnit：core/domain/model/BitstreamUnits.h 里那个
// 同名结构是 model::NalUnit（结果快照），而 ExtradataParser 产出的是
// videoeye::NalUnit。两者字段相同但不是一个类型，include 错了会拿到
// "missing type specifier" 一串难懂的报错。
#include "core/media/codec/ExtradataTypes.h"

#include "core/domain/model/FrameOrderInfo.h"
#include "core/domain/model/H264BitstreamInfo.h"

#include "core/analysis/codec/FrameOrderTypes.h"

namespace videoeye {

class H264PocTracker {
public:
    // 与 ParameterSetTracker::Framing 同义；两种编码共用一份枚举（见 FrameOrderTypes.h）
    using Framing = NalFraming;
    using Options = FrameOrderOptions;

    void Reset(int stream_index, const Options& options = Options{});
    void SetFraming(Framing framing) {
        framing_ = framing;
    }
    void SetLengthPrefixBytes(int bytes) {
        prefix_bytes_ = bytes;
    }

    // 喂一个媒体包的负载（Annex B 或长度前缀均可，Auto 时逐包判定）。
    // 返回这一包里有几帧是新帧（first_mb_in_slice == 0 的 slice 数），便于调用方对齐。
    int Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index);

    // 用容器 extradata 里的参数集做初始化。
    //
    // 为什么必须有这一条：MP4/MKV 把 SPS/PPS 写进 avcC 之后，编码器就**不再**在
    // 码流里重复它们（libx264 的 b_repeat_headers 在 AV_CODEC_FLAG_GLOBAL_HEADER
    // 下直接关掉）。只扫包的 tracker 在这类文件上会看到"零个 SPS"，于是每条 slice
    // 都解不出 POC —— 表现出来是"帧顺序不可用"，而真正的原因只是没喂这份配置。
    //
    // 返回是否至少拿到一份 SPS。
    bool FeedExtradata(const uint8_t* data, size_t size);

    // 收尾并计算 reorder / GOP / 重复 / 缺口。可重复调用（幂等）。
    const model::FrameOrderTimeline& Finish();
    const model::FrameOrderTimeline& timeline() const {
        return timeline_;
    }
    int stream_index() const {
        return stream_index_;
    }

private:
    struct SliceHead {
        uint32_t first_mb_in_slice = 0;
        int slice_type = -1; // 归一化 0=P 1=B 2=I 3=SP 4=SI
        int frame_num = 0;
        bool idr = false;
        bool field_pic = false;
        bool bottom_field = false;
        int poc_lsb = 0;
        int32_t delta_poc_bottom = 0;
        int32_t delta_poc_top = 0;
        int32_t delta_poc_bottom_field = 0;
    };

    // 切分语法的决定与遍历都在 Feed 里（见 cpp）：容器给了前缀宽度就照它切，
    // 没给才逐包判定。为什么不能无条件自动判定，见 cpp 里对 `00 00 01 XX` 前缀的说明。
    bool DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const;
    // 处理单个 NAL。参数是 NalView（不拥有内存）：逐包路径直接拿零拷贝视图，
    // extradata 路径用 ToNalView() 把已复制好的 NalUnit 借成视图，两条路共用一份逻辑。
    void FeedNal(const NalView& nal, bool has_time, double time_seconds, int64_t packet_index, int& new_frames);
    // 返回 false = 这份 SPS 不合法（log2_* 越界），调用方据此判定"没拿到参数集"
    bool HandleSps(const NalView& nal);
    bool HandlePps(const NalView& nal);
    // 非 const：解析过程中要把 field_pic 记进 timeline_.field_coded
    bool ParseSliceHeader(const NalView& nal, SliceHead& out);
    int64_t ComputePoc(const SliceHead& head);
    void PushFrame(const SliceHead& head, int64_t poc, bool has_time, double time_seconds, int64_t packet_index);
    // 汇总重排 / GOP / 重复 / 缺口。规则本体在 FrameOrderStats.cpp，与 HEVC 共用。
    void Recompute();

    int stream_index_ = -1;
    Options options_;
    Framing framing_ = Framing::Auto;
    int prefix_bytes_ = 0;

    // 参数集：只保留"当前生效"的一份。多 PPS 的完整管理在 ParameterSetTracker 那边，
    // 这里只需要能算出 POC 的那几个字段。
    bool sps_ready_ = false;
    bool pps_ready_ = false;
    model::H264SpsInfo sps_;
    model::H264PpsInfo pps_;

    // POC 状态机（H.264 8.2.1）
    int64_t max_frame_num_ = 0;
    int64_t max_poc_lsb_ = 0;
    int64_t prev_poc_msb_ = 0;
    int64_t prev_poc_lsb_ = 0;
    int64_t prev_frame_num_ = 0;
    int64_t prev_frame_num_offset_ = 0;

    model::FrameOrderTimeline timeline_;
    bool finished_ = false;
};

} // namespace videoeye
