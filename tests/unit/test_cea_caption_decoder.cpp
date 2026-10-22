// ============================================================================
// 隐藏字幕解码（D2）单元测试：CEA-608 状态机 + CEA-708 DTVCC
//
// 重点不是"能不能抽出几个字母"，而是三件旧实现做不到的事：
//   1. pop-on 的文本必须等到 EOC 才上屏（时间不能提前）；
//   2. CC1/CC2 是两条独立时间线（不能把双语节目串成一句）；
//   3. 708 必须解到 service 级（S1 主字幕 / S2 二语），并按包序号发现丢包。
// ============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/diagnostics/CeaCaptionDecoder.h"

namespace {

using videoeye::Cea608Decoder;
using videoeye::Cea708Decoder;
using videoeye::CeaCaptionDecoder;
using videoeye::model::CaptionCue;

// 608 控制码（场 1 / 通道 1）
constexpr uint8_t kRcl = 0x20;
constexpr uint8_t kEdm = 0x2C;
constexpr uint8_t kCr = 0x2D;
constexpr uint8_t kEnm = 0x2E;
constexpr uint8_t kEoc = 0x2F;
constexpr uint8_t kRu2 = 0x25;

void PushAscii(Cea608Decoder& decoder, const std::string& text, double ts) {
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        decoder.PushPair(static_cast<uint8_t>(text[i]), static_cast<uint8_t>(text[i + 1]), 1, ts);
    }
}

// ---------------------------------------------------------------------------
// CEA-608
// ---------------------------------------------------------------------------

TEST(Cea608DecoderTest, PopOnTextAppearsAtEocNotAtPreload) {
    Cea608Decoder decoder;
    decoder.Reset();

    decoder.PushPair(0x14, kRcl, 1, 0.0);      // 进入 pop-on 装载
    PushAscii(decoder, "HELLO ", 0.2);         // 预装载：此时屏幕上什么都还没有
    // 预装载阶段不应该产生任何 cue —— 这正是旧实现的错误（时间提前）
    EXPECT_TRUE(decoder.cues().empty());

    decoder.PushPair(0x14, kEoc, 1, 1.0);      // 上屏
    EXPECT_TRUE(decoder.cues().empty());       // 还在显示，没有结束时间

    decoder.PushPair(0x14, kEdm, 1, 3.0);      // 擦除
    ASSERT_EQ(decoder.cues().size(), 1u);
    const CaptionCue& cue = decoder.cues().front();
    EXPECT_EQ(cue.text, "HELLO");
    EXPECT_DOUBLE_EQ(cue.start_seconds, 1.0);
    EXPECT_DOUBLE_EQ(cue.end_seconds, 3.0);
    EXPECT_EQ(cue.channel, 1);
    EXPECT_FALSE(cue.kind_708);
    EXPECT_EQ(cue.mode, "POP-ON");
}

TEST(Cea608DecoderTest, RollUpProducesOneCuePerVisibleState) {
    Cea608Decoder decoder;
    decoder.Reset();

    decoder.PushPair(0x14, kRu2, 1, 0.0);      // RU2：两行滚动
    PushAscii(decoder, "ONE ", 0.5);           // 出现在最底行
    decoder.PushPair(0x14, kCr, 1, 1.0);       // 回车：整体上移一行
    PushAscii(decoder, "TWO ", 1.5);           // 新一行
    decoder.PushPair(0x14, kEdm, 1, 3.0);

    ASSERT_EQ(decoder.cues().size(), 2u);
    EXPECT_EQ(decoder.cues()[0].text, "ONE");
    EXPECT_DOUBLE_EQ(decoder.cues()[0].start_seconds, 0.5);
    EXPECT_DOUBLE_EQ(decoder.cues()[0].end_seconds, 1.5);
    EXPECT_EQ(decoder.cues()[0].mode, "ROLL-UP");
    EXPECT_EQ(decoder.cues()[1].text, "ONE\nTWO");
    EXPECT_DOUBLE_EQ(decoder.cues()[1].end_seconds, 3.0);
}

