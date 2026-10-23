// MPEG-TS PES 载荷级检查（IDR 间隔 / PID 断流）单元测试
//
// 为什么需要这一层：只读 PES 头的话，"这条视频一路没有 IDR"和"某个 PID 中途断了"
// 两类问题在包头里完全看不出来 —— PCR 正常、连续计数正常、PSI 完整，但画面
// 起播慢 / 后半段冻住。这里用手工构造的 TS 验证载荷级统计是否算得对。
//
// 样本构造方式：PAT + PMT（各一个 section，CRC 故意留 0 —— 解析器不校验 CRC）
// + 视频/音频 PES 序列。视频 ES 里放 AUD + IDR/非 IDR 的 NAL，用来验证起始码
// 识别与 IDR 间隔；音频 PES 用于验证断流。
//
// 依赖边界：只用标准库，不碰 Qt / FFmpeg。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/analysis/container/TsStructureAnalyzer.h"

namespace fs = std::filesystem;

namespace {

using videoeye::TsStructureAnalyzer;
using videoeye::TsStructureOptions;
using videoeye::model::ContainerStructureResult;
using videoeye::model::TransportStreamInfo;

constexpr uint16_t kPmtPid = 0x1000;
constexpr uint16_t kVideoPid = 0x0100;
constexpr uint16_t kAudioPid = 0x0101;

// ---------------------------------------------------------------------------
// TS 组包
// ---------------------------------------------------------------------------

class TsBuilder {
public:
    void Packet(uint16_t pid, bool pusi, const uint8_t* data, int len) {
        uint8_t pkt[188] = {0};
        pkt[0] = 0x47;
        pkt[1] = static_cast<uint8_t>((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
        pkt[2] = static_cast<uint8_t>(pid & 0xFF);
        const int room = 184;
        int payload_at = 4;
        if (len < room) {
            // 不够一包时用 adaptation field 填充：留 0 会在 ES 里塞进假数据，
            // 甚至拼出假的 NAL 起始码。
            const int af_len = room - len - 1;
            pkt[3] = static_cast<uint8_t>(0x30 | (NextCc(pid) & 0x0F));
            pkt[4] = static_cast<uint8_t>(af_len);
            if (af_len > 0) {
                pkt[5] = 0x00;
                for (int i = 1; i < af_len; ++i)
                    pkt[5 + i] = 0xFF;
            }
            payload_at = 5 + af_len;
        } else {
            pkt[3] = static_cast<uint8_t>(0x10 | (NextCc(pid) & 0x0F));
        }
        const int copy = std::min(len, 188 - payload_at);
        for (int i = 0; i < copy; ++i)
            pkt[payload_at + i] = data[i];
        buf_.insert(buf_.end(), pkt, pkt + 188);
    }

    // 把一段字节按 184 字节切片塞进同一个 PID 的若干 TS 包
    void Payload(uint16_t pid, const std::vector<uint8_t>& data) {
        size_t pos = 0;
        bool first = true;
        while (pos < data.size()) {
            const size_t take = std::min<size_t>(184, data.size() - pos);
            Packet(pid, first, data.data() + pos, static_cast<int>(take));
            pos += take;
            first = false;
        }
    }

    void Write(const fs::path& path) const {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf_.data()), static_cast<std::streamsize>(buf_.size()));
    }

private:
    uint8_t NextCc(uint16_t pid) {
        uint8_t& cc = cc_[pid];
        const uint8_t v = cc;
        cc = static_cast<uint8_t>((cc + 1) & 0x0F);
        return v;
    }

