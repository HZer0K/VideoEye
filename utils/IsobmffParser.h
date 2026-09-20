#pragma once

// ISOBMFF (MP4 / MOV / fMP4) 轻量解析器。
//
// 存在的理由: 原来这部分由 Bento4 承担，但它作为源码集成的第三方库，
// 让构建依赖树里多了一整套 SDK（以及配套的 cmake 子项目、patch 维护成本）。
// 本项目真正需要的只有两件事：box 树展示 + sample table 交叉校验，
// 自研实现约 600 行，且完全可控（表项上限、深度上限都能按需要收紧）。
//
// 依赖边界: 纯 C++17 + 标准库，不碰 Qt / FFmpeg / Bento4，便于单测。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace utils {

// ---- box 树 ----
struct IsobmffBox {
    std::string type;          // 4CC，如 "moov" / "trak" / "stts"
    uint64_t size = 0;         // box 总大小（含 header）
    uint64_t offset = 0;       // 文件内偏移
    uint32_t header_size = 0;  // 8 / 16 / 24（含 largesize / uuid）
    int depth = 0;
    std::vector<IsobmffBox> children;
};

// ---- sample table 条目 ----
struct SttsEntry {
    uint32_t sample_count = 0;
    uint32_t sample_delta = 0;
};

struct CttsEntry {
    uint32_t sample_count = 0;
    int32_t sample_offset = 0;   // v1 可为负
};

struct StscEntry {
    uint32_t first_chunk = 0;
    uint32_t samples_per_chunk = 0;
    uint32_t sample_description_index = 0;
};

struct ElstEntry {
    uint64_t segment_duration = 0;   // 电影时基
    int64_t media_time = 0;          // 媒体时基；-1 = 空编辑
    int32_t media_rate_integer = 1;  // 16.16 定点的整数部分
};

// stsz / stz2 统一表示
struct StszTable {
    uint32_t default_size = 0;   // sample_size != 0 时的统一样本大小
    uint32_t sample_count = 0;
    uint32_t field_size = 0;     // stz2: 4/8/16
    std::vector<uint32_t> sizes; // default_size == 0 时逐样本大小
};

// ---- 一个 trak 的全部信息 ----
struct IsobmffTrack {
    uint32_t track_id = 0;
    std::string handler;          // 原始 4CC: "vide" / "soun" / "hint" / "text" ...
    std::string codec;            // stsd 首个 entry 的 4CC: "avc1" / "mp4a" ...
    uint32_t media_timescale = 0; // mdhd
    uint64_t media_duration = 0;  // mdhd（媒体时基）

    bool has_stts = false;
    bool has_ctts = false;
    bool has_stss = false;
    bool has_stsz = false;
    bool has_stz2 = false;
    bool has_stsc = false;
    bool has_stco = false;
    bool has_co64 = false;
    bool has_elst = false;

    std::vector<SttsEntry> stts;
    std::vector<CttsEntry> ctts;
    std::vector<uint32_t> stss;          // 1-based 样本序号
    StszTable stsz;
    std::vector<StscEntry> stsc;
    std::vector<uint64_t> chunk_offsets; // stco 与 co64 统一成 uint64
    std::vector<ElstEntry> elst;

    // 表被截断时置位（超大文件保护，不代表文件有问题）
    bool tables_truncated = false;

    // 便捷换算
    std::string TypeName() const;        // "vide" -> "video"
    uint32_t SttsSampleCount() const;
    uint32_t CttsSampleCount() const;
};

// ---- fMP4 分片 ----
struct IsobmffFragment {
    uint32_t moof_index = 0;
    uint32_t index = 0;
    uint32_t sequence_number = 0;   // mfhd
    uint32_t track_id = 0;
    uint64_t offset = 0;            // moof 在文件内的偏移
    uint64_t size = 0;              // moof 大小
    bool has_tfdt = false;
    uint64_t base_media_decode_time = 0;

    uint32_t sample_count = 0;
    uint64_t duration = 0;          // 媒体时基
    uint64_t total_size = 0;

    bool base_data_offset_present = false;
    bool default_base_is_moof = false;
    bool sample_description_index_present = false;
    bool default_sample_duration_present = false;
    bool default_sample_size_present = false;
    bool duration_is_empty = false;
    uint64_t base_data_offset = 0;

    uint32_t trun_count = 0;
    bool trun_data_offset_present = false;
    int64_t trun_data_offset = 0;
};

// ---- 解析结果 ----
struct IsobmffFile {
    bool ok = false;
    std::string error_message;
    uint64_t file_size = 0;

    std::vector<IsobmffBox> top_level;
    std::vector<std::string> top_level_order;

    uint32_t movie_timescale = 0;   // mvhd
    std::vector<IsobmffTrack> tracks;
    std::vector<IsobmffFragment> fragments;

    uint32_t sidx_count = 0;
    uint32_t styp_count = 0;
    bool fragmented = false;
    bool moov_before_mdat = false;
    uint64_t moov_offset = 0;
    uint64_t moov_size = 0;
    uint64_t first_mdat_offset = 0;
    uint64_t first_mdat_size = 0;
    bool found_moov = false;
    bool found_mdat = false;
};

class IsobmffParser {
public:
    struct Options {
        // 单个样本表最多解析多少条（防止 GB 级文件把内存吃光）
        uint32_t max_entries_per_table = 500000;
        // 递归深度上限，防御构造出"无限嵌套"的恶意 box
        int max_depth = 8;
        // 是否只扫顶层 + 容器结构，不解析样本表（更快）
        bool skip_sample_tables = false;
    };

    static bool Parse(const std::string& file_path, IsobmffFile& out,
                      const Options& options = Options{});
};

}  // namespace utils
}  // namespace videoeye
