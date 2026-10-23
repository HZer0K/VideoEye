#pragma once

// 隐藏字幕（closed caption）服务层的数据模型：CEA-608 / CEA-708（DTVCC）。
//
// 为什么需要它：旧实现只把字节对里的可见 ASCII 抽出来拼成一串，于是
//   * pop-on 字幕的"预装载"内容被当成已显示内容（时间早了半句）；
//   * EDM/ENM 擦除被忽略（上一句刚走、下一句已经在显示）；
//   * CC1 与 CC2 串在一起（双语节目直接变成乱码）；
//   * 708 一个字段都没解（只有流 metadata）。
// 这一层的目标不是还原样式/位置，而是把**上屏文本与它真正的起止时间**分对：
// 审核要判的是"这句什么时候出现、停多久、有没有第二语言轨"。
//
// 明确不做（边界，别默默当成"已支持"）：
//   * 608 的字符属性（颜色/下划线/斜体）与 PAC 行缩进精确定位；
//   * 708 的窗口锚点/样式/优先级对画面的影响；
//   * 图形字幕（PGS/DVB/VobSub）的任何像素级分析（见 docs 里的边界说明）。
//
// 纯 C++17，不依赖 FFmpeg / Qt。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 一条隐藏字幕 cue（已从 608 通道 / 708 服务里解出来的成品）
struct CaptionCue {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    std::string text; // 已按行拼成多行文本（'\n' 分隔）
    // 608: 1..4 表示 CC1..CC4；708: 服务号 1..63。两类用 caption_service 字段区分
    // 由 kind_708 决定语义，UI 不混用。
    int channel = 0;
    bool kind_708 = false;
    std::string mode; // 608: "POP-ON"/"PAINT-ON"/"ROLL-UP"；708: "WIN0"

    double Duration() const {
        return end_seconds - start_seconds;
    }
};

// CEA-608 状态机的统计事实
struct Cea608Stats {
    // 出现过的通道位图：bit0 = CC1, bit1 = CC2, bit2 = CC3, bit3 = CC4
    int channel_mask = 0;
    int pop_on_count = 0;   // RCL/EOC 组数
    int paint_on_count = 0; // RDC 次数
    int roll_up_count = 0;  // RU2/RU3/RU4 次数
    int pac_count = 0;      // 行定位码
    int mid_row_count = 0;  // 行内属性码
    int control_count = 0;  // 其它控制码（EDM/ENM/CR/BS...）
    int special_char_count = 0;
    int extended_char_count = 0;
    int xds_count = 0;               // 场 2 的 XDS 包（不是字幕，别混进文本）
    int invalid_pairs = 0;           // 两个字节都是 0 之类
    int duplicate_control_count = 0; // 被抑制掉的重复控制码（608 规范连发两遍）
    int max_row = 0;                 // 出现过的最大行号（1-based，0 = 没定位过）
    bool field2_used = false;
    std::string note;

    bool Any() const {
        return channel_mask != 0 || pop_on_count > 0 || paint_on_count > 0;
    }
    // 人类可读的通道列表，如 "CC1, CC3"
    std::string ChannelText() const;
};

// CEA-708 (DTVCC) 状态机的统计事实
struct Cea708Stats {
    // 出现过的服务号（升序）。service 1 是"主字幕"，其它多为二语/听力障碍版。
    std::vector<int> services;
    int packet_count = 0; // 完整 DTVCC 包数
    int service_block_count = 0;
    int sequence_gap_count = 0; // sequence_number 不连续（丢包）
    int window_count = 0;       // 见过 DefineWindow 的窗口数
    int command_count = 0;
    int char_count = 0;
    std::map<int, int> cues_per_service;
    std::string note;

    bool Any() const {
        return !services.empty();
    }
    std::string ServiceText() const;
};

// 一条隐藏字幕流的合并结果（608 与 708 可能同时存在于同一路 cc_data）
struct CeaCaptionResult {
    bool decoded = false; // 真的喂过 cc_data
    std::vector<CaptionCue> cues;
    Cea608Stats cc608;
    Cea708Stats cc708;

    bool Has608() const {
        return cc608.Any();
    }
    bool Has708() const {
        return cc708.Any();
    }
    int CueCount() const {
        return static_cast<int>(cues.size());
    }
};

} // namespace model
} // namespace videoeye
