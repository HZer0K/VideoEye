// 五个容器结构分析器的参数化健壮性测试
//
// Asf / Avi / Flv / Ogg / Ts 五个 StructureAnalyzer 的接口完全一致：
//     bool Analyze(const std::string& file_path, model::ContainerStructureResult& result);
// 但此前一个单测都没有（见 docs/ARCHITECTURE_REVIEW_2026-10-04 第 10 项）。
// 这里用 TEST_P 把"五个分析器 × 三种输入"压成一个矩阵，覆盖的是最容易出错、
// 又最不依赖真实媒体文件的三类边界：
//   1. 文件不存在   —— 必须返回 false 并给出统一的"无法打开文件"
//   2. 魔数对不上   —— 必须返回 false 并给出非空错误说明（OGG 例外，见文末）
//   3. 最小合法文件 —— 必须返回 true，并填满 format / 结构树 / summary
//
// 样本字节全部在内存里拼好后落盘（QTemporaryDir），不依赖任何真实媒体文件，
// 因此这一组测试在任何环境都能跑。

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "core/analysis/container/AsfStructureAnalyzer.h"
#include "core/analysis/container/AviStructureAnalyzer.h"
#include "core/analysis/container/FlvStructureAnalyzer.h"
#include "core/analysis/container/OggStructureAnalyzer.h"
#include "core/analysis/container/TsStructureAnalyzer.h"
#include "core/domain/model/ContainerStructureInfo.h"

using namespace videoeye;

