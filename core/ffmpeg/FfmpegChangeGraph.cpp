#include "core/ffmpeg/FfmpegChangeGraph.h"

#include <QFileInfo>
#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegFilterGraph.h"

namespace videoeye {
namespace ffmpeg {
namespace {

// ---------------------------------------------------------------- 命令声明

struct DeclaredOptions {
    QString video_codec; // "libx265" / "copy"；空 = 没写
    QString audio_codec;
    QString subtitle_codec;
    QString video_option; // 生效的那个选项名（"-c:v" / "-c"），用于核对
    QString audio_option;
    QString crf;
    QString video_bitrate;
    QString audio_bitrate;
    QString preset;
    QString scale; // -s 的值（注意：scale 滤镜也能改尺寸，那条走滤镜）
    QString fps;   // -r 的值
    QString pix_fmt;
    QString sample_rate;    // -ar
    QString audio_channels; // -ac
    bool drop_video = false;
    bool drop_audio = false;
    bool drop_subtitle = false;
    QString vf;
    QString af;
    QString filter_complex;
    QStringList maps;
};

/// 最后一个同名选项生效（ffmpeg 里后写的覆盖先写的）。
QString LastOptionValue(const QStringList& arguments, const FfmpegCommandStructure& structure, const QStringList& names,
                        QString* option_name = nullptr) {
    QString value;
    for (size_t i = 0; i < structure.args.size(); ++i) {
        const FfmpegArgPlacement& place = structure.args.at(i);
        if (place.role != FfmpegArgRole::Option || place.value_index < 0) {
            continue;
        }
        const QString name = arguments.at(static_cast<int>(i));
        if (!names.contains(name, Qt::CaseInsensitive)) {
            continue;
        }
        value = arguments.at(place.value_index);
        if (option_name != nullptr) {
            *option_name = name;
        }
    }
    return value;
}

bool HasFlag(const QStringList& arguments, const FfmpegCommandStructure& structure, const QString& name) {
    for (size_t i = 0; i < structure.args.size(); ++i) {
        if (structure.args.at(i).role != FfmpegArgRole::Option) {
            continue;
        }
        if (arguments.at(static_cast<int>(i)).compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

void CollectRepeated(const QStringList& arguments, const FfmpegCommandStructure& structure, const QString& name,
                     QStringList* out) {
    for (size_t i = 0; i < structure.args.size(); ++i) {
        const FfmpegArgPlacement& place = structure.args.at(i);
        if (place.role != FfmpegArgRole::Option || place.value_index < 0) {
            continue;
        }
        if (arguments.at(static_cast<int>(i)).compare(name, Qt::CaseInsensitive) == 0) {
            out->append(arguments.at(place.value_index));
        }
    }
}

/// `-c` / `-codec` 管所有流；`-c:v` / `-vcodec` 管视频；`-c:a` / `-acodec` 管音频；
/// `-c:s` / `-scodec` 管字幕。带流序号（`-c:v:0`）的只覆盖单路，不参与整类判定。
void ReadCodecs(const QStringList& arguments, const FfmpegCommandStructure& structure, DeclaredOptions* out) {
    for (size_t i = 0; i < structure.args.size(); ++i) {
        const FfmpegArgPlacement& place = structure.args.at(i);
        if (place.role != FfmpegArgRole::Option || place.value_index < 0) {
            continue;
        }
        QString name = arguments.at(static_cast<int>(i)).toLower();
        const QString value = arguments.at(place.value_index);
        if (name == QLatin1String("-codec")) {
            name = QStringLiteral("-c");
        }
        if (name.startsWith(QLatin1String("-codec:"))) {
            name = QStringLiteral("-c") + name.mid(6);
        }
        if (name.count(QLatin1Char(':')) > 1) {
            continue; // -c:v:0：只管单路
        }
        if (name == QLatin1String("-c") || name == QLatin1String("-codec")) {
            out->video_codec = value;
            out->audio_codec = value;
            out->subtitle_codec = value;
            out->video_option = arguments.at(static_cast<int>(i));
            out->audio_option = arguments.at(static_cast<int>(i));
        } else if (name == QLatin1String("-c:v") || name == QLatin1String("-vcodec")) {
            out->video_codec = value;
            out->video_option = arguments.at(static_cast<int>(i));
        } else if (name == QLatin1String("-c:a") || name == QLatin1String("-acodec")) {
            out->audio_codec = value;
            out->audio_option = arguments.at(static_cast<int>(i));
        } else if (name == QLatin1String("-c:s") || name == QLatin1String("-scodec")) {
            out->subtitle_codec = value;
        }
    }
}

DeclaredOptions ReadDeclared(const QStringList& arguments, const FfmpegCommandStructure& structure) {
    DeclaredOptions out;
    ReadCodecs(arguments, structure, &out);
    out.crf = LastOptionValue(arguments, structure, {QStringLiteral("-crf")});
    out.video_bitrate = LastOptionValue(arguments, structure, {QStringLiteral("-b:v"), QStringLiteral("-vb")});
    out.audio_bitrate = LastOptionValue(arguments, structure, {QStringLiteral("-b:a"), QStringLiteral("-ab")});
    out.preset = LastOptionValue(arguments, structure, {QStringLiteral("-preset")});
    out.scale = LastOptionValue(arguments, structure, {QStringLiteral("-s"), QStringLiteral("-video_size")});
    out.fps = LastOptionValue(arguments, structure, {QStringLiteral("-r"), QStringLiteral("-framerate")});
    out.pix_fmt = LastOptionValue(arguments, structure, {QStringLiteral("-pix_fmt")});
    out.sample_rate = LastOptionValue(arguments, structure, {QStringLiteral("-ar")});
    out.audio_channels = LastOptionValue(arguments, structure, {QStringLiteral("-ac")});
    out.vf = LastOptionValue(arguments, structure, {QStringLiteral("-vf"), QStringLiteral("-filter:v")});
    out.af = LastOptionValue(arguments, structure, {QStringLiteral("-af"), QStringLiteral("-filter:a")});
    out.filter_complex = LastOptionValue(arguments, structure, {QStringLiteral("-filter_complex")});
    CollectRepeated(arguments, structure, QStringLiteral("-map"), &out.maps);
    out.drop_video = HasFlag(arguments, structure, QStringLiteral("-vn"));
    out.drop_audio = HasFlag(arguments, structure, QStringLiteral("-an"));
    out.drop_subtitle = HasFlag(arguments, structure, QStringLiteral("-sn"));
    return out;
}

// ---------------------------------------------------------------- 配对

bool IsPlayablePairCandidate(const MediaStreamSnapshot& stream) {
    // 封面图不参与"视频路数"配对：它是元数据里的图，不是一路可播视频。
    return !stream.attached_pic;
}

struct MapTarget {
    int input_stream = -1;
    MediaStreamKind kind = MediaStreamKind::Unknown;
};

/// 把 `-map` 展开成"每个输出流对应的输入流"。
///
/// 只支持能确定性解释的写法，其余一律返回 false 让调用方退回按类型配对 ——
/// 猜错配对比不配对更糟（会把 aac → opus 说成 aac → aac 未变）。
///   -map 0:v      该输入的全部视频流（ffmpeg 的语义，不是"第一路"）
///   -map 0:v:0    第一路视频
///   -map 0:1      流序号 1
///   -map 0        该输入的全部流
///   -map [v]      滤镜输出标签 → 不解释
///   -map -0:a     排除 → 不解释
bool ExpandMaps(const QStringList& maps, const MediaProbeSnapshot& input, std::vector<MapTarget>* out) {
    for (const QString& raw : maps) {
        const QString map = raw.trimmed();
        if (map.isEmpty() || map.startsWith(QLatin1Char('-')) || map.contains(QLatin1Char('['))) {
            return false;
        }
        const QStringList sections = map.split(QLatin1Char(':'));
        if (sections.isEmpty()) {
            return false;
        }
        bool ok_input = false;
        const int input_index = sections.at(0).toInt(&ok_input);
        if (!ok_input || input_index != 0) {
            return false; // 只探测了第一个输入；其余输入无法核对
        }
        if (sections.size() == 1) {
            for (const MediaStreamSnapshot& stream : input.streams) {
                if (IsPlayablePairCandidate(stream)) {
                    out->push_back({stream.index, stream.kind});
                }
            }
            continue;
        }
        const QString selector = sections.at(1).trimmed();
        if (selector.isEmpty() || selector.size() > 2) {
            return false;
        }
        const QChar head = selector.at(0);
        if (head.isDigit()) {
            bool ok_stream = false;
            const int stream_index = selector.toInt(&ok_stream);
            if (!ok_stream) {
                return false;
            }
            const MediaStreamSnapshot* found = nullptr;
            for (const MediaStreamSnapshot& stream : input.streams) {
                if (stream.index == stream_index) {
                    found = &stream;
                    break;
                }
            }
            if (found == nullptr) {
                return false;
            }
            out->push_back({found->index, found->kind});
            continue;
        }
        MediaStreamKind kind = MediaStreamKind::Unknown;
        switch (head.toLower().unicode()) {
        case 'v':
            kind = MediaStreamKind::Video;
            break;
        case 'a':
            kind = MediaStreamKind::Audio;
            break;
        case 's':
            kind = MediaStreamKind::Subtitle;
            break;
        case 'd':
            kind = MediaStreamKind::Data;
            break;
        case 't':
            kind = MediaStreamKind::Attachment;
            break;
        default:
            return false;
        }
        int within_kind = -1;
        if (sections.size() > 2) {
            bool ok_within = false;
            within_kind = sections.at(2).toInt(&ok_within);
            if (!ok_within) {
                return false;
            }
        }
        int seen = 0;
        bool found_any = false;
        for (const MediaStreamSnapshot& stream : input.streams) {
            if (stream.kind != kind || !IsPlayablePairCandidate(stream)) {
                continue;
            }
            if (within_kind >= 0) {
                if (seen == within_kind) {
                    out->push_back({stream.index, stream.kind});
                    found_any = true;
                    break;
                }
                ++seen;
                continue;
            }
            out->push_back({stream.index, stream.kind});
            found_any = true;
        }
        if (!found_any) {
            return false;
        }
    }
    return true;
}

int IndexOfInputStream(const MediaProbeSnapshot& input, int stream_index) {
    for (size_t i = 0; i < input.streams.size(); ++i) {
        if (input.streams.at(i).index == stream_index) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------- 字段对比

FieldChange MakeTextField(const QString& key, const QString& label, const QString& before, const QString& after) {
    FieldChange change;
    change.key = key;
    change.label = label;
    change.before = before;
    change.after = after;
    change.source = ChangeSource::Measured;
    if (before.isEmpty() || after.isEmpty()) {
        change.state = ChangeState::Unknown;
    } else {
        change.state = before == after ? ChangeState::Unchanged : ChangeState::Changed;
    }
    return change;
}

/// tolerance 是相对误差。帧率 29.97 vs 29.970001、码率 ±0.5% 属于同一个值 ——
/// 把它们画成"变了"会让每一次重编码都亮一片无意义的高亮。
FieldChange MakeNumericField(const QString& key, const QString& label, const QString& before, const QString& after,
                             double before_value, double after_value, double tolerance = 0.005) {
    FieldChange change = MakeTextField(key, label, before, after);
    change.numeric = true;
    change.before_value = before_value;
    change.after_value = after_value;
    if (!before.isEmpty() && before == after) {
        // 显示精度之内的差异不算变化。报一个"变了"却给两个一模一样的数
        // （时长 0:01.0 → 0:01.0），比不报更糟 —— 用户会开始怀疑整张图。
        change.state = ChangeState::Unchanged;
        return change;
    }
    if (before_value <= 0.0 || after_value <= 0.0) {
        change.state = ChangeState::Unknown;
        return change;
    }
    if (change.before.isEmpty() || change.after.isEmpty()) {
        change.state = ChangeState::Unknown;
        return change;
    }
    const double baseline = std::max(std::fabs(before_value), std::fabs(after_value));
    const double delta = std::fabs(after_value - before_value);
    change.state = (baseline > 0.0 && delta / baseline <= tolerance) ? ChangeState::Unchanged : ChangeState::Changed;
    return change;
}

QString CodecText(const MediaStreamSnapshot& stream) {
    if (stream.codec.isEmpty()) {
        return QString();
    }
    return stream.profile.isEmpty() ? stream.codec : stream.codec + QLatin1Char(' ') + stream.profile;
}

std::vector<FieldChange> BuildStreamChanges(const MediaStreamSnapshot& before, const MediaStreamSnapshot& after) {
    std::vector<FieldChange> changes;

    const QString codec_before = CodecText(before);
    const QString codec_after = CodecText(after);
    if (!codec_before.isEmpty() || !codec_after.isEmpty()) {
        changes.push_back(MakeTextField(QStringLiteral("codec"), QStringLiteral("编码"), codec_before, codec_after));
    }

    if (before.kind == MediaStreamKind::Video || after.kind == MediaStreamKind::Video) {
        const bool size_before = before.has_size();
        const bool size_after = after.has_size();
        if (size_before || size_after) {
            changes.push_back(
                MakeTextField(QStringLiteral("size"), QStringLiteral("分辨率"),
                              size_before ? FormatResolutionHuman(before.width, before.height) : QString(),
                              size_after ? FormatResolutionHuman(after.width, after.height) : QString()));
        }
        if (before.fps > 0.0 || after.fps > 0.0) {
            changes.push_back(MakeNumericField(QStringLiteral("fps"), QStringLiteral("帧率"),
                                               before.fps > 0.0 ? FormatFpsHuman(before.fps) : QString(),
                                               after.fps > 0.0 ? FormatFpsHuman(after.fps) : QString(), before.fps,
                                               after.fps));
        }
        if (!before.pix_fmt.isEmpty() || !after.pix_fmt.isEmpty()) {
            changes.push_back(
                MakeTextField(QStringLiteral("pix_fmt"), QStringLiteral("像素格式"), before.pix_fmt, after.pix_fmt));
        }
    }

    if (before.kind == MediaStreamKind::Audio || after.kind == MediaStreamKind::Audio) {
        const bool rate_before = before.sample_rate > 0;
        const bool rate_after = after.sample_rate > 0;
        if (rate_before || rate_after) {
            changes.push_back(
                MakeNumericField(QStringLiteral("sample_rate"), QStringLiteral("采样率"),
                                 rate_before ? QStringLiteral("%1 Hz").arg(before.sample_rate) : QString(),
                                 rate_after ? QStringLiteral("%1 Hz").arg(after.sample_rate) : QString(),
                                 before.sample_rate, after.sample_rate, 0.0));
        }
        if (!before.channel_layout.isEmpty() || !after.channel_layout.isEmpty()) {
            changes.push_back(MakeTextField(QStringLiteral("channels"), QStringLiteral("声道"), before.channel_layout,
                                            after.channel_layout));
        }
    }

    if (before.bitrate_bps >= 0 || after.bitrate_bps >= 0) {
        changes.push_back(MakeNumericField(QStringLiteral("bitrate"), QStringLiteral("码率"),
                                           before.bitrate_bps >= 0 ? FormatBitrateHuman(before.bitrate_bps) : QString(),
                                           after.bitrate_bps >= 0 ? FormatBitrateHuman(after.bitrate_bps) : QString(),
                                           before.bitrate_bps, after.bitrate_bps));
    }
    return changes;
}

ChangeState AggregateState(const std::vector<FieldChange>& changes) {
    bool any_changed = false;
    bool any_unchanged = false;
    bool any_known = false;
    for (const FieldChange& change : changes) {
        switch (change.state) {
        case ChangeState::Changed:
            any_changed = true;
            any_known = true;
            break;
        case ChangeState::Unchanged:
            any_unchanged = true;
            any_known = true;
            break;
        default:
            break;
        }
    }
    if (any_changed) {
        return ChangeState::Changed;
    }
    if (any_known && any_unchanged) {
        return ChangeState::Unchanged;
    }
    return ChangeState::Unknown;
}

QString BuildEncoderDetail(const DeclaredOptions& declared, MediaStreamKind kind) {
    QStringList parts;
    if (kind == MediaStreamKind::Video) {
        if (!declared.crf.isEmpty()) {
            parts << QStringLiteral("crf %1").arg(declared.crf);
        }
        if (!declared.video_bitrate.isEmpty()) {
            parts << QStringLiteral("-b:v %1").arg(declared.video_bitrate);
        }
        if (!declared.preset.isEmpty()) {
            parts << QStringLiteral("preset %1").arg(declared.preset);
        }
        if (!declared.scale.isEmpty()) {
            parts << QStringLiteral("-s %1").arg(declared.scale);
        }
        if (!declared.fps.isEmpty()) {
            parts << QStringLiteral("-r %1").arg(declared.fps);
        }
        if (!declared.pix_fmt.isEmpty()) {
            parts << declared.pix_fmt;
        }
    } else if (kind == MediaStreamKind::Audio) {
        if (!declared.audio_bitrate.isEmpty()) {
            parts << QStringLiteral("-b:a %1").arg(declared.audio_bitrate);
        }
        if (!declared.sample_rate.isEmpty()) {
            parts << QStringLiteral("-ar %1").arg(declared.sample_rate);
        }
        if (!declared.audio_channels.isEmpty()) {
            parts << QStringLiteral("-ac %1").arg(declared.audio_channels);
        }
    }
    return parts.join(QStringLiteral(" · "));
}

void FillEncoder(const DeclaredOptions& declared, MediaStreamKind kind, GraphRow* row) {
    QString codec;
    QString option;
    bool drop = false;
    if (kind == MediaStreamKind::Video) {
        codec = declared.video_codec;
        option = declared.video_option;
        drop = declared.drop_video;
    } else if (kind == MediaStreamKind::Audio) {
        codec = declared.audio_codec;
        option = declared.audio_option;
        drop = declared.drop_audio;
    } else if (kind == MediaStreamKind::Subtitle) {
        codec = declared.subtitle_codec;
        drop = declared.drop_subtitle;
    }

    const QString detail = BuildEncoderDetail(declared, kind);
    if (drop) {
        row->encoder_kind = ProcessKind::Drop;
        row->encoder_label = QStringLiteral("丢弃");
        row->encoder_detail = kind == MediaStreamKind::Video
                                  ? QStringLiteral("按声明不写入视频流（-vn）")
                                  : (kind == MediaStreamKind::Audio ? QStringLiteral("按声明不写入音频流（-an）")
                                                                    : QStringLiteral("按声明不写入字幕流（-sn）"));
        return;
    }
    if (codec.compare(QLatin1String("copy"), Qt::CaseInsensitive) == 0) {
        row->encoder_kind = ProcessKind::Copy;
        row->encoder_label = QStringLiteral("copy");
        row->encoder_detail = detail.isEmpty() ? QStringLiteral("不重编码，原样搬运（实际参数看下方实测）")
                                               : QStringLiteral("不重编码 · %1").arg(detail);
        return;
    }
    if (!codec.isEmpty()) {
        row->encoder_kind = ProcessKind::Encoder;
        row->encoder_label = codec;
        row->encoder_detail = detail.isEmpty()
                                  ? QStringLiteral("重编码（%1）").arg(option.isEmpty() ? QStringLiteral("-c") : option)
                                  : QStringLiteral("重编码 · %1").arg(detail);
        return;
    }
    row->encoder_kind = ProcessKind::Default;
    row->encoder_label = QStringLiteral("未指定");
    row->encoder_detail = QStringLiteral("命令没写编码器，由 ffmpeg 按容器选默认（实际结果看实测）");
}

QStringList FilterLabels(const QString& filter_text, bool complex_graph) {
    QStringList labels;
    if (filter_text.isEmpty()) {
        return labels;
    }
    const FfmpegFilterGraph graph = ParseFilterGraph(filter_text, complex_graph);
    if (!graph.parse_ok) {
        labels << QStringLiteral("滤镜串未能拆解");
        return labels;
    }
    for (const FfmpegFilterChain& chain : graph.chains) {
        for (const FfmpegFilterNode& node : chain.nodes) {
            labels << (node.arguments.isEmpty() ? node.name : node.name + QLatin1Char(' ') + node.arguments);
        }
    }
    return labels;
}

} // namespace

std::vector<ChangeGraph> BuildChangeGraphs(const MediaProbeSnapshot& input, const QStringList& arguments,
                                           const std::vector<MediaProbeSnapshot>& measured) {
    std::vector<ChangeGraph> graphs;
    if (!input.parsed) {
        ChangeGraph graph;
        graph.note = input.error.isEmpty() ? QStringLiteral("输入文件没有探测成功") : input.error;
        graphs.push_back(graph);
        return graphs;
    }
    if (IsInformationCommand(arguments)) {
        return graphs; // 查信息的命令没有"进出素材"，不该画图谱
    }

    const FfmpegCommandStructure structure = AnalyzeCommandStructure(arguments);
    if (structure.output_count == 0) {
        ChangeGraph graph;
        graph.note = QStringLiteral("命令里没有输出目标，没有可对比的产出");
        graphs.push_back(graph);
        return graphs;
    }
    if (!structure.complete) {
        ChangeGraph graph;
        graph.note = QStringLiteral("命令结构不完整（有参数归属不明），图谱可能失准");
        graphs.push_back(graph);
        return graphs;
    }

    const DeclaredOptions declared = ReadDeclared(arguments, structure);

    // 收集输出目标（按命令里的顺序），与 measured 一一对应。
    QStringList targets;
    for (const FfmpegArgPlacement& place : structure.args) {
        if (place.role == FfmpegArgRole::OutputUrl) {
            targets << arguments.at(place.index);
        }
    }

    // 多输出时不给"声明层"的工序归属：`-c:v` 到底作用于哪个输出，AnalyzeCommandStructure
    // 并没有记录，硬套就会把 out1 的编码器画到 out2 上。
    const bool single_output = targets.size() == 1;

    for (int output_i = 0; output_i < targets.size(); ++output_i) {
        ChangeGraph graph;
        graph.target = targets.at(output_i);
        graph.ok = true;
        graph.container_before = input.container;

        MediaProbeSnapshot output;
        if (output_i < static_cast<int>(measured.size())) {
            output = measured.at(static_cast<size_t>(output_i));
        }
        graph.output_measured = output.parsed;
        if (!output.parsed) {
            graph.output_error = output.error;
        } else {
            graph.container_after = output.container;
        }

        if (!single_output) {
            graph.note = QStringLiteral("多输出命令：工序按整条命令给出，未必只作用于这个输出");
        }

        // ---- 工序节点（声明层）----
        if (single_output) {
            graph.has_complex_filter = !declared.filter_complex.isEmpty();
        }

        // ---- 行：输入流 → 输出流 ----
        struct RowSeed {
            int input_index = -1;
            int output_index = -1;
        };
        std::vector<RowSeed> seeds;
        bool mapped = false;

        if (single_output && !declared.maps.isEmpty()) {
            std::vector<MapTarget> expanded;
            if (ExpandMaps(declared.maps, input, &expanded) && output.parsed &&
                expanded.size() == output.streams.size()) {
                bool kinds_match = true;
                for (size_t k = 0; k < expanded.size(); ++k) {
                    if (expanded.at(k).kind != output.streams.at(k).kind) {
                        kinds_match = false;
                        break;
                    }
                }
                if (kinds_match) {
                    mapped = true;
                    for (size_t k = 0; k < expanded.size(); ++k) {
                        const int input_index = IndexOfInputStream(input, expanded.at(k).input_stream);
                        seeds.push_back({input_index, static_cast<int>(k)});
                    }
                    graph.pairing_note = QStringLiteral("按 -map 配对");
                    // 被 -map 遗漏的输入流也要出一行 —— "只映射了音频，视频被丢了"
                    // 正是用户最需要看到的那种变化，不能因为配不上就消失。
                    std::vector<int> used;
                    for (const RowSeed& seed : seeds) {
                        used.push_back(seed.input_index);
                    }
                    for (size_t i = 0; i < input.streams.size(); ++i) {
                        if (!IsPlayablePairCandidate(input.streams.at(i))) {
                            continue;
                        }
                        if (std::find(used.begin(), used.end(), static_cast<int>(i)) != used.end()) {
                            continue;
                        }
                        seeds.push_back({static_cast<int>(i), -1});
                    }
                }
            }
            if (!mapped && output.parsed) {
                graph.pairing_note =
                    QStringLiteral("-map 用法无法确定（滤镜标签 / 排除 / 跨输入），改为按类型顺序配对");
            }
        }

        if (!mapped) {
            static const MediaStreamKind kKinds[] = {MediaStreamKind::Video, MediaStreamKind::Audio,
                                                     MediaStreamKind::Subtitle, MediaStreamKind::Data,
                                                     MediaStreamKind::Attachment};
            for (const MediaStreamKind kind : kKinds) {
                std::vector<int> input_side;
                std::vector<int> output_side;
                for (size_t i = 0; i < input.streams.size(); ++i) {
                    if (input.streams.at(i).kind == kind && IsPlayablePairCandidate(input.streams.at(i))) {
                        input_side.push_back(static_cast<int>(i));
                    }
                }
                for (size_t i = 0; i < output.streams.size(); ++i) {
                    if (output.streams.at(i).kind == kind && IsPlayablePairCandidate(output.streams.at(i))) {
                        output_side.push_back(static_cast<int>(i));
                    }
                }
                const size_t paired = std::min(input_side.size(), output_side.size());
                for (size_t k = 0; k < paired; ++k) {
                    seeds.push_back({input_side.at(k), output_side.at(k)});
                }
                for (size_t k = paired; k < input_side.size(); ++k) {
                    seeds.push_back({input_side.at(k), -1});
                }
                for (size_t k = paired; k < output_side.size(); ++k) {
                    seeds.push_back({-1, output_side.at(k)});
                }
            }
            if (!output.parsed) {
                // 没有输出就没有"配对"这回事，别留一句看起来像已经配过的话。
                graph.pairing_note.clear();
            } else if (graph.pairing_note.isEmpty()) {
                graph.pairing_note = declared.maps.isEmpty() ? QStringLiteral("按类型顺序配对（命令里没有 -map）")
                                                             : QStringLiteral("按类型顺序配对（-map 未能解释）");
            }
        }

        // 没探测到输出时，行只能来自输入侧 —— 否则会画出一排"输出：未知"的空节点。
        if (!output.parsed) {
            seeds.clear();
            for (size_t i = 0; i < input.streams.size(); ++i) {
                if (IsPlayablePairCandidate(input.streams.at(i))) {
                    seeds.push_back({static_cast<int>(i), -1});
                }
            }
        }

        for (const RowSeed& seed : seeds) {
            GraphRow row;
            if (seed.input_index >= 0) {
                const MediaStreamSnapshot& stream = input.streams.at(static_cast<size_t>(seed.input_index));
                row.kind = stream.kind;
                row.input_index = seed.input_index;
                row.input_label = stream.input_label;
                row.input_summary = DescribeStreamSummary(stream);
            }
            if (seed.output_index >= 0) {
                const MediaStreamSnapshot& stream = output.streams.at(static_cast<size_t>(seed.output_index));
                row.kind = stream.kind;
                row.output_index = seed.output_index;
                row.output_label = stream.input_label;
                row.output_summary = DescribeStreamSummary(stream);
            }

            FillEncoder(declared, row.kind, &row);

            if (single_output) {
                if (row.kind == MediaStreamKind::Video) {
                    row.filter_labels = FilterLabels(declared.vf, false);
                } else if (row.kind == MediaStreamKind::Audio) {
                    row.filter_labels = FilterLabels(declared.af, false);
                }
            }

            if (row.input_index >= 0 && row.output_index >= 0) {
                row.changes = BuildStreamChanges(input.streams.at(static_cast<size_t>(row.input_index)),
                                                 output.streams.at(static_cast<size_t>(row.output_index)));
                row.state = AggregateState(row.changes);
            } else if (row.input_index >= 0) {
                row.state = output.parsed ? ChangeState::Removed : ChangeState::Unknown;
                if (output.parsed) {
                    if (row.encoder_kind == ProcessKind::Drop) {
                        row.note = QStringLiteral("按声明丢弃（-vn / -an），输出里确实没有这一路");
                    } else if (!declared.maps.isEmpty()) {
                        row.note = QStringLiteral("未被 -map 选中，输出里没有这一路");
                    } else {
                        row.note = QStringLiteral("输出里没有对应的这一路");
                    }
                } else {
                    row.note = QStringLiteral("输出未探测：运行时才有实测参数");
                }
            } else {
                row.state = ChangeState::Added;
                row.note = QStringLiteral("输出里新生成的流（滤镜 / lavfi），没有对应的输入流");
            }
            if (row.filter_labels.isEmpty() && graph.has_complex_filter) {
                row.note = row.note.isEmpty() ? QStringLiteral("工序走 -filter_complex，未逐路归属")
                                              : row.note + QStringLiteral("；工序走 -filter_complex，未逐路归属");
            }
            graph.rows.push_back(row);
        }

        // ---- 容器级对比 ----
        if (output.parsed) {
            FieldChange container =
                MakeTextField(QStringLiteral("container"), QStringLiteral("容器"), input.container, output.container);
            const QString before_suffix = QFileInfo(input.path).suffix().toLower();
            const QString after_suffix = QFileInfo(graph.target).suffix().toLower();
            if (container.state == ChangeState::Unchanged && !before_suffix.isEmpty() && !after_suffix.isEmpty() &&
                before_suffix != after_suffix) {
                container.note = QStringLiteral("同一个 demuxer（%1），扩展名 %2 → %3 不同")
                                     .arg(input.container_aliases, before_suffix, after_suffix);
            }
            graph.container_changes.push_back(container);

            graph.container_changes.push_back(MakeNumericField(
                QStringLiteral("duration"), QStringLiteral("时长"), FormatDurationHuman(input.duration_seconds),
                FormatDurationHuman(output.duration_seconds), input.duration_seconds, output.duration_seconds));

            graph.container_changes.push_back(
                MakeNumericField(QStringLiteral("total_bitrate"), QStringLiteral("总码率"),
                                 FormatBitrateHuman(input.bitrate_bps), FormatBitrateHuman(output.bitrate_bps),
                                 static_cast<double>(input.bitrate_bps), static_cast<double>(output.bitrate_bps)));

            if (input.file_size_bytes >= 0 && output.file_size_bytes >= 0) {
                FieldChange size = MakeNumericField(
                    QStringLiteral("file_size"), QStringLiteral("体积"),
                    QStringLiteral("%1 MB").arg(
                        QString::number(static_cast<double>(input.file_size_bytes) / 1048576.0, 'f', 2)),
                    QStringLiteral("%1 MB").arg(
                        QString::number(static_cast<double>(output.file_size_bytes) / 1048576.0, 'f', 2)),
                    static_cast<double>(input.file_size_bytes), static_cast<double>(output.file_size_bytes));
                graph.container_changes.push_back(size);
            }

            const auto counts = [](const MediaProbeSnapshot& snapshot) {
                return QStringLiteral("%1 视频 / %2 音频 / %3 字幕")
                    .arg(snapshot.CountPlayableStreams(MediaStreamKind::Video))
                    .arg(snapshot.CountPlayableStreams(MediaStreamKind::Audio))
                    .arg(snapshot.CountPlayableStreams(MediaStreamKind::Subtitle));
            };
            graph.container_changes.push_back(
                MakeTextField(QStringLiteral("stream_count"), QStringLiteral("流数"), counts(input), counts(output)));
        }

        graphs.push_back(graph);
    }
    return graphs;
}

int ChangeGraph::CountRows(ChangeState state) const {
    int count = 0;
    for (const GraphRow& row : rows) {
        if (row.state == state) {
            ++count;
        }
    }
    return count;
}

int ChangeGraph::CountChanges(ChangeState state) const {
    int count = 0;
    for (const FieldChange& change : container_changes) {
        if (change.state == state) {
            ++count;
        }
    }
    for (const GraphRow& row : rows) {
        for (const FieldChange& change : row.changes) {
            if (change.state == state) {
                ++count;
            }
        }
    }
    return count;
}

QString ChangeStateName(ChangeState state) {
    switch (state) {
    case ChangeState::Unchanged:
        return QStringLiteral("未变");
    case ChangeState::Changed:
        return QStringLiteral("已变");
    case ChangeState::Added:
        return QStringLiteral("新增");
    case ChangeState::Removed:
        return QStringLiteral("移除");
    case ChangeState::Unknown:
        break;
    }
    return QStringLiteral("未知");
}

QString ChangeStateColor(ChangeState state) {
    switch (state) {
    case ChangeState::Changed:
        return QStringLiteral("#d9a441");
    case ChangeState::Unchanged:
        return QStringLiteral("#7d838c");
    case ChangeState::Added:
        return QStringLiteral("#5aa9d6");
    case ChangeState::Removed:
        return QStringLiteral("#cf6b6b");
    case ChangeState::Unknown:
        break;
    }
    return QStringLiteral("#6b7078");
}

QString DescribeChange(const FieldChange& change) {
    const QString before = change.before.isEmpty() ? QStringLiteral("—") : change.before;
    const QString after = change.after.isEmpty() ? QStringLiteral("—") : change.after;
    // 逐段 arg：多参数 .arg(a, b, c) 会让静态分层审计（audit_qt_domain_border）
    // 拿不到接收者类型；拆开写既等价，也让这条链可被审计确认。
    QString text = QStringLiteral("%1：%2 → %3").arg(change.label).arg(before).arg(after);
    if (change.state == ChangeState::Unknown) {
        text += QStringLiteral("（未读到）");
    } else if (change.state == ChangeState::Unchanged) {
        text += QStringLiteral("（未变）");
    }
    if (!change.note.isEmpty()) {
        text += QStringLiteral(" — ") + change.note;
    }
    return text;
}

QString DescribeChangeSummary(const ChangeGraph& graph) {
    if (!graph.ok) {
        return graph.note.isEmpty() ? QStringLiteral("没有可对比的数据") : graph.note;
    }
    if (!graph.output_measured) {
        QString text = QStringLiteral("输出未探测：现在只有工序预览，跑完之后才有实测对比");
        if (!graph.output_error.isEmpty()) {
            text = QStringLiteral("输出探测失败：%1").arg(graph.output_error);
        }
        return text;
    }
    QStringList parts;
    const int changed = graph.CountChanges(ChangeState::Changed);
    // 新增 / 移除是**整条流**的层面（行级），不是字段级 —— 两处都要数。
    const int added = graph.CountChanges(ChangeState::Added) + graph.CountRows(ChangeState::Added);
    const int removed = graph.CountChanges(ChangeState::Removed) + graph.CountRows(ChangeState::Removed);
    const int unchanged = graph.CountChanges(ChangeState::Unchanged);
    const int unknown = graph.CountChanges(ChangeState::Unknown);
    parts << QStringLiteral("%1 处变化").arg(changed);
    if (added > 0) {
        parts << QStringLiteral("%1 处新增").arg(added);
    }
    if (removed > 0) {
        parts << QStringLiteral("%1 处移除").arg(removed);
    }
    parts << QStringLiteral("%1 处未变").arg(unchanged);
    if (unknown > 0) {
        parts << QStringLiteral("%1 处未知").arg(unknown);
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace ffmpeg
} // namespace videoeye
