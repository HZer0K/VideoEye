#include "core/ffmpeg/FfmpegProgressTracker.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>

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

int SkipSpaces(const QString& text, int pos) {
    const int n = static_cast<int>(text.size());
    int i = pos;
    while (i < n && text.at(i).isSpace()) {
        ++i;
    }
    return i;
}

/// 读一个十进制数（允许前导符号与小数），pos 处允许有空格。
/// end（可为 null）返回数字结束的位置 —— 单位/speed 的 'x' 就紧跟在后面。
bool ParseDecimal(const QString& text, int pos, double* out_value, int* end = nullptr) {
    const int n = static_cast<int>(text.size());
    int i = SkipSpaces(text, pos);
    bool negative = false;
    if (i < n && (text.at(i) == QLatin1Char('-') || text.at(i) == QLatin1Char('+'))) {
        negative = text.at(i) == QLatin1Char('-');
        ++i;
    }

    const int start = i;
    double value = 0.0;
    while (i < n && text.at(i).isDigit()) {
        value = value * 10.0 + (text.at(i).unicode() - '0');
        ++i;
    }
    if (i < n && text.at(i) == QLatin1Char('.')) {
        ++i;
        double scale = 0.1;
        while (i < n && text.at(i).isDigit()) {
            value += (text.at(i).unicode() - '0') * scale;
            scale /= 10.0;
            ++i;
        }
    }
    if (i == start) {
        return false; // "N/A" / 空
    }
    *out_value = negative ? -value : value;
    if (end != nullptr) {
        *end = i;
    }
    return true;
}

/// "14080kB" / "1.2MiB" / "N/A" → 字节数。
///
/// 单位按首字母与有没有 'i' 区分：ffmpeg 的 kB/mB 是十进制（1000），
/// KiB/MiB 是 1024 进制；两种都要认，因为不同版本与不同字段的写法不一样。
bool ParseByteSize(const QString& text, int pos, qint64* out_bytes) {
    const int n = static_cast<int>(text.size());
    int i = SkipSpaces(text, pos);
    if (i >= n || text.mid(i, 3) == QLatin1String("N/A")) {
        return false;
    }

    double value = 0.0;
    int end = i;
    if (!ParseDecimal(text, i, &value, &end)) {
        return false;
    }
    i = SkipSpaces(text, end);

    QString unit;
    while (i < n && text.at(i).isLetter()) {
        unit += text.at(i);
        ++i;
    }
    if (unit.isEmpty()) {
        return false;
    }

    const bool binary = unit.contains(QLatin1Char('i'), Qt::CaseInsensitive);
    const double base = binary ? 1024.0 : 1000.0;
    const QChar head = unit.at(0).toLower();
    double scale = 1.0;
    if (head == QLatin1Char('k')) {
        scale = base;
    } else if (head == QLatin1Char('m')) {
        scale = base * base;
    } else if (head == QLatin1Char('g')) {
        scale = base * base * base;
    } else if (head == QLatin1Char('t')) {
        scale = base * base * base * base;
    } else if (head != QLatin1Char('b')) {
        return false;
    }

    *out_bytes = static_cast<qint64>(value * scale + 0.5);
    return true;
}

} // namespace

void FfmpegProgressTracker::Reset() {
    duration_seen_ = false;
    duration_ms_ = -1;
    position_ms_ = 0;
    fps_ = -1.0;
    size_bytes_ = -1;
    speed_count_ = 0;
    speed_next_ = 0;
    for (double& sample : speed_samples_) {
        sample = 0.0;
    }
}

