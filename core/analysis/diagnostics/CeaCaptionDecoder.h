#pragma once

// 隐藏字幕解码（D2）：CEA-608 状态机 + CEA-708 DTVCC，纯 C++17，不依赖 FFmpeg / Qt。
//
// 三个类的关系：
//   * Cea608Decoder —— 一个字节对进、一条条 cue 出；按场（field 1/2）分成 CC1-CC4
//     四个独立通道，各自有"显示/预装载"两块显存和 pop-on/paint-on/roll-up 三种模式；
//   * Cea708Decoder —— 一个 cc_data 字节对流进；先拼 DTVCC 包（sequence + size），
//     再按 service block 分服务，最后跑 C0/C1 命令与 G0/G1 字符；
//   * CeaCaptionDecoder —— 门面：吃 MP4 的 2 字节对或 MPEG-2 的 cc_data 三元组，
//     按 cc_type 分流到上面两个解码器，合并 cue 与统计。
//
// ❗ 608 的"文本"必须按显存状态取，不能按到达顺序拼：pop-on 字幕先预装载后上屏，
// 直接拼会把整句提前半秒到一秒；roll-up 是滚动窗口，不擦除会越积越长。
// 这是旧实现（SubtitleAnalyzer::DecodeCea608Field）最实质的错误。
//
// 边界（有意不做，别当成 bug）：
//   * 不做字符属性（颜色/下划线/斜体）与 PAC 精确缩进 —— 审核要看的是文本与时长；
//   * 708 的窗口锚点/样式/优先级不参与画面布局，只用来划分文本块；
//   * 不做 608 的 Text Mode（TR/RTD）内容输出：那不是字幕，混进来只会污染 cue。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/CeaCaptionInfo.h"

namespace videoeye {

// ---------------------------------------------------------------------------
// CEA-608
// ---------------------------------------------------------------------------
class Cea608Decoder {
public:
    void Reset();

    // 一个字节对。field = 1 或 2；b1/b2 是去掉奇偶校验位后的 7 位数据（高位会被忽略）。
    // ts_seconds 是该包的时间戳（秒）。608 没有显式时长，cue 的起止时间只能由
    // "显存内容变化"推断，所以时间戳必须逐包给准。
    void PushPair(uint8_t b1, uint8_t b2, int field, double ts_seconds);

    // 收尾：把仍然显示中的字幕以 end_ts_seconds 结束掉（否则最后一句永远没有结束时间）
    void Finish(double end_ts_seconds);

    const std::vector<model::CaptionCue>& cues() const {
        return cues_;
    }
    const model::Cea608Stats& stats() const {
        return stats_;
    }

private:
    enum class Mode { PopOn, PaintOn, RollUp };

    struct Channel {
        std::vector<std::string> displayed;    // 15 行，正在显示的
        std::vector<std::string> nondisplayed; // 15 行，pop-on 的预装载缓冲
        Mode mode = Mode::PopOn;
        int rollup_rows = 2;
        int row = 14; // 0-based 光标行
        int col = 0;  // 0-based 光标列
        // 上屏状态跟踪（cue 的起止就是靠它变化的时刻）
        std::string active_text; // 已提交（已产出 cue）的画面文本
        double active_start = 0.0;
        Mode active_mode = Mode::PopOn;
        // 待提交的画面：写入只更新它，真正的 cue 切分留到结构事件（CR/EDM/EOC/
        // 模式切换/收尾）。否则 paint-on 逐字符输入会产出"O""ON""ONE"三条 cue。
        std::string pending_text;
        double pending_ts = 0.0;
    };

    Channel& ChannelFor(int index);
    void HandleControl(Channel& channel, int channel_index, uint8_t b1, uint8_t b2, double ts_seconds);
    void HandlePac(Channel& channel, uint8_t b1, uint8_t b2);
    void WriteChar(Channel& channel, const std::string& utf8, double ts_seconds);
    void CarriageReturn(Channel& channel);
    void Backspace(Channel& channel);
    void DeleteToEndOfRow(Channel& channel);
    void ClearDisplayed(Channel& channel);
    void ClearNonDisplayed(Channel& channel);
    // 写入后记录"画面已经开始变成新内容"的时刻（不产出 cue）
    void TrackPending(Channel& channel, double ts_seconds);
    // 结构事件处提交：把 active 画面按 pending_ts 结束，再按同一时刻开新画面
    void CommitChannel(Channel& channel, int channel_index, double ts_seconds);
    void CloseActive(Channel& channel, int channel_index, double ts_seconds);