TEST(Cea608DecoderTest, TwoChannelsKeepSeparateTimelines) {
    Cea608Decoder decoder;
    decoder.Reset();

    // CC1：英文；CC2：另一语言。两条时间线完全重叠，必须各自成 cue。
    decoder.PushPair(0x14, kRcl, 1, 0.0);      // CC1 进入 pop-on
    PushAscii(decoder, "EN ", 0.1);
    decoder.PushPair(0x1C, kRcl, 1, 0.0);      // 0x1C = CC2 的 RCL
    PushAscii(decoder, "CN ", 0.1);

    decoder.PushPair(0x14, kEoc, 1, 1.0);
    decoder.PushPair(0x1C, kEoc, 1, 1.0);
    decoder.PushPair(0x14, kEdm, 1, 2.0);
    decoder.PushPair(0x1C, kEdm, 1, 2.0);

    ASSERT_EQ(decoder.cues().size(), 2u);
    bool saw_cc1 = false;
    bool saw_cc2 = false;
    for (const CaptionCue& cue : decoder.cues()) {
        if (cue.channel == 1 && cue.text == "EN") saw_cc1 = true;
        if (cue.channel == 2 && cue.text == "CN") saw_cc2 = true;
    }
    EXPECT_TRUE(saw_cc1);
    EXPECT_TRUE(saw_cc2);
    EXPECT_EQ(decoder.stats().channel_mask, 0x03);
    EXPECT_EQ(decoder.stats().ChannelText(), "CC1, CC2");
}

TEST(Cea608DecoderTest, Field2XdsIsNotTreatedAsCaptionText) {
    Cea608Decoder decoder;
    decoder.Reset();
    // 场 2 的 0x01-0x0E 是 XDS（节目信息），不能进字幕
    decoder.PushPair(0x01, 0x03, 2, 0.0);
    decoder.PushPair(0x02, 0x43, 2, 0.1);
    decoder.Finish(1.0);
    EXPECT_EQ(decoder.stats().xds_count, 2);
    EXPECT_TRUE(decoder.cues().empty());
    EXPECT_TRUE(decoder.stats().field2_used);
}

TEST(Cea608DecoderTest, SpecialAndExtendedCharacters) {
    Cea608Decoder decoder;
    decoder.Reset();
    decoder.PushPair(0x14, kRcl, 1, 0.0);
    decoder.PushPair(0x41, 0x00, 1, 0.1);   // 只有 'A'（第二字节 NUL 不是可见字符）
    decoder.PushPair(0x12, 0x20, 1, 0.2);   // 扩展字符：退格 + Á
    decoder.PushPair(0x11, 0x37, 1, 0.3);   // 特殊字符：♪
    decoder.PushPair(0x14, kEoc, 1, 1.0);
    decoder.Finish(2.0);

    ASSERT_EQ(decoder.cues().size(), 1u);
    // "Á♪"：扩展字符是"退格 + 替换"语义，不能变成 "AÁ♪"
    EXPECT_EQ(decoder.cues().front().text, "\xC3\x81\xE2\x99\xAA");
    EXPECT_EQ(decoder.stats().extended_char_count, 1);
    EXPECT_EQ(decoder.stats().special_char_count, 1);
}

TEST(Cea608DecoderTest, FinishClosesLastVisibleCaption) {
    Cea608Decoder decoder;
    decoder.Reset();
    decoder.PushPair(0x14, kRcl, 1, 0.0);
    PushAscii(decoder, "END ", 0.1);
    decoder.PushPair(0x14, kEoc, 1, 1.0);
    // 文件到此结束：最后一句必须被收尾，不能永远挂着
    decoder.Finish(9.5);
    ASSERT_EQ(decoder.cues().size(), 1u);
    EXPECT_DOUBLE_EQ(decoder.cues().front().end_seconds, 9.5);
}

TEST(Cea608DecoderTest, EnmClearsPreloadButNotScreen) {
    Cea608Decoder decoder;
    decoder.Reset();
    decoder.PushPair(0x14, kRcl, 1, 0.0);
    PushAscii(decoder, "GONE ", 0.1);
    decoder.PushPair(0x14, kEnm, 1, 0.2);   // 清预装载
    decoder.PushPair(0x14, kEoc, 1, 1.0);   // 上屏的是空屏
    decoder.Finish(2.0);
    EXPECT_TRUE(decoder.cues().empty());
}

