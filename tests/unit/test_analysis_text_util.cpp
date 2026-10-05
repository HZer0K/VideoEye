// analysis 层去 Qt 之后留下的三件过渡形态，钉住它们的语义，别让"替身"自己漂移：
//
//   1) detail/AnalysisTextUtil.h —— Qt 字符串 API 的 std 替身（StrCat / Fixed /
//      HexFill / TrimCopy ...）。MediaInfoAnalyzer 整篇 .cpp 就是靠它从 .arg()
//      链改写过来的，拼错一字要打开一个媒体文件才看得见；
//   2) detail/BytesOrder.h —— qFromBigEndian 的逐字节替身；
//   3) detail/SeqFileReader.h —— QFile 的 1:1 std 替身。
//
// 这一整条用例都不碰 Qt：它跑在 analysis 已经不再依赖 Qt6::Core 的前提上，
// 顺带当一道回归 —— 哪天有人在 analysis 里把 QString 塞回来，头几个编译就会炸。
//
// 命名沿用 tests/unit 的既有约定：文件名 = 被测单元，gtest 用例数进
// scripts/summarize_tests.py 的统计。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/BytesOrder.h"
#include "core/analysis/detail/SeqFileReader.h"

#include <gtest/gtest.h>

namespace {

using videoeye::BytesToHex;
using videoeye::FileExtension;
using videoeye::Fixed;
using videoeye::HexFill;
using videoeye::HexFillLow;
using videoeye::HexToBytes;
using videoeye::RemoveAllCopy;
using videoeye::StartsWith;
using videoeye::StrCat;
using videoeye::ToLowerCopy;
using videoeye::ToUpperCopy;
using videoeye::TrimCopy;

// ---------------------------------------------------------------------------
// StrCat：QString("%1%2").arg(x).arg(y) 的唯一归宿
// ---------------------------------------------------------------------------

TEST(AnalysisTextUtilTest, StrCatSubstitutesPositionalPlaceholdersInOrder) {
    EXPECT_EQ(StrCat("%1", 7), "7");
    EXPECT_EQ(StrCat("(%1x%2)", 1920, 1080), "(1920x1080)");
    // %N 是「按出现顺序」的占位符：第 N 个出现的 %N 吃第 N 个实参。
    // 想要换序得换实参顺序，指望 "%3 %1 %2" 重排是拿不到的（头文件里写死了这条）。
    EXPECT_EQ(StrCat("%3 %1 %2", 1, 2, 3), "1 2 3");
    EXPECT_EQ(StrCat("%1 %2 %3", 3, 1, 2), "3 1 2");
    EXPECT_EQ(StrCat("a%1b%2c%3d", "X", "Y", "Z"), "aXbYcZd");
    EXPECT_EQ(StrCat("v=%1", 3.5), "v=3.5");
}

TEST(AnalysisTextUtilTest, StrCatKeepsLiteralOnlyFormatsIntact) {
    EXPECT_EQ(StrCat("no args here"), "no args here");
    EXPECT_EQ(StrCat(""), "");
    // 一个占位符都没有却带了实参：实参被吃掉，不会漏在串尾
    EXPECT_EQ(StrCat("plain", 5), "plain");
}

TEST(AnalysisTextUtilTest, StrCatHandlesPercentEscapesWithoutEatingAnArgument) {
    // Qt 的 arg() 只认 "%N" 这个词面，`%%` 是字面百分号、后面不跟实参。
    EXPECT_EQ(StrCat("100%%"), "100%");
    EXPECT_EQ(StrCat("a%%b"), "a%b");
    // 转义符在占位符之前：%% 仍不吃实参，后面的 %1 吃
    EXPECT_EQ(StrCat("pct=%1%%", 5), "pct=5%");
}

TEST(AnalysisTextUtilTest, StrCatMatchesTheManualConcatenationsItReplaced) {
    // MediaInfoAnalyzer 里这几处是被脚本机械改过来的，拼错一字就是
    // "找不到裸流 demuxererrbuf" 这种串 —— 只有打开一个 .pcm 才看得见。
    const std::string demuxer = "pcm_s16le";
    EXPECT_EQ(StrCat("找不到裸流 demuxer: %1", demuxer), std::string("找不到裸流 demuxer: pcm_s16le"));
    const std::string long_name = "pcm_s24le";
    EXPECT_EQ(StrCat(" (%1)", long_name), std::string(" (pcm_s24le)"));
    EXPECT_EQ(StrCat(" - %1", long_name), std::string(" - pcm_s24le"));
}

// ---------------------------------------------------------------------------
// 数值 / 十六进制格式化
// ---------------------------------------------------------------------------

TEST(AnalysisTextUtilTest, FixedMatchesQStringNumberWithFEncoding) {
    // QString::number(v, 'f', n) 的等价物：定宽小数、不切科学计数法。
    EXPECT_EQ(Fixed(3.14159, 2), "3.14");
    EXPECT_EQ(Fixed(0.5, 3), "0.500");
    EXPECT_EQ(Fixed(123.0, 0), "123");
    EXPECT_EQ(Fixed(-1.25, 2), "-1.25");
    // 大数也不能被 ostream 默认精度截成 6 位有效数字
    EXPECT_EQ(Fixed(1234567.891, 2), "1234567.89");
}

TEST(AnalysisTextUtilTest, HexFillHonoursWidthAndCase) {
    // QString::arg(v, width, 16, QChar('0'))：定宽、不足补零。
    EXPECT_EQ(HexFillLow(0x1, 8), "00000001");
    EXPECT_EQ(HexFillLow(0xabcdef, 4), "abcdef");
    // 定值超过宽度时不截断，与 Qt 一致
    EXPECT_EQ(HexFillLow(0x12345678, 4), "12345678");
    EXPECT_EQ(HexFill(0xabcdef, 6), "ABCDEF");
    EXPECT_EQ(HexFill(0x1, 2), "01");
}

// ---------------------------------------------------------------------------
// 字符串变换
// ---------------------------------------------------------------------------

TEST(AnalysisTextUtilTest, HexAndBytesRoundTrip) {
    EXPECT_EQ(HexToBytes("48656c6c6f"), "Hello");
    EXPECT_EQ(BytesToHex(std::string("Hello")), "48656c6c6f");
    // 大写输入要认（容器里的 magic 就是大写写的）
    EXPECT_EQ(HexToBytes("DEADBEEF"), std::string("\xDE\xAD\xBE\xEF", 4));
    // 非 hex 字符忽略、下划线跳过、奇数长度丢掉最后一个半字节 —— 全对齐 Qt
    EXPECT_EQ(HexToBytes("48 65"), "He");
    EXPECT_EQ(HexToBytes("4_8_6_5"), "He");
    EXPECT_EQ(HexToBytes("486"), "H");
    // 注意别挑含 a~f 的串当"非 hex"样例 —— 它们本来就合法
    EXPECT_EQ(HexToBytes("zzzz"), "");
    EXPECT_EQ(HexToBytes("!!!"), "");
    EXPECT_EQ(HexToBytes(""), "");
}

TEST(AnalysisTextUtilTest, TrimAndCaseCopiesDoNotMutateInput) {
    EXPECT_EQ(TrimCopy("  \t a b \n\r\f\v "), "a b");
    EXPECT_EQ(TrimCopy(""), "");
    EXPECT_EQ(TrimCopy("   "), "");
    // QString::trimmed 只吃首尾，中间的空白原样保留
    EXPECT_EQ(TrimCopy("  a  b  "), "a  b");

    const std::string src = "AbC-123_xyz";
    EXPECT_EQ(ToUpperCopy(src), "ABC-123_XYZ");
    EXPECT_EQ(ToLowerCopy(src), "abc-123_xyz");
    EXPECT_EQ(src, "AbC-123_xyz");  // 返回副本，原串不变
    // 大小写变换只动字母：容器名里的数字/连字符/下划线原样
    EXPECT_EQ(ToUpperCopy("pcm_s24le"), "PCM_S24LE");
}

TEST(AnalysisTextUtilTest, RemoveAllCopyDropsEveryOccurrence) {
    // 分析层只用来剥 UTF-16 里的结尾 NUL；Qt 的 remove 是全量删（只删第一个的叫 removeFirst）
    EXPECT_EQ(RemoveAllCopy(std::string("ab\0cd\0", 6), '\0'), "abcd");
    EXPECT_EQ(RemoveAllCopy("aaa", 'a'), "");
    EXPECT_EQ(RemoveAllCopy("bbb", 'a'), "bbb");
    EXPECT_EQ(RemoveAllCopy("", 'x'), "");
}

TEST(AnalysisTextUtilTest, StartsWithMatchesQtSemanticsIncludingTooLongPrefix) {
    EXPECT_TRUE(StartsWith("Ogg data", "Ogg"));
    EXPECT_FALSE(StartsWith("Ogg data", "ogg"));  // 大小写敏感
    EXPECT_FALSE(StartsWith("Ogg", "OggS"));      // prefix 比被查串长 → false
    EXPECT_TRUE(StartsWith("whatever", ""));
    EXPECT_TRUE(StartsWith("", ""));
    // char* 重载（OggStructureAnalyzer 用的就是它）
    EXPECT_TRUE(StartsWith("qtbrand", "qt"));
    // 中间夹着 NUL 的串要走 std::string 那条重载
    EXPECT_TRUE(StartsWith(std::string("ab\0cd", 5), "ab"));
}

TEST(AnalysisTextUtilTest, FileExtensionTakesTheLastDotAfterTheLastSlash) {
    EXPECT_EQ(FileExtension("clip.mp4"), "mp4");
    EXPECT_EQ(FileExtension("C:/media/CLIP.MKV"), "mkv");
    EXPECT_EQ(FileExtension("C:\\media\\a.b\\clip.avi"), "avi");
    // 与 QFileInfo 一致的三条边界：没有点、点在最后、点在最前面
    EXPECT_EQ(FileExtension("clip"), "");
    EXPECT_EQ(FileExtension("clip."), "");
    EXPECT_EQ(FileExtension(".hidden"), "");
    EXPECT_EQ(FileExtension("/a.b/clip"), "");
}

// ---------------------------------------------------------------------------
// BytesOrder
// ---------------------------------------------------------------------------

TEST(BytesOrderTest, LoadsBigEndianValuesByteByByte) {
    const unsigned char b16[2] = {0x12, 0x34};
    const unsigned char b24[3] = {0x01, 0x02, 0x03};
    const unsigned char b32[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    const unsigned char b64[8] = {0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};

    EXPECT_EQ(videoeye::LoadBE16(b16), 0x1234u);
    EXPECT_EQ(videoeye::LoadBE24(b24), 0x010203u);
    EXPECT_EQ(videoeye::LoadBE32(b32), 0xDEADBEEFu);
    EXPECT_EQ(videoeye::LoadBE64(b64), 0x0000000102030405ull);
}

// ---------------------------------------------------------------------------
// SeqFileReader：QFile 的 1:1 std 替身
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;

class SeqFileReaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "videoeye_seqfile_test";
        fs::create_directories(dir_);
        path_ = dir_ / "sample.bin";
        // "MAGIC!!" (7) + "\x01\x02\x03\x04" (4) = 11 字节
        std::ofstream out(path_, std::ios::binary);
        out << "MAGIC!!" << '\x01' << '\x02' << '\x03' << '\x04';
        out.close();
    }

    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
    fs::path path_;
};