    Channel channels_[4];
    bool field_channel_[2] = {false, false}; // 每个场当前选中的是不是第二个通道
    // 重复控制码抑制：608 的控制码按规范要连发两次（抗误码），第二次必须忽略。
    // 按通道（已编码场 + 通道）保存上一次处理过的控制字节对，-1 = 没有。
    // 不抑制的后果很具体：连续两个 EOC 会把两块显存交换两次，刚上屏的
    // 字幕立刻被换掉，本该显示 2 s 的 cue 只剩 33 ms。
    int last_control_[4] = {-1, -1, -1, -1};
    std::vector<model::CaptionCue> cues_;
    model::Cea608Stats stats_;
    bool touched_ = false;
};

// ---------------------------------------------------------------------------
// CEA-708 (DTVCC)
// ---------------------------------------------------------------------------
class Cea708Decoder {
public:
    void Reset();

    // 一个 cc_data 字节对。packet_start = true 时 d0 是 DTVCC 包头（sequence 2 bit +
    // packet_size_code 6 bit），d1 是该包的第一个载荷字节；false 时 d0/d1 都是载荷。
    // 只有 cc_type == 3 才是 packet_start，cc_type == 2 是 packet data。
    void PushCcData(uint8_t d0, uint8_t d1, bool packet_start, double ts_seconds);

    void Finish(double end_ts_seconds);

    const std::vector<model::CaptionCue>& cues() const {
        return cues_;
    }
    const model::Cea708Stats& stats() const {
        return stats_;
    }

private:
    struct Window {
        bool defined = false;
        bool visible = false;
        int row_count = 0;
        int col_count = 0;
        std::vector<std::string> rows;
        int pen_row = 0;
        int pen_col = 0;
        std::string BuildText() const;
        void Reset();
    };

    struct Service {
        Window windows[8];
        int current_window = 0; // 当前笔所在窗口（DefineWindow / SPL / DLC 会改它）
        std::string active_text;
        double active_start = 0.0;
        int active_window = -1;
        bool seen = false;
    };

    void ParsePacket(double ts_seconds);
    void ParseServiceBlock(int service_number, const std::uint8_t* data, std::size_t size, double ts_seconds);
    void ExecuteCommands(Service& service, const std::uint8_t* data, std::size_t size, double ts_seconds);
    void HandleDefineWindow(Service& service, int window_id, const std::uint8_t* args);
    void HandleC1(Service& service, const std::uint8_t* data, std::size_t size, std::size_t& index, int command);
    void HandleExt1(Service& service, const std::uint8_t* data, std::size_t size, std::size_t& index);
    Window* ActiveWindow(Service& service);
    void WriteChar(Service& service, std::uint32_t codepoint);
    // 已经是 UTF-8 的文本（G2/G3 表项），不能再走码点编码，否则会被二次编码
    void WriteRaw(Service& service, const std::string& utf8);
    void SyncService(Service& service, int service_number, double ts_seconds);
    void CloseActive(Service& service, int service_number, double ts_seconds);

    Service services_[64];
    std::vector<std::uint8_t> packet_;
    std::size_t expected_size_ = 0;
    int last_sequence_ = -1;
    std::vector<model::CaptionCue> cues_;
    model::Cea708Stats stats_;
};

// ---------------------------------------------------------------------------
// 门面：cc_data 分流
// ---------------------------------------------------------------------------
class CeaCaptionDecoder {
public:
    void Reset();

    // MP4/EIA-608 样本：每 2 字节一个场 1 字节对（没有 cc_type 位）
    void PushPairSample(const std::uint8_t* data, std::size_t size, double ts_seconds);

    // MPEG-2 cc_data 三元组数组（ATSC / SCTE-20 / MP4 c708）：
    //   每个三元组 3 字节：b0 里 bit2 = cc_valid、bit1..0 = cc_type，b1/b2 是数据。
    void PushTriplets(const std::uint8_t* data, std::size_t size, double ts_seconds);

    // 单个三元组（逐包路径 / 单测用）
    void PushTriplet(std::uint8_t cc_valid, std::uint8_t cc_type, std::uint8_t d0, std::uint8_t d1, double ts_seconds);

    // 收尾并合并两个解码器的结果
    void Finish(double end_ts_seconds);

    const model::CeaCaptionResult& result() const {
        return result_;
    }
    const std::vector<model::CaptionCue>& cues() const {
        return result_.cues;
    }

private:
    void MergeResults(double end_ts_seconds);

    Cea608Decoder cc608_;
    Cea708Decoder cc708_;
    model::CeaCaptionResult result_;
    bool finished_ = false;
};

} // namespace videoeye