// ---------------------------------------------------------------------------
// CEA-708
// ---------------------------------------------------------------------------

// 把一段 service block 数据打成 DTVCC 包，再拆成 cc_data 三元组喂进去。
// block_data 里已经含 service block header 之后的命令字节。
void PushServiceBlock(CeaCaptionDecoder& decoder, int service_number,
                      const std::vector<uint8_t>& block_data, int sequence, double ts) {
    std::vector<uint8_t> payload;
    const int block_size = static_cast<int>(block_data.size());
    payload.push_back(static_cast<uint8_t>((service_number << 5) | (block_size & 0x1F)));
    payload.insert(payload.end(), block_data.begin(), block_data.end());

    // 包头：sequence(2) + packet_size(6)
    const uint8_t header = static_cast<uint8_t>(((sequence & 0x03) << 6) |
                                                (payload.size() & 0x3F));
    // 第一个三元组：type 2，携带包头 + 第一个载荷字节
    size_t index = 0;
    decoder.PushTriplet(1, 2, header, payload[index++], ts);
    while (index < payload.size()) {
        const uint8_t d0 = payload[index++];
        const uint8_t d1 = (index < payload.size()) ? payload[index++] : 0;
        decoder.PushTriplet(1, 3, d0, d1, ts);
    }
}

std::vector<uint8_t> DefineWindowVisible(int rows, int cols) {
    return {0x80, 0x20, static_cast<uint8_t>(rows & 0x0F),
            static_cast<uint8_t>(cols & 0x3F), 0x00, 0x00, 0x00};
}

TEST(Cea708DecoderTest, ServiceOneTextAndClear) {
    CeaCaptionDecoder decoder;
    decoder.Reset();

    std::vector<uint8_t> block = DefineWindowVisible(2, 32);
    const std::string text = "HELLO";
    block.insert(block.end(), text.begin(), text.end());
    PushServiceBlock(decoder, 1, block, 0, 1.0);

    // DLC 清窗口 -> 上一条 cue 结束
    PushServiceBlock(decoder, 1, {0x89, 0x00}, 1, 3.0);
    decoder.Finish(4.0);

    const auto& result = decoder.result();
    ASSERT_EQ(result.cues.size(), 1u);
    EXPECT_EQ(result.cues.front().text, "HELLO");
    EXPECT_DOUBLE_EQ(result.cues.front().start_seconds, 1.0);
    EXPECT_DOUBLE_EQ(result.cues.front().end_seconds, 3.0);
    EXPECT_TRUE(result.cues.front().kind_708);
    EXPECT_EQ(result.cues.front().channel, 1);
    EXPECT_EQ(result.cc708.packet_count, 2);
    ASSERT_EQ(result.cc708.services.size(), 1u);
    EXPECT_EQ(result.cc708.services.front(), 1);
}

TEST(Cea708DecoderTest, SecondServiceIsReportedSeparately) {
    CeaCaptionDecoder decoder;
    decoder.Reset();

    auto block_a = DefineWindowVisible(2, 32);
    const std::string text_a = "MAIN";
    block_a.insert(block_a.end(), text_a.begin(), text_a.end());
    PushServiceBlock(decoder, 1, block_a, 0, 0.0);

    auto block_b = DefineWindowVisible(2, 32);
    const std::string text_b = "ALT";
    block_b.insert(block_b.end(), text_b.begin(), text_b.end());
    PushServiceBlock(decoder, 2, block_b, 1, 0.0);

    decoder.Finish(5.0);
    const auto& result = decoder.result();
    ASSERT_EQ(result.cues.size(), 2u);
    EXPECT_EQ(result.cc708.ServiceText(), "S1, S2");
    // 两条服务各自独立成 cue（不是拼成一句）
    EXPECT_EQ(result.cues[0].channel, 1);
    EXPECT_EQ(result.cues[1].channel, 2);
}

