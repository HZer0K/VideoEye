#include "core/repair/RepairUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/domain/io/Utf8Path.h"

namespace videoeye {
namespace repair {

std::string FormatIso8601Now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", tm_local.tm_year + 1900, tm_local.tm_mon + 1,
                  tm_local.tm_mday, tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    return std::string(buffer);
}

std::string FormatLocalRunId() {
    // 时间戳 + 4 位随机后缀：同一秒内连续发起两次修复也不会撞名。
    const std::string stamp = FormatIso8601Now();
    std::string digits;
    digits.reserve(stamp.size());
    for (char c : stamp) {
        if (c >= '0' && c <= '9')
            digits.push_back(c);
    }
    std::random_device rd;
    const unsigned suffix = rd() % 10000u;
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u", suffix);
    return digits + "-" + std::string(buffer);
}

uint64_t Fnv1a64Begin() {
    return 1469598103934665603ULL;
}

uint64_t Fnv1a64Update(uint64_t state, const std::uint8_t* data, std::size_t size) {
    if (!data)
        return state;
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (std::size_t i = 0; i < size; ++i) {
        state ^= static_cast<uint64_t>(data[i]);
        state *= kPrime;
    }
    return state;
}

std::string LowerExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size())
        return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string FileStem(const std::string& path) {
    const std::string name = FileName(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return name;
    return name.substr(0, dot);
}

std::string FileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool SamePath(const std::string& a, const std::string& b) {
    return PathsAlias(a, b);
}

std::string FormatHex64(uint64_t value) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

// 把路径折成"文件系统身份键"：绝对化 + 解析已存在部分的符号链接 +
// Windows 大小写折叠。产物路径此刻通常不存在，weakly_canonical 仍能把它的
// 父目录解析到真实位置，于是 "out/x.mp4.part" 和别名目录下的同一个文件
// 不会漏判。
std::string PathAliasKey(const std::string& path) {
    if (path.empty())
        return {};
    std::error_code ec;
    // 必须是 ToNativePath：path(std::string) 在 C locale 下对非 ASCII 字节抛
    // system_error，中文路径的"别名判定"会直接炸掉调用方。
    std::filesystem::path p = ToNativePath(path);
    if (!p.is_absolute()) {
        const std::filesystem::path abs = std::filesystem::absolute(p, ec);
        if (!ec)
            p = abs;
        ec.clear();
    }
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(p.lexically_normal(), ec);
    if (!ec)
        p = resolved;
    std::string key = p.u8string();
#if defined(_WIN32)
    // Windows 文件系统默认大小写不敏感。只折 ASCII，避免破坏 UTF-8 多字节序列。
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); });
#endif
    return key;
}

bool PathsAlias(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty())
        return false;
    std::error_code ec_a;
    std::error_code ec_b;
    const std::filesystem::path na = ToNativePath(a);
    const std::filesystem::path nb = ToNativePath(b);
    // 两边都存在时用 equivalent：能抓到硬链接 / 符号链接 / 大小写别名。
    if (std::filesystem::exists(na, ec_a) && std::filesystem::exists(nb, ec_b)) {
        std::error_code eq_ec;
        const bool equivalent = std::filesystem::equivalent(na, nb, eq_ec);
        return !eq_ec && equivalent;
    }
    return PathAliasKey(a) == PathAliasKey(b);
}

