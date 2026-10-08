#pragma once

// ============================================================
// 合成媒体工厂（测试专用）
//
// 为什么需要它: 导出 / 转码这类"成功路径"以前完全没有单测覆盖，根因就是仓库里
// 没有一个能被测试稳定生成、又不进版本库的媒体样本 —— 手写字节拼出来的 MP4
// 最多喂给 IsobmffParser，喂不进 avcodec（那要真能解码的码流）。
//
// 这里直接用 FFmpeg 的编码 API 现场生成一份极小的 MP4：
//   * 128x72 / 25 fps / 1.6 秒（40 帧）/ H.264 + AAC
//   * 画面与音频由确定性算法算出（不读时钟、不读随机数），同一份 spec 每次生成
//     的像素与采样完全一致 —— 断言"帧数 / 时长 / 流数量"才有意义
//   * 只依赖仓库自带的预编译 FFmpeg，不需要机器上装 ffmpeg 命令行，也不 spawn
//     任何子进程（CI 沙箱里 QProcess 起不来）
//
// ProbeMediaFile 是它的另一半：**导出产物必须能被 FFmpeg 重新打开**。只断言
// "文件存在且大于 0 字节"会把"写了个打不开的空壳"判成成功。
// ============================================================

#include <string>

namespace videoeye_test {

struct TestMediaSpec {
    int width = 128;
    int height = 72;
    int fps = 25;
    int frame_count = 40;      // 40 / 25 = 1.6 s
    int sample_rate = 44100;
    int channels = 2;
    bool with_audio = true;
};

struct TestMedia {
    std::string path;
    std::string video_codec;   // 实际用到的编码器名（libx264 / mpeg4 ...）
    std::string audio_codec;
    int width = 0;
    int height = 0;
    int fps = 0;
    int frame_count = 0;
    int sample_rate = 0;
    int channels = 0;
    double duration_seconds = 0.0;
    bool ok = false;
    std::string error;         // 生成失败时的原因（供 ASSERT 打印）
};

// 在 directory 下生成 <name>.mp4；失败时 ok=false 且 error 有内容。
TestMedia CreateTestMedia(const std::string& directory, const std::string& name,
                          const TestMediaSpec& spec = TestMediaSpec());

// ---- 产物校验 ----
struct MediaProbe {
    bool opened = false;                // FFmpeg 能否重新打开
    int video_streams = 0;
    int audio_streams = 0;
    std::string video_codec;
    std::string audio_codec;
    int width = 0;
    int height = 0;
    double duration_seconds = 0.0;      // 容器层时长
    double video_duration_seconds = 0.0;// 视频流层时长（容器层可能是估算值）
    double video_start_seconds = 0.0;   // 视频流的起始时间（>0 说明容器用编辑列表裁掉了开头）
    long long video_nb_frames = 0;      // 容器声明的帧数（元数据，可能与真解码结果不同）
    int frame_count = 0;                // **真实解码**出来的帧数
    int video_packet_count = 0;
    std::string error;
};

// 重新打开 path：统计流数量 / 时长，并把视频流完整解码一遍数出帧数。
MediaProbe ProbeMediaFile(const std::string& path);

// 生成一个确定内容的"损坏"媒体：字节是递增填充，没有合法魔数。
// 用来触发"打开失败 / 探测失败"的分支。
std::string CreateCorruptMediaFile(const std::string& directory, const std::string& name);

}  // namespace videoeye_test
