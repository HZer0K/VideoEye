// videoeye::IsobmffParser 真实解析路径单元测试
//
// 目的: test_mp4_sample_table.cpp 只测 Mp4SampleTableAnalyzer::Validate()（纯静态校验），
// 覆盖不到「从字节流里把 stbl 读出来」这一段。自研 IsobmffParser 替换掉第三方库之后，
// 这段必须有人盯着 —— 历史上 stts/ctts 就出过把 entry_count 当成 sample_count 的 bug，
// 结果是正常 MP4 只能解析到第一条 entry，而 Validate() 完全看不出来。
//
// 做法: 在内存里拼一个最小但结构合法的 MP4（ftyp + moov/trak/mdia/minf/stbl + mdat），
// 落盘后走 IsobmffParser::Parse()。不依赖 Qt / FFmpeg / 任何第三方容器库。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/media/container/IsobmffParser.h"

using namespace videoeye;

namespace {

// ---- 字节序辅助（ISOBMFF 是大端）----
void Put8(std::vector<uint8_t>& v, uint8_t x) {
    v.push_back(x);
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFF));
}

// std::vector 没有 operator+，这里自己拼（读起来比一堆 insert 清楚）
std::vector<uint8_t> Cat(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

std::vector<uint8_t> CatAll(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

std::vector<uint8_t> Box(const std::string& type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    Put32(out, static_cast<uint32_t>(8 + payload.size()));
    for (char c : type)
        Put8(out, static_cast<uint8_t>(c));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// version/flags(4) + body
std::vector<uint8_t> FullBox(const std::string& type, uint8_t version, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> payload{version, 0, 0, 0};
    payload.insert(payload.end(), body.begin(), body.end());
    return Box(type, payload);
}

using CountDelta = std::pair<uint32_t, uint32_t>; // (sample_count, delta 或 offset)

std::vector<uint8_t> MakeStts(const std::vector<CountDelta>& entries) {
    std::vector<uint8_t> body;
    Put32(body, static_cast<uint32_t>(entries.size()));
    for (const auto& e : entries) {
        Put32(body, e.first);
        Put32(body, e.second);
    }
    return FullBox("stts", 0, body);
}

std::vector<uint8_t> MakeCtts(uint8_t version, const std::vector<CountDelta>& entries) {
    std::vector<uint8_t> body;
    Put32(body, static_cast<uint32_t>(entries.size()));
    for (const auto& e : entries) {
        Put32(body, e.first);
        Put32(body, e.second);
    }
    return FullBox("ctts", version, body);
}

// stz2: version/flags(4) reserved(3)+field_size(1) sample_count(4) + 打包好的样本
std::vector<uint8_t> MakeStz2(uint8_t field_size, uint32_t sample_count, const std::vector<uint8_t>& packed) {
    std::vector<uint8_t> body{0, 0, 0, field_size};
    Put32(body, sample_count);
    body.insert(body.end(), packed.begin(), packed.end());
    return FullBox("stz2", 0, body);
}

std::vector<uint8_t> MakeStsz(uint32_t default_size, uint32_t sample_count) {
    std::vector<uint8_t> body;
    Put32(body, default_size);
    Put32(body, sample_count);
    return FullBox("stsz", 0, body);
}

std::vector<uint8_t> MakeStsd() {
    std::vector<uint8_t> body;
    Put32(body, 1);  // entry_count
    Put32(body, 16); // 首个 entry 的 size
    for (char c : std::string("avc1"))
        Put8(body, static_cast<uint8_t>(c));
    Put32(body, 0); // 补齐 entry 的其余字段
    return FullBox("stsd", 0, body);
}

// 一个结构完整、可被 IsobmffParser 接受的 trak（vide + timescale + 各类表）
std::vector<uint8_t> MakeTrak(const std::vector<uint8_t>& stbl_body) {
    std::vector<uint8_t> tkhd_body;
    Put32(tkhd_body, 0);    // creation_time
    Put32(tkhd_body, 0);    // modification_time
    Put32(tkhd_body, 1);    // track_id
    Put32(tkhd_body, 0);    // reserved
    Put32(tkhd_body, 1000); // duration
    const std::vector<uint8_t> tkhd = FullBox("tkhd", 0, tkhd_body);

    std::vector<uint8_t> mdhd_body;
    Put32(mdhd_body, 0);     // creation_time
    Put32(mdhd_body, 0);     // modification_time
    Put32(mdhd_body, 30000); // timescale
    Put32(mdhd_body, 60000); // duration
    const std::vector<uint8_t> mdhd = FullBox("mdhd", 0, mdhd_body);

    std::vector<uint8_t> hdlr_body;
    Put32(hdlr_body, 0); // pre_defined
    for (char c : std::string("vide"))
        Put8(hdlr_body, static_cast<uint8_t>(c));
    const std::vector<uint8_t> hdlr = FullBox("hdlr", 0, hdlr_body);

    const std::vector<uint8_t> stbl = Box("stbl", stbl_body);
    const std::vector<uint8_t> minf = Box("minf", stbl);
    const std::vector<uint8_t> mdia = Box("mdia", CatAll({mdhd, hdlr, minf}));
    return Box("trak", Cat(tkhd, mdia));
}

std::vector<uint8_t> MakeFile(const std::vector<uint8_t>& stbl_body) {
    std::vector<uint8_t> ftyp_body;
    for (char c : std::string("isom"))
        Put8(ftyp_body, static_cast<uint8_t>(c));
    Put32(ftyp_body, 0);
    for (char c : std::string("isom"))
        Put8(ftyp_body, static_cast<uint8_t>(c));
    const std::vector<uint8_t> ftyp = Box("ftyp", ftyp_body);

    std::vector<uint8_t> mvhd_body;
    Put32(mvhd_body, 0);
    Put32(mvhd_body, 0);
    Put32(mvhd_body, 1000); // timescale
    Put32(mvhd_body, 2000); // duration
    const std::vector<uint8_t> mvhd = FullBox("mvhd", 0, mvhd_body);

    const std::vector<uint8_t> moov = Box("moov", Cat(mvhd, MakeTrak(stbl_body)));
    const std::vector<uint8_t> mdat = Box("mdat", std::vector<uint8_t>(64, 0xAB));
    return CatAll({ftyp, moov, mdat});
}

bool WriteTemp(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return f.good();
}

// 拼文件 → 解析 → 返回第一条轨道（解析失败返回 nullptr 由调用方判空）
videoeye::IsobmffFile ParseFirstTrackFile(const std::vector<uint8_t>& stbl_body,
                                          videoeye::IsobmffParser::Options opt = {}) {
    const std::string path = "isobmff_parser_test_tmp.mp4";
    EXPECT_TRUE(WriteTemp(path, MakeFile(stbl_body)));
    videoeye::IsobmffFile out;
    EXPECT_TRUE(videoeye::IsobmffParser::Parse(path, out, opt));
    std::remove(path.c_str());
    return out;
}

// ---------------------------------------------------------------------------
// stts: entry_count 是「条目数」，不是「样本数」
// ---------------------------------------------------------------------------
TEST(IsobmffParserTest, SttsParsesAllEntriesNotJustTheFirst) {
    // 两条 entry 一共 150 个样本。旧实现用 added += sample_count 与 entry_count 比，
    // 第一条的 sample_count=100 立刻超过 entry_count=2，导致只解析出 1 条。
    const videoeye::IsobmffFile f = ParseFirstTrackFile(Cat(MakeStts({{100, 3000}, {50, 3000}}), MakeStsz(100, 150)));

    ASSERT_EQ(f.tracks.size(), 1u);
    const videoeye::IsobmffTrack& t = f.tracks[0];
    EXPECT_TRUE(t.has_stts);
    ASSERT_EQ(t.stts.size(), 2u);
    EXPECT_EQ(t.stts[0].sample_count, 100u);
    EXPECT_EQ(t.stts[1].sample_count, 50u);
    EXPECT_EQ(t.SttsSampleCount(), 150u);
    EXPECT_FALSE(t.tables_truncated);
}

// ---------------------------------------------------------------------------
// ctts v1: 偏移是有符号的（B 帧重排序会出现负值）
// ---------------------------------------------------------------------------
TEST(IsobmffParserTest, CttsVersion1KeepsSignedOffsets) {
    const uint32_t neg3000 = static_cast<uint32_t>(-3000);
    const videoeye::IsobmffFile f = ParseFirstTrackFile(Cat(MakeCtts(1, {{1, neg3000}, {1, 3000}}), MakeStsz(100, 2)));

    ASSERT_EQ(f.tracks.size(), 1u);
    const videoeye::IsobmffTrack& t = f.tracks[0];
    EXPECT_TRUE(t.has_ctts);
    ASSERT_EQ(t.ctts.size(), 2u);
    EXPECT_EQ(t.ctts[0].sample_offset, -3000);
    EXPECT_EQ(t.ctts[1].sample_offset, 3000);
}

// ---------------------------------------------------------------------------
// ctts v0: 偏移是无符号的，超大值夹到 INT32_MAX 而不是变成负数
// ---------------------------------------------------------------------------
TEST(IsobmffParserTest, CttsVersion0TreatsOffsetAsUnsigned) {
    const videoeye::IsobmffFile f = ParseFirstTrackFile(Cat(MakeCtts(0, {{1, 0xFFFFFFF0u}}), MakeStsz(100, 1)));

    ASSERT_EQ(f.tracks.size(), 1u);
    const videoeye::IsobmffTrack& t = f.tracks[0];
    ASSERT_EQ(t.ctts.size(), 1u);
    EXPECT_EQ(t.ctts[0].sample_offset, std::numeric_limits<int32_t>::max());
}

// ---------------------------------------------------------------------------
// stz2 field_size=4: 奇数样本时最后一字节的低 4 位是填充，不能算一个样本
// ---------------------------------------------------------------------------
TEST(IsobmffParserTest, Stz2FourBitOddSampleCountDropsPaddingNibble) {
    // 0x12 -> 1, 2；0x30 -> 3, (0x0 是填充)
    const videoeye::IsobmffFile f = ParseFirstTrackFile(MakeStz2(4, 3, {0x12, 0x30}));

    ASSERT_EQ(f.tracks.size(), 1u);
    const videoeye::IsobmffTrack& t = f.tracks[0];
    EXPECT_TRUE(t.has_stz2);
    EXPECT_EQ(t.stsz.field_size, 4u);
    ASSERT_EQ(t.stsz.sizes.size(), 3u);
    EXPECT_EQ(t.stsz.sizes[0], 1u);
    EXPECT_EQ(t.stsz.sizes[1], 2u);
    EXPECT_EQ(t.stsz.sizes[2], 3u);
}

TEST(IsobmffParserTest, Stz2FourBitEvenSampleCountUsesBothNibbles) {
    const videoeye::IsobmffFile f = ParseFirstTrackFile(MakeStz2(4, 4, {0x12, 0x34}));

    ASSERT_EQ(f.tracks.size(), 1u);
    const videoeye::IsobmffTrack& t = f.tracks[0];
    ASSERT_EQ(t.stsz.sizes.size(), 4u);
    EXPECT_EQ(t.stsz.sizes[3], 4u);
}

// ---------------------------------------------------------------------------
// 超大文件的保护: 撞到 max_entries 时要置 tables_truncated
// ---------------------------------------------------------------------------
TEST(IsobmffParserTest, MaxEntriesTruncatesAndFlags) {
    videoeye::IsobmffParser::Options opt;
    opt.max_entries_per_table = 1;
    const videoeye::IsobmffFile f =
        ParseFirstTrackFile(Cat(MakeStts({{100, 3000}, {50, 3000}}), MakeStsz(100, 150)), opt);

    ASSERT_EQ(f.tracks.size(), 1u);
    EXPECT_EQ(f.tracks[0].stts.size(), 1u);
    EXPECT_TRUE(f.tracks[0].tables_truncated);
}

// ---------------------------------------------------------------------------
// 基本容器结构（保证上面的断言不是建立在一条空轨道上）
// ---------------------------------------------------------------------------
TEST(IsobmffParserTest, ParsesContainerAndTrackBasics) {
    const videoeye::IsobmffFile f =
        ParseFirstTrackFile(CatAll({MakeStsd(), MakeStts({{10, 3000}}), MakeStsz(100, 10)}));

    EXPECT_TRUE(f.ok);
    EXPECT_TRUE(f.found_moov);
    EXPECT_TRUE(f.found_mdat);
    EXPECT_TRUE(f.moov_before_mdat);
    EXPECT_EQ(f.movie_timescale, 1000u);
    ASSERT_EQ(f.tracks.size(), 1u);
    EXPECT_EQ(f.tracks[0].track_id, 1u);
    EXPECT_EQ(f.tracks[0].handler, "vide");
    EXPECT_EQ(f.tracks[0].media_timescale, 30000u);
    EXPECT_EQ(f.tracks[0].TypeName(), "video");
    ASSERT_FALSE(f.top_level_order.empty());
    EXPECT_EQ(f.top_level_order[0], "ftyp");
}

// ===========================================================================
// box 字段级解析：ftyp / mvhd / tkhd / mdhd / hdlr / avc1 入口 + 编码配置子树
//
// 以前只有样本表那 8 类 box 有字段，ftyp/tkhd/avcC/colr 在树里都是空节点；
// stsd 的 sample entry（avc1）压根不算容器，于是 avcC/colr/pasp/btrt 整棵子树
// 从来没进过结构树。这一组把"打开 MP4 第一眼该看到什么"钉住。
// ===========================================================================

void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

void PutFourCC(std::vector<uint8_t>& v, const char* s) {
    for (int i = 0; i < 4; ++i)
        Put8(v, static_cast<uint8_t>(s[i]));
}

// avc1 入口：78 字节 SampleEntry 头 + 编码配置子树
std::vector<uint8_t> MakeAvc1Entry() {
    std::vector<uint8_t> payload;
    for (int i = 0; i < 6; ++i)
        Put8(payload, 0); // reserved
    Put16(payload, 1);    // data_reference_index
    for (int i = 0; i < 16; ++i)
        Put8(payload, 0);       // pre_defined/reserved
    Put16(payload, 1920);       // width
    Put16(payload, 1080);       // height
    Put32(payload, 0x00480000); // horizresolution 72dpi
    Put32(payload, 0x00480000); // vertresolution
    Put32(payload, 0);          // reserved
    Put16(payload, 1);          // frame_count
    for (int i = 0; i < 32; ++i)
        Put8(payload, 0);   // compressorname
    Put16(payload, 24);     // depth
    Put16(payload, 0xFFFF); // pre_defined
    // ↑ 到这里 78 字节，下面是子 box

    // avcC: High(100) / Level 4.0(40) / lengthSize=4 / 1 个 SPS + 1 个 PPS
    std::vector<uint8_t> avcc_body;
    Put8(avcc_body, 1);    // configurationVersion
    Put8(avcc_body, 100);  // AVCProfileIndication
    Put8(avcc_body, 0);    // profile_compatibility
    Put8(avcc_body, 40);   // AVCLevelIndication
    Put8(avcc_body, 0xFF); // 保留 + lengthSizeMinusOne=3
    Put8(avcc_body, 0xE1); // 保留 + numOfSPS=1
    Put16(avcc_body, 4);   // SPS 长度
    for (uint8_t b : std::vector<uint8_t>{0x67, 0x64, 0x00, 0x28})
        Put8(avcc_body, b);
    Put8(avcc_body, 1); // numOfPPS
    Put16(avcc_body, 4);
    for (uint8_t b : std::vector<uint8_t>{0x68, 0xEE, 0x3C, 0x80})
        Put8(avcc_body, b);

    // colr: nclx / BT.2020 primaries(9) / PQ transfer(16) / BT.2020 matrix(9) / 有限范围
    std::vector<uint8_t> colr_body;
    PutFourCC(colr_body, "nclx");
    Put16(colr_body, 9);
    Put16(colr_body, 16);
    Put16(colr_body, 9);
    Put8(colr_body, 0x00);

    std::vector<uint8_t> pasp_body;
    Put32(pasp_body, 1);
    Put32(pasp_body, 1);

    std::vector<uint8_t> btrt_body;
    Put32(btrt_body, 1000);    // bufferSizeDB
    Put32(btrt_body, 8000000); // maxBitrate
    Put32(btrt_body, 5000000); // avgBitrate

    const std::vector<uint8_t> children =
        CatAll({Box("avcC", avcc_body), Box("colr", colr_body), Box("pasp", pasp_body), Box("btrt", btrt_body)});
    return Box("avc1", Cat(payload, children));
}

// 一个字段完整的文件：ftyp / mvhd / tkhd / mdia(mdhd+hdlr) / stbl(stsd)
std::vector<uint8_t> MakeRichFile() {
    std::vector<uint8_t> ftyp_body;
    PutFourCC(ftyp_body, "isom");
    Put32(ftyp_body, 0x200); // minor_version
    for (const char* brand : {"isom", "iso2", "avc1", "mp41"})
        PutFourCC(ftyp_body, brand);
    const std::vector<uint8_t> ftyp = Box("ftyp", ftyp_body);

    std::vector<uint8_t> mvhd_body;
    Put32(mvhd_body, 0);          // creation_time
    Put32(mvhd_body, 0);          // modification_time
    Put32(mvhd_body, 1000);       // timescale
    Put32(mvhd_body, 2000);       // duration
    Put32(mvhd_body, 0x00010000); // rate
    Put16(mvhd_body, 0x0100);     // volume
    for (int i = 0; i < 10; ++i)
        Put8(mvhd_body, 0); // reserved
    for (int i = 0; i < 36; ++i)
        Put8(mvhd_body, 0); // matrix
    for (int i = 0; i < 24; ++i)
        Put8(mvhd_body, 0); // pre_defined
    Put32(mvhd_body, 7);    // next_track_id
    const std::vector<uint8_t> mvhd = FullBox("mvhd", 0, mvhd_body);

    std::vector<uint8_t> tkhd_body;
    Put32(tkhd_body, 0);    // creation_time
    Put32(tkhd_body, 0);    // modification_time
    Put32(tkhd_body, 1);    // track_id
    Put32(tkhd_body, 0);    // reserved
    Put32(tkhd_body, 2000); // duration
    for (int i = 0; i < 8; ++i)
        Put8(tkhd_body, 0);   // reserved
    Put16(tkhd_body, 0);      // layer
    Put16(tkhd_body, 0);      // alternate_group
    Put16(tkhd_body, 0x0100); // volume
    Put16(tkhd_body, 0);      // reserved
    for (int i = 0; i < 36; ++i)
        Put8(tkhd_body, 0);        // matrix
    Put32(tkhd_body, 1920u << 16); // width (16.16)
    Put32(tkhd_body, 1080u << 16); // height
    const std::vector<uint8_t> tkhd = FullBox("tkhd", 0, tkhd_body);

    std::vector<uint8_t> mdhd_body;
    Put32(mdhd_body, 0);      // creation_time
    Put32(mdhd_body, 0);      // modification_time
    Put32(mdhd_body, 30000);  // timescale
    Put32(mdhd_body, 60000);  // duration
    Put16(mdhd_body, 0x15C7); // language = "eng"
    Put16(mdhd_body, 0);      // pre_defined
    const std::vector<uint8_t> mdhd = FullBox("mdhd", 0, mdhd_body);

    std::vector<uint8_t> hdlr_body;
    Put32(hdlr_body, 0);          // pre_defined
    PutFourCC(hdlr_body, "vide"); // handler_type
    for (int i = 0; i < 12; ++i)
        Put8(hdlr_body, 0); // reserved
    for (char c : std::string("VideoHandler"))
        Put8(hdlr_body, static_cast<uint8_t>(c));
    Put8(hdlr_body, 0);
    const std::vector<uint8_t> hdlr = FullBox("hdlr", 0, hdlr_body);

    std::vector<uint8_t> stsd_body;
    Put32(stsd_body, 1); // entry_count
    const std::vector<uint8_t> stsd = FullBox("stsd", 0, Cat(stsd_body, MakeAvc1Entry()));

    const std::vector<uint8_t> stbl = Box("stbl", stsd);
    const std::vector<uint8_t> minf = Box("minf", stbl);
    const std::vector<uint8_t> mdia = Box("mdia", CatAll({mdhd, hdlr, minf}));
    const std::vector<uint8_t> trak = Box("trak", Cat(tkhd, mdia));
    const std::vector<uint8_t> moov = Box("moov", Cat(mvhd, trak));
    return CatAll({ftyp, moov, Box("mdat", std::vector<uint8_t>(32, 0xAB))});
}

const videoeye::IsobmffBox* FindBox(const std::vector<videoeye::IsobmffBox>& boxes, const std::string& type) {
    for (const auto& b : boxes) {
        if (b.type == type)
            return &b;
        if (const videoeye::IsobmffBox* inner = FindBox(b.children, type))
            return inner;
    }
    return nullptr;
}

std::string FieldOf(const videoeye::IsobmffBox& box, const std::string& name) {
    for (const auto& f : box.fields) {
        if (f.first == name)
            return f.second;
    }
    return std::string();
}

TEST(IsobmffParserTest, FillsBrandTimescaleAndTrackGeometry) {
    const std::string path = "isobmff_parser_rich_tmp.mp4";
    ASSERT_TRUE(WriteTemp(path, MakeRichFile()));
    videoeye::IsobmffFile out;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(path, out));
    std::remove(path.c_str());

    const videoeye::IsobmffBox* ftyp = FindBox(out.top_level, "ftyp");
    ASSERT_TRUE(ftyp != nullptr);
    EXPECT_EQ(FieldOf(*ftyp, "major_brand"), "isom");
    EXPECT_EQ(FieldOf(*ftyp, "minor_version"), "512");
    EXPECT_NE(FieldOf(*ftyp, "compatible_brands").find("avc1"), std::string::npos);

    const videoeye::IsobmffBox* mvhd = FindBox(out.top_level, "mvhd");
    ASSERT_TRUE(mvhd != nullptr);
    EXPECT_EQ(FieldOf(*mvhd, "timescale"), "1000");
    EXPECT_EQ(FieldOf(*mvhd, "duration"), "2000");
    EXPECT_EQ(FieldOf(*mvhd, "duration_seconds"), "2.000");
    EXPECT_EQ(FieldOf(*mvhd, "next_track_id"), "7");

    const videoeye::IsobmffBox* tkhd = FindBox(out.top_level, "tkhd");
    ASSERT_TRUE(tkhd != nullptr);
    EXPECT_EQ(FieldOf(*tkhd, "track_id"), "1");
    EXPECT_EQ(FieldOf(*tkhd, "width"), "1920");
    EXPECT_EQ(FieldOf(*tkhd, "height"), "1080");

    const videoeye::IsobmffBox* mdhd = FindBox(out.top_level, "mdhd");
    ASSERT_TRUE(mdhd != nullptr);
    EXPECT_EQ(FieldOf(*mdhd, "timescale"), "30000");
    EXPECT_EQ(FieldOf(*mdhd, "language"), "eng");

    const videoeye::IsobmffBox* hdlr = FindBox(out.top_level, "hdlr");
    ASSERT_TRUE(hdlr != nullptr);
    EXPECT_EQ(FieldOf(*hdlr, "handler_type"), "vide");
    EXPECT_EQ(FieldOf(*hdlr, "name"), "VideoHandler");
}

TEST(IsobmffParserTest, DescendsIntoStsdSampleEntryCodecBoxes) {
    const std::string path = "isobmff_parser_rich2_tmp.mp4";
    ASSERT_TRUE(WriteTemp(path, MakeRichFile()));
    videoeye::IsobmffFile out;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(path, out));
    std::remove(path.c_str());

    // avc1 入口本身要有宽高/参考索引
    const videoeye::IsobmffBox* avc1 = FindBox(out.top_level, "avc1");
    ASSERT_TRUE(avc1 != nullptr) << "stsd 的 sample entry 必须出现在 box 树里";
    EXPECT_EQ(FieldOf(*avc1, "width"), "1920");
    EXPECT_EQ(FieldOf(*avc1, "height"), "1080");
    EXPECT_EQ(FieldOf(*avc1, "data_reference_index"), "1");

    // 子树：avcC / colr / pasp / btrt 以前完全不进树
    const videoeye::IsobmffBox* avcc = FindBox(out.top_level, "avcC");
    ASSERT_TRUE(avcc != nullptr);
    EXPECT_EQ(FieldOf(*avcc, "profile_idc"), "100");
    EXPECT_EQ(FieldOf(*avcc, "level_idc"), "40");
    EXPECT_EQ(FieldOf(*avcc, "length_size_minus_one"), "3");
    EXPECT_EQ(FieldOf(*avcc, "num_of_sps"), "1");
    EXPECT_EQ(FieldOf(*avcc, "num_of_pps"), "1");

    const videoeye::IsobmffBox* colr = FindBox(out.top_level, "colr");
    ASSERT_TRUE(colr != nullptr);
    EXPECT_EQ(FieldOf(*colr, "colour_type"), "nclx");
    EXPECT_EQ(FieldOf(*colr, "colour_primaries"), "9");
    EXPECT_EQ(FieldOf(*colr, "transfer_characteristics"), "16");
    EXPECT_EQ(FieldOf(*colr, "matrix_coefficients"), "9");

    const videoeye::IsobmffBox* pasp = FindBox(out.top_level, "pasp");
    ASSERT_TRUE(pasp != nullptr);
    EXPECT_EQ(FieldOf(*pasp, "hSpacing"), "1");
    EXPECT_EQ(FieldOf(*pasp, "vSpacing"), "1");

    const videoeye::IsobmffBox* btrt = FindBox(out.top_level, "btrt");
    ASSERT_TRUE(btrt != nullptr);
    EXPECT_EQ(FieldOf(*btrt, "maxBitrate"), "8000000");
    EXPECT_EQ(FieldOf(*btrt, "avgBitrate"), "5000000");
}

// ---------------------------------------------------------------------------
// stsd 多 sample entry：codec 仍取第一个，但全部描述都要按顺序留下
//
// Dolby Vision 双层、多码流打包等场景下 stsd 会声明多个 entry，样本通过
// stsc 的 sample_description_index 选择描述。只记第一个会导致其余样本被
// 误判编码，因此解析层必须把整张表暴露出来（容器规则再决定是否告警）。
// ---------------------------------------------------------------------------
TEST(IsobmffParserTest, StsdCollectsAllSampleEntries) {
    std::vector<uint8_t> body;
    Put32(body, 2);  // entry_count
    Put32(body, 16); // 第 1 个 entry 的 size
    PutFourCC(body, "avc1");
    Put32(body, 0); // 补齐到声明的 16 字节
    Put32(body, 0);
    Put32(body, 16); // 第 2 个 entry 的 size
    PutFourCC(body, "dvhe");
    Put32(body, 0);
    Put32(body, 0);

    const videoeye::IsobmffFile f = ParseFirstTrackFile(Cat(FullBox("stsd", 0, body), MakeStsz(100, 1)));

    ASSERT_EQ(f.tracks.size(), 1u);
    const videoeye::IsobmffTrack& t = f.tracks[0];
    ASSERT_EQ(t.sample_descriptions.size(), 2u);
    EXPECT_EQ(t.sample_descriptions[0], "avc1");
    EXPECT_EQ(t.sample_descriptions[1], "dvhe");
    EXPECT_EQ(t.codec, "avc1") << "codec 仍然是第一个 entry，保持既有行为";
}

// ===========================================================================
// fMP4 分片：首样本随机访问标志（阶段 8 的"实证层"）
// ===========================================================================
//
// 背景缺陷（2026-10 审计）：SegmentQcAnalyzer 以前读到 tfdt 就把
// starts_with_keyframe 置真。但 tfdt 只是**解码起始时间**，与"首样本是不是
// 随机访问点"是两件事 —— CMAF 约定分片以 IDR 开头，约定不是证据。
//
// 真正的依据在 sample_flags 里（ISO/IEC 14496-12 §8.8.8）：
//   bit 16    = sample_is_non_sync_sample
//   bit 24-25 = sample_depends_on（2 = 不依赖他人 = I 帧）
// 三个来源按"越具体越优先"取值：
//   trun.first_sample_flags > 首样本的 sample_flags > tfhd.default_sample_flags
//
// 这组用例同时盯住另一个回归点：tfhd 的 0x000020(default_sample_flags) 曾经
// 被漏掉，导致 pos 少推 4 字节，其后的 tfdt/trun 全部错位。

namespace {

void Put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 7; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFF));
}

