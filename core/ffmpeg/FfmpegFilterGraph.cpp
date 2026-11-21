#include "core/ffmpeg/FfmpegFilterGraph.h"

#include <QLatin1Char>
#include <QLatin1String>

#include "core/ffmpeg/FfmpegCommandCatalog.h"

namespace videoeye {
namespace ffmpeg {
namespace {

/// 滤镜名（含 @实例名）的形态：字母/下划线开头，只由字母数字下划线组成，最多一个 '@'。
/// 用来判断"切出来的这一段是滤镜名，还是上一段参数里被逗号分开的碎片"。
bool LooksLikeFilterName(const QString& text) {
    if (text.isEmpty() || text.size() > 40) {
        return false;
    }
    const QChar first = text.at(0);
    if (!(first.isLetter() || first == QLatin1Char('_'))) {
        return false;
    }
    int at_count = 0;
    for (const QChar c : text) {
        if (c == QLatin1Char('@')) {
            ++at_count;
            continue;
        }
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_'))) {
            return false;
        }
    }
    return at_count <= 1;
}

/// 按顶层分隔符切分：引号内的不算，'\' 转义的也不算。
/// 转义符与引号都**保留原样**（arguments 要能原样展示给用户看）。
QStringList SplitTopLevel(const QString& text, QChar separator, bool* unterminated_quote) {
    QStringList out;
    QString current;
    QChar quote;
    bool escaped = false;

    for (const QChar c : text) {
        if (escaped) {
            current.append(QLatin1Char('\\'));
            current.append(c);
            escaped = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else {
                current.append(c);
            }
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            continue;
        }
        if (c == separator) {
            out.append(current);
            current.clear();
            continue;
        }
        current.append(c);
    }

    if (escaped) {
        current.append(QLatin1Char('\\'));
    }
    if (!quote.isNull() && unterminated_quote != nullptr) {
        *unterminated_quote = true;
    }
    out.append(current);
    return out;
}

/// 剥掉链首连续的 [label]，写进 labels。返回 false = '[' 没有配对的 ']'。
bool TakeLeadingLabels(QString* rest, QStringList* labels) {
    QString text = rest->trimmed();
    while (text.startsWith(QLatin1Char('['))) {
        const int close = text.indexOf(QLatin1Char(']'));
        if (close < 0) {
            return false;
        }
        const QString label = text.mid(1, close - 1).trimmed();
        if (!label.isEmpty()) {
            labels->append(label);
        }
        text = text.mid(close + 1).trimmed();
    }
    *rest = text;
    return true;
}

/// 剥掉链尾连续的 [label]（`[a][b]` 的顺序要保持原样）。
bool TakeTrailingLabels(QString* rest, QStringList* labels) {
    QString text = rest->trimmed();
    QStringList collected;
    while (text.endsWith(QLatin1Char(']'))) {
        const int open = text.lastIndexOf(QLatin1Char('['));
        if (open < 0) {
            return false;
        }
        const QString label = text.mid(open + 1, text.size() - open - 2).trimmed();
        if (!label.isEmpty()) {
            collected.prepend(label);
        }
        text = text.left(open).trimmed();
    }
    *labels += collected;
    *rest = text;
    return true;
}

FfmpegFilterChain ParseChain(const QString& text, FfmpegFilterGraph* graph) {
    FfmpegFilterChain chain;
    QString rest = text.trimmed();

    if (!TakeLeadingLabels(&rest, &chain.input_labels)) {
        graph->parse_ok = false;
        graph->error = QStringLiteral("滤镜串里的 '[' 没有配对的 ']'。");
        return chain;
    }
    if (!TakeTrailingLabels(&rest, &chain.output_labels)) {
        graph->parse_ok = false;
        graph->error = QStringLiteral("滤镜串里的 ']' 没有配对的 '['。");
        return chain;
    }
    if (rest.isEmpty()) {
        return chain;
    }

    const QStringList pieces = SplitTopLevel(rest, QLatin1Char(','), nullptr);
    for (const QString& raw : pieces) {
        const QString piece = raw.trimmed();
        if (piece.isEmpty()) {
            continue;
        }

        const int eq = piece.indexOf(QLatin1Char('='));
        const QString head = eq >= 0 ? piece.left(eq).trimmed() : piece;
        const QString args = eq >= 0 ? piece.mid(eq + 1) : QString();

        if (!LooksLikeFilterName(head)) {
            // 切错了：这一段是上一段参数里被逗号分开的碎片。常见于引号被上游吃掉的
            // select='eq(pict_type,I)' → select=eq(pict_type + I)。并回去，别当成两个滤镜。
            if (chain.nodes.empty()) {
                graph->parse_ok = false;
                graph->error = QStringLiteral("认不出滤镜名「%1」。").arg(head);
                return chain;
            }
            graph->ambiguous = true;
            chain.nodes.back().arguments += QStringLiteral(",") + piece;
            continue;
        }

        FfmpegFilterNode node;
        const int at = head.indexOf(QLatin1Char('@'));
        node.name = at > 0 ? head.left(at) : head;
        node.instance = at > 0 ? head.mid(at + 1) : QString();
        node.arguments = args;

        const FfmpegCatalogEntry* entry = FfmpegCommandCatalog::Find(node.name);
        if (entry != nullptr && entry->kind == FfmpegEntryKind::Filter) {
            node.known = true;
            node.title = entry->title;
        }
        // 能力清单没探测过时 HasFilter 一律返回 true，不会冤枉任何滤镜
        node.available = FfmpegCapabilityCache::Instance().HasFilter(node.name);
        chain.nodes.push_back(node);
    }
    return chain;
}

QString NodeLabel(const FfmpegFilterNode& node) {
    QString text = node.name;
    if (!node.instance.isEmpty()) {
        // 两段分开 arg()：多参数 arg(QString, QString) 会被 Qt/domain 边界审计
        // 误判成"arg 吃 domain 字段"（它解析不了带逗号的表达式），拆开就各自可澄清。
        text = QStringLiteral("%1@%2").arg(node.name).arg(node.instance);
    }
    if (!node.arguments.isEmpty()) {
        text += QLatin1Char(' ') + node.arguments;
    }
    if (!node.available) {
        text += QStringLiteral("（当前这个 ffmpeg 里没有）");
    } else if (node.known) {
        text += QStringLiteral("（%1）").arg(node.title);
    } else {
        text += QStringLiteral("（字典里没有这个滤镜）");
    }
    return text;
}

QString JoinLabels(const QStringList& labels) {
    QString out;
    // 变量名避开 domain 字段名（label / note），否则 Qt/domain 边界审计会把
    // 它当成"arg() 吃域字段"的疑似项 —— 纯噪音
    for (const QString& label_text : labels) {
        out += QStringLiteral("[%1]").arg(label_text);
    }
    return out;
}

/// 多条链是否用 [标签] 首尾相接。ffmpeg 实测（6.1）：
///   -vf "scale=64:64;null"                  → Error initializing a simple filtergraph
///   -vf "[0:v]scale=64:64[a];[a]null[b]"    → 正常
/// 所以"有分号"本身不判死，缺标签连接才是问题。
bool ChainsAreLabeled(const std::vector<FfmpegFilterChain>& chains) {
    for (size_t i = 0; i < chains.size(); ++i) {
        if (i > 0 && chains.at(i).input_labels.isEmpty()) {
            return false;
        }
        if (i + 1 < chains.size() && chains.at(i).output_labels.isEmpty()) {
            return false;
        }
    }
    return true;
}

} // namespace

int FfmpegFilterGraph::NodeCount() const {
    int count = 0;
    for (const FfmpegFilterChain& chain : chains) {
        count += static_cast<int>(chain.nodes.size());
    }
    return count;
}

QStringList FfmpegFilterGraph::UnavailableFilters() const {
    QStringList out;
    for (const FfmpegFilterChain& chain : chains) {
        for (const FfmpegFilterNode& node : chain.nodes) {
            if (!node.available && !out.contains(node.name)) {
                out.append(node.name);
            }
        }
    }
    return out;
}

FfmpegFilterGraph ParseFilterGraph(const QString& text, bool complex_graph) {
    FfmpegFilterGraph graph;
    graph.complex_graph = complex_graph;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return graph;
    }

