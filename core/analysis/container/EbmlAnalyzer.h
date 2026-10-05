#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/analysis/detail/SeqFileReader.h"

#include "core/domain/model/EbmlInfo.h"

namespace videoeye {

/// EBML/MKV/WebM 结构分析器
/// 按 MKV 规范深度解析 EBML 元素树、Track 表、Cues 索引、Block 二进制格式
class EbmlAnalyzer {
public:
    // 解析上限：畸形 / 超大文件不能把解析线程拖死。
    //   kMaxDepth   正常 MKV 最深也就七八层（Segment / Tracks / TrackEntry / Video / Colour …），
    //               64 层足够挡住"元素自己套自己"的递归炸弹。
    //   kMaxNodes   真实大文件节点数在万级，20 万对普通文件绰绰有余，
    //               对"故意喂一个巨大/无限的文件"则是硬约束（到顶就停解析）。
    static constexpr int kMaxDepth = 64;
    static constexpr int kMaxNodes = 200000;

    EbmlAnalyzer();
    ~EbmlAnalyzer();

    EbmlAnalyzer(const EbmlAnalyzer&) = delete;
    EbmlAnalyzer& operator=(const EbmlAnalyzer&) = delete;

    // cancel: 可选的取消标志（nullptr = 不关心取消）。MKV 的 Cluster 数随片长线性增长，
    // 解析中途被取消必须当场停，否则切了媒体旧的解析还在后台刷盘。
    bool Analyze(const std::string& filePath, model::EbmlAnalysisResult& result,
                 const std::atomic<bool>* cancel = nullptr);
    void Reset();

private:
    // --- 二进制 IO ---
    // 读游标是**引用**：调用方（ParseElement）持有它，ReadVInt 读完就把字节流往前推。
    // 以前这里是 std::istream&，那是 QDataStream 时代的写法（ds.status() / ds.device()
    // 全是 QDataStream 的 API，换成 std::ifstream 后一个都不存在）。整份 EBML 头部
    // 很小，用"字节缓冲 + 游标"和 Ts/Flv/Asf/Avi 几层保持一致。
    uint64_t ReadVInt(const std::string& buf, int64_t& pos, int& size_out) const;

    // EBML 的 "unknown size"：size 字段全 1（7 字节），表示长度未知。
    // 这种元素的实际内容一直延伸到父元素末尾，不能再当 size 用。
    static constexpr uint64_t kUnknownSize = 0xFFFFFFFFFFFFFFULL;
    
    // --- 元素解析 ---
    bool ParseElement(const std::string& buf, int64_t& pos, int64_t end_offset, int depth,
                      model::EbmlElementNode* parent,
                      model::EbmlAnalysisResult& result,
                      const std::atomic<bool>* cancel = nullptr);

    /// 解析叶子元素值，同时提取关键数据 (如 DocType, TimestampScale 等)
    void ParseLeafValue(model::EbmlElementNode& node, const std::string& data,
                        model::EbmlAnalysisResult& result);

    /// 深度解析 Block 二进制格式
    /// @param data  Block 的原始数据
    /// @param result 填充 EbmlBlockSummary
    static std::string ParseBlockData(const std::string& data, model::EbmlBlockSummary& summary);

    /// 深度解析 SimpleBlock 二进制格式 (比 Block 多 TrackNumber+Timecode+Flags 头部)
    static std::string ParseSimpleBlockData(const std::string& data, model::EbmlBlockSummary& summary);

    /// 解析 TrackEntry 子树 → 填充 result.tracks
    void ExtractTrackInfo(const model::EbmlElementNode& track_entry,
                          model::EbmlAnalysisResult& result);

    /// 解析一个 CuePoint 子树 → 填充 result.cues
    void ExtractCueInfo(const model::EbmlElementNode& cue_point,
                        model::EbmlAnalysisResult& result);

    // --- 辅助 ---
    static std::string ElementName(uint64_t id);
    static std::string CodecIdToName(const std::string& codec_id);
    static std::string TrackTypeName(int type);
    static std::map<uint64_t, std::string>& ElementNames();

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

    // 当前正在解析的 Cluster 的元素起始偏移 (供 Block/SimpleBlock 记录归属 Cluster)
    uint64_t current_cluster_offset_ = 0;

    // 已解析出的元素个数（Analyze() 开头重置），到 kMaxNodes 就停解析
    uint64_t node_count_ = 0;
};

} // namespace videoeye