    std::vector<uint8_t> buf_;
    std::map<uint16_t, uint8_t> cc_;
};

std::vector<uint8_t> MakePat() {
    std::vector<uint8_t> s;
    s.push_back(0x00);              // table_id
    s.push_back(0xB0);              // section_syntax_indicator + 长度高 4 位
    s.push_back(0x0D);              // section_length = 13
    s.push_back(0x00); s.push_back(0x01);  // transport_stream_id
    s.push_back(0xC1);              // version 0 + current_next
    s.push_back(0x00); s.push_back(0x00);
    s.push_back(0x00); s.push_back(0x01);  // program_number = 1
    s.push_back(static_cast<uint8_t>(0xE0 | ((kPmtPid >> 8) & 0x1F)));
    s.push_back(static_cast<uint8_t>(kPmtPid & 0xFF));
    s.push_back(0x00); s.push_back(0x00); s.push_back(0x00); s.push_back(0x00);  // CRC（不校验）
    return s;
}

std::vector<uint8_t> MakePmt() {
    std::vector<uint8_t> s;
    s.push_back(0x02);              // table_id
    s.push_back(0xB0);
    s.push_back(23);                // section_length = 9 + 2*5 + 4
    s.push_back(0x00); s.push_back(0x01);  // program_number
    s.push_back(0xC1);
    s.push_back(0x00); s.push_back(0x00);
    s.push_back(0x1F); s.push_back(0xFF);  // PCR_PID = 0x1FFF（不采 PCR）
    s.push_back(0xF0); s.push_back(0x00);  // program_info_length = 0
    auto es = [&s](uint8_t stream_type, uint16_t pid) {
        s.push_back(stream_type);
        s.push_back(static_cast<uint8_t>(0xE0 | ((pid >> 8) & 0x1F)));
        s.push_back(static_cast<uint8_t>(pid & 0xFF));
        s.push_back(0xF0); s.push_back(0x00);
    };
    es(0x1B, kVideoPid);  // H.264
    es(0x0F, kAudioPid);  // AAC (ADTS)
    s.push_back(0x00); s.push_back(0x00); s.push_back(0x00); s.push_back(0x00);  // CRC
    return s;
}

std::vector<uint8_t> MakePes(uint8_t stream_id, uint64_t pts, const std::vector<uint8_t>& es) {
    std::vector<uint8_t> p;
    p.push_back(0x00); p.push_back(0x00); p.push_back(0x01);
    p.push_back(stream_id);
    p.push_back(0x00); p.push_back(0x00);  // PES_packet_length（本用例不依赖）
    p.push_back(0x80);                     // '10' + 无加扰
    p.push_back(0x80);                     // 只有 PTS
    p.push_back(0x05);                     // PES_header_data_length
    p.push_back(static_cast<uint8_t>(0x21 | (((pts >> 30) & 0x07) << 1)));
    p.push_back(static_cast<uint8_t>((pts >> 22) & 0xFF));
    p.push_back(static_cast<uint8_t>(0x01 | (((pts >> 15) & 0x7F) << 1)));
    p.push_back(static_cast<uint8_t>((pts >> 7) & 0xFF));
    p.push_back(static_cast<uint8_t>(0x01 | ((pts & 0x7F) << 1)));
    p.insert(p.end(), es.begin(), es.end());
    return p;
}

// AUD + IDR / 非 IDR 切片各一个。
// with_aud = false 时 ES 直接以三字节起始码 00 00 01 <header> 开头 —— 这是
// "起始码正好落在 PES 载荷第一个字节"的形态（有的封装器省 AUD），滑窗必须在
// 窗口首次填满时就认出它，否则整条流会被判成"没有 IDR"。
std::vector<uint8_t> MakeVideoEs(bool idr, bool with_aud = true) {
    const uint8_t nal_header = idr ? 0x65 : 0x41;
    std::vector<uint8_t> es;
    if (with_aud) es = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};
    const std::vector<uint8_t> start_code =
        with_aud ? std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01}
                 : std::vector<uint8_t>{0x00, 0x00, 0x01};
    es.insert(es.end(), start_code.begin(), start_code.end());
    es.push_back(nal_header);
    const std::vector<uint8_t> tail = {0x88, 0x84, 0x21, 0xA0};
    es.insert(es.end(), tail.begin(), tail.end());
    return es;
}

std::vector<uint8_t> MakeAudioEs() {
    return {0xFF, 0xF1, 0x50, 0x80, 0x01, 0x7F, 0xFC};
}

