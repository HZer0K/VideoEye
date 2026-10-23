// ============================================================================
// 音频位流头解析（D1）单元测试
//
// 覆盖 ADTS / LOAS / AC-3 / E-AC-3 / OpusHead / Opus TOC / AAC ASC。
// 测试数据全部由本文件内的逐位 BitWriter 按**规范字段顺序**自己拼出来，
// 不从 parser 里反推位序 —— 否则就成了"parser 与测试互相配合的错误"
// （VP9 那次漏读 show_frame 一位就是这么埋进去的）。
// ============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/analysis/codec/AudioBitstreamParser.h"

namespace {

using videoeye::AacAudioSpecificConfig;
using videoeye::Ac3Header;
using videoeye::AdtsHeader;
using videoeye::AudioBitstreamParser;
using videoeye::LatmHeader;
using videoeye::OpusHead;
using videoeye::OpusToc;

// MSB-first 位拼装器（音频头字段都是 MSB-first）
class BitWriter {
public:
    void Put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            const int pos = static_cast<int>(bits_.size());
            if (pos % 8 == 0)
                bytes_.push_back(0);
            const uint32_t bit = (value >> i) & 1u;
            bytes_.back() |= static_cast<uint8_t>(bit << (7 - (pos % 8)));
            bits_.push_back(bit != 0);
        }
    }
    // 补齐到字节边界（规范里的 stuffing）
    void Align() {
        while (bits_.size() % 8 != 0)
            Put(0, 1);
    }
    const std::vector<uint8_t>& bytes() const {
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    std::vector<bool> bits_;
};

// ---------------------------------------------------------------------------
// ADTS
// ---------------------------------------------------------------------------

TEST(AudioBitstreamParserTest, AdtsBasicFields) {
    // MPEG-4 / AAC LC / 44100 / 双声道 / 帧长 100 / 每帧 1 个 raw_data_block
    const uint8_t data[7] = {0xFF, 0xF1, 0x50, 0x80, 0x0C, 0x80, 0x00};
    AdtsHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseAdts(data, sizeof(data), header));
    EXPECT_EQ(header.mpeg_version, 4);
    EXPECT_EQ(header.audio_object_type, 2); // profile 1 + 1
    EXPECT_EQ(header.sampling_frequency_index, 4);
    EXPECT_EQ(header.sample_rate, 44100);
    EXPECT_EQ(header.channel_configuration, 2);
    EXPECT_EQ(header.channels, 2);
    EXPECT_EQ(header.frame_length, 100);
    EXPECT_EQ(header.num_raw_data_blocks, 0);
    EXPECT_TRUE(header.protection_absent);
    EXPECT_EQ(header.header_bytes, 7);
}

TEST(AudioBitstreamParserTest, AdtsMpeg2AndCrc) {
    // data[1] = 0xF8：1111 + ID=1(MPEG-2) + layer=00 + protection_absent=0（要 9 字节头）
    const uint8_t data[9] = {0xFF, 0xF8, 0x50, 0x80, 0x0C, 0x80, 0x00, 0xAA, 0xBB};
    AdtsHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseAdts(data, sizeof(data), header));
    EXPECT_EQ(header.mpeg_version, 2);
    EXPECT_FALSE(header.protection_absent);
    EXPECT_EQ(header.header_bytes, 9);
    EXPECT_EQ(header.frame_length, 100);
}

TEST(AudioBitstreamParserTest, AdtsRejectsBadSyncAndShortFrame) {
    AdtsHeader header;
    const uint8_t garbage[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(AudioBitstreamParser::ParseAdts(garbage, sizeof(garbage), header));
    // 帧长小于头长（100 -> 0）必须判无效
    const uint8_t tiny[7] = {0xFF, 0xF1, 0x50, 0x80, 0x00, 0x00, 0x00};
    EXPECT_FALSE(AudioBitstreamParser::ParseAdts(tiny, sizeof(tiny), header));
    EXPECT_FALSE(AudioBitstreamParser::ParseAdts(nullptr, 0, header));
}

TEST(AudioBitstreamParserTest, AdtsMultipleFramesPerPacket) {
    // num_raw_data_blocks = 2 -> 每帧 3 个 AAC 帧
    const uint8_t data[7] = {0xFF, 0xF1, 0x50, 0x80, 0x0C, 0x80, 0x02};
    AdtsHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseAdts(data, sizeof(data), header));
    EXPECT_EQ(header.num_raw_data_blocks, 2);
}

