#pragma once

// 给「文件结构」页喂一份容器结构 —— 布局回归的第二轮扫描用。
//
// 为什么只喂这一页（这是量出来的，不是猜的）
// ------------------------------------------
// 第二轮扫描的目的是让"只有有数据才显示"的控件显示出来，再扫一遍几何。
// 实际度量（2026-10-30，逐 tab 全切后的控件并集）：
//   只扫默认 tab        365 个控件
//   逐 tab 全切（空态）  529 个（+164，这部分是 tab 遍历的功劳，见测试本体）
//   逐 tab + 喂数据      581 个（+52）
// 而那 +52 里，「文件结构」页一页就占了 +56（34 → 90）：容器树、流表、元数据表
// 在空态是隐藏的。其余各页的"有数据 vs 空态"差分都在 ±2 以内 —— 也就是说给它们
// 造数据（音频 QC / 字幕 / 码率 GOP / 参数集…）**一分覆盖都换不来**，只换来一堆
// 随 SetResult 签名变化就会腐烂的构造代码。那些页的数值语义本来也由各自的用例负责。
//
// 所以这里只构造 ContainerStructureResult。要加页之前先量一遍：如果那一页的
// "有数据 vs 空态"控件并集没有明显差分，就别加。

#include <string>

#include "core/domain/model/ContainerStructureInfo.h"

#include "ui/analysis_panel/AnalysisPageFactory.h"
#include "ui/analysis_panel/ContainerStructurePage.h"

namespace videoeye_test {

namespace model = videoeye::model;
namespace ui = videoeye::ui;

// 一棵小容器树 + 两条流 + 几条元数据。刻意做成 3 层、每层有多个兄弟节点：
// 只有单节点的树往往撑不开它的 viewport，量不到真实排布。
inline model::ContainerStructureResult MakeRichContainerStructure() {
    model::ContainerStructureResult container;
    container.valid = true;
    container.format = model::ContainerFormat::MP4;
    container.format_name = "MP4";
    container.file_path = "rich_sample.mp4";
    container.summary = "MP4，2 条流，moov 在 mdat 之后";

    model::ContainerElement moov;
    moov.name = "moov";
    moov.type = "Box";
    moov.size = 8192;
    moov.offset = 0;
    moov.depth = 0;

    model::ContainerElement trak;
    trak.name = "trak";
    trak.type = "Box";
    trak.size = 4096;
    trak.offset = 8;
    trak.depth = 1;

    model::ContainerElement stbl;
    stbl.name = "stbl";
    stbl.type = "Box";
    stbl.size = 2048;
    stbl.offset = 16;
    stbl.depth = 2;
    trak.children.push_back(stbl);

    // 第二个兄弟节点：让 moov 有多个子项，树才有真正的层级排布
    model::ContainerElement mdat;
    mdat.name = "mdat";
    mdat.type = "Box";
    mdat.size = 800000;
    mdat.offset = 8192;
    mdat.depth = 1;
    moov.children.push_back(trak);
    moov.children.push_back(mdat);
    container.element_tree.push_back(moov);

    model::ContainerStreamInfo video;
    video.index = 0;
    video.type = "video";
    video.codec = "h264";
    video.details = "1280x720 @25fps";
    model::ContainerStreamInfo audio;
    audio.index = 1;
    audio.type = "audio";
    audio.codec = "aac";
    audio.details = "48000Hz 2ch";
    container.streams = {video, audio};

    container.metadata["title"] = "rich sample";
    container.metadata["encoder"] = "Lavf60";
    container.metadata["major_brand"] = "isom";

    return container;
}

// 灌进「文件结构」页。pages.container 为空时什么也不做（建页失败时测试会先报数量不符）。
inline void FeedContainerData(const ui::AnalysisPages& pages, const model::ContainerStructureResult& container) {
    if (pages.container != nullptr)
        pages.container->SetResult(container);
}

} // namespace videoeye_test
