#include "core/ffmpeg/FfmpegMediaProbe.h"

#include <QChar>
#include <QFileInfo>
#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "core/ffmpeg/FfmpegCommandPreflight.h"

namespace videoeye {
namespace ffmpeg {
namespace {

// 顶层逗号切分：括号 () 与方括号 [] 里的逗号不算。
// 必须这么切 —— 像素格式那一段是 `yuv420p(tv, bt709, progressive)`，直接 split(',')
// 会把它拆成三段，后两段还会被当成"声道布局"之类的字段。
QStringList SplitTopLevelComma(const QString& text) {
    QStringList out;
    QString current;
    int paren = 0;
    int bracket = 0;
    for (const QChar c : text) {
        if (c == QLatin1Char('(')) {
            ++paren;
        } else if (c == QLatin1Char(')')) {
            if (paren > 0) {
                --paren;
            }
        } else if (c == QLatin1Char('[')) {
            ++bracket;
        } else if (c == QLatin1Char(']')) {
            if (bracket > 0) {
                --bracket;
            }
        }
        if (c == QLatin1Char(',') && paren == 0 && bracket == 0) {
            out.append(current.trimmed());
            current.clear();
            continue;
        }
        current.append(c);
    }
    out.append(current.trimmed());
    return out;
}

/// "5940 kb/s" / "128000 bit/s" / "1.5 Mb/s" → 位/秒。认不出返回 -1（不是 0）。
qint64 ParseBitrateText(const QString& text) {
    QString body = text.trimmed().toLower();
    if (!body.endsWith(QLatin1String("/s"))) {
        return -1;
    }
    body.chop(2);
    body = body.trimmed();
    if (body.endsWith(QLatin1String("bit"))) {
        body.chop(3);
        body = body.trimmed();
    }
    if (body.endsWith(QLatin1Char('b'))) {
        body.chop(1);
        body = body.trimmed();
    }
    double multiplier = 1.0;
    if (!body.isEmpty()) {
        const QChar unit = body.at(body.size() - 1).toLower();
        if (unit == QLatin1Char('k')) {
            multiplier = 1000.0;
            body.chop(1);
        } else if (unit == QLatin1Char('m')) {
            multiplier = 1000.0 * 1000.0;
            body.chop(1);
        } else if (unit == QLatin1Char('g')) {
            multiplier = 1000.0 * 1000.0 * 1000.0;
            body.chop(1);
        }
    }
    bool ok = false;
    const double value = body.trimmed().toDouble(&ok);
    if (!ok) {
        return -1;
    }
    return static_cast<qint64>(value * multiplier + 0.5);
}

/// "00:01:23.45" → 秒。"N/A" → -1。
double ParseClockText(const QString& text) {
    const QString body = text.trimmed();
    if (body.isEmpty() || body.compare(QLatin1String("N/A"), Qt::CaseInsensitive) == 0) {
        return -1.0;
    }
    const QStringList parts = body.split(QLatin1Char(':'));
    if (parts.size() != 3) {
        return -1.0;
    }
    bool ok_h = false;
    bool ok_m = false;
    bool ok_s = false;
    const double hours = parts.at(0).toDouble(&ok_h);
    const double minutes = parts.at(1).toDouble(&ok_m);
    const double seconds = parts.at(2).toDouble(&ok_s);
    if (!ok_h || !ok_m || !ok_s) {
        return -1.0;
    }
    return hours * 3600.0 + minutes * 60.0 + seconds;
}

/// 取第一层 "(" 里的内容（`aac (LC) (mp4a / 0x..)` → "LC"）。
/// 4CC 标签那一层（含 '/' 或 0x 前缀）跳过 —— 它说的是 tag，不是 profile。
QString ExtractProfile(const QString& text) {
    int pos = 0;
    while (true) {
        const int open = text.indexOf(QLatin1Char('('), pos);
        if (open < 0) {
            return QString();
        }
        const int close = text.indexOf(QLatin1Char(')'), open);
        if (close < 0) {
            return QString();
        }
        const QString inner = text.mid(open + 1, close - open - 1).trimmed();
        pos = close + 1;
        if (inner.isEmpty() || inner.contains(QLatin1Char('/')) || inner.contains(QLatin1Char('[')) ||
            inner.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
            continue;
        }
        return inner;
    }
}

/// 编码名 = 第一个 "(" 或空格之前的那截。"h264 (High) (avc1 / 0x..)" → "h264"。
QString ExtractCodecName(const QString& text) {
    QString body = text.trimmed();
    const int cut = body.indexOf(QLatin1Char('('));
    if (cut >= 0) {
        body = body.left(cut);
    }
    body = body.trimmed();
    const int space = body.indexOf(QLatin1Char(' '));
    if (space >= 0) {
        body = body.left(space);
    }
    return body;
}

/// "1920x1080 [SAR 1:1 DAR 16:9]" → 1920 / 1080。
/// 用 `\d{2,5}x\d{2,5}` 是为了躲开 4CC 里的 `0x31637661`（x 前只有一位数字）。
bool ParseResolution(const QString& text, int* width, int* height) {
    static const QRegularExpression re(QStringLiteral("(\\d{2,5})x(\\d{2,5})"));
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    *width = match.captured(1).toInt();
    *height = match.captured(2).toInt();
    return *width > 0 && *height > 0;
}

/// "SAR 1:1 DAR 16:9" 里按需取一个。
QString ExtractAspect(const QString& text, const QString& key) {
    const int pos = text.indexOf(key, 0, Qt::CaseInsensitive);
    if (pos < 0) {
        return QString();
    }
    int start = pos + key.size();
    while (start < text.size() && text.at(start).isSpace()) {
        ++start;
    }
    int end = start;
    while (end < text.size() && (text.at(end).isDigit() || text.at(end) == QLatin1Char(':'))) {
        ++end;
    }
    return text.mid(start, end - start);
}

bool ParseFpsText(const QString& text, double* fps) {
    const QString body = text.trimmed().toLower();
    if (!body.endsWith(QLatin1String("fps"))) {
        return false;
    }
    bool ok = false;
    const double value = body.left(body.size() - 3).trimmed().toDouble(&ok);
    if (!ok || value <= 0.0) {
        return false; // "0.00 fps" 是"不知道"，不是"零帧"
    }
    *fps = value;
    return true;
}

bool ParseSampleRateText(const QString& text, int* rate) {
    const QString body = text.trimmed().toLower();
    if (!body.endsWith(QLatin1String("hz"))) {
        return false;
    }
    bool ok = false;
    const int value = body.left(body.size() - 2).trimmed().toInt(&ok);
    if (!ok || value <= 0) {
        return false;
    }
    *rate = value;
    return true;
}

/// 像素格式：已知前缀 + 可选的 `(tv, bt709, …)` 尾巴。
/// 用前缀白名单而不是正则 —— `yuv420p10le` / `p010le` / `nv12` 这类名字没有统一规律。
bool ParsePixelFormat(const QString& text, QString* pix_fmt) {
    static const QStringList kPrefixes = {
        QStringLiteral("yuv"),  QStringLiteral("yuva"), QStringLiteral("yuvj"), QStringLiteral("rgb"),
        QStringLiteral("rgba"), QStringLiteral("bgr"),  QStringLiteral("bgra"), QStringLiteral("gbr"),
        QStringLiteral("gbra"), QStringLiteral("gray"), QStringLiteral("nv"),   QStringLiteral("p0"),
        QStringLiteral("p1"),   QStringLiteral("p2"),   QStringLiteral("p4"),   QStringLiteral("xyz"),
        QStringLiteral("uyvy"), QStringLiteral("yuyv"), QStringLiteral("yvyu"), QStringLiteral("ya"),
        QStringLiteral("abgr"), QStringLiteral("argb"), QStringLiteral("0rgb"), QStringLiteral("pal"),
    };
    QString body = text.trimmed();
    const int cut = body.indexOf(QLatin1Char('('));
    if (cut >= 0) {
        body = body.left(cut);
    }
    body = body.trimmed();
    if (body.isEmpty() || body.size() > 24) {
        return false;
    }
    bool known_prefix = false;
    for (const QString& prefix : kPrefixes) {
        if (body.startsWith(prefix)) {
            known_prefix = true;
            break;
        }
    }
    if (!known_prefix) {
        return false;
    }
    for (const QChar c : body) {
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_'))) {
            return false;
        }
    }
    *pix_fmt = body;
    return true;
}

bool IsChannelLayout(const QString& text) {
    static const QStringList kLayouts = {
        QStringLiteral("mono"), QStringLiteral("stereo"), QStringLiteral("quad"),    QStringLiteral("2.1"),
        QStringLiteral("3.0"),  QStringLiteral("4.0"),    QStringLiteral("5.0"),     QStringLiteral("5.1"),
        QStringLiteral("6.1"),  QStringLiteral("7.1"),    QStringLiteral("downmix"),
    };
    const QString body = text.trimmed();
    for (const QString& layout : kLayouts) {
        if (body.compare(layout, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    // "5.1(side)" / "7.1(wide)" 这类带括号变体
    const int cut = body.indexOf(QLatin1Char('('));
    if (cut > 0) {
        const QString head = body.left(cut);
        for (const QString& layout : kLayouts) {
            if (head.compare(layout, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool IsSampleFormat(const QString& text) {
    static const QStringList kFormats = {
        QStringLiteral("u8"),   QStringLiteral("s16"),  QStringLiteral("s32"),  QStringLiteral("s64"),
        QStringLiteral("flt"),  QStringLiteral("dbl"),  QStringLiteral("u8p"),  QStringLiteral("s16p"),
        QStringLiteral("s32p"), QStringLiteral("s64p"), QStringLiteral("fltp"), QStringLiteral("dblp"),
    };
    return kFormats.contains(text.trimmed(), Qt::CaseInsensitive);
}

/// disposition 尾巴的样子：`(default)` / `(attached pic)` / `(forced)`。
///
/// 必须用白名单，不能"括号里没分隔符就算 disposition" —— 声道布局 `5.1(side)` 的
/// `(side)`、像素格式的 `(pc)` 都会因此被剥掉，变成 5.1 与 yuvj420p(pc 的错位。
bool LooksLikeDispositionText(const QString& inner) {
    static const QStringList kKnown = {
        QStringLiteral("default"),         QStringLiteral("dub"),
        QStringLiteral("original"),        QStringLiteral("comment"),
        QStringLiteral("lyrics"),          QStringLiteral("karaoke"),
        QStringLiteral("forced"),          QStringLiteral("hearing impaired"),
        QStringLiteral("visual impaired"), QStringLiteral("clean effects"),
        QStringLiteral("attached pic"),    QStringLiteral("timed thumbnails"),
        QStringLiteral("non-diegetic"),    QStringLiteral("captions"),
        QStringLiteral("descriptions"),    QStringLiteral("metadata"),
        QStringLiteral("still image"),     QStringLiteral("dependent"),
    };
    const QString body = inner.trimmed();
    return kKnown.contains(body, Qt::CaseInsensitive);
}

/// 从一段的尾部剥掉 `(default)` / `(attached pic)` 这类 disposition。
/// 它们常常粘在别的字段后面（`15360 tbn (default)`、`0 kb/s (default)`），
/// 整段匹配抓不到，必须从尾部剥。
void TakeTrailingDispositions(QString* part, QStringList* out) {
    while (part->endsWith(QLatin1Char(')'))) {
        const int open = part->lastIndexOf(QLatin1Char('('));
        if (open < 0) {
            return;
        }
        const QString inner = part->mid(open + 1, part->size() - open - 2).trimmed();
        if (!LooksLikeDispositionText(inner)) {
            return;
        }
        out->append(inner);
        *part = part->left(open).trimmed();
    }
}

/// 一段是不是"整段就是一个 disposition"（`(default)` 独占一段时走这里）。
/// 正常输出里 disposition 基本都粘在别的字段尾部，由 TakeTrailingDispositions 处理。
bool IsBareDisposition(const QString& text) {
    const QString body = text.trimmed();
    if (!body.startsWith(QLatin1Char('(')) || !body.endsWith(QLatin1Char(')'))) {
        return false;
    }
    return LooksLikeDispositionText(body.mid(1, body.size() - 2));
}

MediaStreamKind KindOfLabel(const QString& label) {
    const QString body = label.trimmed().toLower();
    if (body == QLatin1String("video")) {
        return MediaStreamKind::Video;
    }
    if (body == QLatin1String("audio")) {
        return MediaStreamKind::Audio;
    }
    if (body == QLatin1String("subtitle")) {
        return MediaStreamKind::Subtitle;
    }
    if (body == QLatin1String("data")) {
        return MediaStreamKind::Data;
    }
    if (body == QLatin1String("attachment")) {
        return MediaStreamKind::Attachment;
    }
    return MediaStreamKind::Unknown;
}

void ParseStreamParts(MediaStreamSnapshot* stream, const QString& rest) {
    const QStringList parts = SplitTopLevelComma(rest);
    if (parts.isEmpty()) {
        return;
    }
    stream->codec = ExtractCodecName(parts.at(0));
    stream->profile = ExtractProfile(parts.at(0));

    for (int i = 1; i < parts.size(); ++i) {
        QString part = parts.at(i).trimmed();
        if (part.isEmpty()) {
            continue;
        }
        if (IsBareDisposition(part)) {
            stream->dispositions.append(part.mid(1, part.size() - 2).trimmed());
            continue;
        }
        TakeTrailingDispositions(&part, &stream->dispositions);
        if (part.isEmpty()) {
            continue;
        }
        double fps = 0.0;
        if (ParseFpsText(part, &fps)) {
            stream->fps = fps;
            continue;
        }
        int rate = 0;
        if (ParseSampleRateText(part, &rate)) {
            stream->sample_rate = rate;
            continue;
        }
        const qint64 bitrate = ParseBitrateText(part);
        if (bitrate >= 0) {
            stream->bitrate_bps = bitrate;
            continue;
        }
        int width = 0;
        int height = 0;
        if (stream->kind == MediaStreamKind::Video && ParseResolution(part, &width, &height)) {
            stream->width = width;
            stream->height = height;
            const QString sar = ExtractAspect(part, QStringLiteral("SAR"));
            if (!sar.isEmpty()) {
                stream->sample_aspect = sar;
            }
            const QString dar = ExtractAspect(part, QStringLiteral("DAR"));
            if (!dar.isEmpty()) {
                stream->display_aspect = dar;
            }
            continue;
        }
        if (stream->kind == MediaStreamKind::Audio && IsChannelLayout(part)) {
            stream->channel_layout = part;
            continue;
        }
        if (stream->kind == MediaStreamKind::Audio && IsSampleFormat(part)) {
            stream->sample_fmt = part;
            continue;
        }
        if (stream->kind == MediaStreamKind::Video && stream->pix_fmt.isEmpty()) {
            QString pix_fmt;
            if (ParsePixelFormat(part, &pix_fmt)) {
                stream->pix_fmt = pix_fmt;
                continue;
            }
        }
        // 其余（tbr / tbn / 语言补充 / attached pic 以外的说明）不参与对比，丢掉。
    }

    for (const QString& disposition : stream->dispositions) {
        if (disposition.compare(QLatin1String("attached pic"), Qt::CaseInsensitive) == 0) {
            stream->attached_pic = true;
        }
    }
}

/// `Stream #0:0[0x1](und): Video: h264 ...`
bool ParseStreamLine(const QString& line, MediaStreamSnapshot* stream) {
    if (line.contains(QLatin1String(" -> "))) {
        return false; // 流映射行（"Stream #0:0 -> #0:0 (h264 -> h264)"），不是流定义
    }
    const int sep = line.indexOf(QLatin1String(": "));
    if (sep < 0) {
        return false;
    }
    const QString head = line.left(sep);
    const QString rest = line.mid(sep + 2).trimmed();

    const int hash = head.indexOf(QLatin1Char('#'));
    if (hash < 0) {
        return false;
    }
    QString index_text = head.mid(hash + 1);
    int cut = index_text.size();
    for (int i = 0; i < index_text.size(); ++i) {
        const QChar c = index_text.at(i);
        if (c == QLatin1Char('[') || c == QLatin1Char('(')) {
            cut = i;
            break;
        }
    }
    const QString tail = index_text.mid(cut);
    index_text = index_text.left(cut).trimmed();
    stream->input_label = index_text;

    const QStringList numbers = index_text.split(QLatin1Char(':'));
    if (numbers.size() == 2) {
        bool ok_program = false;
        bool ok_index = false;
        stream->program = numbers.at(0).toInt(&ok_program);
        stream->index = numbers.at(1).toInt(&ok_index);
        if (!ok_index) {
            stream->index = -1;
        }
        if (!ok_program) {
            stream->program = 0;
        }
    } else if (numbers.size() == 1) {
        stream->index = numbers.at(0).toInt();
    }

    const int lang_open = tail.lastIndexOf(QLatin1Char('('));
    const int lang_close = tail.lastIndexOf(QLatin1Char(')'));
    if (lang_open >= 0 && lang_close > lang_open) {
        const QString language = tail.mid(lang_open + 1, lang_close - lang_open - 1).trimmed();
        if (!language.isEmpty() && language.size() <= 8) {
            stream->language = language;
        }
    }

    const int colon = rest.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        return false;
    }
    stream->kind_label = rest.left(colon).trimmed();
    stream->kind = KindOfLabel(stream->kind_label);
    ParseStreamParts(stream, rest.mid(colon + 1).trimmed());
    stream->raw = line;
    return true;
}

/// `Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'in.mp4':`
void ParseInputLine(const QString& line, MediaProbeSnapshot* snapshot) {
    const int comma = line.indexOf(QLatin1String(", "));
    if (comma < 0) {
        return;
    }
    QString rest = line.mid(comma + 2);
    const int from_pos = rest.indexOf(QLatin1String(", from '"));
    if (from_pos >= 0) {
        rest = rest.left(from_pos);
    }
    snapshot->container_aliases = rest.trimmed();
    snapshot->container = snapshot->container_aliases.section(QLatin1Char(','), 0, 0).trimmed();
}

QString ExplainProbeFailure(const QString& text) {
    struct Pattern {
        const char* needle;
        const QString message;
    };
    const Pattern kPatterns[] = {
        {"No such file or directory", QStringLiteral("文件不存在，或路径写错了")},
        {"Invalid data found when processing input",
         QStringLiteral("ffmpeg 认不出这个文件的内容（可能已损坏，或只是分片）")},
        {"Protocol not found", QStringLiteral("当前 ffmpeg 不支持这个协议（地址）")},
        {"Permission denied", QStringLiteral("没有读取权限")},
        {"Server returned 404", QStringLiteral("地址返回 404")},
        {"Connection refused", QStringLiteral("连接被拒绝（地址或端口不通）")},
        {"Connection timed out", QStringLiteral("连接超时")},
    };
    for (const Pattern& pattern : kPatterns) {
        if (text.contains(QLatin1String(pattern.needle))) {
            return pattern.message;
        }
    }
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QString lowered = line.toLower();
        if (lowered.contains(QLatin1String("error")) || lowered.contains(QLatin1String("failed")) ||
            lowered.contains(QLatin1String("invalid"))) {
            return line.size() > 160 ? line.left(160) + QStringLiteral("…") : line;
        }
    }
    return QStringLiteral("ffmpeg 没有输出任何流信息");
}

} // namespace

QStringList BuildMediaProbeArguments(const QString& path) {
    // -nostdin: 这条命令是"查完就退", 别让它等 stdin。
    return {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-i"), path};
}

MediaProbeSnapshot ParseMediaProbe(const QString& stderr_text, const QString& path) {
    MediaProbeSnapshot snapshot;
    snapshot.path = path;

    const QStringList lines = stderr_text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1String("Input #"))) {
            if (snapshot.parsed) {
                break; // 多输入只取第一个（合并由调用方按输入组做）
            }
            snapshot.parsed = true;
            ParseInputLine(line, &snapshot);
            continue;
        }
        if (line.startsWith(QLatin1String("Output #"))) {
            break; // 后面是输出块与映射日志，不属于"输入素材"
        }
        if (line.startsWith(QLatin1String("Duration:"))) {
            const QStringList parts = SplitTopLevelComma(line.mid(9).trimmed());
            if (!parts.isEmpty()) {
                snapshot.duration_seconds = ParseClockText(parts.at(0));
            }
            for (int i = 1; i < parts.size(); ++i) {
                const QString part = parts.at(i);
                if (part.startsWith(QLatin1String("start:"), Qt::CaseInsensitive)) {
                    bool ok = false;
                    const double start = part.mid(6).trimmed().toDouble(&ok);
                    snapshot.start_seconds = ok ? start : 0.0;
                } else if (part.startsWith(QLatin1String("bitrate:"), Qt::CaseInsensitive)) {
                    snapshot.bitrate_bps = ParseBitrateText(part.mid(8));
                }
            }
            continue;
        }
        if (line.startsWith(QLatin1String("Stream #"))) {
            MediaStreamSnapshot stream;
            if (ParseStreamLine(line, &stream)) {
                snapshot.streams.push_back(stream);
            }
        }
    }

    if (!snapshot.parsed) {
        snapshot.error = ExplainProbeFailure(stderr_text);
    } else if (snapshot.streams.empty()) {
        snapshot.error = QStringLiteral("读到了容器信息，但一条流都没解析出来");
    }
    return snapshot;
}

void FillLocalFileFacts(MediaProbeSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }
    snapshot->file_size_bytes = -1;
    if (snapshot->path.isEmpty() || !IsLocalPathTarget(snapshot->path)) {
        return;
    }
    const QFileInfo info(snapshot->path);
    if (info.exists() && info.isFile()) {
        snapshot->file_size_bytes = info.size();
    }
}

int MediaProbeSnapshot::CountStreams(MediaStreamKind kind) const {
    int count = 0;
    for (const MediaStreamSnapshot& stream : streams) {
        if (stream.kind == kind) {
            ++count;
        }
    }
    return count;
}

int MediaProbeSnapshot::CountPlayableStreams(MediaStreamKind kind) const {
    int count = 0;
    for (const MediaStreamSnapshot& stream : streams) {
        if (stream.kind == kind && !stream.attached_pic) {
            ++count;
        }
    }
    return count;
}

const MediaStreamSnapshot* MediaProbeSnapshot::FirstStream(MediaStreamKind kind) const {
    for (const MediaStreamSnapshot& stream : streams) {
        if (stream.kind == kind) {
            return &stream;
        }
    }
    return nullptr;
}

QString FormatDurationHuman(double seconds) {
    if (seconds < 0.0) {
        return QStringLiteral("—");
    }
    const qint64 tenths = static_cast<qint64>(seconds * 10.0 + 0.5);
    const qint64 hours = tenths / 36000;
    const qint64 minutes = (tenths / 600) % 60;
    const qint64 secs = (tenths % 600) / 10;
    const qint64 fraction = tenths % 10;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3.%4")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'))
            .arg(fraction);
    }
    return QStringLiteral("%1:%2.%3").arg(minutes).arg(secs, 2, 10, QLatin1Char('0')).arg(fraction);
}

QString FormatBitrateHuman(qint64 bps) {
    if (bps < 0) {
        return QStringLiteral("—");
    }
    if (bps >= 1000000) {
        return QStringLiteral("%1 Mbps").arg(QString::number(static_cast<double>(bps) / 1000000.0, 'f', 2));
    }
    if (bps >= 1000) {
        return QStringLiteral("%1 kbps").arg(QString::number(static_cast<double>(bps) / 1000.0, 'f', 0));
    }
    return QStringLiteral("%1 bps").arg(bps);
}

QString FormatFpsHuman(double fps) {
    if (fps <= 0.0) {
        return QStringLiteral("—");
    }
    const double rounded = static_cast<double>(qRound64(fps));
    if (qAbs(fps - rounded) < 0.005) {
        return QStringLiteral("%1 fps").arg(static_cast<qint64>(rounded));
    }
    return QStringLiteral("%1 fps").arg(QString::number(fps, 'f', 2));
}

QString FormatResolutionHuman(int width, int height) {
    if (width <= 0 || height <= 0) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1×%2").arg(width).arg(height);
}

QString MediaStreamKindName(MediaStreamKind kind) {
    switch (kind) {
    case MediaStreamKind::Video:
        return QStringLiteral("视频");
    case MediaStreamKind::Audio:
        return QStringLiteral("音频");
    case MediaStreamKind::Subtitle:
        return QStringLiteral("字幕");
    case MediaStreamKind::Data:
        return QStringLiteral("数据");
    case MediaStreamKind::Attachment:
        return QStringLiteral("附件");
    case MediaStreamKind::Unknown:
        break;
    }
    return QStringLiteral("未知");
}

QString DescribeStreamSummary(const MediaStreamSnapshot& stream) {
    QStringList parts;
    if (!stream.codec.isEmpty()) {
        parts << (stream.profile.isEmpty() ? stream.codec : stream.codec + QLatin1Char(' ') + stream.profile);
    }
    if (stream.kind == MediaStreamKind::Video) {
        if (stream.has_size()) {
            parts << FormatResolutionHuman(stream.width, stream.height);
        }
        if (stream.fps > 0.0) {
            parts << FormatFpsHuman(stream.fps);
        }
        if (!stream.pix_fmt.isEmpty()) {
            parts << stream.pix_fmt;
        }
    } else if (stream.kind == MediaStreamKind::Audio) {
        if (stream.sample_rate > 0) {
            parts << QStringLiteral("%1 Hz").arg(stream.sample_rate);
        }
        if (!stream.channel_layout.isEmpty()) {
            parts << stream.channel_layout;
        }
    } else if (!stream.language.isEmpty()) {
        parts << stream.language;
    }
    if (stream.bitrate_bps >= 0) {
        parts << FormatBitrateHuman(stream.bitrate_bps);
    }
    if (stream.attached_pic) {
        parts << QStringLiteral("封面图");
    }
    if (parts.isEmpty()) {
        return QStringLiteral("未读到参数");
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace ffmpeg
} // namespace videoeye
