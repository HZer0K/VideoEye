#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <QString>

#include "core/domain/model/ContainerStructureInfo.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace analyzer {

/// 统一容器结构分析调度器
/// 根据文件格式自动选择对应的解析器, 输出统一的 ContainerStructureResult
class ContainerStructureAnalyzer {
public:
    ContainerStructureAnalyzer();
    ~ContainerStructureAnalyzer();

    /// 分析文件容器结构
    /// @param cancel 可选取消标志: 非空时（1）交给 FFmpeg 的 AVIO 中断回调, 使关闭流程
    ///               （CancelAll）能及时中止阻塞 IO；（2）往下传给每种容器解析器,
    ///               在 box / element / segment / 分片级别轮询。传空则不做取消。
    ///               MP4 / MKV / AVI / FLV / TS / ASF / OGG 路径都吃这一套。
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result,
                 std::shared_ptr<std::atomic<bool>> cancel = {});

    /// 重置
    void Reset();

private:
    // 阶段终态。三态而不是 bool 是有原因的:「失败」允许回退 FFmpeg 再试一次，
    // 「取消」绝不允许 —— 取消时 result 里只有半份数据，既不能设 valid=true 发布出去，
    // 也不能再让 FFmpeg 兜底把同一份取消悄悄换成"分析成功"（FFmpeg 回退成功还会
    // 把 format 改写成 FFmpeg_Generic，用户看到的结论完全变了）。
    enum class StageStatus {
        kDone,      // 走完且产出了可用结果
        kCancelled, // 中途被取消，外层必须放弃本次结果
        kFailed,    // 走完但失败，允许回退 FFmpeg
    };

    // 下面三个"树转换"函数返回 bool：false 表示中途被取消（不再堆出半成品树），
    // 让 Analyze() 能把结果归成"已取消"，而不是把半棵树当成有效的结构树交出去。
    /// 将 Mp4BoxNode 树映射为 ContainerElement 树
    bool ConvertMp4Tree(const std::vector<model::Mp4BoxNode>& nodes, int depth,
                        std::vector<model::ContainerElement>& out,
                        const std::atomic<bool>* cancel);

    /// 将 EbmlElementNode 树映射为 ContainerElement 树
    bool ConvertEbmlTree(const std::vector<model::EbmlElementNode>& nodes, int depth,
                         std::vector<model::ContainerElement>& out,
                         const std::atomic<bool>* cancel);

    /// 从 MP4 Box 树中提取丰富的流信息
    bool ExtractMp4StreamInfo(const std::vector<model::Mp4BoxNode>& box_tree,
                              model::ContainerStructureResult& result,
                              const std::atomic<bool>* cancel);

    /// 从 EBML 树中提取丰富的流信息
    void ExtractEbmlStreamInfo(const model::EbmlAnalysisResult& ebml_detail,
                               model::ContainerStructureResult& result);

    /// FFmpeg 通用元数据回退分析
    /// @param cancel 可选取消标志, 交给 FFmpeg 的 AVIO 中断回调
    ///               (见 core/ffmpeg_io/FfmpegInterrupt.h)
    bool AnalyzeWithFFmpeg(const QString& file_path, model::ContainerStructureResult& result,
                           std::shared_ptr<std::atomic<bool>> cancel = {});

    /// HLS (.m3u8) / DASH (.mpd) 清单解析。
    /// 只走自研解析器（std::ifstream），绝不把清单交给 FFmpeg ——
    /// avformat 会把它当播放列表去发网络请求，离线 QC 场景不可控也无法单测。
    ///
    /// 返回三态：kCancelled 时 result 里的清单数据只到"被取消那一刻"为止，
    /// 调用方必须整条放弃（置 valid=false + error_message="已取消"，不得回退 FFmpeg）。
    StageStatus AnalyzeStreamingManifest(const QString& file_path,
                                         model::ContainerStructureResult& result,
                                         const std::atomic<bool>* cancel);

    /// 清单结构 -> 通用结构树 / 流信息 / 元数据（供"文件结构"页复用同一套渲染）
    /// 返回 false 表示中途被取消：树没建完，外层不得把半成品树发出去。
    bool BuildStreamingTree(model::ContainerStructureResult& result,
                            const std::atomic<bool>* cancel);

    /// 用 TsStructureAnalyzer 抽查若干 TS 分片（复用已有 TS 容器分析）
    /// 返回 false 表示中途被取消（已置位 container_parse_failed 的那一两个分片不算）。
    bool ProbeTsSegments(model::ContainerStructureResult& result,
                         const std::atomic<bool>* cancel);
};

} // namespace analyzer
} // namespace videoeye
