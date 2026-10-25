#pragma once

// 公共头不 include libav*：实现放在 AnalysisStreamSelection.cpp。
// 这里只用到指针，前向声明即可。
struct AVStream;
struct AVFormatContext;

namespace videoeye {

// 可播放的视频轨：封面图片（APIC / AV_DISPOSITION_ATTACHED_PIC）不算。
// 带内嵌封面的 MP3/M4A 在 FFmpeg 里就是一条"一帧的 MJPEG 视频流"，
// 把它当主视频会让码率/GOP/色彩/码流分析全部指向一张封面图。
bool IsAnalysisVideoStream(const AVStream* stream);

// 选主轨：优先 av_find_best_stream（它认 AV_DISPOSITION_DEFAULT 与容器默认轨，
// 单靠"第一条同类型流"会选中封面图或字幕带的缩略图轨），
// 结果不合法时再线性兜底。
// type 取 AVMediaType 的值（AVMEDIA_TYPE_VIDEO / AUDIO / SUBTITLE ...）。
// 用 int 而不是 AVMediaType，同样是为了让公共头不必 include libavutil。
int SelectAnalysisStream(AVFormatContext* format, int type);

} // namespace videoeye