TEST_F(SeqFileReaderTest, ReportsSizeAndMovesThroughReadSeekPos) {
    videoeye::SeqFileReader f(path_.string());
    ASSERT_TRUE(f.IsOpen());
    EXPECT_EQ(f.Size(), 11);
    EXPECT_EQ(f.Pos(), 0);
    EXPECT_FALSE(f.AtEnd());

    EXPECT_EQ(f.Read(4), "MAGI");
    EXPECT_EQ(f.Pos(), 4);
    EXPECT_EQ(f.Read(4), std::string("C!!", 3) + '\x01');

    ASSERT_TRUE(f.Seek(7));
    EXPECT_EQ(f.Pos(), 7);
    EXPECT_EQ(f.Read(4), std::string("\x01\x02\x03\x04", 4));
    EXPECT_TRUE(f.AtEnd());
    EXPECT_EQ(f.Pos(), f.Size());
}

TEST_F(SeqFileReaderTest, ShortReadsReturnFewerBytesRatherThanFailing) {
    videoeye::SeqFileReader f(path_.string());
    ASSERT_TRUE(f.IsOpen());

    // 越过 EOF 再读：返回空串，但状态没坏 —— 还能 seek 回去接着读
    EXPECT_TRUE(f.Seek(9));
    EXPECT_EQ(f.Read(8), "\x03\x04");
    EXPECT_TRUE(f.AtEnd());
    EXPECT_TRUE(f.Seek(0));
    EXPECT_EQ(f.Read(11), "MAGIC!!\x01\x02\x03\x04");
}

