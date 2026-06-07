#pragma once

#include <QString>
#include <QFile>
#include <QMap>
#include <QDataStream>
#include "core/model/EbmlInfo.h"

namespace videoeye {
namespace analyzer {

/// EBML/MKV/WebM 结构分析器
/// 按 MKV 规范深度解析 EBML 元素树、Track 表、Cues 索引、Block 二进制格式
class EbmlAnalyzer {
public:
    EbmlAnalyzer();
    ~EbmlAnalyzer();

    EbmlAnalyzer(const EbmlAnalyzer&) = delete;
    EbmlAnalyzer& operator=(const EbmlAnalyzer&) = delete;

    bool Analyze(const QString& filePath, model::EbmlAnalysisResult& result);
    void Reset();

private:
    // --- 二进制 IO ---
    uint64_t ReadVInt(QDataStream& ds, int& size_out) const;
    
    // --- 元素解析 ---
    bool ParseElement(QDataStream& ds, qint64 end_offset, int depth,
                      model::EbmlElementNode* parent,
                      model::EbmlAnalysisResult& result);

    /// 解析叶子元素值，同时提取关键数据 (如 DocType, TimestampScale 等)
    void ParseLeafValue(model::EbmlElementNode& node, const QByteArray& data,
                        model::EbmlAnalysisResult& result);

    /// 深度解析 Block 二进制格式
    /// @param data  Block 的原始数据
    /// @param result 填充 EbmlBlockSummary
    static QString ParseBlockData(const QByteArray& data, model::EbmlBlockSummary& summary);

    /// 深度解析 SimpleBlock 二进制格式 (比 Block 多 TrackNumber+Timecode+Flags 头部)
    static QString ParseSimpleBlockData(const QByteArray& data, model::EbmlBlockSummary& summary);

    /// 解析 TrackEntry 子树 → 填充 result.tracks
    void ExtractTrackInfo(const model::EbmlElementNode& track_entry,
                          model::EbmlAnalysisResult& result);

    /// 解析一个 CuePoint 子树 → 填充 result.cues
    void ExtractCueInfo(const model::EbmlElementNode& cue_point,
                        model::EbmlAnalysisResult& result);

    // --- 辅助 ---
    static QString ElementName(uint64_t id);
    static QString CodecIdToName(const QString& codec_id);
    static QString TrackTypeName(int type);
    static QMap<uint64_t, QString>& ElementNames();

    /// 判断是否为容器元素 (含子元素的复合类型)
    static bool IsContainerElement(uint64_t id);

    // 解析过程中的临时状态
    struct ParseState {
        int track_count = 0;
        int cluster_count = 0;
        int blockgroup_count = 0;
        int simpleblock_count = 0;
        int block_count = 0;
        uint64_t current_cluster_offset = 0;
        uint64_t timestamp_scale = 1000000;
    };
};

} // namespace analyzer
} // namespace videoeye