namespace {

using AnalyzeFn = std::function<bool(const std::string&, model::ContainerStructureResult&)>;

struct AnalyzerCase {
    const char* name;                // gtest 打印用的参数名
    AnalyzeFn run;
    model::ContainerFormat format;   // 最小合法样本应识别出的格式
    const char* format_name;
    QByteArray minimal_valid;        // 最小合法样本字节
    bool rejects_foreign_magic;      // 魔数对不上时是否报错
};

// ---- 最小合法样本 ----------------------------------------------------------
// 每个样本都刻意做到"刚好能通过魔数校验、然后没有任何子结构可解析"，
// 这样断言只针对入口契约，不牵扯子结构解析的细节。

// ASF: Header Object GUID + size(30) + 子对象数 0 + 2 字节保留
QByteArray AsfMinimal() {
    QByteArray b = QByteArray::fromHex("3026B2758E66CF11A6D900AA0062CE6C");
    b.append(QByteArray::fromHex("1E00000000000000"));   // LE64: 30（= 本文件长度）
    b.append(QByteArray::fromHex("00000000"));           // LE32: 0 个子对象
    b.append(QByteArray::fromHex("0000"));               // reserved
    return b;
}

// AVI: "RIFF" + size(0) + "AVI "
QByteArray AviMinimal() {
    QByteArray b("RIFF");
    b.append(QByteArray::fromHex("00000000"));
    b.append("AVI ");
    return b;
}

// FLV: "FLV" + version + flags(video|audio) + header_size(9, BE)
QByteArray FlvMinimal() {
    QByteArray b("FLV");
    b.append(char(0x01));
    b.append(char(0x05));                        // 0x01 video | 0x04 audio
    b.append(QByteArray::fromHex("00000009"));   // BE32: header size = 9
    return b;
}

// MPEG-TS: 两个 188 字节的 null packet（PID 0x1FFF），满足"隔 188 字节再遇 0x47"
// 的同步字节判定，又不引入任何真实的 PAT/PMT 解析。
QByteArray TsMinimal() {
    QByteArray pkt(188, '\0');
    pkt[0] = char(0x47);    // sync byte
    pkt[1] = char(0x1F);    // PID 高 5 位 -> 0x1FFF (null packet)
    pkt[2] = char(0xFF);    // PID 低 8 位
    pkt[3] = char(0x10);    // 仅载荷, 无适配域, cc=0
    QByteArray b = pkt;
    b.append(pkt);
    return b;
}

// OGG: 一个 BOS page（"OggS" + 27 字节头 + 1 段长度为 0），没有载荷
QByteArray OggMinimal() {
    QByteArray b("OggS");
    b.append(char(0x00));                        // version
    b.append(char(0x02));                        // header_type = BOS
    b.append(QByteArray(8, '\0'));               // granule position
    b.append(QByteArray::fromHex("01000000"));   // serial number
    b.append(QByteArray(4, '\0'));               // page sequence
    b.append(QByteArray(4, '\0'));               // checksum（本测试不校验）
    b.append(char(0x01));                        // 1 个 segment
    b.append(char(0x00));                        // segment 长度 0
    return b;
}

std::vector<AnalyzerCase> AllCases() {
    return {
        {"Asf",  [](const std::string& p, model::ContainerStructureResult& r) {
             return videoeye::AsfStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::ASF, "ASF", AsfMinimal(), true},
        {"Avi",  [](const std::string& p, model::ContainerStructureResult& r) {
             return videoeye::AviStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::AVI, "AVI", AviMinimal(), true},
        {"Flv",  [](const std::string& p, model::ContainerStructureResult& r) {
             return videoeye::FlvStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::FLV, "FLV", FlvMinimal(), true},
        {"Ogg",  [](const std::string& p, model::ContainerStructureResult& r) {
             return videoeye::OggStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::OGG, "OGG", OggMinimal(), false},
        {"Ts",   [](const std::string& p, model::ContainerStructureResult& r) {
             return videoeye::TsStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::MPEG_TS, "MPEG-TS", TsMinimal(), true},
    };
}

std::vector<AnalyzerCase> MagicCheckingCases() {
    std::vector<AnalyzerCase> out;
    for (const auto& c : AllCases()) {
        if (c.rejects_foreign_magic) out.push_back(c);
    }
    return out;
}

auto CaseName = [](const testing::TestParamInfo<AnalyzerCase>& info) {
    return std::string(info.param.name);
};

// 把一个字节串写成临时文件，返回路径（失败返回空串）
// 返回 std::string 而不是 QString：样本落盘后交给的是已经不碰 Qt 的容器分析器，
// 边界转换就落在这一行，调用点不必再各自 toStdString()。
std::string WriteTemp(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return std::string();
    f.write(bytes);
    f.close();
    return path.toStdString();
}

// ---- 1. 文件不存在 --------------------------------------------------------
class MissingFileTest : public testing::TestWithParam<AnalyzerCase> {};

TEST_P(MissingFileTest, ReportsOpenFailureWithoutTouchingResult) {
    const AnalyzerCase& c = GetParam();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    model::ContainerStructureResult result;
    const bool ok = c.run(dir.filePath("does_not_exist.dat").toStdString(), result);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_message, "无法打开文件");
    EXPECT_TRUE(result.element_tree.empty());
}

INSTANTIATE_TEST_SUITE_P(AllAnalyzers, MissingFileTest,
                         testing::ValuesIn(AllCases()), CaseName);

// ---- 2. 魔数对不上 --------------------------------------------------------
class ForeignMagicTest : public testing::TestWithParam<AnalyzerCase> {};

TEST_P(ForeignMagicTest, RejectsNonMatchingSignature) {
    const AnalyzerCase& c = GetParam();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // 64 字节的定长垃圾：既不是任何一家的魔数，也足够长到能走完各自的头部读取。
    const std::string path = WriteTemp(dir, "garbage.dat", QByteArray(64, char(0xAB)));
    ASSERT_FALSE(path.empty());

    model::ContainerStructureResult result;
    const bool ok = c.run(path, result);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error_message.empty()) << "拒绝文件时必须给出可读的错误说明";
    EXPECT_TRUE(result.element_tree.empty());
}

INSTANTIATE_TEST_SUITE_P(MagicCheckingAnalyzers, ForeignMagicTest,
                         testing::ValuesIn(MagicCheckingCases()), CaseName);

// ---- 3. 最小合法文件 ------------------------------------------------------
class MinimalValidTest : public testing::TestWithParam<AnalyzerCase> {};

TEST_P(MinimalValidTest, AcceptsAndFillsCoreFields) {
    const AnalyzerCase& c = GetParam();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const std::string path = WriteTemp(dir, "minimal.dat", c.minimal_valid);
    ASSERT_FALSE(path.empty());

    model::ContainerStructureResult result;
    const bool ok = c.run(path, result);

    EXPECT_TRUE(ok) << "错误说明: " << result.error_message;
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, c.format);
    EXPECT_EQ(result.format_name, c.format_name);
    EXPECT_FALSE(result.element_tree.empty()) << "至少要挂上根节点";
    EXPECT_FALSE(result.summary.empty());
    EXPECT_TRUE(result.error_message.empty());
}

INSTANTIATE_TEST_SUITE_P(AllAnalyzers, MinimalValidTest,
                         testing::ValuesIn(AllCases()), CaseName);

// OGG 是唯一不校验魔数的分析器：它会把任意字节当成"待重新同步的页流"，
// 扫不到 OggS 就当 0 页正常返回。这是刻意的容错（Ogg 允许前置垃圾数据），
// 不是 bug —— 单独立一个用例把这个差异固化下来，免得有人"顺手修好"它。
TEST(OggAnalyzerLeniency, AcceptsArbitraryBytesByDesign) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = WriteTemp(dir, "not_ogg.dat", QByteArray(64, char(0xAB)));
    ASSERT_FALSE(path.empty());

    model::ContainerStructureResult result;
    EXPECT_TRUE(videoeye::OggStructureAnalyzer().Analyze(path, result));
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, model::ContainerFormat::OGG);
}

// ===========================================================================
// MPEG-TS 深度解析：包长自适应 / 失步重同步 / 描述符 / SDT / 连续计数
//
// 样本全部在内存里按 ISO/IEC 13818-1 的字段宽度手工拼装，不依赖真实媒体文件，
// 但每一段的位宽都严格对照规范 —— 所以它能钉住"解析器读到第几个字节"这类错误，
// 而不是像"喂一段真实文件看它不崩"那样只能测出崩溃。
// ===========================================================================

// 一个 188 字节 TS 包（afc=1 仅载荷）。载荷之后填 0xFF：
// PSI 解析按 section_length 走，尾部填充不该被当成新段 —— 这条假设本身也在这里钉住。
QByteArray TsPacket(uint16_t pid, bool pusi, int cc, const QByteArray& payload) {
    QByteArray pkt(188, '\0');
    pkt[0] = char(0x47);
    pkt[1] = char((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    pkt[2] = char(pid & 0xFF);
    pkt[3] = char(0x10 | (cc & 0x0F));
    const int n = std::min<int>(static_cast<int>(payload.size()), 184);
    for (int i = 0; i < n; ++i) pkt[4 + i] = payload[i];
    for (int i = 4 + n; i < 188; ++i) pkt[i] = char(0xFF);
    return pkt;
}

// PSI 载荷：pointer_field(1) + section
QByteArray PsiPayload(const QByteArray& section) {
    QByteArray p;
    p.append(char(0x00));
    p.append(section);
    return p;
}

// PAT：一个节目。section_length = 固定头 5 + 条目 4 + CRC 4
QByteArray PatSection(uint16_t program, uint16_t pmt_pid) {
    const int len = 5 + 4 + 4;
    QByteArray s;
    s.append(char(0x00));                       // table_id
    s.append(char(0xB0 | ((len >> 8) & 0x0F)));
    s.append(char(len & 0xFF));
    s.append(QByteArray::fromHex("0001"));      // transport_stream_id
    s.append(char(0xC1));                       // version=0 / current_next=1
    s.append(char(0x00));                       // section_number
    s.append(char(0x00));                       // last_section_number
    s.append(char((program >> 8) & 0xFF));
    s.append(char(program & 0xFF));
    s.append(char(0xE0 | ((pmt_pid >> 8) & 0x1F)));
    s.append(char(pmt_pid & 0xFF));
    s.append(QByteArray(4, '\0'));              // CRC（本解析器不校验）
    return s;
}

// PMT：两条 ES —— H.264(0x1B) 与"0x06 + registration AC-3 + lang eng"
QByteArray PmtSection(uint16_t program, uint16_t pcr_pid) {
    QByteArray es;
    // ES #1: H.264, pid=0x1000, 无描述符
    es.append(char(0x1B));
    es.append(char(0xE0 | ((0x1000 >> 8) & 0x1F)));
    es.append(char(0x00));
    es.append(char(0x00));   // es_info_length 高 4 位
    es.append(char(0x00));   // es_info_length 低 8 位
    // ES #2: 私有数据 0x06 + 描述符
    QByteArray desc;
    desc.append(char(0x0A));                    // ISO_639_language_descriptor
    desc.append(char(0x04));
    desc.append("eng", 3);
    desc.append(char(0x00));                    // audio_type
    desc.append(char(0x05));                    // registration_descriptor
    desc.append(char(0x04));
    desc.append("AC-3", 4);
    desc.append(char(0x6A));                    // AC-3_descriptor
    desc.append(char(0x01));
    desc.append(char(0x00));
    es.append(char(0x06));
    es.append(char(0xE0 | ((0x1001 >> 8) & 0x1F)));
    es.append(char(0x1001 & 0xFF));
    es.append(char((desc.size() >> 8) & 0x0F));
    es.append(char(desc.size() & 0xFF));
    es.append(desc);

    const int len = 9 + static_cast<int>(es.size()) + 4;
    QByteArray s;
    s.append(char(0x02));                       // table_id
    s.append(char(0xB0 | ((len >> 8) & 0x0F)));
    s.append(char(len & 0xFF));
    s.append(char((program >> 8) & 0xFF));
    s.append(char(program & 0xFF));
    s.append(char(0xC1));
    s.append(char(0x00));
    s.append(char(0x00));
    s.append(char(0xE0 | ((pcr_pid >> 8) & 0x1F)));
    s.append(char(pcr_pid & 0xFF));
    s.append(char(0xF0));                       // program_info_length 高 4 位
    s.append(char(0x00));
    s.append(es);
    s.append(QByteArray(4, '\0'));
    return s;
}

// SDT（table_id 0x42）：一个 service + service_descriptor
QByteArray SdtSection(uint16_t service_id, const char* name, const char* provider) {
    const QByteArray name_b(name);
    const QByteArray provider_b(provider);

    QByteArray desc;
    desc.append(char(0x48));                    // service_descriptor
    desc.append(char(1 + 1 + provider_b.size() + 1 + name_b.size()));
    desc.append(char(0x01));                    // service_type = 数字电视
    desc.append(char(provider_b.size()));
    desc.append(provider_b);
    desc.append(char(name_b.size()));
    desc.append(name_b);

    const int len = 8 + 5 + static_cast<int>(desc.size()) + 4;
    QByteArray s;
    s.append(char(0x42));
    s.append(char(0xB0 | ((len >> 8) & 0x0F)));
    s.append(char(len & 0xFF));
    s.append(QByteArray::fromHex("0001"));      // transport_stream_id
    s.append(char(0xC1));
    s.append(char(0x00));
    s.append(char(0x00));
    s.append(QByteArray::fromHex("0001"));      // original_network_id
    s.append(char(0xFF));                       // reserved
    s.append(char((service_id >> 8) & 0xFF));
    s.append(char(service_id & 0xFF));
    // reserved(6) + EIT_schedule_flag(1) + EIT_present_following_flag(1)
    s.append(char(0xFC));
    // running_status=4（running）(3) + free_CA_mode(1) + descriptors_loop_length 高 4 位
    s.append(char(0x80 | ((desc.size() >> 8) & 0x0F)));
    s.append(char(desc.size() & 0xFF));
    s.append(desc);
    s.append(QByteArray(4, '\0'));
    return s;
}

// M2TS/AVCHD：192 字节包 = 4 字节 TP_extra_header + 188 字节 TS 包
QByteArray WrapAs192(const QByteArray& ts188) {
    QByteArray out;
    out.append(QByteArray(4, '\0'));
    out.append(ts188);
    return out;
}

TEST(TsStructureDepthTest, Detects192BytePacketLayout) {
    QByteArray ts;
    for (int i = 0; i < 4; ++i) {
        ts.append(WrapAs192(TsPacket(0x0000, true, i, PsiPayload(PatSection(1, 0x0100)))));
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = WriteTemp(dir, "sample.m2ts", ts);

    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result)) << result.error_message;
    // 以前一律按 188 硬编码：这里会直接报"未找到 TS 同步字节"
    EXPECT_EQ(result.metadata["包长"], "192 字节");
    EXPECT_EQ(result.metadata["同步偏移"], "4");
    EXPECT_EQ(result.metadata["节目数"], "1");
}

TEST(TsStructureDepthTest, ParsesPatPmtDescriptorsAndSdt) {
    QByteArray ts;
    ts.append(TsPacket(0x0000, true, 0, PsiPayload(PatSection(1, 0x0100))));
    ts.append(TsPacket(0x0100, true, 0, PsiPayload(PmtSection(1, 0x1000))));
    ts.append(TsPacket(0x0011, true, 0, PsiPayload(SdtSection(1, "CCTV-1 HD", "CCTV"))));
    // 一条视频 PES 头（00 00 01 E0 ...），只为确认 PES 采样通路没被打断
    ts.append(TsPacket(0x1000, true, 0, QByteArray::fromHex("000001e000000f0")));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = WriteTemp(dir, "sample.ts", ts);

    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result)) << result.error_message;