// ---------------------------------------------------------------------------
// LOAS / LATM
// ---------------------------------------------------------------------------

TEST(AudioBitstreamParserTest, LoasHeader) {
    // sync 11 bit = 0x2B7，随后 13 bit audioMuxLengthBytes = 100，1 bit useSameStreamMux
    const uint8_t data[4] = {0x56, 0xE0, 0x64, 0x00};
    int length = 0;
    bool same_mux = true;
    ASSERT_TRUE(AudioBitstreamParser::ParseLoasHeader(data, sizeof(data), length, same_mux));
    EXPECT_EQ(length, 100);
    EXPECT_FALSE(same_mux);

    const uint8_t reuse[4] = {0x56, 0xE0, 0x64, 0x80};
    ASSERT_TRUE(AudioBitstreamParser::ParseLoasHeader(reuse, sizeof(reuse), length, same_mux));
    EXPECT_TRUE(same_mux);
}

TEST(AudioBitstreamParserTest, LatmUseSameStreamMuxIsReported) {
    // 首 bit = 1 表示沿用上一帧配置，此时必须报"没有新配置"而不是瞎猜
    const uint8_t data[2] = {0x80, 0x00};
    LatmHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseLatmStreamMuxConfig(data, sizeof(data), header));
    EXPECT_TRUE(header.use_same_stream_mux);
    EXPECT_FALSE(header.note.empty());
}

// ---------------------------------------------------------------------------
// AC-3 / E-AC-3
// ---------------------------------------------------------------------------

TEST(AudioBitstreamParserTest, Ac3BsiFields) {
    BitWriter w;
    w.Put(0x0B77, 16); // syncword
    w.Put(0, 16);      // crc1
    w.Put(0, 2);       // fscod = 0 -> 48 kHz
    w.Put(10, 6);      // frmsizecod = 10 -> 80 kbps / 320 字节
    w.Put(8, 5);       // bsid
    w.Put(0, 5);       // bsmod
    w.Put(7, 3);       // acmod = 3/2
    w.Put(0, 2);       // cmixlev（acmod&1 != 0）
    w.Put(0, 2);       // surmixlev（acmod&4 != 0）
    w.Put(1, 1);       // lfeon
    w.Put(27, 5);      // dialnorm = 27 -> -27 dBFS
    w.Align();

    Ac3Header header;
    ASSERT_TRUE(AudioBitstreamParser::ParseAc3(w.bytes().data(), w.bytes().size(), false, header));
    EXPECT_FALSE(header.eac3);
    EXPECT_EQ(header.sample_rate, 48000);
    EXPECT_EQ(header.frame_size_bytes, 320);
    EXPECT_EQ(header.frame_samples, 1536);
    EXPECT_EQ(header.bit_rate_kbps, 80);
    EXPECT_EQ(header.bsid, 8);
    EXPECT_EQ(header.acmod, 7);
    EXPECT_TRUE(header.lfeon);
    EXPECT_EQ(header.channels, 6);
    EXPECT_TRUE(header.has_dialnorm);
    EXPECT_EQ(header.dialnorm_dbfs, -27);
}

TEST(AudioBitstreamParserTest, Ac3SkipsMixLevelsAccordingToAcmod) {
    // acmod = 2（2/0）：要读 surmixlev 和 dsurmod，但没有 cmixlev。
    // 位序错一位的话 dialnorm 会读到 0，正好用 dialnorm 断言钉住。
    BitWriter w;
    w.Put(0x0B77, 16);
    w.Put(0, 16);
    w.Put(0, 2);  // fscod 48 kHz
    w.Put(20, 6); // frmsizecod 20 -> 192 kbps
    w.Put(8, 5);  // bsid
    w.Put(0, 5);  // bsmod
    w.Put(2, 3);  // acmod = 2
    w.Put(0, 2);  // dsurmod（acmod==2 才有；surmixlev 不出现，位序错一位 dialnorm 就废了）
    w.Put(0, 1);  // lfeon
    w.Put(31, 5); // dialnorm = 31 -> -31
    w.Align();

    Ac3Header header;
    ASSERT_TRUE(AudioBitstreamParser::ParseAc3(w.bytes().data(), w.bytes().size(), false, header));
    EXPECT_EQ(header.acmod, 2);
    EXPECT_FALSE(header.lfeon);
    EXPECT_EQ(header.channels, 2);
    EXPECT_EQ(header.dialnorm_dbfs, -31);
    EXPECT_EQ(header.bit_rate_kbps, 192);
}