TEST_F(SeqFileReaderTest, RecoversAfterEofInsteadOfBeingStuckOnFailbit) {
    // 这是 SeqFileReader 与裸 std::ifstream 最要紧的一处差异，也是它存在的理由：
    // ifstream 撞上 EOF 之后 seekg() 会静默变成空操作（不报错也不动），
    // "读到文件尾再回头"的解析路径就永远停在半截数据上，且症状是偶发的。
    videoeye::SeqFileReader f(path_.string());
    ASSERT_TRUE(f.IsOpen());

    EXPECT_EQ(f.ReadAll(), "MAGIC!!\x01\x02\x03\x04");
    EXPECT_TRUE(f.AtEnd());
    EXPECT_TRUE(f.Seek(0));
    EXPECT_FALSE(f.AtEnd());
    EXPECT_EQ(f.Read(4), "MAGI");
}

TEST_F(SeqFileReaderTest, ReadRawFillsACallersBufferAndReportsShortReads) {
    videoeye::SeqFileReader f(path_.string());
    ASSERT_TRUE(f.IsOpen());

    char buf[8] = {0};
    ASSERT_TRUE(f.ReadRaw(buf, 8));
    EXPECT_EQ(std::string(buf, 7), "MAGIC!!");

    ASSERT_TRUE(f.Seek(7));
    char rest[4] = {0};
    EXPECT_TRUE(f.ReadRaw(rest, 4));
    EXPECT_EQ(std::string(rest, 4), std::string("\x01\x02\x03\x04", 4));

    // 只剩 2 字节了还要 4 字节 → 报短读
    EXPECT_FALSE(f.ReadRaw(rest, 4));
}

TEST_F(SeqFileReaderTest, OpenFailureLeavesTheReaderClosed) {
    videoeye::SeqFileReader f((dir_ / "definitely_absent.bin").string());
    EXPECT_FALSE(f.IsOpen());
    EXPECT_EQ(f.Size(), 0);
    EXPECT_EQ(f.ReadAll(), "");
}

}  // namespace