    EXPECT_EQ(result.metadata["节目数"], "1");
    EXPECT_EQ(result.metadata["SDT 服务数"], "1");
    ASSERT_EQ(result.streams.size(), 2u);
    EXPECT_EQ(result.streams[0].type, "video");
    EXPECT_NE(result.streams[0].codec.find("H.264"), std::string::npos);
    // stream_type 0x06 本身只表示"私有数据"，必须是 registration_descriptor 让它变成 AC-3
    EXPECT_EQ(result.streams[1].type, "audio");
    EXPECT_NE(result.streams[1].codec.find("AC-3"), std::string::npos);
    EXPECT_NE(result.streams[1].details.find("lang=eng"), std::string::npos);
    EXPECT_NE(result.streams[1].details.find("reg=AC-3"), std::string::npos);

    // 节目名要挂到 Program 节点上（SDT → PAT 的 program_number 关联）
    bool node_has_service_name = false;
    const auto& root = result.element_tree[0];
    for (const auto& child : root.children) {
        if (child.name.find("CCTV-1 HD") != std::string::npos) node_has_service_name = true;
    }
    EXPECT_TRUE(node_has_service_name);
}

TEST(TsStructureDepthTest, ResyncsAfterSyncLoss) {
    QByteArray ts;
    ts.append(TsPacket(0x0000, true, 0, PsiPayload(PatSection(1, 0x0100))));
    ts.append(TsPacket(0x0100, true, 0, PsiPayload(PmtSection(1, 0x1000))));
    // 传输错误 / 拼接缝：一段不对齐的垃圾字节
    ts.append(QByteArray(32, char(0xAB)));
    ts.append(TsPacket(0x1000, true, 0, QByteArray(20, '\0')));
    ts.append(TsPacket(0x1001, true, 0, QByteArray(20, '\0')));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = WriteTemp(dir, "broken.ts", ts);

    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result)) << result.error_message;
    // 以前遇到第一个非 0x47 字节就 break：这里只会扫到 2 个包
    EXPECT_EQ(result.metadata["失步重同步次数"], "1");
    EXPECT_EQ(result.metadata["已扫描包数"], "4");
    EXPECT_EQ(result.metadata["节目数"], "1");
}

