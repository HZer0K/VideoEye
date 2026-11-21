#pragma once

// 从 ffmpeg 的 stderr 里读进度。
//
// ffmpeg 没有"已处理 / 总时长"这种现成字段，只能从两处拼出来：
//   Duration: 00:00:10.00, start: 0.000000, bitrate: 2456 kb/s   ← 读输入头时打一次
//   frame=  300 fps= 60 q=28.0 size=... time=00:00:10.00 ...     ← 编码时反复刷
// 两行都在 stderr 上；进度行用 \r 覆盖同一行，FfmpegProcessRunner 已按 \r 切成行。
//
// 为什么不另起 ffprobe / 调内置 FFmpeg 库去探时长：ffmpeg 自己已经把 Duration
// 打出来了，同一份输出里顺手读掉最省事，也不用为这一页再拖进 media 模块。
// 代价是流式输入（Duration 为 N/A）拿不到总时长 —— 这时 percent() 返回负数，
// 界面只显示已处理时长，不编一个百分比。
//
// 口径提醒: 百分比 = 已处理时长 / **输入总时长**。命令里带 -ss / -t 时这个分母
// 不等于这次要产出的时长，百分比会偏小或提前到 100%（见 ui 侧的提示文案）。

#include <QString>

namespace videoeye {
namespace ffmpeg {

class FfmpegProgressTracker {
public:
    void Reset();

    /// 喂一行输出。返回 true 表示时长或位置变了（界面可以刷新一次）。
    bool Feed(const QString& line);

    bool duration_known() const {
        return duration_known_;
    }
    qint64 duration_ms() const {
        return duration_ms_;
    }
    qint64 position_ms() const {
        return position_ms_;
    }

    /// 0..100；没有总时长（流 / N/A）时返回 -1，不猜。
    double percent() const;

    /// "00:04" / "1:02:03"
    QString position_text() const;
    QString duration_text() const;

private:
    bool duration_known_ = false;
    qint64 duration_ms_ = 0;
    qint64 position_ms_ = 0;
};

/// 毫秒 → ffmpeg 风格时钟串：不足 1 小时是 MM:SS，否则 H:MM:SS（负值按 0 处理）。
QString FormatProgressClock(qint64 ms);

} // namespace ffmpeg
} // namespace videoeye
