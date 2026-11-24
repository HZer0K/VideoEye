#pragma once

// FFmpeg 指令字典（内置数据）。
//
// 只收录**高频**参数，目标是"写一条常见转码命令时够用"，不是把 ffmpeg 全部文档搬进来。
// 每条记录回答四件事: 这个参数干什么（summary/detail）、该放在命令的哪个位置
// （position —— ffmpeg 的参数是**位置敏感**的，放错位置等于没写）、怎么用（example）、
// 和谁一起用（related）。
//
// 关于"哪些编码器/过滤器可用": 内置数据**不回答这个问题**。同一份 ffmpeg 在
// Windows 与 Linux 上编进来的编码器可能完全不同，写死的清单只会骗人。实际清单由
// FfmpegCapabilityCache 承载，页面启动时用 `ffmpeg -encoders` / `-filters` 查出来填进去。

#include <QString>
#include <QStringList>
#include <vector>

#include "core/ffmpeg/FfmpegToolKind.h"

namespace videoeye {
namespace ffmpeg {

enum class FfmpegEntryCategory {
    Global,          // 全局
    InputOutput,     // 输入输出
    Time,            // 时间与裁剪
    StreamSelection, // 流选择
    Encoding,        // 编码
    Video,           // 画面
    Audio,           // 音频
    Muxing,          // 封装
    ProbeOutput,     // 信息输出（ffprobe 专有：-show_streams / -of …）
    Playback,        // 播放控制（ffplay 专有：-autoexit / -x / -nodisp …）
};

enum class FfmpegEntryKind {
    Option, // 命令行选项（-crf）
    Filter, // 滤镜（scale），只能出现在 -vf / -af / -filter_complex 里
};

// 参数值该拿什么清单去校验。None 表示不需要校验。
enum class FfmpegValueKind {
    None,
    Encoder, // 值是编码器名（libx264 / aac ...），查 `-encoders`
    Filter,  // 值是滤镜名
    Format,  // 值是容器/封装格式名（mp4 / matroska ...），查 `-formats`
};

struct FfmpegCatalogEntry {
    QString name;  // "-crf"；滤镜没有前导 '-'
    QString title; // "视频质量 (CRF)" —— 解释区流程里显示的名字
    FfmpegEntryCategory category = FfmpegEntryCategory::Encoding;
    FfmpegEntryKind kind = FfmpegEntryKind::Option;
    bool takes_value = false;
    FfmpegValueKind value_kind = FfmpegValueKind::None;
    QString summary;     // 一句话作用
    QString detail;      // 详细说明
    QString position;    // 适用位置
    QString example;     // 示例片段
    QString insert_text; // 「插入」按钮往命令行里写的东西
    QStringList typical_values;
    QStringList related;

    /// 这个选项的值**可以以 '-' 开头**。命令行解析靠"值不以 '-' 开头"来区分
    /// 「选项的值」与「下一个选项」；但对 -map / -map_metadata 这类选项，
    /// `-map -0:d`（排除第 0 个输入里的数据流）、`-map_metadata -1`（清掉元数据）
    /// 都是合法写法。少了这个标记，负号值会被当成选项，连带把它后面的输出目标
    /// 吞成"该选项的值"，最后误报"没有输出文件"。
    /// 放在最后：词条表是位置初始化，新增字段追加在末尾才不会挪动已有词条。
    bool value_may_start_with_dash = false;

    /// 这个选项归哪些工具（FfmpegToolMask 位掩码）。
    /// **0（kToolNone）= 没显式标注**：由 FfmpegCommandCatalog 按"通用选项表 + 默认 ffmpeg"
    /// 推导。做成"不标就走推导"而不是"每条都标"，是因为词条表是**位置初始化**：
    /// 66 条已有条目逐个补字段，只要有一条对不齐就会静默错位，而错位的后果是解释文案串行。
    /// 新加的 ffprobe / ffplay 专有词条才显式写自己的工具。
    /// 同样放在最后，理由同上。
    unsigned tools = kToolNone;
};

// `ffmpeg -formats` 的两张清单。**必须分开存**：D 是"能读"（解复用 / demuxer），
// E 是"能写"（复用 / muxer）。`-f xxx` 到底是读还是写，取决于它落在输入侧还是
// 输出侧 —— 拿一张表互相替代就会在 `-f lavfi`（只有 D）、`-f ffmetadata`（只有 E）
// 这类组合上误报"这个 ffmpeg 不支持 xxx"。
struct FfmpegFormatLists {
    QStringList demuxers; // 能读
    QStringList muxers;   // 能写
};

/// 当前 ffmpeg 实际支持的编码器 / 滤镜 / 格式。
/// 页面启动时跑一次 `-encoders` / `-filters` / `-formats` 填进来；没查过之前
/// HasXxx() 一律返回 true（不冤枉任何一个参数）。
class FfmpegCapabilityCache {
public:
    static FfmpegCapabilityCache& Instance();