TEST(TsStructureDepthTest, CountsContinuityCounterErrors) {
    QByteArray ts;
    ts.append(TsPacket(0x0000, true, 0, PsiPayload(PatSection(1, 0x0100))));
    ts.append(TsPacket(0x0000, true, 1, PsiPayload(PatSection(1, 0x0100))));
    // cc 从 1 直接跳到 3：丢了一个包，记一次 CC 错误
    ts.append(TsPacket(0x0000, true, 3, PsiPayload(PatSection(1, 0x0100))));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = WriteTemp(dir, "cc.ts", ts);

    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result)) << result.error_message;
    EXPECT_EQ(result.metadata["连续计数错误"], "1");
}

TEST(TsStructureDepthTest, ReportsScanTruncation) {
    QByteArray ts;
    for (int i = 0; i < 8; ++i) {
        ts.append(TsPacket(0x0000, true, i, PsiPayload(PatSection(1, 0x0100))));
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = WriteTemp(dir, "long.ts", ts);

    // 只给 3 个包的预算：结论必须明说"没扫完"，不能静悄悄当成全文件结论
    TsStructureOptions options;
    options.max_scan_packets = 3;
    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result, nullptr, options))
        << result.error_message;
    EXPECT_EQ(result.metadata["已扫描包数"], "3");
    EXPECT_NE(result.metadata["扫描是否完整"].find("否"), std::string::npos);
}

