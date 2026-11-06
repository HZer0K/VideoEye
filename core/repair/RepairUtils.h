#pragma once

// 修复模块内部的小工具：时间格式、运行 id、载荷哈希、路径判断。
// 纯 std / filesystem：不 include FFmpeg / Qt，方便单测与其它层复用。

#include <cstddef>
#include <cstdint>
#include <string>

namespace videoeye {
namespace repair {

std::string FormatIso8601Now();
std::string FormatLocalRunId();

// FNV-1a 64 位累计哈希，用来证明"输入 / 输出视频包载荷一致"。
// 它不是加密摘要，目的只是发现改动 —— 碰撞概率对"同一次修复的前后比对"足够低。
uint64_t Fnv1a64Update(uint64_t state, const std::uint8_t* data, std::size_t size);
uint64_t Fnv1a64Begin();

std::string FormatHex64(uint64_t value);

std::string LowerExtension(const std::string& path);
std::string FileStem(const std::string& path);
std::string FileName(const std::string& path);

// 两个路径是否指向同一个文件（任一方不存在时按字符串规范化比较）。
bool SamePath(const std::string& a, const std::string& b);

// ---- R01：路径别名 ----
//
// SamePath 只在两边都存在时用 equivalent；任何一个产物路径此时还不存在，
// 于是 .part 源文件与 final_path + ".part" 会被当成两个不同路径。
// PathAliasKey 把"绝对路径 + 父目录符号链接解析 + Windows 大小写折叠"做成稳定键，
// 让尚不存在的产物也能和已存在的源文件比对。
std::string PathAliasKey(const std::string& path);
bool PathsAlias(const std::string& a, const std::string& b);

// 独占创建一个空文件（O_CREAT|O_EXCL）。已存在时返回 false，绝不截断。
// 用途：在写任何字节之前把临时产物路径预留下来，避免 TOCTOU 与误删用户文件。
bool CreateExclusiveFile(const std::string& path);

// ---- R08：内容摘要 ----
//
// 只比大小挡不住"同大小改写"。这里取内容摘要，并如实写明覆盖范围：
// 大文件只取头尾 + 长度，是弱证据，不能当作全文件摘要。
std::string ComputeContentHash(const std::string& path, std::string* method, uint64_t* hashed_bytes);

std::string FormatSeconds(double seconds);

// ---- R07：逐轨守恒 ----
// status: conserved / loss / excess / unmapped
std::string EvaluateTrackConservation(std::uint64_t expected_samples, std::uint64_t packets_written);
// relation: one_to_one / split / merged_or_lost / unknown
std::string ConservationRelation(std::uint64_t expected_samples, std::uint64_t packets_written);

// ---- R03：参考段兼容 ----
struct RepairCompatibilityVerdict {
    // verified / unverified / rejected
    std::string status;
    std::string reason;
};

// source_timescale：从源分片 sidx 独立读到的时基（0 = 没有证据）。
// reference_timescale：参考初始化段 mdhd 的时基（0 = 未解析到）。
// source_bitstream：源分片逐 NAL 实测的码流类型（h264 / hevc / unknown）。
// reference_codecs：参考段 stsd 里声明的 sample entry 列表（如 "avc1,mp4a"）。
RepairCompatibilityVerdict EvaluateReferenceCompatibility(std::uint32_t source_timescale,
                                                          std::uint32_t reference_timescale,
                                                          const std::string& source_bitstream,
                                                          const std::string& reference_codecs);

// 源侧样本数 vs 实际写入包数的守恒判定。
//
// ⚠️ 这是遗留的「全轨合计 + 5% 容差」粗判据，只能作为辅助信号：
// 一条大轨道会把小轨道整轨丢失掩盖掉（1000/951 曾经被判成"无需报告"）。
// 完整性结论必须走 EvaluateTrackConservation 逐轨比对，引擎不再调用本函数。
bool SampleLossExceedsTolerance(std::uint64_t expected_samples, std::uint64_t packets_written);

} // namespace repair
} // namespace videoeye