bool FfmpegProgressTracker::Feed(const QString& line) {
    if (line.isEmpty()) {
        return false;
    }
    bool changed = false;

    // 只认第一个 Duration：多输入（-i a.mp4 -i b.mp4）时后面那个不是这次的总量。
    // 即使第一个是 "N/A" 也照样认下"已经见过"这件事 —— 否则第二个 Duration
    // 会被当成总量，而 position 是整体输出的位置，两个口径对不上。
    if (!duration_seen_) {
        const int at = line.indexOf(QLatin1String("Duration:"));
        if (at >= 0) {
            duration_seen_ = true;
            qint64 ms = 0;
            if (ParseTimecode(line, SkipSpaces(line, at + 9), &ms) && ms > 0) {
                duration_ms_ = ms;
                changed = true;
            }
        }
    }

    // 进度行末尾的 time=，取最后一个（同一条行里可能还有别的时间串）
    const int time_at = line.lastIndexOf(QLatin1String("time="));
    if (time_at >= 0) {
        qint64 ms = 0;
        if (ParseTimecode(line, SkipSpaces(line, time_at + 5), &ms)) {
            if (ms < 0) {
                ms = 0; // 第一帧可能为负，当作 0
            }
            if (ms != position_ms_) {
                position_ms_ = ms;
                changed = true;
            }
        }
    }

    // fps=：首行是 0.0（一帧都还没编完），当未知处理，免得状态行写"0 fps"。
    const int fps_at = line.lastIndexOf(QLatin1String("fps="));
    if (fps_at >= 0) {
        double value = 0.0;
        if (ParseDecimal(line, fps_at + 4, &value) && value > 0.0 && value != fps_) {
            fps_ = value;
            changed = true;
        }
    }

    // size=：summary 行写的是 Lsize=，其中也含 "size="，lastIndexOf 正好一起吃到。
    const int size_at = line.lastIndexOf(QLatin1String("size="));
    if (size_at >= 0) {
        qint64 bytes = -1;
        if (ParseByteSize(line, size_at + 5, &bytes) && bytes > 0 && bytes != size_bytes_) {
            size_bytes_ = bytes;
            changed = true;
        }
    }

    // speed=：可能是 "N/A"（还没开始）或 "0.000x"（刚开始），都当没采到。
    const int speed_at = line.lastIndexOf(QLatin1String("speed="));
    if (speed_at >= 0) {
        double value = 0.0;
        if (ParseDecimal(line, speed_at + 6, &value) && value > 0.0) {
            const double before = speed();
            AppendSpeedSample(value);
            if (speed() != before) {
                changed = true;
            }
        }
    }

    return changed;
}

void FfmpegProgressTracker::AppendSpeedSample(double value) {
    speed_samples_[speed_next_] = value;
    speed_next_ = (speed_next_ + 1) % kSpeedWindow;
    if (speed_count_ < kSpeedWindow) {
        ++speed_count_;
    }
}

