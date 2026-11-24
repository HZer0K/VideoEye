#include "core/ffmpeg/FfmpegCommandText.h"

#include <QChar>

#include "core/ffmpeg/FfmpegCommandCatalog.h"

namespace videoeye {
namespace ffmpeg {
namespace {

/// `-map -0:d`：值以 '-' 开头但其实是值。判据是字典里标了 value_may_start_with_dash，
/// 且这个负号词本身不是字典认识的选项 —— 两个条件都满足才当值，不猜。
bool IsNegativeValueFor(const QString& option_word, const QString& candidate) {
    if (!candidate.startsWith(QLatin1Char('-'))) {
        return false;
    }
    const FfmpegCatalogEntry* entry = FfmpegCommandCatalog::Find(OptionNameOf(option_word));
    if (entry == nullptr || !entry->value_may_start_with_dash) {
        return false;
    }
    return FfmpegCommandCatalog::Find(OptionNameOf(candidate)) == nullptr;
}

} // namespace

bool IsOptionWord(const QString& word) {
    // 单个 "-" 是"输出到 stdout"的写法，不是选项。
    // 以数字开头也不是选项：ffmpeg 的选项名从来不以数字开头，`-0:d` 是流说明符
    // （配合 -map 用），`-1` 是 -map_metadata 的"清掉元数据"。把它们当成选项，
    // 后面那个真正的路径就会被并到它们名下，看起来像"这个参数吃掉了输出文件"。
    return word.size() > 1 && word.startsWith(QLatin1Char('-')) && !word.at(1).isDigit();
}

QString OptionNameOf(const QString& word) {
    QString name = word;
    if (name.startsWith(QLatin1Char('"')) || name.startsWith(QLatin1Char('\''))) {
        return QString(); // 引号包起来的一定是路径，不会是选项名
    }
    const int eq = name.indexOf(QLatin1Char('='));
    if (eq > 0) {
        name.truncate(eq);
    }
    return name;
}

std::vector<FfmpegCommandWord> SplitCommandWords(const QString& text) {
    std::vector<FfmpegCommandWord> words;
    int i = 0;
    const int size = text.size();
    while (i < size) {
        if (text.at(i).isSpace()) {
            ++i;
            continue;
        }
        const int begin = i;
        bool in_quote = false;
        QChar quote_char;
        while (i < size) {
            const QChar c = text.at(i);
            if (in_quote) {
                ++i;
                if (c == quote_char) {
                    in_quote = false;
                }
                continue;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                in_quote = true;
                quote_char = c;
                ++i;
                continue;
            }
            if (c.isSpace()) {
                break;
            }
            ++i;
        }
        words.push_back({begin, i});
    }
    return words;
}

QStringList WrapCommandIntoLines(const QString& text) {
    const std::vector<FfmpegCommandWord> words = SplitCommandWords(text);
    QStringList lines;
    QString current;
    bool current_is_option = false;
    bool current_has_value = false;

    for (const FfmpegCommandWord& word : words) {
        const QString piece = text.mid(word.begin, word.end - word.begin);
        const bool is_option = IsOptionWord(piece);

        if (current.isEmpty()) {
            current = piece;
            current_is_option = is_option;
            current_has_value = false;
            continue;
        }

        if (is_option) {
            lines.append(current);
            current = piece;
            current_is_option = true;
            current_has_value = false;
            continue;
        }

        // 形如 -0:d / -1 的"负号词"：字典说当前选项允许这种值才并进来，
        // 否则它自己占一行，并且**不吸收**后面那个词（它不是选项，没有值可吃）
        if (piece.startsWith(QLatin1Char('-')) && !IsNegativeValueFor(current, piece)) {
            lines.append(current);
            current = piece;
            current_is_option = false;
            current_has_value = false;
            continue;
        }

        if (current_is_option && !current_has_value) {
            // 选项的值跟着选项走：`-c:v libx264` 拆成两行读起来反而更乱
            current += QLatin1Char(' ') + piece;
            current_has_value = true;
            continue;
        }

        lines.append(current);
        current = piece;
        current_is_option = false;
        current_has_value = false;
    }
    if (!current.isEmpty()) {
        lines.append(current);
    }
    return lines;
}

QString FlattenCommandLines(const QString& text) {
    const QStringList raw_lines = text.split(QLatin1Char('\n'));
    QStringList kept;
    kept.reserve(raw_lines.size());
    for (const QString& line : raw_lines) {
        // 只 trim 行的首尾（行内连续空格可能属于引号里的路径，不能压）
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            kept.append(trimmed);
        }
    }
    return kept.join(QLatin1Char(' '));
}

} // namespace ffmpeg
} // namespace videoeye