// ==========================================================================
// 真实封装样本（A1 / A5）
//
// 上面所有用例的样本都是逐字节拼出来的。它们能锁住"畸形输入不崩"和"手工构造的
// PSI 表算得对"，但锁不住一件事：**真实文件上算出来的数是不是对的**。
// 手工 PSI 只有我们放进去的那几个包，而真实 TS 里还有 PCR、stuffing、PES 分片、
// 跨包的 section —— 连续计数、PCR 间隔、节目/流计数这些只有在真文件上才有意义。
//
// 样本由 tests/corpus/generate_real_corpus.py 用 ffmpeg 生成并入库；
// generate_real_corpus.py --check（ctest: RealCorpusCheck）负责盯住它们不漂移。
// ==========================================================================

namespace {

const char* kContainerCorpusDir = VIDEOEYE_CONTAINER_CORPUS_DIR;

// 样本缺失时返回空串（GTEST_SKIP 不能在非 void 函数里用），由调用点决定跳过。
// 它是 ffmpeg 生成的产物，没生成过的机器上不该 FAIL；生成过的机器上样本随仓库
// 一起走，这里就是必跑的。
std::string RealSample(const QString& name) {
    const QString path = QDir(kContainerCorpusDir).filePath(name);
    if (!QFileInfo::exists(path)) return std::string();
    return path.toStdString();
}

void SkipIfMissing(const std::string& path, const char* name) {
    if (path.empty()) {
        GTEST_SKIP() << "缺真实样本 " << name
                     << "，先跑 tests/corpus/generate_real_corpus.py";
    }
}

}  // namespace