double FfmpegProgressTracker::percent() const {
    if (duration_ms_ <= 0) {
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

double FfmpegProgressTracker::speed() const {
    if (speed_count_ <= 0) {
        return -1.0;
    }
    double sum = 0.0;
    for (int i = 0; i < speed_count_; ++i) {
        sum += speed_samples_[i];
    }
    return sum / static_cast<double>(speed_count_);
}

double FfmpegProgressTracker::fps() const {
    return fps_;
}

qint64 FfmpegProgressTracker::size_bytes() const {
    return size_bytes_;
}

qint64 FfmpegProgressTracker::eta_ms() const {
    // 没有总时长就没有"还剩多少"；position 还没动时算出来的是满额，没有参考价值。
    if (duration_ms_ <= 0 || position_ms_ <= 0) {
        return -1;
    }
    const double rate = speed();
    if (rate <= 0.0) {
        return -1;
    }
    const qint64 remaining_ms = duration_ms_ - position_ms_;
    if (remaining_ms <= 0) {
        return 0;
    }
    // speed 的单位是"媒体秒 / 墙钟秒"，所以剩余媒体时长除以它就是墙钟时长。
    return static_cast<qint64>(static_cast<double>(remaining_ms) / rate + 0.5);
}

FfmpegProgressSnapshot FfmpegProgressTracker::Snapshot() const {
    FfmpegProgressSnapshot snapshot;
    snapshot.percent = percent();
    snapshot.position_ms = position_ms_;
    snapshot.duration_ms = duration_ms_;
    snapshot.speed = speed();
    snapshot.fps = fps_;
    snapshot.size_bytes = size_bytes_;
    snapshot.eta_ms = eta_ms();
    return snapshot;
}

QString FfmpegProgressTracker::position_text() const {
    return FormatProgressClock(position_ms_);
}

QString FfmpegProgressTracker::duration_text() const {
    return duration_ms_ > 0 ? FormatProgressClock(duration_ms_) : QString();
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

QString FormatByteSize(qint64 bytes) {
    if (bytes < 0) {
        return QString();
    }
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    const double value = static_cast<double>(bytes);
    if (value < kKiB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (value < kMiB) {
        return QStringLiteral("%1 KiB").arg(value / kKiB, 0, 'f', 1);
    }
    if (value < kGiB) {
        return QStringLiteral("%1 MiB").arg(value / kMiB, 0, 'f', 1);
    }
    return QStringLiteral("%1 GiB").arg(value / kGiB, 0, 'f', 2);
}

namespace {

/// 速度文案："4.2×" / "129×" —— 超过 10 倍就不必给小数了。
QString FormatSpeed(double speed) {
    if (speed < 10.0) {
        return QStringLiteral("%1×").arg(speed, 0, 'f', 1);
    }
    return QStringLiteral("%1×").arg(speed, 0, 'f', 0);
}

} // namespace

QString FormatProgressSummary(const FfmpegProgressSnapshot& snapshot) {
    QString text;
    if (snapshot.percent >= 0.0) {
        text = QStringLiteral("%1%（%2 / %3）")
                   .arg(snapshot.percent, 0, 'f', 1)
                   .arg(FormatProgressClock(snapshot.position_ms))
                   .arg(FormatProgressClock(snapshot.duration_ms));
    } else {
        text = QStringLiteral("已处理 %1（总时长未知）").arg(FormatProgressClock(snapshot.position_ms));
    }

    if (snapshot.speed > 0.0) {
        text += QStringLiteral(" · %1").arg(FormatSpeed(snapshot.speed));
    }
    // 只有知道总时长时"约剩"才有意义（eta_ms 本身也是这么算的，这里再守一道）
    if (snapshot.eta_ms >= 0 && snapshot.percent >= 0.0) {
        text += QStringLiteral(" · 约剩 %1").arg(FormatProgressClock(snapshot.eta_ms));
    }
    return text;
}

QString FormatProgressDetail(const FfmpegProgressSnapshot& snapshot) {
    QStringList parts;
    if (snapshot.percent >= 0.0) {
        parts << QStringLiteral("已处理 %1 / %2（%3%）")
                     .arg(FormatProgressClock(snapshot.position_ms))
                     .arg(FormatProgressClock(snapshot.duration_ms))
                     .arg(snapshot.percent, 0, 'f', 1);
    } else {
        parts << QStringLiteral("已处理 %1（总时长未知）").arg(FormatProgressClock(snapshot.position_ms));
    }
    if (snapshot.speed > 0.0) {
        parts << QStringLiteral("速度 %1").arg(FormatSpeed(snapshot.speed));
    }
    if (snapshot.fps > 0.0) {
        parts << QStringLiteral("帧率 %1 fps").arg(snapshot.fps, 0, 'f', 0);
    }
    if (snapshot.size_bytes > 0) {
        parts << QStringLiteral("已写入 %1").arg(FormatByteSize(snapshot.size_bytes));
    }
    if (snapshot.eta_ms >= 0 && snapshot.percent >= 0.0) {
        parts << QStringLiteral("预计还需 %1").arg(FormatProgressClock(snapshot.eta_ms));
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace ffmpeg
} // namespace videoeye
