#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/analysis/detail/SeqFileReader.h"

// 统一安全预算（深度 / 节点数等默认值集中在 core/media/detail/ParserLimits.h，
// analysis 依赖 media 是允许方向，别再各解析器各写一套魔法数字）
#include "core/media/detail/ParserLimits.h"

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
    static constexpr int kMaxDepth = ParserLimits::kMaxDepth;
    static constexpr int kMaxNodes = static_cast<int>(ParserLimits::kMaxNodes);

    // 扫描窗口：只把文件开头这一段读进内存建树。
    // 以前是 ReadAll() —— 4 GB 的 MKV 就是 4 GB 的 std::string，长片直接 OOM。
    // 32 MiB 覆盖得下 EBML 头 + Info + Tracks + 前若干 Cluster；尾部（Cues/Tags/
    // Chapters）落在窗口外时靠 scan_truncated 明确告知，而不是假装文件就这么长。
    // QC 侧的整份事实另有 MkvContainerScanner 负责（只读元素头，内存 O(1)）。
    static constexpr int64_t kMaxScanBytes = static_cast<int64_t>(ParserLimits::kMaxScanBytes);

    // 元素树里保留的块节点（SimpleBlock / BlockGroup）上限。
    // 块是唯一随片长线性增长的节点类型：3 小时的片子有十万量级，全挂进树必然撞
    // kMaxNodes，而撞上限等于整个结构页判"解析失败"回退 FFmpeg —— 长片连容器骨架
    // 都看不到。超限后块照旧计数（total_simpleblocks / total_blockgroups 仍然精确），
    // 只是不再挂树、载荷也不再拷进 node.value。
    static constexpr uint64_t kMaxTreeBlocks = 20000;

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

    // --- 元素解析 ---
    bool ParseElement(const std::string& buf, int64_t& pos, int64_t end_offset, int depth,
                      model::EbmlElementNode* parent, model::EbmlAnalysisResult& result,
                      const std::atomic<bool>* cancel = nullptr);

    /// 解析叶子元素值，同时提取关键数据 (如 DocType, TimestampScale 等)
    void ParseLeafValue(model::EbmlElementNode& node, const std::string& data, model::EbmlAnalysisResult& result);

    /// 深度解析 Block 二进制格式
    /// @param data  Block 的原始数据
    /// @param result 填充 EbmlBlockSummary
    static std::string ParseBlockData(const std::string& data, model::EbmlBlockSummary& summary);

    /// 深度解析 SimpleBlock 二进制格式 (比 Block 多 TrackNumber+Timecode+Flags 头部)
    static std::string ParseSimpleBlockData(const std::string& data, model::EbmlBlockSummary& summary);

    /// 解析 TrackEntry 子树 → 填充 result.tracks
    void ExtractTrackInfo(const model::EbmlElementNode& track_entry, model::EbmlAnalysisResult& result);

    /// 解析一个 CuePoint 子树 → 填充 result.cues
    void ExtractCueInfo(const model::EbmlElementNode& cue_point, model::EbmlAnalysisResult& result);

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

    // 块节点计数（Analyze() 开头重置）。
    // 计数在解析时记账，不再像以前那样靠遍历元素树反推 —— 块一旦因为超预算
    // 不挂树，树遍历就数不出来了，而 total_simpleblocks / total_blockgroups
    // 恰恰是结构页要的"这份文件有多少帧"的近似答案。
    uint64_t block_tree_nodes_ = 0; // 已挂进树的块节点数
    uint64_t blockgroup_count_ = 0;
    uint64_t simpleblock_count_ = 0;
    bool block_budget_hit_ = false;

    // 解析途中撞到深度/节点上限时为 true。上限触发后元素树只是"半份"，
    // Analyze() 必须据此把结果标成无效 —— 半份树 + valid=true 是最危险的
    // 失败模式（阶段 3 要求：错误状态不可继续产出"有效结果"）。
    bool limit_reached_ = false;
};

} // namespace videoeye
