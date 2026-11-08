// 结构分析失败原因的语义守护
//
// 背景：ContainerStructureAnalyzer 的 FFmpeg 回退以前把**所有**打开失败压成一句
// 「FFmpeg 无法打开文件」，而 QcRunner::IsAnalyzableFile 早就把「路径为空 / 不存在 /
// 目录 / 文件为空」分开了。两条独立入口各说各话，用户在结构页上分不清是素材坏了、
// 路径写错了，还是这个格式不支持。
//
// 这一组用例把「失败类别」钉成机器可读的事实（不是从 error_message 反推），
// 并守住守恒契约：valid=true -> None，valid=false -> 非 None 且错误说明非空。
// 离线语料门禁 scripts/corpus_defect_gate.py 消费的就是这些稳定 ASCII 键。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/domain/io/MediaInputPreflight.h"

namespace fs = std::filesystem;

namespace {

using videoeye::model::ContainerFailureKind;
using videoeye::model::ContainerStructureResult;

fs::path ScratchDir(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir =
        fs::temp_directory_path() / (std::string("videoeye_failure_kind_") + tag + "_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string CorpusPath(const char* relative) {
    return std::string(VIDEOEYE_CORPUS_DIR) + "/" + relative;
}

// 全 0 在任何 FFmpeg demuxer 上都探测失败（见 test_qc_runner.cpp 的说明）。
// ⚠️ 扩展名必须用 .bin：FormatDetector 对 .mp4 有扩展名兜底，全 0 的 .mp4
//    会被 Mp4BoxAnalyzer 容错成「1 个空 box」并返回成功，根本走不到 FFmpeg 打开。
fs::path WriteGarbage(const fs::path& dir) {
    const fs::path file = dir / "garbage.bin";
    std::ofstream out(file, std::ios::binary);
    const std::string bytes(4096, '\0');
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return file;
}

void ExpectFailure(const std::string& path, ContainerFailureKind expected, const std::string& text_token) {
    videoeye::ContainerStructureAnalyzer analyzer;
    ContainerStructureResult result;
    EXPECT_FALSE(analyzer.Analyze(path, result)) << path;
    EXPECT_FALSE(result.valid) << path;
    EXPECT_EQ(result.failure_kind, expected) << path;
    EXPECT_NE(result.error_message.find(text_token), std::string::npos)
        << "错误说明应包含 " << text_token << "，实际: " << result.error_message;
}

} // namespace

TEST(ContainerFailureKindTest, EmptyPathHasItsOwnKind) {
    ExpectFailure("", ContainerFailureKind::EmptyPath, "路径为空");
}

TEST(ContainerFailureKindTest, MissingFileHasItsOwnKind) {
    const fs::path dir = ScratchDir("missing");
    ExpectFailure((dir / "not-there.mp4").string(), ContainerFailureKind::NotFound, "文件不存在");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(ContainerFailureKindTest, DirectoryHasItsOwnKind) {
    const fs::path dir = ScratchDir("directory");
    ExpectFailure(dir.string(), ContainerFailureKind::IsDirectory, "目录");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(ContainerFailureKindTest, ZeroByteFileHasItsOwnKind) {
    const fs::path dir = ScratchDir("empty");
    const fs::path file = dir / "zero.bin";
    std::ofstream(file).close();
    ExpectFailure(file.string(), ContainerFailureKind::EmptyFile, "文件为空");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(ContainerFailureKindTest, UnsupportedContentIsOpenFailedWithFfmpegDetail) {
    // 纯垃圾内容不是「文件级错误」：文件存在、非空、不是目录，
    // 失败发生在 FFmpeg 探测阶段，且必须带 av_strerror 的具体原因。
    const fs::path dir = ScratchDir("garbage");
    const std::string path = WriteGarbage(dir).string();
    videoeye::ContainerStructureAnalyzer analyzer;
    ContainerStructureResult result;
    EXPECT_FALSE(analyzer.Analyze(path, result));
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.failure_kind, ContainerFailureKind::OpenFailed);
    EXPECT_NE(result.error_message.find("FFmpeg 无法打开文件"), std::string::npos);
    EXPECT_GT(result.error_message.size(), std::string("FFmpeg 无法打开文件").size())
        << "必须附 av_strerror 细节，不能只留笼统一句";
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(ContainerFailureKindTest, ValidMediaYieldsNoneAndNoError) {
    videoeye::ContainerStructureAnalyzer analyzer;
    ContainerStructureResult result;
    const std::string path = CorpusPath("container/real_h264_aac_faststart.mp4");
    ASSERT_TRUE(analyzer.Analyze(path, result)) << result.error_message;
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.failure_kind, ContainerFailureKind::None);
    EXPECT_TRUE(result.error_message.empty());
}

TEST(ContainerFailureKindTest, CancelledYieldsCancelled) {
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    videoeye::ContainerStructureAnalyzer analyzer;
    ContainerStructureResult result;
    EXPECT_FALSE(analyzer.Analyze(CorpusPath("container/real_h264_aac_faststart.mp4"), result, cancel));
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.failure_kind, ContainerFailureKind::Cancelled);
    EXPECT_EQ(result.error_message, "已取消");
}

TEST(ContainerFailureKindTest, ReusedResultClearsPreviousFailureKind) {
    // 调用方复用同一个 result 时，上一次的失败类别不能残留到成功结果上。
    const fs::path dir = ScratchDir("reuse");
    videoeye::ContainerStructureAnalyzer analyzer;
    ContainerStructureResult result;
    ASSERT_FALSE(analyzer.Analyze("", result));
    ASSERT_EQ(result.failure_kind, ContainerFailureKind::EmptyPath);
    ASSERT_TRUE(analyzer.Analyze(CorpusPath("container/real_h264_aac_faststart.mp4"), result)) << result.error_message;
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.failure_kind, ContainerFailureKind::None);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(ContainerFailureKindTest, StableAsciiKeysAreFrozen) {
    // 离线门禁按这些键分组；改名会让门禁静默失灵，必须在这里钉死。
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::None), "none");
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::EmptyPath), "empty_path");
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::NotFound), "not_found");
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::IsDirectory), "is_directory");
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::EmptyFile), "empty_file");
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::OpenFailed), "open_failed");
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::Cancelled), "cancelled");
    EXPECT_STREQ(videoeye::model::ContainerFailureKindName(ContainerFailureKind::ParseFailed), "parse_failed");
}

TEST(ContainerFailureKindTest, PreflightMappingIsTotalAndOkIsNone) {
    // 预检的每一种失败都必须映射到确定的类别，不能落回 None。
    const videoeye::InputPreflightKind kinds[] = {
        videoeye::InputPreflightKind::EmptyPath,
        videoeye::InputPreflightKind::NotFound,
        videoeye::InputPreflightKind::IsDirectory,
        videoeye::InputPreflightKind::EmptyFile,
    };
    for (auto kind : kinds) {
        EXPECT_NE(videoeye::model::ContainerFailureFromPreflight(kind), ContainerFailureKind::None)
            << static_cast<int>(kind);
    }
    EXPECT_EQ(videoeye::model::ContainerFailureFromPreflight(videoeye::InputPreflightKind::Ok),
              ContainerFailureKind::None);

    // 同一份预检事实：文案与类别都由它给出，QcRunner 与结构分析共用。
    const auto preflight = videoeye::PreflightLocalFile("");
    EXPECT_FALSE(preflight.ok());
    EXPECT_EQ(preflight.reason, "路径为空");
    EXPECT_STREQ(videoeye::InputPreflightKindName(preflight.kind), "empty_path");
}
