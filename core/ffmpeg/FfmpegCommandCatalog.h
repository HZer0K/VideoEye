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

namespace videoeye {
namespace ffmpegtool {

enum class FfmpegEntryCategory {
    Global,           // 全局
    InputOutput,      // 输入输出
    Time,             // 时间与裁剪
    StreamSelection,  // 流选择
    Encoding,         // 编码
    Video,            // 画面
    Audio,            // 音频
    Muxing,           // 封装
};

enum class FfmpegEntryKind {
    Option,  // 命令行选项（-crf）
    Filter,  // 滤镜（scale），只能出现在 -vf / -af / -filter_complex 里
};

// 参数值该拿什么清单去校验。None 表示不需要校验。
enum class FfmpegValueKind {
    None,
    Encoder,  // 值是编码器名（libx264 / aac ...），查 `-encoders`
    Filter,   // 值是滤镜名
    Format,   // 值是容器/封装格式名（mp4 / matroska ...），查 `-formats`
};

struct FfmpegCatalogEntry {
    QString name;            // "-crf"；滤镜没有前导 '-'
    QString title;           // "视频质量 (CRF)" —— 解释区流程里显示的名字
    FfmpegEntryCategory category = FfmpegEntryCategory::Encoding;
    FfmpegEntryKind kind = FfmpegEntryKind::Option;
    bool takes_value = false;
    FfmpegValueKind value_kind = FfmpegValueKind::None;
    QString summary;         // 一句话作用
    QString detail;          // 详细说明
    QString position;        // 适用位置
    QString example;         // 示例片段
    QString insert_text;     // 「插入」按钮往命令行里写的东西
    QStringList typical_values;
    QStringList related;
};

/// 当前 ffmpeg 实际支持的编码器 / 滤镜 / 格式。
/// 页面启动时跑一次 `-encoders` / `-filters` / `-formats` 填进来；没查过之前
/// HasXxx() 一律返回 true（不冤枉任何一个参数）。
class FfmpegCapabilityCache {
public:
    static FfmpegCapabilityCache& Instance();

    void SetEncoders(const QStringList& names);
    void SetFilters(const QStringList& names);
    void SetFormats(const QStringList& names);

    bool HasEncoder(const QString& name) const;
    bool HasFilter(const QString& name) const;
    bool HasFormat(const QString& name) const;

    bool encoders_known() const { return encoders_known_; }
    bool filters_known() const { return filters_known_; }
    bool formats_known() const { return formats_known_; }

    int encoder_count() const { return encoders_.size(); }
    int filter_count() const { return filters_.size(); }

    void Clear();

private:
    FfmpegCapabilityCache() = default;
    QStringList encoders_;
    QStringList filters_;
    QStringList formats_;
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

    /// 分类显示名
    static QString CategoryName(FfmpegEntryCategory category);
    static QStringList CategoryNames();

    /// 关键词搜索（匹配名字、标题、说明）；category_name 为空表示不限分类
    static std::vector<const FfmpegCatalogEntry*> Search(const QString& keyword,
                                                         const QString& category_name = QString());

    // ---- `ffmpeg -encoders` / `-filters` / `-formats` 输出解析 ----
    static QStringList ParseEncoderNames(const QString& output);
    static QStringList ParseFilterNames(const QString& output);
    static QStringList ParseMuxerNames(const QString& output);
};

}  // namespace ffmpegtool
}  // namespace videoeye
