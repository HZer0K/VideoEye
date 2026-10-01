#pragma once

#include <atomic>
#include <memory>
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
    /// @param cancel 可选取消标志: 非空时交给 FFmpeg 的 AVIO 中断回调,
    ///               使关闭流程（CancelAll）能及时中止阻塞 IO。传空则不做取消。
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result,
                 std::shared_ptr<std::atomic<bool>> cancel = {});

    /// 重置
    void Reset();

private:
    /// 将 Mp4BoxNode 树映射为 ContainerElement 树
    void ConvertMp4Tree(const QVector<model::Mp4BoxNode>& nodes, int depth,
                        QVector<model::ContainerElement>& out);

    /// 将 EbmlElementNode 树映射为 ContainerElement 树
    void ConvertEbmlTree(const QVector<model::EbmlElementNode>& nodes, int depth,
                         QVector<model::ContainerElement>& out);

    /// 从 MP4 Box 树中提取丰富的流信息
    void ExtractMp4StreamInfo(const QVector<model::Mp4BoxNode>& box_tree,
                              model::ContainerStructureResult& result);

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
    bool AnalyzeStreamingManifest(const QString& file_path,
                                  model::ContainerStructureResult& result);

    /// 清单结构 -> 通用结构树 / 流信息 / 元数据（供"文件结构"页复用同一套渲染）
    void BuildStreamingTree(model::ContainerStructureResult& result);

    /// 用 TsStructureAnalyzer 抽查若干 TS 分片（复用已有 TS 容器分析）
    void ProbeTsSegments(model::ContainerStructureResult& result);
};

} // namespace analyzer
} // namespace videoeye
