#pragma once

// 修复模块内部的小工具：时间格式、运行 id、载荷哈希、路径判断。
// 纯 std：不 include FFmpeg / Qt，方便单测与其它层复用。

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

std::string LowerExtension(const std::string& path);
std::string FileStem(const std::string& path);
std::string FileName(const std::string& path);

// 两个路径是否指向同一个文件（任一方不存在时按字符串规范化比较）。
bool SamePath(const std::string& a, const std::string& b);

std::string FormatSeconds(double seconds);

// 源侧声明样本数 vs 实际写入包数的守恒判定。
// 只有"缺初始化段、靠参考段拼出来"的路径才有精确源侧样本数（逐段 moof/trun 数出来）；
// 允许 5% 以内的偏差（容器可能合并/拆分访问单元），超过就判定内容缺失。
// 抽成纯函数是为了让这条"坏了看不出来"的降级判据能被单测直接钉住。
bool SampleLossExceedsTolerance(std::uint64_t expected_samples, std::uint64_t packets_written);

} // namespace repair
} // namespace videoeye