TEST(AudioBitstreamParserTest, Eac3IndependentSubstream) {
    BitWriter w;
    w.Put(0x0B77, 16);
    w.Put(0, 2);    // strmtyp = 0（独立子流）
    w.Put(0, 3);    // substreamid
    w.Put(159, 11); // frmsiz -> (159+1)*2 = 320 字节
    w.Put(0, 2);    // fscod = 0 -> 48 kHz
    w.Put(3, 2);    // numblkscod = 3 -> 6 blocks -> 1536 样本
    w.Put(7, 3);    // acmod
    w.Put(1, 1);    // lfeon
    w.Put(16, 5);   // bsid = 16（E-AC-3）
    w.Put(27, 5);   // dialnorm
    w.Put(0, 1);    // compre
    w.Align();

    Ac3Header header;
    ASSERT_TRUE(AudioBitstreamParser::ParseAc3(w.bytes().data(), w.bytes().size(), true, header));
    EXPECT_TRUE(header.eac3);
    EXPECT_EQ(header.strmtyp, 0);
    EXPECT_EQ(header.frame_size_bytes, 320);
    EXPECT_EQ(header.num_blocks, 6);
    EXPECT_EQ(header.frame_samples, 1536);
    EXPECT_EQ(header.bit_rate_kbps, 80);
    EXPECT_EQ(header.channels, 6);
    EXPECT_EQ(header.dialnorm_dbfs, -27);
    EXPECT_FALSE(header.has_chanmap);
}

TEST(AudioBitstreamParserTest, Eac3DependentSubstreamChanmap) {
    BitWriter w;
    w.Put(0x0B77, 16);
    w.Put(1, 2);       // strmtyp = 1（从属子流）
    w.Put(1, 3);       // substreamid = 1
    w.Put(99, 11);     // frmsiz -> 200 字节
    w.Put(0, 2);       // fscod
    w.Put(3, 2);       // numblkscod = 3
    w.Put(7, 3);       // acmod（从属子流本体的 acmod，不参与节目布局）
    w.Put(0, 1);       // lfeon
    w.Put(16, 5);      // bsid
    w.Put(27, 5);      // dialnorm
    w.Put(0, 1);       // compre
    w.Put(1, 1);       // chanmape
    w.Put(0x000C, 16); // chanmap：bit2/bit3 -> 2 个声道
    w.Align();

    Ac3Header header;
    ASSERT_TRUE(AudioBitstreamParser::ParseAc3(w.bytes().data(), w.bytes().size(), true, header));
    EXPECT_EQ(header.strmtyp, 1);
    EXPECT_EQ(header.substreamid, 1);
    EXPECT_TRUE(header.has_chanmap);
    EXPECT_EQ(header.chanmap_channels, 2);
    EXPECT_EQ(header.chanmap, 0x000C);
}