// 一个可配置的节目：视频 PES 每 0.5 s 一个，音频 PES 每 0.25 s 一个。
struct StreamSpec {
    int video_pes = 24;          // 视频 PES 个数
    int video_idr_every = 4;     // 每几个 PES 插一个 IDR（0 = 只在第一个）
    int only_idr_at = -1;        // >=0 时：除第一个之外只在该下标处插 IDR
    int audio_pes = 48;          // 音频 PES 个数（0 = 没有音频流载荷）
    bool no_idr = false;         // true = 整段一个 IDR 都不放
    bool aud = true;             // false = 视频 ES 省掉 AUD，直接以三字节起始码开头
    bool pmt = true;
};

void BuildStream(const fs::path& path, const StreamSpec& spec) {
    TsBuilder b;
    if (spec.pmt) {
        const std::vector<uint8_t> pat = MakePat();
        std::vector<uint8_t> pat_sec;
        pat_sec.push_back(0x00);  // pointer_field
        pat_sec.insert(pat_sec.end(), pat.begin(), pat.end());
        b.Payload(0x0000, pat_sec);

        const std::vector<uint8_t> pmt = MakePmt();
        std::vector<uint8_t> pmt_sec;
        pmt_sec.push_back(0x00);
        pmt_sec.insert(pmt_sec.end(), pmt.begin(), pmt.end());
        b.Payload(kPmtPid, pmt_sec);
    }

    // 按时间顺序交错输出：0.25 s 一个时隙，音频每格一条，视频每两格一条。
    const int slots = std::max(spec.video_pes * 2, spec.audio_pes);
    int video_i = 0;
    int audio_i = 0;
    for (int slot = 0; slot < slots; ++slot) {
        if (audio_i < spec.audio_pes) {
            const uint64_t pts = static_cast<uint64_t>(audio_i) * 22500;  // 0.25 s
            b.Payload(kAudioPid, MakePes(0xC0, pts, MakeAudioEs()));
            ++audio_i;
        }
        if (slot % 2 == 0 && video_i < spec.video_pes) {
            bool idr = false;
            if (!spec.no_idr) {
                idr = (video_i == 0);
                if (spec.only_idr_at >= 0) {
                    idr = idr || (video_i == spec.only_idr_at);
                } else if (spec.video_idr_every > 0) {
                    idr = idr || (video_i % spec.video_idr_every == 0);
                }
            }
            const uint64_t pts = static_cast<uint64_t>(video_i) * 45000;  // 0.5 s
            b.Payload(kVideoPid, MakePes(0xE0, pts, MakeVideoEs(idr, spec.aud)));
            ++video_i;
        }
    }
    b.Write(path);
}

fs::path MakeTempPath(const std::string& name) {
    const fs::path base = fs::temp_directory_path() / ("videoeye_ts_payload_" + name);
    std::error_code ec;
    fs::remove(base, ec);
    return base;
}

TransportStreamInfo Analyze(const fs::path& path) {
    ContainerStructureResult result;
    const bool ok = TsStructureAnalyzer().Analyze(path.string(), result);
    EXPECT_TRUE(ok) << result.error_message;
    return result.transport;
}

}  // namespace

