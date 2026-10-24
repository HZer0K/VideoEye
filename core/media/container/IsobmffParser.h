#pragma once

// ISOBMFF (MP4 / MOV / fMP4) 轻量解析器。
//
// 存在的理由: 原来这部分由 Bento4 承担，但它作为源码集成的第三方库，
// 让构建依赖树里多了一整套 SDK（以及配套的 cmake 子项目、patch 维护成本）。
// 本项目真正需要的只有两件事：box 树展示 + sample table 交叉校验，
// 自研实现约 600 行，且完全可控（表项上限、深度上限都能按需要收紧）。
//
// 依赖边界: 纯 C++17 + 标准库，不碰 Qt / FFmpeg / Bento4，便于单测。

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace videoeye {

// ---- box 树 ----
struct IsobmffBox {
    std::string type;         // 4CC，如 "moov" / "trak" / "stts"
    uint64_t size = 0;        // box 总大小（含 header）
    uint64_t offset = 0;      // 文件内偏移
    uint32_t header_size = 0; // 8 / 16 / 24（含 largesize / uuid）
    int depth = 0;
    std::vector<IsobmffBox> children;

    // 语义字段（规范字段名 → 可读值），按 box 类型逐个填。
    //
    // 为什么放在解析层而不是展示层：以前只有样本表那 8 类 box 有字段
    // （见 analysis/container/Mp4BoxAnalyzer 的 FillTableFields），
    // 于是 ftyp 的 brand、mvhd 的 timescale、avcC/hvcC 的 profile、colr 的
    // 色彩参数在树里全是空节点 —— 而这些恰好是"打开一个 MP4 第一眼想看"的东西。
    // 现在字段跟着 box 解析走，展示层只负责显示，不再需要自己判 type。
    //
    // 只填"规范里有名字、且能从 box 载荷直接读出来"的字段；样本表这类
    // 可能上万条的数据仍然走 IsbmffTrack 的结构化表，不进这里。
    std::vector<std::pair<std::string, std::string>> fields;
};

// ---- sample table 条目 ----
struct SttsEntry {
    uint32_t sample_count = 0;
    uint32_t sample_delta = 0;
};

struct CttsEntry {
    uint32_t sample_count = 0;
    int32_t sample_offset = 0; // v1 可为负
};

struct StscEntry {
    uint32_t first_chunk = 0;
    uint32_t samples_per_chunk = 0;
    uint32_t sample_description_index = 0;
};

struct ElstEntry {
    uint64_t segment_duration = 0;  // 电影时基
    int64_t media_time = 0;         // 媒体时基；-1 = 空编辑
    int32_t media_rate_integer = 1; // 16.16 定点的整数部分
};

// stsz / stz2 统一表示
struct StszTable {
    uint32_t default_size = 0; // sample_size != 0 时的统一样本大小
    uint32_t sample_count = 0;
    uint32_t field_size = 0;     // stz2: 4/8/16
    std::vector<uint32_t> sizes; // default_size == 0 时逐样本大小
};

// ---- 一个 trak 的全部信息 ----
struct IsobmffTrack {
    uint32_t track_id = 0;
    std::string handler; // 原始 4CC: "vide" / "soun" / "hint" / "text" ...
    std::string codec;   // stsd 首个 entry 的 4CC: "avc1" / "mp4a" ...
    // stsd 里声明的全部 sample entry（按声明顺序）。绝大多数文件只有 1 条，
    // 但 Dolby Vision 双层、多码率自适应等会把多条描述放进同一个 stsd；
    // track.codec 仍然只表示第一条（兼容旧调用点），完整列表看这里。
    std::vector<std::string> sample_descriptions;
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
    std::vector<uint32_t> stss; // 1-based 样本序号
    StszTable stsz;
    std::vector<StscEntry> stsc;
    std::vector<uint64_t> chunk_offsets; // stco 与 co64 统一成 uint64
    std::vector<ElstEntry> elst;

    // 表被截断时置位（超大文件保护，不代表文件有问题）
    bool tables_truncated = false;

    // 便捷换算
    std::string TypeName() const; // "vide" -> "video"
    uint32_t SttsSampleCount() const;
    uint32_t CttsSampleCount() const;
};

// ---- fMP4 分片 ----
struct IsobmffFragment {
    uint32_t moof_index = 0;
    uint32_t index = 0;
    uint32_t sequence_number = 0; // mfhd
    uint32_t track_id = 0;
    uint64_t offset = 0; // moof 在文件内的偏移
    uint64_t size = 0;   // moof 大小
    bool has_tfdt = false;
    uint64_t base_media_decode_time = 0;

    uint32_t sample_count = 0;
    uint64_t duration = 0; // 媒体时基
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

    // ---- 首样本随机访问标志（实证层）----
    //
    // 为什么要单独记：以前判断"这个分片能不能独立解码"只看有没有 tfdt，
    // 但 tfdt 只是**解码起点时间**，和"首样本是不是随机访问点"是两件事。
    // 真正的依据在 trun/tfhd 的 sample_flags 里（ISO/IEC 14496-12 §8.8.8）：
    //   bit 16 (0x00010000) = sample_is_non_sync_sample
    //   bit 24-25          = sample_depends_on（2 = I 帧）
    // 三个来源的优先级与缺省关系（trun 里最具体的优先）：
    //   trun.first_sample_flags  >  首样本自己的 sample_flags  >  tfhd.default_sample_flags
    // 三者都没有时 has_first_sample_flags 保持 false —— 上层必须据此报"无法证实"，
    // 绝不能退回"假定是关键帧"。
    bool has_first_sample_flags = false;
    uint32_t first_sample_flags = 0;
    bool first_sample_is_sync = false; // (flags & 0x00010000) == 0
    int first_sample_depends_on = -1;  // 0=unknown 1=depends 2=independent(-1 未给出)
    bool tfhd_default_sample_flags_present = false;
    uint32_t tfhd_default_sample_flags = 0;
    // tfhd 里没有 default flags 时，CMAF 的默认值来自 init 段的 trex。
    // 本解析器不读 trex（跨文件），所以只能置位让上层知道"缺省值可能来自 trex"。
    bool first_sample_flags_incomplete = false;
};

// ---- 解析结果 ----
struct IsobmffFile {
    bool ok = false;
    std::string error_message;
    uint64_t file_size = 0;

    std::vector<IsobmffBox> top_level;
    std::vector<std::string> top_level_order;

    uint32_t movie_timescale = 0; // mvhd
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

// 解析选项。
//
// ⚠️ 必须定义在 namespace 作用域，不能嵌套进 IsobmffParser：GCC 会把
// 「带默认成员初始化器(NSDMI)的成员」的解析推迟到**最外层**外围类结束处，
// 于是类内默认实参 `Options{}` 用不到这些初始化器，报
// "default member initializer ... required before the end of its enclosing class"。
// （MSVC 不做这个延迟，所以这套写法在 Windows 上一直能编过。）
struct IsobmffOptions {
    // 单个样本表最多解析多少条（防止 GB 级文件把内存吃光）
    uint32_t max_entries_per_table = 500000;
    // 递归深度上限，防御构造出"无限嵌套"的恶意 box
    int max_depth = 8;
    // 是否只扫顶层 + 容器结构，不解析样本表（更快）
    bool skip_sample_tables = false;
    // 可选取消标志（nullptr = 不关心取消）。box 级扫描是 GB 级文件里最耗时的一段，
    // 切换媒体/关闭窗口置位后必须能当场停手，否则旧任务会继续和新任务抢 CPU 与磁盘。
    const std::atomic<bool>* cancel = nullptr;
};

class IsobmffParser {
public:
    // 兼容既有的 IsobmffParser::Options 写法
    using Options = IsobmffOptions;

    static bool Parse(const std::string& file_path, IsobmffFile& out, const Options& options = Options{});
};

} // namespace videoeye
