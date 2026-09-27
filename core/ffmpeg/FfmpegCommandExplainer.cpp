#include "core/ffmpeg/FfmpegCommandExplainer.h"

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandParser.h"

namespace videoeye {
namespace ffmpegtool {
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

}  // namespace

FfmpegCommandExplanation FfmpegCommandExplainer::Explain(const QStringList& arguments) {
    FfmpegCommandExplanation out;
    out.arguments = arguments;

    const int first_input_index = arguments.indexOf(QStringLiteral("-i"));

    int i = 0;
    while (i < arguments.size()) {
        const QString token = arguments.at(i);

        if (token.startsWith(QLatin1Char('-'))) {
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

                // 取值: 下一个 token 不是选项就算它的值。
                // ffmpeg 里确实有值以 '-' 开头的写法（如 -ss -10），但极其罕见，
                // 认错一次顶多少解释一项，反过来把输出文件当值会破坏整条流程。
                if (entry->takes_value && i + 1 < arguments.size() &&
                    !arguments.at(i + 1).startsWith(QLatin1Char('-'))) {
                    item.value = arguments.at(i + 1);
                    ++i;
                }

                // 值是否真的能用（编码器 / 滤镜 / 格式）
                if (!item.value.isEmpty()) {
                    if (entry->value_kind == FfmpegValueKind::Encoder &&
                        item.value != QLatin1String("copy") &&
                        !FfmpegCapabilityCache::Instance().HasEncoder(item.value)) {
                        item.warning = QStringLiteral("当前这个 ffmpeg 里没有编码器「%1」，运行会报 Unknown encoder。"
                                                      "用 ffmpeg -encoders 可以列出实际可用的编码器。")
                                           .arg(item.value);
                    } else if (entry->value_kind == FfmpegValueKind::Format &&
                               !FfmpegCapabilityCache::Instance().HasFormat(item.value)) {
                        item.warning = QStringLiteral("当前这个 ffmpeg 不支持封装格式「%1」。").arg(item.value);
                    } else if (entry->value_kind == FfmpegValueKind::Filter &&
                               !FfmpegCapabilityCache::Instance().HasFilter(FirstFilterName(item.value))) {
                        item.warning = QStringLiteral("滤镜「%1」在当前这个 ffmpeg 里不可用。")
                                           .arg(FirstFilterName(item.value));
                    }
                }

                if (token == QLatin1String("-i")) {
                    item.role = FfmpegTokenRole::InputFile;
                    item.role_label = QStringLiteral("输入文件");
                    out.has_input = true;
                }

                if (first_input_index >= 0 && i < first_input_index && !IsInputSideOption(token)) {
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
            ++i;
            continue;
        }

        // 裸露 token
        FfmpegTokenExplanation item;
        item.token = token;
        if (i + 1 == arguments.size()) {
            item.role = FfmpegTokenRole::OutputFile;
            item.role_label = QStringLiteral("输出文件");
            item.description = QStringLiteral("命令的最后一个参数，ffmpeg 把结果写到这里。"
                                              "扩展名决定封装格式（除非用 -f 强制指定）。");
            item.position = QStringLiteral("命令末尾；所有作用于输出的参数都要写在它之前");
            out.has_output = true;
        } else if (token.startsWith(QLatin1Char('[')) && token.endsWith(QLatin1Char(']'))) {
            item.role = FfmpegTokenRole::BareArgument;
            item.role_label = QStringLiteral("滤镜标签");
            item.description = QStringLiteral("滤镜图里的一个连接点标签，配合 -map 把滤镜输出接到输出文件上。");
        } else {
            item.role = FfmpegTokenRole::BareArgument;
            item.role_label = QStringLiteral("裸露参数");
            item.description = QStringLiteral("位置上它既不像输入文件（前面没有 -i）也不是最后一个参数，"
                                              "通常是漏写了 -i 或多写了一个文件名。");
            item.warning = QStringLiteral("位置不明：ffmpeg 可能把它当成输出文件，也可能直接报错。");
        }
        out.tokens.push_back(item);
        ++i;
    }

    // ---- 命令级提醒 ----
    if (arguments.isEmpty()) {
        out.notes.push_back(QStringLiteral("命令为空。"));
        return out;
    }
    if (!out.has_input && !arguments.contains(QStringLiteral("-version")) &&
        !arguments.contains(QStringLiteral("-encoders")) && !arguments.contains(QStringLiteral("-filters")) &&
        !arguments.contains(QStringLiteral("-formats")) && !arguments.contains(QStringLiteral("-h"))) {
        out.notes.push_back(QStringLiteral("没有 -i，ffmpeg 拿不到输入（除非用的是 -f lavfi 之类的虚拟输入）。"));
    }
    if (!out.has_output) {
        out.notes.push_back(QStringLiteral("没有输出文件。ffmpeg 至少要有一个输出目标，"
                                           "否则它只会报错退出（或把数据写到 stdout，而本页不支持那样做）。"));
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
        out.notes.push_back(QStringLiteral("参数「%1」表示把媒体数据写到标准输出（管道）。"
                                           "本页的日志框只接收文本，无法承接二进制流 —— 请改成输出到文件。")
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

}  // namespace ffmpegtool
}  // namespace videoeye