// ---------------------------------------------------------------------------
// 健康流：IDR 规律、没有断流
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, HealthyStreamCountsIdrAndNoStall) {
    const fs::path path = MakeTempPath("healthy.ts");
    StreamSpec spec;
    spec.video_pes = 24;       // 12 s
    spec.video_idr_every = 4;  // 每 2 s 一个 IDR
    spec.audio_pes = 48;
    BuildStream(path, spec);

    const TransportStreamInfo ts = Analyze(path);
    EXPECT_TRUE(ts.pes_payload_scanned);
    EXPECT_EQ(2, ts.pes_streams_checked) << "视频 + 音频两条 ES 都应参与载荷级跟踪";
    EXPECT_EQ(6, ts.idr_count) << "12 s / 2 s = 6 个 IDR";
    EXPECT_EQ(0, ts.video_pids_without_idr);
    // 相邻 IDR 间隔应为 2 s（容差留给毫秒换算）
    EXPECT_NEAR(ts.max_no_idr_interval_ms, 2000.0, 1.0);
    EXPECT_STREQ("0x0100", ts.max_no_idr_pid.c_str());
    // 视频 PES 间隔 0.5 s、音频 0.25 s
    EXPECT_NEAR(ts.max_pid_gap_ms, 500.0, 1.0);
    EXPECT_EQ(0, ts.stalled_pids);

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 长时间没有 IDR：只有两个 IDR，中间隔 16.5 s
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, LongGapWithoutIdrIsMeasured) {
    const fs::path path = MakeTempPath("no_idr.ts");
    StreamSpec spec;
    spec.video_pes = 40;       // 20 s
    spec.only_idr_at = 33;     // 第一个 + 第 33 个（16.5 s）
    spec.audio_pes = 80;
    BuildStream(path, spec);

    const TransportStreamInfo ts = Analyze(path);
    EXPECT_EQ(2, ts.idr_count);
    EXPECT_NEAR(ts.max_no_idr_interval_ms, 16500.0, 1.0);
    EXPECT_STREQ("0x0100", ts.max_no_idr_pid.c_str());

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 整段没有 IDR：跨度就是"最长无 IDR"
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, SingleIdrUsesWholeSpan) {
    const fs::path path = MakeTempPath("single_idr.ts");
    StreamSpec spec;
    spec.video_pes = 20;       // 10 s
    spec.video_idr_every = 0;  // 只有第一个 PES 带 IDR
    spec.audio_pes = 40;
    BuildStream(path, spec);

    const TransportStreamInfo ts = Analyze(path);
    EXPECT_EQ(1, ts.idr_count);
    EXPECT_NEAR(ts.max_no_idr_interval_ms, 9500.0, 1.0) << "从一个 IDR 到最后一个 PES 的跨度";
    EXPECT_EQ(0, ts.video_pids_without_idr);

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 整段没有 IDR 的计数要带时间尺度：短流不算，长流才算
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, VideoWithoutIdrIsCountedOnlyBeyondThreshold) {
    // 5 s 的视频（10 个 PES，跨度 4.5 s），阈值 10 s → 不计入
    const fs::path short_path = MakeTempPath("no_idr_short.ts");
    StreamSpec short_spec;
    short_spec.video_pes = 10;
    short_spec.no_idr = true;
    short_spec.audio_pes = 20;
    BuildStream(short_path, short_spec);
    {
        const TransportStreamInfo ts = Analyze(short_path);
        EXPECT_EQ(0, ts.idr_count) << "整段都应识别成非 IDR";
        EXPECT_EQ(0, ts.video_pids_without_idr)
            << "跨度 4.5 s 没超过 10 s 阈值，短分片本来就不该有 IDR";
        // 但没有 IDR 这件事本身仍然要能看出来：跨度就是"最长无 IDR"
        EXPECT_NEAR(ts.max_no_idr_interval_ms, 4500.0, 1.0);
    }

    // 20 s 的视频（40 个 PES，跨度 19.5 s）→ 计入
    const fs::path long_path = MakeTempPath("no_idr_long.ts");
    StreamSpec long_spec;
    long_spec.video_pes = 40;
    long_spec.no_idr = true;
    long_spec.audio_pes = 80;
    BuildStream(long_path, long_spec);
    {
        const TransportStreamInfo ts = Analyze(long_path);
        EXPECT_EQ(0, ts.idr_count);
        EXPECT_EQ(1, ts.video_pids_without_idr);
        EXPECT_STREQ("0x0100", ts.max_no_idr_pid.c_str());
    }

    std::error_code ec;
    fs::remove(short_path, ec);
    fs::remove(long_path, ec);
}