TEST(RealContainerSampleTest, TsPacketSizeAndScanRangeMatchTheFile) {
    const std::string path = RealSample("real_h264_aac.ts");
    SkipIfMissing(path, "real_h264_aac.ts");

    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result)) << result.error_message;
    EXPECT_EQ(result.format, model::ContainerFormat::MPEG_TS);
    EXPECT_TRUE(result.valid);

    // 34028 / 188 = 181 —— 步长与扫描范围必须和文件本身对得上，
    // "扫了多少包"是后面所有传输层结论的分母，错了会连锁骗人。
    const model::TransportStreamInfo& ts = result.transport;
    EXPECT_TRUE(ts.analyzed);
    EXPECT_EQ(ts.packet_size, 188);
    EXPECT_EQ(ts.scanned_packets, 181);
    EXPECT_EQ(ts.estimated_packets, 181);
    EXPECT_TRUE(ts.scan_complete);
}

TEST(RealContainerSampleTest, TsProgramAndStreamTablesAreParsed) {
    const std::string path = RealSample("real_h264_aac.ts");
    SkipIfMissing(path, "real_h264_aac.ts");

    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result)) << result.error_message;

    // ffmpeg 出的是单节目 + 一路视频 + 一路音频：PAT/PMT 都得有，ES 数是 2。
    // 手工样本永远测不到"真实 PMT 里混着描述符"的情况。
    const model::TransportStreamInfo& ts = result.transport;
    EXPECT_TRUE(ts.has_pat);
    EXPECT_TRUE(ts.has_pmt);
    EXPECT_EQ(ts.programs, 1);
    EXPECT_EQ(ts.streams, 2);

    // 干净文件：不该有失步，也不该有连续计数错误。
    EXPECT_EQ(ts.sync_loss, 0);
    EXPECT_EQ(ts.continuity_errors, 0);
    EXPECT_EQ(ts.scrambled_packets, 0);
    // PCR 必须采得到，否则"PCR 间隔"类规则永远拿不到数。
    EXPECT_GT(ts.pcr_max_gap_ms, 0.0);
}