// FullBox，但 flags 由调用方指定（tfhd/trun 的语义全在 flags 里）
std::vector<uint8_t> FullBoxWithFlags(const std::string& type, uint8_t version, uint32_t flags,
                                      const std::vector<uint8_t>& body) {
    std::vector<uint8_t> payload{version};
    payload.push_back(static_cast<uint8_t>((flags >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((flags >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(flags & 0xFF));
    payload.insert(payload.end(), body.begin(), body.end());
    return Box(type, payload);
}

struct TfhdSpec {
    bool default_sample_flags = false;
    uint32_t default_sample_flags_value = 0;
    bool default_base_is_moof = true;
};

struct TrunSpec {
    bool first_sample_flags = false;
    uint32_t first_sample_flags_value = 0;
    bool per_sample_flags = false;
    uint32_t per_sample_flags_value = 0;
    uint32_t sample_count = 2;
};

// moof(mfhd + traf(tfhd + tfdt + trun))
std::vector<uint8_t> MakeMoof(uint32_t seq, const TfhdSpec& tfhd_spec, const TrunSpec& trun_spec) {
    std::vector<uint8_t> mfhd_body;
    Put32(mfhd_body, seq);
    const std::vector<uint8_t> mfhd = FullBox("mfhd", 0, mfhd_body);

    uint32_t tfhd_flags = 0;
    std::vector<uint8_t> tfhd_body;
    Put32(tfhd_body, 1); // track_ID
    if (tfhd_spec.default_base_is_moof)
        tfhd_flags |= 0x020000;
    tfhd_flags |= 0x000008; // default_sample_duration
    Put32(tfhd_body, 1000);
    tfhd_flags |= 0x000010; // default_sample_size
    Put32(tfhd_body, 500);
    if (tfhd_spec.default_sample_flags) {
        tfhd_flags |= 0x000020;
        Put32(tfhd_body, tfhd_spec.default_sample_flags_value);
    }
    const std::vector<uint8_t> tfhd = FullBoxWithFlags("tfhd", 0, tfhd_flags, tfhd_body);

    std::vector<uint8_t> tfdt_body;
    Put32(tfdt_body, 90000); // baseMediaDecodeTime
    const std::vector<uint8_t> tfdt = FullBox("tfdt", 0, tfdt_body);

    uint32_t trun_flags = 0;
    std::vector<uint8_t> trun_body;
    Put32(trun_body, trun_spec.sample_count);
    if (trun_spec.first_sample_flags) {
        trun_flags |= 0x000004;
        Put32(trun_body, trun_spec.first_sample_flags_value);
    }
    if (trun_spec.per_sample_flags)
        trun_flags |= 0x000400;
    for (uint32_t i = 0; i < trun_spec.sample_count; ++i) {
        if (trun_spec.per_sample_flags)
            Put32(trun_body, trun_spec.per_sample_flags_value);
    }
    const std::vector<uint8_t> trun = FullBoxWithFlags("trun", 0, trun_flags, trun_body);

    const std::vector<uint8_t> traf = Box("traf", CatAll({tfhd, tfdt, trun}));
    return Box("moof", Cat(mfhd, traf));
}

// ftyp + moov + moof + mdat（分片文件）
std::vector<uint8_t> MakeFragmentedFile(const TfhdSpec& tfhd_spec, const TrunSpec& trun_spec) {
    std::vector<uint8_t> ftyp_body;
    PutFourCC(ftyp_body, "isom");
    Put32(ftyp_body, 0);
    PutFourCC(ftyp_body, "isom");
    const std::vector<uint8_t> ftyp = Box("ftyp", ftyp_body);

    std::vector<uint8_t> mvhd_body;
    Put32(mvhd_body, 0);
    Put32(mvhd_body, 0);
    Put32(mvhd_body, 1000);
    Put32(mvhd_body, 2000);
    const std::vector<uint8_t> moov =
        Box("moov", Cat(FullBox("mvhd", 0, mvhd_body), MakeTrak(Cat(MakeStts({{2, 1000}}), MakeStsz(500, 2)))));

    const std::vector<uint8_t> moof = MakeMoof(1, tfhd_spec, trun_spec);
    const std::vector<uint8_t> mdat = Box("mdat", std::vector<uint8_t>(1000, 0xAB));
    return CatAll({ftyp, moov, moof, mdat});
}

videoeye::IsobmffFile ParseFragmented(const TfhdSpec& tfhd_spec, const TrunSpec& trun_spec) {
    const std::string path = "isobmff_fmp4_test_tmp.mp4";
    EXPECT_TRUE(WriteTemp(path, MakeFragmentedFile(tfhd_spec, trun_spec)));
    videoeye::IsobmffFile out;
    EXPECT_TRUE(videoeye::IsobmffParser::Parse(path, out, {}));
    std::remove(path.c_str());
    return out;
}

constexpr uint32_t kSyncIndependent = 0x02000000; // depends_on=2, 非 non-sync
constexpr uint32_t kNonSyncDepends = 0x01010000;  // non-sync + depends_on=1

} // namespace

// 正例：trun.first_sample_flags 最具体，应当优先于 tfhd.default_sample_flags。
// 这条同时是 tfhd 0x000020 分支的回归：漏读该字段会让 pos 错位，tfdt 便读不到。
TEST(IsobmffParserTest, Fmp4FirstSampleFlagsPrefersTrunOverTfhd) {
    TfhdSpec tfhd;
    tfhd.default_sample_flags = true;
    tfhd.default_sample_flags_value = kNonSyncDepends; // 整段默认 non-sync
    TrunSpec trun;
    trun.first_sample_flags = true;
    trun.first_sample_flags_value = kSyncIndependent; // 但首样本是 I 帧

    const videoeye::IsobmffFile f = ParseFragmented(tfhd, trun);
    ASSERT_EQ(f.fragments.size(), 1u);
    const videoeye::IsobmffFragment& frag = f.fragments[0];
    EXPECT_TRUE(frag.has_tfdt) << "tfhd 带 default_sample_flags 时 tfdt 也必须能读出来";
    EXPECT_EQ(frag.base_media_decode_time, 90000u);
    EXPECT_TRUE(frag.has_first_sample_flags);
    EXPECT_EQ(frag.first_sample_flags, kSyncIndependent);
    EXPECT_TRUE(frag.first_sample_is_sync);
    EXPECT_EQ(frag.first_sample_depends_on, 2);
    EXPECT_FALSE(frag.first_sample_flags_incomplete);
}

// 反例：首样本确实是 non-sync。以前无条件 starts_with_keyframe=true 会把它报成
// "可独立解码"，这里必须能看出不是随机访问点。
TEST(IsobmffParserTest, Fmp4DetectsNonSyncFirstSample) {
    TfhdSpec tfhd; // 不给 default flags
    TrunSpec trun;
    trun.first_sample_flags = true;
    trun.first_sample_flags_value = kNonSyncDepends;

    const videoeye::IsobmffFile f = ParseFragmented(tfhd, trun);
    ASSERT_EQ(f.fragments.size(), 1u);
    const videoeye::IsobmffFragment& frag = f.fragments[0];
    EXPECT_TRUE(frag.has_first_sample_flags);
    EXPECT_FALSE(frag.first_sample_is_sync) << "bit16(sample_is_non_sync_sample) 置位就是非同步样本";
    EXPECT_EQ(frag.first_sample_depends_on, 1);
}

// 首样本自己的 sample_flags（trun 里逐样本 flags）优先于 tfhd 缺省值
TEST(IsobmffParserTest, Fmp4UsesPerSampleFlagsWhenNoFirstSampleFlags) {
    TfhdSpec tfhd;
    tfhd.default_sample_flags = true;
    tfhd.default_sample_flags_value = kNonSyncDepends;
    TrunSpec trun;
    trun.per_sample_flags = true;
    trun.per_sample_flags_value = kSyncIndependent;

    const videoeye::IsobmffFile f = ParseFragmented(tfhd, trun);
    ASSERT_EQ(f.fragments.size(), 1u);
    const videoeye::IsobmffFragment& frag = f.fragments[0];
    EXPECT_TRUE(frag.has_first_sample_flags);
    EXPECT_EQ(frag.first_sample_flags, kSyncIndependent);
    EXPECT_TRUE(frag.first_sample_is_sync);
}

// trun 完全没有 flags 时退回 tfhd.default_sample_flags
TEST(IsobmffParserTest, Fmp4FallsBackToTfhdDefaultFlags) {
    TfhdSpec tfhd;
    tfhd.default_sample_flags = true;
    tfhd.default_sample_flags_value = kSyncIndependent;
    TrunSpec trun; // 无任何 flags

    const videoeye::IsobmffFile f = ParseFragmented(tfhd, trun);
    ASSERT_EQ(f.fragments.size(), 1u);
    const videoeye::IsobmffFragment& frag = f.fragments[0];
    EXPECT_TRUE(frag.has_first_sample_flags);
    EXPECT_TRUE(frag.tfhd_default_sample_flags_present);
    EXPECT_TRUE(frag.first_sample_is_sync);
}

// 反例：三个来源都没有 → 必须显式"无法证实"，绝不能默认成"是关键帧"。
// 这是本阶段最核心的一条：缺证据不等于没问题。
TEST(IsobmffParserTest, Fmp4MissingFlagsIsReportedAsIncomplete) {
    TfhdSpec tfhd; // 无 default flags
    TrunSpec trun; // 无任何 flags

    const videoeye::IsobmffFile f = ParseFragmented(tfhd, trun);
    ASSERT_EQ(f.fragments.size(), 1u);
    const videoeye::IsobmffFragment& frag = f.fragments[0];
    EXPECT_TRUE(frag.has_tfdt) << "tfdt 本身仍然要读到";
    EXPECT_FALSE(frag.has_first_sample_flags);
    EXPECT_FALSE(frag.first_sample_is_sync);
    EXPECT_EQ(frag.first_sample_depends_on, -1);
    EXPECT_TRUE(frag.first_sample_flags_incomplete)
        << "缺省值可能来自 init 段的 trex，本解析器不跨文件读，必须显式告知";
}

// ---------------------------------------------------------------------------
// 字段级解析：派发表里登记过的每一种 box，都必须真能填出字段
//
// 动机（量过，不是凭感觉）：改表驱动之前，18 种 box 里只有 8 种有字段断言
// （ftyp / mvhd / tkhd / mdhd / hdlr / avcC / colr / pasp），
// 剩下 10 种 —— styp / sidx / mfhd / tfhd / tfdt / trun / hvcC / av1C / esds / btrt ——
// **一条断言都没有**。Fill* 的长度守卫写错（n < X 算大了）会让那种 box 永远填不出字段，
// 而现有用例一条都不会红。
// ---------------------------------------------------------------------------

std::vector<uint8_t> Put32All(std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out;
    for (uint32_t w : words)
        Put32(out, w);
    return out;
}

// FullBox 载荷：version/flags(4) + 若干 32 位大端字段
std::vector<uint8_t> FullWords(uint8_t version, std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out{version, 0, 0, 0};
    for (uint32_t w : words)
        Put32(out, w);
    return out;
}

// 一种 box 的最小合法载荷（不含 box 头），只要够走完该类型的字段解析。
// ❗ 新增支持的 box 类型时这里必须同步 —— 下面会明确报"不认识"，不会静默跳过。
std::vector<uint8_t> MinimalFieldBoxPayload(const std::string& type, bool& known) {
    known = true;
    if (type == "ftyp" || type == "styp") { // major_brand(4) + minor_version(4)
        std::vector<uint8_t> p;
        for (char c : std::string("isom"))
            Put8(p, static_cast<uint8_t>(c));
        Put32(p, 512);
        return p;
    }
    if (type == "mvhd") // creation(4) mod(4) timescale(4) duration(4)
        return FullWords(0, {0, 0, 1000, 2000});
    if (type == "tkhd") { // track_id@12、duration@20、width/height@76 起
        std::vector<uint8_t> p = FullWords(0, {0, 0, 1, 0, 2000});
        p.resize(84, 0);
        return p;
    }
    if (type == "mdhd") { // timescale@12、duration@16、language@20
        std::vector<uint8_t> p = FullWords(0, {0, 0, 30000, 2000});
        p.resize(24, 0);
        return p;
    }
    if (type == "hdlr") { // predefined(4) + handler_type(4)@8
        std::vector<uint8_t> p = FullWords(0, {0});
        for (char c : std::string("vide"))
            Put8(p, static_cast<uint8_t>(c));
        return p;
    }
    if (type == "sidx") { // reference_id@4 timescale@8 ept@12 first_offset@16 ref_count@22
        std::vector<uint8_t> p = FullWords(0, {1, 1000, 0, 0});
        p.resize(24, 0);
        return p;
    }
    if (type == "mfhd")
        return FullWords(0, {7}); // sequence_number
    if (type == "tfhd")
        return FullWords(0, {1}); // track_id
    if (type == "tfdt")
        return FullWords(0, {0}); // base_media_decode_time
    if (type == "trun")
        return FullWords(0, {3}); // sample_count
    if (type == "avcC")           // 非 FullBox，载荷从 configurationVersion 开始
        return {1, 100, 0, 41, 0xFF, 0x01};
    if (type == "hvcC") {
        std::vector<uint8_t> p(23, 0);
        p[0] = 1;    // configurationVersion
        p[12] = 120; // general_level_idc
        return p;
    }
    if (type == "av1C")
        return {0x81, 0x00, 0x00, 0x00};
    if (type == "esds") { // FullBox(4) + DecoderConfigDescriptor(tag 0x04, len 13, body 13)
        std::vector<uint8_t> p = FullWords(0, {0});
        Put8(p, 0x04);
        Put8(p, 13);
        p.resize(p.size() + 13, 0);
        return p;
    }
    if (type == "colr") { // "nclx" + primaries(2) transfer(2) matrix(2) range(1)
        std::vector<uint8_t> p;
        for (char c : std::string("nclx"))
            Put8(p, static_cast<uint8_t>(c));
        const std::vector<uint8_t> tail = {0, 1, 0, 1, 0, 1, 0x00};
        p.insert(p.end(), tail.begin(), tail.end());
        return p;
    }
    if (type == "pasp")
        return Put32All({1, 1}); // hSpacing / vSpacing
    if (type == "btrt")
        return Put32All({0, 128000, 96000});
    known = false;
    return {};
}

TEST(IsobmffParserTest, EveryFieldBoxTypeFillsAtLeastOneField) {
    const std::vector<std::string> types = videoeye::IsobmffParser::FieldBoxTypes();
    ASSERT_FALSE(types.empty()) << "FieldBoxTypes() 返回空 —— 派发表被清空了";

    // 所有 box 平铺在顶层：FillBoxFields 对每个遇到的 box 都会跑，与层级无关。
    std::vector<uint8_t> file;
    std::vector<std::string> unknown;
    for (const std::string& type : types) {
        bool known = false;
        const std::vector<uint8_t> payload = MinimalFieldBoxPayload(type, known);
        if (!known || payload.empty()) {
            unknown.push_back(type);
            continue;
        }
        const std::vector<uint8_t> box = Box(type, payload);
        file.insert(file.end(), box.begin(), box.end());
    }
    // 一次性把不认识的全报出来：逐个 ASSERT 会让后面所有类型的覆盖一起丢掉。
    if (!unknown.empty()) {
        std::string joined;
        for (const std::string& t : unknown)
            joined += " " + t;
        FAIL() << "FieldBoxTypes() 里有用例不认识的类型:" << joined
               << " —— 请补上它的最小载荷（新增一种受支持的 box 必须同步这里）";
    }

    const std::string path = "isobmff_field_boxes_tmp.mp4";
    ASSERT_TRUE(WriteTemp(path, file));
    videoeye::IsobmffFile out;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(path, out)) << out.error_message;
    std::remove(path.c_str());

    int filled = 0;
    for (const std::string& type : types) {
        const videoeye::IsobmffBox* box = FindBox(out.top_level, type);
        if (box == nullptr) {
            ADD_FAILURE() << "顶层没找到「" << type << "」—— fixture 没拼进去？";
            continue;
        }
        if (!box->fields.empty()) {
            ++filled;
        } else {
            ADD_FAILURE() << "「" << type
                          << "」在派发表里登记过，却一个字段都没填出来 —— 要么 Fill* 的长度守卫"
                             "（n < X）算大了，要么派发到了空实现";
        }
    }
    // 覆盖守护：Parse 失败 / FindBox 失效时上面的循环会静默空转。
    EXPECT_EQ(filled, static_cast<int>(types.size()))
        << "只有 " << filled << " / " << types.size() << " 种 box 填出了字段";
}

} // namespace
