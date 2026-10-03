// 五个容器结构分析器的参数化健壮性测试
//
// Asf / Avi / Flv / Ogg / Ts 五个 StructureAnalyzer 的接口完全一致：
//     bool Analyze(const QString& file_path, model::ContainerStructureResult& result);
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

#include <functional>
#include <string>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/analysis/container/AsfStructureAnalyzer.h"
#include "core/analysis/container/AviStructureAnalyzer.h"
#include "core/analysis/container/FlvStructureAnalyzer.h"
#include "core/analysis/container/OggStructureAnalyzer.h"
#include "core/analysis/container/TsStructureAnalyzer.h"
#include "core/domain/model/ContainerStructureInfo.h"

using namespace videoeye;

namespace {

using AnalyzeFn = std::function<bool(const QString&, model::ContainerStructureResult&)>;

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
        {"Asf",  [](const QString& p, model::ContainerStructureResult& r) {
             return analyzer::AsfStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::ASF, "ASF", AsfMinimal(), true},
        {"Avi",  [](const QString& p, model::ContainerStructureResult& r) {
             return analyzer::AviStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::AVI, "AVI", AviMinimal(), true},
        {"Flv",  [](const QString& p, model::ContainerStructureResult& r) {
             return analyzer::FlvStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::FLV, "FLV", FlvMinimal(), true},
        {"Ogg",  [](const QString& p, model::ContainerStructureResult& r) {
             return analyzer::OggStructureAnalyzer().Analyze(p, r);
         }, model::ContainerFormat::OGG, "OGG", OggMinimal(), false},
        {"Ts",   [](const QString& p, model::ContainerStructureResult& r) {
             return analyzer::TsStructureAnalyzer().Analyze(p, r);
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
QString WriteTemp(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(bytes);
    f.close();
    return path;
}

// ---- 1. 文件不存在 --------------------------------------------------------
class MissingFileTest : public testing::TestWithParam<AnalyzerCase> {};

TEST_P(MissingFileTest, ReportsOpenFailureWithoutTouchingResult) {
    const AnalyzerCase& c = GetParam();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    model::ContainerStructureResult result;
    const bool ok = c.run(dir.filePath("does_not_exist.dat"), result);

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
    const QString path = WriteTemp(dir, "garbage.dat", QByteArray(64, char(0xAB)));
    ASSERT_FALSE(path.isEmpty());

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

    const QString path = WriteTemp(dir, "minimal.dat", c.minimal_valid);
    ASSERT_FALSE(path.isEmpty());

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
    const QString path = WriteTemp(dir, "not_ogg.dat", QByteArray(64, char(0xAB)));
    ASSERT_FALSE(path.isEmpty());

    model::ContainerStructureResult result;
    EXPECT_TRUE(analyzer::OggStructureAnalyzer().Analyze(path, result));
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, model::ContainerFormat::OGG);
}

}  // namespace