bool CreateExclusiveFile(const std::string& path) {
    if (path.empty())
        return false;
    const std::filesystem::path native = ToNativePath(path);
#if defined(_WIN32)
    const int fd = _wopen(native.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    const int fd = ::open(native.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    if (fd < 0)
        return false;
#if defined(_WIN32)
    _close(fd);
#else
    ::close(fd);
#endif
    return true;
}

std::string ComputeContentHash(const std::string& path, std::string* method, uint64_t* hashed_bytes,
                               const RepairRunBudget* budget) {
    if (method)
        *method = "unavailable";
    if (hashed_bytes)
        *hashed_bytes = 0;
    if (budget != nullptr && budget->Checkpoint())
        return {};
    std::error_code ec;
    const std::filesystem::path native = ToNativePath(path);
    const uint64_t size = std::filesystem::file_size(native, ec);
    if (ec)
        return {};
    std::ifstream in(native, std::ios::binary);
    if (!in)
        return {};

    // 大文件全量摘要会把"修复前核对"本身变成一次全盘读；超过阈值只取头尾。
    // 这不是加密摘要，也不能证明中间字节未变，方法名会把这个局限带进报告。
    constexpr uint64_t kFullHashLimit = 256ull * 1024 * 1024;
    constexpr std::size_t kChunk = 1024u * 1024u;
    constexpr uint64_t kEdgeBytes = 4ull * 1024 * 1024;
    std::vector<char> buffer(kChunk);
    uint64_t state = Fnv1a64Begin();
    const uint64_t size_be = size;
    state = Fnv1a64Update(state, reinterpret_cast<const uint8_t*>(&size_be), sizeof(size_be));

    auto hash_range = [&](uint64_t begin, uint64_t length) -> bool {
        if (length == 0)
            return true;
        in.clear();
        in.seekg(static_cast<std::streamoff>(begin), std::ios::beg);
        if (!in)
            return false;
        uint64_t left = length;
        while (left > 0) {
            // 逐块检查：256 MiB 全量摘要不打断的话，取消要等整个文件读完才生效。
            if (budget != nullptr && budget->Checkpoint())
                return false;
            const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(left, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(want));
            const std::streamsize got = in.gcount();
            if (got <= 0)
                return false;
            state =
                Fnv1a64Update(state, reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<std::size_t>(got));
            left -= static_cast<uint64_t>(got);
        }
        return true;
    };

    bool ok = false;
    uint64_t covered = 0;
    if (size <= kFullHashLimit) {
        ok = hash_range(0, size);
        covered = ok ? size : 0;
        if (method)
            *method = "fnv1a64_full";
    } else {
        ok = hash_range(0, kEdgeBytes);
        if (ok) {
            ok = hash_range(size - kEdgeBytes, kEdgeBytes);
        }
        covered = ok ? kEdgeBytes * 2 : 0;
        if (method)
            *method = "fnv1a64_head_tail";
    }
    if (!ok) {
        if (method)
            *method = "unavailable";
        return {};
    }
    if (hashed_bytes)
        *hashed_bytes = covered;
    return FormatHex64(state);
}

std::string EvaluateTrackConservation(std::uint64_t expected_samples, std::uint64_t packets_written) {
    if (expected_samples == 0)
        return "unmapped";
    if (packets_written == expected_samples)
        return "conserved";
    if (packets_written < expected_samples)
        return "loss";
    return "excess";
}

std::string ConservationRelation(std::uint64_t expected_samples, std::uint64_t packets_written) {
    if (expected_samples == 0)
        return "unknown";
    if (packets_written == expected_samples)
        return "one_to_one";
    if (packets_written > expected_samples)
        return "split";
    return "merged_or_lost";
}

bool CodecListContains(const std::string& codecs, const char* needle) {
    return codecs.find(needle) != std::string::npos;
}

// 正向确认：参考段必须真的声明了源侧逐 NAL 实测的码流家族。
// 只做「反向冲突」检查会把"参考段只有 mp4a、根本没有视频解码配置"也判成 verified。
bool CodecFamilyMatches(const std::string& source_bitstream, const std::string& reference_codecs) {
    if (source_bitstream == "h264")
        return CodecListContains(reference_codecs, "avc") || CodecListContains(reference_codecs, "264");
    if (source_bitstream == "hevc")
        return CodecListContains(reference_codecs, "hvc") || CodecListContains(reference_codecs, "hev") ||
               CodecListContains(reference_codecs, "265");
    return false;
}

RepairCompatibilityVerdict EvaluateReferenceCompatibility(std::uint32_t source_timescale,
                                                          std::uint32_t reference_timescale,
                                                          const std::string& source_bitstream,
                                                          const std::string& reference_codecs) {
    // 时基是播放速度的分母：源侧 sidx 给出了独立于参考文件的时基证据，
    // 与 mdhd 不一致就必须拒绝，否则"全部帧都解码成功、时长却变了"。
    if (source_timescale > 0 && reference_timescale > 0 && source_timescale != reference_timescale) {
        return {"rejected", "源分片 sidx 时基 " + std::to_string(source_timescale) + " 与参考段 mdhd 时基 " +
                                std::to_string(reference_timescale) +
                                " 不一致；借用该参考段会改变播放时长与速度，不能只凭 track_id 判定兼容"};
    }
    // 码流类型硬冲突：源分片逐 NAL 实测与参考段 stsd 声明对不上。
    if (source_bitstream == "h264" &&
        (CodecListContains(reference_codecs, "hvc") || CodecListContains(reference_codecs, "hev") ||
         CodecListContains(reference_codecs, "av01") || CodecListContains(reference_codecs, "vp09"))) {
        return {"rejected", "源分片逐 NAL 实测为 H.264，参考段声明的编码为 " + reference_codecs};
    }
    if (source_bitstream == "hevc" &&
        (CodecListContains(reference_codecs, "avc") || CodecListContains(reference_codecs, "av01") ||
         CodecListContains(reference_codecs, "vp09"))) {
        return {"rejected", "源分片逐 NAL 实测为 H.265，参考段声明的编码为 " + reference_codecs};
    }
    // 有独立时基证据、未冲突，且参考段正向确认了码流家族，才算"已核对"。
    const bool codec_confirmed = CodecFamilyMatches(source_bitstream, reference_codecs);
    if (source_timescale > 0 && reference_timescale > 0 && codec_confirmed) {
        return {"verified", "源侧 sidx 时基与参考段一致（" + std::to_string(reference_timescale) + "），参考段声明 [" +
                                reference_codecs + "] 与源侧逐 NAL 码流类型 " + source_bitstream + " 相符"};
    }
    if (source_timescale > 0 && reference_timescale > 0 && !codec_confirmed) {
        return {"unverified", "参考段声明 [" + reference_codecs + "] 未能正向确认源侧逐 NAL 实测的 " +
                                  source_bitstream +
                                  " 码流家族；没有反向冲突不等于解码参数同源，track_id 相同不能证明兼容"};
    }
    return {"unverified", "源侧没有足够的独立证据（sidx 时基 / in-band 码流参数）核对参考段的时基与解码参数；"
                          "track_id 相同不能证明同源，借用参数可能改变播放时间轴"};
}

std::string FormatSeconds(double seconds) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
    return std::string(buffer);
}

bool SampleLossExceedsTolerance(std::uint64_t expected_samples, std::uint64_t packets_written) {
    if (expected_samples == 0 || packets_written >= expected_samples)
        return false;
    // 写成加法而不是 expected*0.95，避免大数乘浮点、也不必引入 <cmath>。
    return packets_written + expected_samples / 20 < expected_samples;
}

} // namespace repair
} // namespace videoeye