TEST(Cea708DecoderTest, ExtendedServiceNumberIsDecoded) {
    CeaCaptionDecoder decoder;
    decoder.Reset();

    // service_number 7 = 扩展：紧跟一个字节给出真实服务号（这里 S2）
    std::vector<uint8_t> block = DefineWindowVisible(2, 32);
    const std::string text = "ALT";
    block.insert(block.end(), text.begin(), text.end());
    // 扩展字节占块内第一个字节，所以 block_size = 1 + 数据长度
    const int block_size = static_cast<int>(block.size()) + 1;
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((7 << 5) | (block_size & 0x1F)));
    payload.push_back(0x02);   // 扩展服务号
    payload.insert(payload.end(), block.begin(), block.end());

    const uint8_t header = static_cast<uint8_t>(payload.size() & 0x3F);
    size_t index = 0;
    decoder.PushTriplet(1, 2, header, payload[index++], 0.0);
    while (index < payload.size()) {
        const uint8_t d0 = payload[index++];
        const uint8_t d1 = (index < payload.size()) ? payload[index++] : 0;
        decoder.PushTriplet(1, 3, d0, d1, 0.5);
    }
    decoder.Finish(4.0);

    const auto& result = decoder.result();
    ASSERT_EQ(result.cc708.services.size(), 1u);
    EXPECT_EQ(result.cc708.services.front(), 2);
    ASSERT_EQ(result.cues.size(), 1u);
    EXPECT_EQ(result.cues.front().channel, 2);
    EXPECT_EQ(result.cues.front().text, "ALT");
}

TEST(Cea708DecoderTest, SequenceGapIsCounted) {
    CeaCaptionDecoder decoder;
    decoder.Reset();
    PushServiceBlock(decoder, 1, {0x00}, 0, 0.0);
    PushServiceBlock(decoder, 1, {0x00}, 2, 0.1);   // 跳过 seq 1 -> 丢包
    decoder.Finish(1.0);
    EXPECT_EQ(decoder.result().cc708.sequence_gap_count, 1);
}

TEST(Cea708DecoderTest, InvalidTripletIsDropped) {
    CeaCaptionDecoder decoder;
    decoder.Reset();
    decoder.PushTriplet(0, 2, 0x0F, 0x2E, 0.0);   // cc_valid = 0
    decoder.Finish(1.0);
    EXPECT_EQ(decoder.result().cc708.packet_count, 0);
    EXPECT_EQ(decoder.result().cues.size(), 0u);
}

// ---------------------------------------------------------------------------
// 门面：608 与 708 混流
// ---------------------------------------------------------------------------

TEST(CeaCaptionDecoderTest, Mixed608And708CuesAreTimeSorted) {
    CeaCaptionDecoder decoder;
    decoder.Reset();

    // 608（cc_type 0）：pop-on，2.0 上屏、4.0 擦除
    decoder.PushTriplet(1, 0, 0x14, kRcl, 0.0);
    for (size_t i = 0; i + 1 < std::string("OLD ").size(); i += 2) {
        decoder.PushTriplet(1, 0, static_cast<uint8_t>("OLD "[i]),
                            static_cast<uint8_t>("OLD "[i + 1]), 0.5);
    }
    decoder.PushTriplet(1, 0, 0x14, kEoc, 2.0);
    decoder.PushTriplet(1, 0, 0x14, kEdm, 4.0);

    // 708（service 1）：0.5 上屏，1.5 清
    auto block = DefineWindowVisible(2, 32);
    const std::string text = "NEW";
    block.insert(block.end(), text.begin(), text.end());
    PushServiceBlock(decoder, 1, block, 0, 0.5);
    PushServiceBlock(decoder, 1, {0x89, 0x00}, 1, 1.5);
    decoder.Finish(6.0);

    const auto& result = decoder.result();
    ASSERT_EQ(result.cues.size(), 2u);
    EXPECT_EQ(result.cues[0].text, "NEW");   // 0.5 在前
    EXPECT_TRUE(result.cues[0].kind_708);
    EXPECT_EQ(result.cues[1].text, "OLD");
    EXPECT_FALSE(result.cues[1].kind_708);
    EXPECT_TRUE(result.Has608());
    EXPECT_TRUE(result.Has708());
    EXPECT_TRUE(result.decoded);
}

}  // namespace