TEST(RealContainerSampleTest, M2tsUses192ByteStride) {
    const std::string path = RealSample("real_h264_aac.m2ts");
    SkipIfMissing(path, "real_h264_aac.m2ts");

    model::ContainerStructureResult result;
    ASSERT_TRUE(TsStructureAnalyzer().Analyze(path, result)) << result.error_message;

    // M2TS 每个包前面多 4 字节 TP_extra_header。这份样本是从同一份 .ts 派生的
    // （generate_real_corpus.py 里插的 4 字节），所以内容一致、只有步长不同 ——
    // 任何差异都只可能来自 192 步长处理本身。34752 / 192 = 181。
    //
    // 这条同时是"末尾包被丢"的回归落点：扫描循环以前一律要求读满一个步长
    // （ReadRaw 短读即失败），而最后一包后面已经没有下一包的 TP_extra 头，
    // 只剩 188 字节 —— 于是 181 个包只数出 180 个，且 scan_complete 照样报真。
    // 手工样本永远撞不到这条：它们都能被步长整除。
    const model::TransportStreamInfo& ts = result.transport;
    EXPECT_TRUE(ts.analyzed);
    EXPECT_EQ(ts.packet_size, 192);
    EXPECT_EQ(ts.scanned_packets, 181);
    EXPECT_TRUE(ts.has_pat);
    EXPECT_EQ(ts.sync_loss, 0);
}

TEST(RealContainerSampleTest, RealAviHasIndexAndStreams) {
    const std::string path = RealSample("real_mpeg4_mp3.avi");
    SkipIfMissing(path, "real_mpeg4_mp3.avi");

    model::ContainerStructureResult result;
    ASSERT_TRUE(AviStructureAnalyzer().Analyze(path, result)) << result.error_message;
    EXPECT_EQ(result.format, model::ContainerFormat::AVI);
    EXPECT_TRUE(result.valid);
    // 真实 AVI 一定有 hdrl / movi / idx1，最小合法样本（"RIFF"+0+"AVI "）没有。
    EXPECT_FALSE(result.element_tree.empty());
    EXPECT_FALSE(result.streams.empty());
    EXPECT_FALSE(result.summary.empty());
}

TEST(RealContainerSampleTest, RealOggHasPageChain) {
    const std::string path = RealSample("real_vorbis.ogg");
    SkipIfMissing(path, "real_vorbis.ogg");

    model::ContainerStructureResult result;
    ASSERT_TRUE(OggStructureAnalyzer().Analyze(path, result)) << result.error_message;
    EXPECT_EQ(result.format, model::ContainerFormat::OGG);
    EXPECT_TRUE(result.valid);
    EXPECT_FALSE(result.element_tree.empty());
    EXPECT_FALSE(result.summary.empty());
}

}  // namespace