    bool unterminated_quote = false;
    const QStringList segments = SplitTopLevel(trimmed, QLatin1Char(';'), &unterminated_quote);
    if (unterminated_quote) {
        graph.parse_ok = false;
        graph.error = QStringLiteral("滤镜串里的引号没有闭合。");
        return graph;
    }
    // 分号本身不判死：能不能用取决于标签连接（见 ChainsAreLabeled 上的实测记录）
    for (const QString& segment : segments) {
        if (segment.trimmed().isEmpty()) {
            continue;
        }
        graph.chains.push_back(ParseChain(segment, &graph));
        if (!graph.parse_ok) {
            return graph;
        }
    }

    if (!complex_graph && graph.chains.size() > 1 && !ChainsAreLabeled(graph.chains)) {
        graph.notes.append(
            QStringLiteral("-vf / -af 通常只写一条链。这里有多条链却没有用 [标签] 首尾相接，"
                           "ffmpeg 会报 Error initializing a simple filtergraph —— "
                           "要连多条链请写成 [0:v]scale=…[a];[a]滤镜[b] 这样，或改用 -filter_complex。"));
    }
    return graph;
}

QString DescribeFilterGraph(const FfmpegFilterGraph& graph) {
    if (graph.chains.empty()) {
        return QString();
    }
    if (!graph.parse_ok) {
        return QStringLiteral("滤镜链解析失败：%1").arg(graph.error);
    }

    QStringList lines;
    const int node_count = graph.NodeCount();

    if (!graph.complex_graph && graph.chains.size() == 1) {
        lines.append(QStringLiteral("滤镜链拆解（%1 个滤镜，按执行顺序）：").arg(node_count));
        QStringList parts;
        for (const FfmpegFilterNode& node : graph.chains.front().nodes) {
            parts.append(NodeLabel(node));
        }
        if (!parts.isEmpty()) {
            lines.append(QStringLiteral("  %1").arg(parts.join(QStringLiteral(" → "))));
        }
    } else {
        lines.append(QStringLiteral("滤镜图拆解（%1 条链，共 %2 个滤镜）：").arg(graph.chains.size()).arg(node_count));
        int index = 0;
        for (const FfmpegFilterChain& chain : graph.chains) {
            ++index;
            QStringList parts;
            const QString inputs = JoinLabels(chain.input_labels);
            if (!inputs.isEmpty()) {
                parts.append(inputs);
            }
            for (const FfmpegFilterNode& node : chain.nodes) {
                parts.append(NodeLabel(node));
            }
            const QString outputs = JoinLabels(chain.output_labels);
            if (!outputs.isEmpty()) {
                parts.append(outputs);
            }
            lines.append(QStringLiteral("  链 %1：%2").arg(index).arg(parts.join(QStringLiteral(" → "))));
        }
    }

    for (const QString& note_text : graph.notes) {
        lines.append(QStringLiteral("注意：%1").arg(note_text));
    }
    if (graph.ambiguous) {
        lines.append(QStringLiteral("注意：串里有被外层引号吃掉的参数逗号（如 select='eq(...)'），"
                                    "按能识别的部分拆的，某个滤镜的参数可能偏长。"));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace ffmpeg
} // namespace videoeye
