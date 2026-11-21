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
// 剩余时间（eta_ms）由同一条 progress 行里的 speed= 推算，同样受这个口径影响。
//
// 各字段的可信度不一样，界面要据此决定显不显示：
//   - Duration 只认第一个（多输入时后面那个不是这次的总量）；
//   - 首行 fps=0.0 / speed=N/A / size=0kB 是"还没开始"的占位值，不是真值；
//   - 纯流拷贝（-c copy）也可能有 speed=，那是几百倍的复用速度，不是转码速度，
//     但用它算剩余时间反而准，照用。

#include <QString>

namespace videoeye {
namespace ffmpeg {

/// 某一时刻的进度快照。全部字段都用"未知 = 负数"表示，不拿 0 冒充已知
/// （0 是合法值：位置 0、速度 0.0、文件 0 字节）。
struct FfmpegProgressSnapshot {
    double percent = -1.0;   ///< 0..100；-1 = 总时长未知
    qint64 position_ms = 0;  ///< 已处理时长
    qint64 duration_ms = -1; ///< 输入总时长；-1 = 未知
    double speed = -1.0;     ///< 处理速度（媒体秒 / 墙钟秒）；-1 = 未知
    double fps = -1.0;       ///< 帧率；-1 = 未知
    qint64 size_bytes = -1;  ///< 已写入字节（ffmpeg 只给到 0.1 精度，是近似值）
    qint64 eta_ms = -1;      ///< 预计还需多久；-1 = 算不出来
};

class FfmpegProgressTracker {
public:
    void Reset();

    /// 喂一行输出。返回 true 表示有字段变了（界面可以刷新一次）。
    bool Feed(const QString& line);

    bool duration_known() const {
        return duration_ms_ > 0;
    }
    qint64 duration_ms() const {
        return duration_ms_;
    }
    qint64 position_ms() const {
        return position_ms_;
    }

    /// 0..100；没有总时长（流 / N/A）时返回 -1，不猜。
    double percent() const;

    /// 处理速度（媒体秒 / 墙钟秒）：最近几次采样的平均值，抑制 mux 抖动；
    /// 没有可用采样时返回 -1。
    double speed() const;

    /// 帧率；首行的 fps=0.0 不算已知（那时一帧都还没编完）。
    double fps() const;

    /// 已写入字节；未知返回 -1。
    qint64 size_bytes() const;

    /// 预计还需多久。position 还没动过、speed 未知、总时长未知时返回 -1。
    qint64 eta_ms() const;

    FfmpegProgressSnapshot Snapshot() const;

    /// "00:04" / "1:02:03"
    QString position_text() const;
    QString duration_text() const;

private:
    void AppendSpeedSample(double value);

    bool duration_seen_ = false; ///< 是否已遇到过 Duration: 行（哪怕是 N/A）
    qint64 duration_ms_ = -1;
    qint64 position_ms_ = 0;
    double fps_ = -1.0;
    qint64 size_bytes_ = -1;

    // 最近几次 speed= 采样。ffmpeg 打的是"从开始到现在的平均速度"，本身已经
    // 比较黏，再取窗口平均是为了压掉首尾的极端值（收尾时 speed 会突然飙高）。
    static constexpr int kSpeedWindow = 3;
    double speed_samples_[kSpeedWindow] = {};
    int speed_count_ = 0;
    int speed_next_ = 0;
};

/// 毫秒 → ffmpeg 风格时钟串：不足 1 小时是 MM:SS，否则 H:MM:SS（负值按 0 处理）。
QString FormatProgressClock(qint64 ms);

/// 字节 → "14.1 MiB"（1024 进制，跟资源管理器一致）。负值返回空串。
QString FormatByteSize(qint64 bytes);

/// 状态行里的短摘要，不含"进度:"前缀：
///   "43.2%（00:12 / 00:28）· 4.2× · 约剩 00:08"
/// 总时长未知时换成 "已处理 00:12（总时长未知）"，速度/剩余时间未知就整段省掉。
QString FormatProgressSummary(const FfmpegProgressSnapshot& snapshot);

/// 悬停提示用的完整一行（含帧率与已写入大小）；字段未知的段自动省略。
QString FormatProgressDetail(const FfmpegProgressSnapshot& snapshot);

} // namespace ffmpeg
} // namespace videoeye