    void SetEncoders(const QStringList& names);
    void SetFilters(const QStringList& names);
    void SetFormats(const QStringList& demuxers, const QStringList& muxers);

    bool HasEncoder(const QString& name) const;
    bool HasFilter(const QString& name) const;
    /// 不区分方向：能读或能写都算有这个格式（用于无法确定方向时的保守判断）
    bool HasFormat(const QString& name) const;
    bool HasDemuxer(const QString& name) const;
    bool HasMuxer(const QString& name) const;

    bool encoders_known() const {
        return encoders_known_;
    }
    bool filters_known() const {
        return filters_known_;
    }
    bool formats_known() const {
        return formats_known_;
    }

    int encoder_count() const {
        return encoders_.size();
    }
    int filter_count() const {
        return filters_.size();
    }
    int demuxer_count() const {
        return demuxers_.size();
    }
    int muxer_count() const {
        return muxers_.size();
    }

    /// 完整清单，供**输入补全**列候选用（"能写成什么样"）。
    /// 判断"这份 ffmpeg 到底支不支持某个值"仍然只走 HasXxx —— 两者语义不同：
    /// 清单里有不代表该参数在这个位置可用，Has 里也没有"方向"信息。
    const QStringList& Encoders() const {
        return encoders_;
    }
    const QStringList& Filters() const {
        return filters_;
    }
    const QStringList& Demuxers() const {
        return demuxers_;
    }
    const QStringList& Muxers() const {
        return muxers_;
    }

    void Clear();

private:
    FfmpegCapabilityCache() = default;
    QStringList encoders_;
    QStringList filters_;
    QStringList demuxers_;
    QStringList muxers_;
    bool encoders_known_ = false;
    bool filters_known_ = false;
    bool formats_known_ = false;
};

class FfmpegCommandCatalog {
public:
    /// 全部词条（顺序: 选项在前，滤镜在后）
    static const std::vector<FfmpegCatalogEntry>& Entries();

    /// 按名字精确查找（"-crf" / "scale"）
    static const FfmpegCatalogEntry* Find(const QString& name);

    /// 按工具查找：只返回 tools 掩码含该工具的条目。kind == Unknown 时返回全部
    /// （"+ 省略程序名"的写法在工具定下来之前不该丢掉任何候选）。
    static std::vector<const FfmpegCatalogEntry*> EntriesFor(FfmpegToolKind kind);

    /// 按名字精确查找，且这条必须归该工具。
    /// 找不到 / 名字属于别的工具都返回 nullptr —— 调用方据此说"这个参数 ffprobe 没有"。
    /// kind == Unknown 时等价于 Find(name)。
    static const FfmpegCatalogEntry* Find(const QString& name, FfmpegToolKind kind);

    /// 这条词条能不能被该工具使用（掩码判据的唯一出口）。
    static bool IsAvailableTo(const FfmpegCatalogEntry& entry, FfmpegToolKind kind);

    /// 分类显示名
    static QString CategoryName(FfmpegEntryCategory category);
    static QStringList CategoryNames();

    /// 关键词搜索（匹配名字、标题、说明）；category_name 为空表示不限分类
    static std::vector<const FfmpegCatalogEntry*> Search(const QString& keyword,
                                                         const QString& category_name = QString());

    // ---- `ffmpeg -encoders` / `-filters` / `-formats` 输出解析 ----
    static QStringList ParseEncoderNames(const QString& output);
    static QStringList ParseFilterNames(const QString& output);
    /// 一次拆出 demuxer / muxer 两张表（`-formats` 每行同时给出两个方向）
    static FfmpegFormatLists ParseFormatNames(const QString& output);
    static QStringList ParseDemuxerNames(const QString& output);
    static QStringList ParseMuxerNames(const QString& output);
};

} // namespace ffmpeg
} // namespace videoeye
