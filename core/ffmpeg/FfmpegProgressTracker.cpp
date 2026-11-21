#include "core/ffmpeg/FfmpegProgressTracker.h"

#include <QLatin1String>

namespace videoeye {
namespace ffmpeg {
namespace {

/// 读 "HH:MM:SS[.xxx]"，pos 指向数字（允许前导 '-'）。
/// ffmpeg 在刚开始编码时偶尔吐 time=-00:00:00.02（第一帧时间戳为负），必须吃得下。
bool ParseTimecode(const QString& text, int pos, qint64* out_ms) {
    const int n = static_cast<int>(text.size());
    int i = pos;
    bool negative = false;
    if (i < n && text.at(i) == QLatin1Char('-')) {
        negative = true;
        ++i;
    }

    const auto read_int = [&text, n, &i](int* value) {
        const int start = i;
        int v = 0;
        while (i < n && text.at(i).isDigit()) {
            v = v * 10 + (text.at(i).unicode() - '0');
            ++i;
        }
        *value = v;
        return i > start;
    };

    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (!read_int(&hours) || i >= n || text.at(i) != QLatin1Char(':')) {
        return false;
    }
    ++i;
    if (!read_int(&minutes) || i >= n || text.at(i) != QLatin1Char(':')) {
        return false;
    }
    ++i;
    if (!read_int(&seconds)) {
        return false;
    }

    double fraction = 0.0;
    if (i < n && (text.at(i) == QLatin1Char('.') || text.at(i) == QLatin1Char(','))) {
        ++i;
        double scale = 0.1;
        while (i < n && text.at(i).isDigit()) {
            fraction += (text.at(i).unicode() - '0') * scale;
            scale /= 10.0;
            ++i;
        }
    }

    qint64 ms = (static_cast<qint64>(hours) * 3600 + minutes * 60 + seconds) * 1000;
    ms += static_cast<qint64>(fraction * 1000.0 + 0.5);
    *out_ms = negative ? -ms : ms;
    return true;
}

} // namespace

void FfmpegProgressTracker::Reset() {
    duration_known_ = false;
    duration_ms_ = 0;
    position_ms_ = 0;
}

bool FfmpegProgressTracker::Feed(const QString& line) {
    if (line.isEmpty()) {
        return false;
    }
    bool changed = false;

    // 只认第一个 Duration：多输入（-i a.mp4 -i b.mp4）时后面那个不是这次的总量。
    if (!duration_known_) {
        const int at = line.indexOf(QLatin1String("Duration:"));
        if (at >= 0) {
            int p = at + 9; // "Duration:" 的长度
            while (p < line.size() && line.at(p).isSpace()) {
                ++p;
            }
            qint64 ms = 0;
            if (ParseTimecode(line, p, &ms) && ms > 0) {
                duration_ms_ = ms;
                duration_known_ = true;
                changed = true;
            }
            // "Duration: N/A" 解析失败 → 保持未知，不重试
        }
    }

    // 进度行末尾的 time=，取最后一个（同一条行里可能还有别的时间串）
    const int at = line.lastIndexOf(QLatin1String("time="));
    if (at >= 0) {
        int p = at + 5;
        while (p < line.size() && line.at(p).isSpace()) {
            ++p;
        }
        qint64 ms = 0;
        if (ParseTimecode(line, p, &ms)) {
            if (ms < 0) {
                ms = 0; // 第一帧可能为负，当作 0
            }
            if (ms != position_ms_) {
                position_ms_ = ms;
                changed = true;
            }
        }
    }
    return changed;
}

double FfmpegProgressTracker::percent() const {
    if (!duration_known_ || duration_ms_ <= 0) {
        return -1.0;
    }
    double value = 100.0 * static_cast<double>(position_ms_) / static_cast<double>(duration_ms_);
    if (value < 0.0) {
        value = 0.0;
    }
    if (value > 100.0) {
        value = 100.0;
    }
    return value;
}

QString FfmpegProgressTracker::position_text() const {
    return FormatProgressClock(position_ms_);
}

QString FfmpegProgressTracker::duration_text() const {
    return duration_known_ ? FormatProgressClock(duration_ms_) : QString();
}

QString FormatProgressClock(qint64 ms) {
    if (ms < 0) {
        ms = 0;
    }
    const qint64 total_seconds = ms / 1000;
    const qint64 hours = total_seconds / 3600;
    const qint64 minutes = (total_seconds / 60) % 60;
    const qint64 seconds = total_seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace ffmpeg
} // namespace videoeye
