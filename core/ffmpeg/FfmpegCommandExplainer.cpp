#include "core/ffmpeg/FfmpegCommandExplainer.h"

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandParser.h"

namespace videoeye {
namespace ffmpeg {
namespace {

// 允许出现在第一个 -i 之前的选项：全局开关 + 输入侧选项 + 两侧都能用的少数几个。
// 其余（编码/画面/封装类）写在 -i 之前不会报错，但也**不会生效** —— 这是 ffmpeg
// 最常见的"参数写了没反应"原因，必须在解释里点出来。
bool IsInputSideOption(const QString& name) {
    static const QStringList kInputSide = {
        QStringLiteral("-y"), QStringLiteral("-n"), QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("-nostdin"), QStringLiteral("-threads"),
        QStringLiteral("-hwaccel"), QStringLiteral("-i"), QStringLiteral("-f"),
        QStringLiteral("-ss"), QStringLiteral("-re"), QStringLiteral("-stream_loop"),
        QStringLiteral("-r"), QStringLiteral("-t"), QStringLiteral("-itsoffset"),
        QStringLiteral("-copyts"), QStringLiteral("-framerate"),
    };
    return kInputSide.contains(name, Qt::CaseInsensitive);
}

// 滤镜链里第一个滤镜名（"scale=1280:-2,format=yuv420p" → "scale"）
QString FirstFilterName(const QString& chain) {
    const QString head = chain.section(QLatin1Char(','), 0, 0).trimmed();
    return head.section(QLatin1Char('='), 0, 0).trimmed();
}

// 一条命令里有多个输出时给它们编号，否则就是普通的"输出文件"
QString OutputRoleLabel(int index, int output_count) {
    return output_count > 1 ? QStringLiteral("输出文件 %1").arg(index + 1)
                            : QStringLiteral("输出文件");
}

QString OutputDescription(const QString& target) {
    QString text = QStringLiteral("ffmpeg 把结果写到这里。扩展名决定封装格式"
                                  "（除非用 -f 强制指定）。");
    if (target == QLatin1String("-")) {
        text = QStringLiteral("单独的 \"-\" 表示标准输出。写成文件时它是输出目标，"
                              "写在这里则会把媒体数据灌进本页的日志区 —— 那是不支持的"
                              "（只有 -f null / md5 这类纯文本格式例外）。");
    } else if (target.startsWith(QLatin1String("pipe:"), Qt::CaseInsensitive)) {
        text = QStringLiteral("管道输出（pipe:N，N=1 是标准输出、N=2 是标准错误）。"
                              "本页的日志区只接收文本，请改成输出到文件。");
    }
    return text;
}

}  // namespace

FfmpegCommandExplanation FfmpegCommandExplainer::Explain(const QStringList& arguments) {
    FfmpegCommandExplanation out;
    out.arguments = arguments;

    if (arguments.isEmpty()) {
        out.notes.push_back(QStringLiteral("命令为空。"));
        return out;
    }

    // 先按 ffmpeg 真正的语法把输入组 / 输出组分出来 —— 多输出命令与"最后一个参数
    // 就是输出文件"的旧假设在这里一次性解决。
    const FfmpegCommandStructure structure = AnalyzeCommandStructure(arguments);
    out.input_count = structure.input_count;
    out.output_count = structure.output_count;
    out.has_input = structure.has_input;
    out.structure_known = structure.complete;

    int i = 0;
    while (i < arguments.size()) {
        const FfmpegArgPlacement& place = structure.args.at(static_cast<size_t>(i));
        const QString token = arguments.at(i);

        // ---- 输出目标 ----
        if (place.role == FfmpegArgRole::OutputUrl) {
            FfmpegTokenExplanation item;
            item.token = token;
            item.role = FfmpegTokenRole::OutputFile;
            item.role_label = OutputRoleLabel(place.output_index, structure.output_count);
            item.description = OutputDescription(token);
            item.position = QStringLiteral("所有作用于这个输出的参数都要写在它之前"
                                           "（多输出命令里，写在两个输出之间的参数作用于后一个）");
            item.group_index = place.output_index;
            if (!place.output_format.isEmpty()) {
                item.related.push_back(QStringLiteral("-f %1").arg(place.output_format));
            }
            out.tokens.push_back(item);
            out.has_output = true;
            ++i;
            continue;
        }

        // ---- 归属不明 ----
        if (place.role == FfmpegArgRole::Uncertain) {
            FfmpegTokenExplanation item;
            item.token = token;
            item.role = FfmpegTokenRole::Unresolved;
            item.role_label = QStringLiteral("未解析");
            item.description =
                QStringLiteral("认不出它是前面那个选项的值，还是一个输出文件"
                               "（前面紧挨着的是一个本字典没有收录的选项）。"
                               "这里不替你定，请以 ffmpeg 实际运行的结果为准。");
            out.tokens.push_back(item);
            ++i;
            continue;
        }

        // 值类 token 都由它们的选项带着，正常情况下不会走到循环头
        if (place.role != FfmpegArgRole::Option) {
            ++i;
            continue;
        }

        // ---- 选项 ----
        FfmpegTokenExplanation item;
        item.token = token;

        const FfmpegCatalogEntry* entry = FfmpegCommandCatalog::Find(token);
        if (entry != nullptr) {
            item.known = true;
            item.role = FfmpegTokenRole::KnownOption;
            item.role_label = entry->title;
            item.description = entry->summary + QLatin1String("\n") + entry->detail;
            item.position = entry->position;
            item.example = entry->example;
            item.related = entry->related;
            item.typical_values = entry->typical_values;

            if (place.value_index >= 0) {
                item.value = arguments.at(place.value_index);
            }

            // 值是否真的能用（编码器 / 滤镜 / 格式）
            if (!item.value.isEmpty()) {
                const auto& caps = FfmpegCapabilityCache::Instance();
                if (entry->value_kind == FfmpegValueKind::Encoder &&
                    item.value != QLatin1String("copy") &&
                    !caps.HasEncoder(item.value)) {
                    item.warning = QStringLiteral("当前这个 ffmpeg 里没有编码器「%1」，运行会报 Unknown encoder。"
                                                  "用 ffmpeg -encoders 可以列出实际可用的编码器。")
                                       .arg(item.value);
                } else if (entry->value_kind == FfmpegValueKind::Format) {
                    // 注意: -f 写在 -i 之前作用的是**读**（demuxer），
                    // 写在输出之前作用的是**写**（muxer）。两张清单必须分开查，
                    // 否则 `-f lavfi`（只能读）会被误报成"不支持的格式"。
                    const bool readable = caps.HasDemuxer(item.value);
                    const bool writable = caps.HasMuxer(item.value);
                    if (place.applies_to_input && !readable && writable) {
                        item.warning = QStringLiteral("当前这个 ffmpeg 能写「%1」但读不了它。"
                                                      "它出现在输入侧（-i 之前），作输入格式不可用。")
                                           .arg(item.value);
                    } else if (!place.applies_to_input && !writable && readable) {
                        item.warning = QStringLiteral("当前这个 ffmpeg 能读「%1」但写不了它。"
                                                      "它出现在输出侧，作输出格式不可用。")
                                           .arg(item.value);
                    } else if (!readable && !writable) {
                        item.warning = QStringLiteral("当前这个 ffmpeg 不支持封装格式「%1」。").arg(item.value);
                    }
                } else if (entry->value_kind == FfmpegValueKind::Filter &&
                           !caps.HasFilter(FirstFilterName(item.value))) {
                    item.warning = QStringLiteral("滤镜「%1」在当前这个 ffmpeg 里不可用。")
                                       .arg(FirstFilterName(item.value));
                }
            }

            if (token == QLatin1String("-i")) {
                item.role = FfmpegTokenRole::InputFile;
                item.role_label = QStringLiteral("输入文件");
                if (item.value == QLatin1String("-") || item.value.compare(QLatin1String("pipe:0"), Qt::CaseInsensitive) == 0) {
                    item.warning = QStringLiteral("它表示从标准输入读数据，而本页不给子进程喂 stdin —— "
                                                  "ffmpeg 会立刻读到 EOF 退出。");
                }
            }

            if (structure.has_input && !place.output_side && !IsInputSideOption(token)) {
                item.warning = QStringLiteral("它出现在第一个 -i 之前。ffmpeg 的输出侧选项要写在"
                                              "输入之后、输出文件之前，放错位置不会报错但**不会生效**。");
            }
        } else {
            item.role = FfmpegTokenRole::UnknownOption;
            item.role_label = QStringLiteral("未知参数");
            item.description =
                QStringLiteral("字典里没有收录「%1」。它可能是编码器私有选项、"
                               "新版本新增的参数，或者拼错了 —— 这里不做猜测，请以实际运行输出为准。")
                    .arg(token);
            // 私有选项通常带值，但值是否以 '-' 开头无从判断，保守地不吞掉下一个 token。
        }
        out.tokens.push_back(item);
        i = (place.value_index >= 0) ? place.value_index + 1 : i + 1;
    }

    // ---- 命令级提醒 ----
    if (arguments.isEmpty()) {
        out.notes.push_back(QStringLiteral("命令为空。"));
        return out;
    }

    // 信息查询命令（-version / -encoders / -formats …）天生没有输入也没有输出，
    // 拿转码命令的标准去挑它的毛病只会给出错误的纠正建议。
    if (IsInformationCommand(arguments)) {
        QStringList queried;
        for (const QString& token : arguments) {
            const QString description = InformationOptionDescription(token);
            if (!description.isEmpty()) {
                queried.push_back(QStringLiteral("· %1 —— %2").arg(token, description));
            }
        }
        out.notes.push_back(
            QStringLiteral("这是一条**查询命令**，不需要输入文件也不需要输出文件：ffmpeg 只把结果打印出来就退出。\n%1")
                .arg(queried.isEmpty() ? QStringLiteral("· 运行后结果会输出在下面。")
                                       : queried.join(QStringLiteral("\n"))));
        out.flow = QStringLiteral("查询 ffmpeg 自身的能力/版本信息");
        return out;
    }

    if (!out.has_input) {
        out.notes.push_back(QStringLiteral("没有 -i，ffmpeg 拿不到输入（除非用的是 -f lavfi 之类的虚拟输入）。"));
    }
    if (!out.has_output) {
        out.notes.push_back(QStringLiteral("没有输出文件。ffmpeg 至少要有一个输出目标，"
                                           "否则它只会报错退出（或把数据写到 stdout，而本页不支持那样做）。"));
    }
    if (arguments.contains(QStringLiteral("-nostdin"))) {
        out.notes.push_back(QStringLiteral("命令里有 -nostdin: ffmpeg 不会读标准输入，"
                                           "所以点击「停止」时无法用 q 让它优雅收尾，只能直接终止进程 —— "
                                           "mp4 这类需要收尾写索引的输出**可能被截断**。"));
    }
    if (out.output_count > 1) {
        out.notes.push_back(QStringLiteral("这条命令有 %1 个输出目标。ffmpeg 会把写在两个输出之间的选项"
                                           "归属于后一个输出 —— 想给某个输出单独配参数，就把参数紧挨着"
                                           "写在它的文件名前面。")
                                .arg(out.output_count));
    }
    if (!out.structure_known) {
        out.notes.push_back(QStringLiteral("命令里有归属不明参数（列表中标为「未解析」的项），"
                                           "整体结构无法可靠判断 —— 上面的流程据此省略了对它们的推断。"));
    }

    const bool stream_copy = arguments.contains(QStringLiteral("-c:v")) &&
                             arguments.indexOf(QStringLiteral("copy")) > arguments.indexOf(QStringLiteral("-c:v"));
    bool reencode_param = false;
    for (const QString& flag : {QStringLiteral("-crf"), QStringLiteral("-b:v"), QStringLiteral("-vf"),
                                QStringLiteral("-s"), QStringLiteral("-pix_fmt"), QStringLiteral("-preset")}) {
        if (arguments.contains(flag)) {
            reencode_param = true;
        }
    }
    if (stream_copy && reencode_param) {
        out.notes.push_back(QStringLiteral("-c:v copy 表示不重新编码，此时 -crf / -vf / -s / -pix_fmt "
                                           "这类需要重新编码的参数都不会生效。要么去掉 copy，要么去掉这些参数。"));
    }
    if (arguments.contains(QStringLiteral("-crf")) && arguments.contains(QStringLiteral("-b:v"))) {
        out.notes.push_back(QStringLiteral("-crf（质量优先）与 -b:v（码率优先）是两种互斥的码率控制方式，"
                                           "同时写时后者会被忽略或产生非预期结果。"));
    }

    const QString pipe_target = DetectStdoutMediaOutput(arguments);
    if (!pipe_target.isEmpty()) {
        out.notes.push_back(QStringLiteral("参数「%1」会把媒体数据写进标准输出/标准错误这样的进程通道"
                                           "（ffmpeg 的 pipe:N，N=1 是 stdout、N=2 是 stderr）。"
                                           "本页的日志区只接收文本，二进制流灌进去只会把界面堵死 —— "
                                           "请改成输出到文件。")
                                .arg(pipe_target));
    }

    // ---- 流程串 ----
    QStringList flow_parts;
    for (const auto& item : out.tokens) {
        if (item.role == FfmpegTokenRole::InputFile || item.role == FfmpegTokenRole::OutputFile) {
            flow_parts.push_back(item.role_label);
        } else if (item.known) {
            // 全局开关（-y / -hide_banner）对"这条命令在干什么"没有信息量，不进流程
            if (item.role_label != QStringLiteral("覆盖输出") &&
                item.role_label != QStringLiteral("不覆盖已存在的文件") &&
                item.role_label != QStringLiteral("隐藏版本横幅") &&
                item.role_label != QStringLiteral("日志详细程度") &&
                item.role_label != QStringLiteral("不读标准输入") &&
                item.role_label != QStringLiteral("线程数")) {
                flow_parts.push_back(item.role_label);
            }
        }
    }
    if (flow_parts.isEmpty()) {
        out.flow = QStringLiteral("（无法概括这条命令的作用）");
    } else {
        out.flow = flow_parts.join(QStringLiteral(" → "));
    }
    return out;
}

}  // namespace ffmpeg
}  // namespace videoeye