TEST(AudioBitstreamParserTest, JocMarkerIsOnlyAHeuristic) {
    // 判定方式是在载荷前 96 字节里搜 ASCII "joc"，不是按扩展语法定位 addbsi 字段。
    // 所以：普通载荷里插进这三个字节，结果就会从"非 Atmos"变成"Atmos"。
    // 这条测试钉住的正是这个局限 —— 命中必须带 atmos_heuristic，不能当确定结论。
    BitWriter plain_writer;
    plain_writer.Put(0x0B77, 16);
    plain_writer.Put(0, 2); // strmtyp = 0
    plain_writer.Put(0, 3);
    plain_writer.Put(159, 11); // frmsiz -> 320 字节
    plain_writer.Put(0, 2);    // fscod = 0 -> 48 kHz
    plain_writer.Put(3, 2);    // numblkscod
    plain_writer.Put(7, 3);    // acmod
    plain_writer.Put(1, 1);    // lfeon
    plain_writer.Put(16, 5);   // bsid = 16
    plain_writer.Put(27, 5);   // dialnorm
    plain_writer.Put(0, 1);    // compre
    plain_writer.Align();

    Ac3Header plain;
    ASSERT_TRUE(AudioBitstreamParser::ParseAc3(plain_writer.bytes().data(), plain_writer.bytes().size(), true, plain));
    EXPECT_FALSE(plain.atmos_joc);
    EXPECT_FALSE(plain.atmos_heuristic);

    // 在普通载荷位置塞进 "joc"：命中，但只能算线索
    std::vector<uint8_t> marked = plain_writer.bytes();
    marked.push_back(static_cast<uint8_t>('j'));
    marked.push_back(static_cast<uint8_t>('o'));
    marked.push_back(static_cast<uint8_t>('c'));
    marked.push_back(4);
    Ac3Header heuristic;
    ASSERT_TRUE(AudioBitstreamParser::ParseAc3(marked.data(), marked.size(), true, heuristic));
    EXPECT_TRUE(heuristic.atmos_joc);
    EXPECT_TRUE(heuristic.atmos_heuristic) << "子串命中的判定必须标成启发式";
}

TEST(AudioBitstreamParserTest, Ac3RejectsBadSync) {
    const uint8_t data[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    Ac3Header header;
    EXPECT_FALSE(AudioBitstreamParser::ParseAc3(data, sizeof(data), false, header));
    EXPECT_FALSE(AudioBitstreamParser::ParseAc3(nullptr, 0, false, header));
}

// ---------------------------------------------------------------------------
// Opus
// ---------------------------------------------------------------------------

TEST(AudioBitstreamParserTest, OpusHeadWithMagic) {
    std::vector<uint8_t> data;
    const char magic[8] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    data.insert(data.end(), magic, magic + 8);
    data.push_back(1);    // version
    data.push_back(2);    // channels
    data.push_back(0x38); // pre-skip = 312 (0x0138, little endian)
    data.push_back(0x01);
    const uint8_t rate[4] = {0x80, 0xBB, 0x00, 0x00}; // 48000 LE
    data.insert(data.end(), rate, rate + 4);
    data.push_back(0x00); // output gain = -256 Q7.8 = -1.0 dB
    data.push_back(0xFF);
    data.push_back(0); // mapping family 0

    OpusHead head;
    ASSERT_TRUE(AudioBitstreamParser::ParseOpusHead(data.data(), data.size(), head));
    EXPECT_EQ(head.version, 1);
    EXPECT_EQ(head.channels, 2);
    EXPECT_EQ(head.pre_skip, 312);
    EXPECT_EQ(head.input_sample_rate, 48000);
    EXPECT_EQ(head.output_gain_q8, -256);
    EXPECT_EQ(head.mapping_family, 0);
}

TEST(AudioBitstreamParserTest, OpusHeadWithoutMagicIsDops) {
    // MP4 的 dOps 没有 magic，直接从 version 开始
    std::vector<uint8_t> data = {0, 6, 0x38, 0x01, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x01, 2, 1, 0, 1, 2, 3, 4, 5};
    OpusHead head;
    ASSERT_TRUE(AudioBitstreamParser::ParseOpusHead(data.data(), data.size(), head));
    EXPECT_EQ(head.channels, 6);
    EXPECT_EQ(head.mapping_family, 1);
    EXPECT_EQ(head.stream_count, 2);
    EXPECT_EQ(head.coupled_count, 1);
}

TEST(AudioBitstreamParserTest, OpusTocHybridFullband20ms) {
    // config 15 = Hybrid / FB / 20 ms；stereo=0；code 0 -> 1 帧
    const uint8_t data[3] = {0x78, 0x00, 0x00};
    OpusToc toc;
    ASSERT_TRUE(AudioBitstreamParser::ParseOpusToc(data, sizeof(data), toc));
    EXPECT_EQ(toc.config, 15);
    EXPECT_EQ(toc.mode, 1);
    EXPECT_EQ(toc.bandwidth, 4);
    EXPECT_DOUBLE_EQ(toc.frame_duration_ms, 20.0);
    EXPECT_FALSE(toc.stereo);
    EXPECT_EQ(toc.frame_count, 1);
    EXPECT_EQ(toc.channels, 1);
}

TEST(AudioBitstreamParserTest, OpusTocCeltFramesAndStereo) {
    // config 31 = CELT / FB / 20 ms；stereo=1；code 3 -> 第二字节低 6 位给帧数
    const uint8_t data[4] = {0xFF, 0x03, 0x5A, 0x00};
    OpusToc toc;
    ASSERT_TRUE(AudioBitstreamParser::ParseOpusToc(data, sizeof(data), toc));
    EXPECT_EQ(toc.mode, 2);
    EXPECT_EQ(toc.bandwidth, 4);
    EXPECT_TRUE(toc.stereo);
    EXPECT_EQ(toc.channels, 2);
    EXPECT_EQ(toc.frame_count, 3);
}

// ---------------------------------------------------------------------------
// AAC AudioSpecificConfig
// ---------------------------------------------------------------------------

TEST(AudioBitstreamParserTest, AacAscLcStereo48k) {
    // AOT=2 / sf_index=3 (48k) / channel_config=2
    const uint8_t asc[2] = {0x11, 0x90};
    AacAudioSpecificConfig config;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc, sizeof(asc), config));
    EXPECT_EQ(config.audio_object_type, 2);
    EXPECT_EQ(config.sample_rate, 48000);
    EXPECT_EQ(config.channels, 2);
    EXPECT_FALSE(config.sbr);
    EXPECT_FALSE(config.ps);
}