// ---------------------------------------------------------------------------
// 末尾长时间没有 IDR：两个 IDR 只隔 1 s，但最后一个 IDR 之后还有 19.5 s
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, TrailingRunWithoutIdrIsMeasured) {
    const fs::path path = MakeTempPath("tail_no_idr.ts");
    StreamSpec spec;
    spec.video_pes = 42;      // 0 .. 20.5 s
    spec.only_idr_at = 2;     // 第一个 + 第 2 个 PES -> IDR 在 0 s 与 1 s
    spec.audio_pes = 84;
    BuildStream(path, spec);

    const TransportStreamInfo ts = Analyze(path);
    EXPECT_EQ(2, ts.idr_count);
    // 只算"IDR 与 IDR 之间"会报 1 s；真正最长的一段是末尾 19.5 s
    EXPECT_NEAR(ts.max_no_idr_interval_ms, 19500.0, 5.0)
        << "最后一个 IDR 到窗口末尾的区间必须计入";
    EXPECT_STREQ("0x0100", ts.max_no_idr_pid.c_str());

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 起始码正好落在 PES 载荷第一个字节：滑窗必须在首次填满时检查
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, StartCodeAtPayloadStartIsDetected) {
    const fs::path path = MakeTempPath("startcode_at_start.ts");
    StreamSpec spec;
    spec.video_pes = 6;
    spec.video_idr_every = 1;   // 每个 PES 都是 IDR
    spec.aud = false;           // ES 直接以 00 00 01 65 开头
    spec.audio_pes = 12;
    BuildStream(path, spec);

    const TransportStreamInfo ts = Analyze(path);
    EXPECT_EQ(6, ts.idr_count) << "三字节起始码在载荷第 1 个字节时不能被漏掉";
    EXPECT_EQ(0, ts.video_pids_without_idr);

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// PID 断流：音频 2 s 后就没有数据了，视频继续到 20 s
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, StalledPidIsDetectedByTrailingGap) {
    const fs::path path = MakeTempPath("stall.ts");
    StreamSpec spec;
    spec.video_pes = 40;       // 20 s
    spec.video_idr_every = 4;
    spec.audio_pes = 8;        // 只有 2 s 音频
    BuildStream(path, spec);

    const TransportStreamInfo ts = Analyze(path);
    // 音频自己内部的 PES 间隔只有 0.25 s，真正的缺口来自"它比视频早停 18 s"——
    // 只看相邻 PES 的间隔是发现不了的，这正是末尾缺口那条判据存在的理由。
    EXPECT_GT(ts.max_pid_gap_ms, 17000.0);
    EXPECT_STREQ("0x0101", ts.max_pid_gap_pid.c_str()) << "报告里要指明是哪个 PID 断了";

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 没有 PMT 时不做载荷级判断（不知道哪些 PID 是 ES）
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, WithoutPmtNoPayloadTracking) {
    const fs::path path = MakeTempPath("no_pmt.ts");
    StreamSpec spec;
    spec.pmt = false;
    spec.video_pes = 10;
    spec.audio_pes = 20;
    BuildStream(path, spec);

    const TransportStreamInfo ts = Analyze(path);
    EXPECT_TRUE(ts.pes_payload_scanned);
    EXPECT_EQ(0, ts.pes_streams_checked);
    EXPECT_DOUBLE_EQ(0.0, ts.max_no_idr_interval_ms);
    EXPECT_DOUBLE_EQ(0.0, ts.max_pid_gap_ms);

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 关掉载荷级检查：字段必须明确报"没扫过"
// ---------------------------------------------------------------------------
TEST(TsPesPayloadTest, ScanSwitchOffReportsNotScanned) {
    const fs::path path = MakeTempPath("scan_off.ts");
    StreamSpec spec;
    spec.video_pes = 12;
    spec.audio_pes = 24;
    BuildStream(path, spec);

    ContainerStructureResult result;
    TsStructureOptions options;
    options.scan_pes_payload = false;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path.string(), result, nullptr, options))
        << result.error_message;

    EXPECT_FALSE(result.transport.pes_payload_scanned);
    EXPECT_EQ(0, result.transport.pes_streams_checked);
    EXPECT_EQ(0, result.transport.idr_count);

    std::error_code ec;
    fs::remove(path, ec);
}