TEST(AudioBitstreamParserTest, AacAscHeAacReportsSbrRate) {
    // AOT=5 (SBR) / 基础 22050 / stereo / 扩展采样率 44100
    const uint8_t asc[3] = {0x2B, 0x92, 0x40};
    AacAudioSpecificConfig config;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc, sizeof(asc), config));
    EXPECT_EQ(config.audio_object_type, 5);
    EXPECT_TRUE(config.sbr);
    EXPECT_FALSE(config.ps);
    EXPECT_EQ(config.extension_sample_rate, 44100);
    // 有 SBR 时对外声明的采样率必须取扩展采样率，否则会比容器小一半
    EXPECT_EQ(config.sample_rate, 44100);
}

TEST(AudioBitstreamParserTest, AacAscRejectsEmpty) {
    AacAudioSpecificConfig config;
    EXPECT_FALSE(AudioBitstreamParser::ParseAacAudioSpecificConfig(nullptr, 0, config));
    EXPECT_FALSE(config.valid);
}

// ---------------------------------------------------------------------------
// 查表函数（UI 与规则共用，错一格整页展示就错）
// ---------------------------------------------------------------------------

TEST(AudioBitstreamParserTest, LookupTables) {
    EXPECT_EQ(AudioBitstreamParser::AacSampleRateFromIndex(0), 96000);
    EXPECT_EQ(AudioBitstreamParser::AacSampleRateFromIndex(4), 44100);
    EXPECT_EQ(AudioBitstreamParser::AacSampleRateFromIndex(13), 0); // 保留值
    EXPECT_EQ(AudioBitstreamParser::AacSampleRateFromIndex(99), 0);

    EXPECT_EQ(AudioBitstreamParser::AacChannelsFromConfig(0), 0); // PCE 定义
    EXPECT_EQ(AudioBitstreamParser::AacChannelsFromConfig(1), 1);
    EXPECT_EQ(AudioBitstreamParser::AacChannelsFromConfig(7), 8);

    EXPECT_EQ(AudioBitstreamParser::Ac3FrameSizeBytes(0, 0), 128); // 64 words
    EXPECT_EQ(AudioBitstreamParser::Ac3FrameSizeBytes(0, 10), 320);
    EXPECT_EQ(AudioBitstreamParser::Ac3FrameSizeBytes(1, 37), 2788); // 1394 words @44.1k
    EXPECT_EQ(AudioBitstreamParser::Ac3FrameSizeBytes(3, 0), 0);     // fscod 保留值
}

} // namespace
