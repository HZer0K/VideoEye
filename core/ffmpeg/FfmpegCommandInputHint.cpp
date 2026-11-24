#include "core/ffmpeg/FfmpegCommandInputHint.h"

#include <algorithm>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegCommandText.h"

namespace videoeye {
namespace ffmpeg {
namespace {

constexpr int kMaxCandidates = 20;

// 「空前缀」时给的清单。不按字典顺序、也不按字母序，而是按写一条常见转码命令
// 的实际先后排：用户从空位置开始敲，最可能的下一步就排在最前面。
// 清单里出现的名字都要能在字典里查到（查不到的直接跳过，不让空说明的候选混进来）。
const QStringList& InputSideCommonOptions() {
    static const QStringList kOptions = {
        QStringLiteral("-i"),           QStringLiteral("-y"),        QStringLiteral("-hide_banner"),
        QStringLiteral("-ss"),          QStringLiteral("-t"),        QStringLiteral("-f"),
        QStringLiteral("-stream_loop"), QStringLiteral("-re"),       QStringLiteral("-hwaccel"),
        QStringLiteral("-threads"),     QStringLiteral("-loglevel"), QStringLiteral("-nostdin"),
    };
    return kOptions;
}

const QStringList& OutputSideCommonOptions() {
    static const QStringList kOptions = {
        QStringLiteral("-c:v"), QStringLiteral("-c:a"),     QStringLiteral("-crf"),      QStringLiteral("-preset"),
        QStringLiteral("-b:v"), QStringLiteral("-vf"),      QStringLiteral("-af"),       QStringLiteral("-map"),
        QStringLiteral("-t"),   QStringLiteral("-pix_fmt"), QStringLiteral("-r"),        QStringLiteral("-movflags"),
        QStringLiteral("-an"),  QStringLiteral("-vn"),      QStringLiteral("-shortest"), QStringLiteral("-threads"),
    };
    return kOptions;
}

// ffprobe 没有"输入侧 / 输出侧"之分：所有参数都写在输入文件之前。
// 这里按"查文件信息时最常用的先来"排，不是字母序。
const QStringList& FfprobeCommonOptions() {
    static const QStringList kOptions = {
        QStringLiteral("-show_streams"),   QStringLiteral("-show_format"),  QStringLiteral("-of"),
        QStringLiteral("-select_streams"), QStringLiteral("-show_entries"), QStringLiteral("-v"),
        QStringLiteral("-show_packets"),   QStringLiteral("-show_frames"),  QStringLiteral("-count_frames"),
        QStringLiteral("-read_intervals"), QStringLiteral("-hide_banner"),  QStringLiteral("-sexagesimal"),
    };
    return kOptions;
}

// ffplay 同理：一条能自然结束的播放命令，最该先想到的是 -autoexit 与限时。
const QStringList& FfplayCommonOptions() {
    static const QStringList kOptions = {
        QStringLiteral("-autoexit"), QStringLiteral("-t"),      QStringLiteral("-ss"),   QStringLiteral("-x"),
        QStringLiteral("-y"),        QStringLiteral("-nodisp"), QStringLiteral("-loop"), QStringLiteral("-volume"),
        QStringLiteral("-fs"),       QStringLiteral("-an"),     QStringLiteral("-vn"),   QStringLiteral("-hide_banner"),
    };
    return kOptions;
}

enum class WordRole {
    Program,
    Option,
    OptionValue,
    InputUrl,
    OutputUrl,
};

struct WordScan {
    QString text;
    WordRole role = WordRole::OutputUrl;
    const FfmpegCatalogEntry* entry = nullptr; // role == Option：字典条目（可能为空）
    const FfmpegCatalogEntry* owner = nullptr; // role == OptionValue / InputUrl：这是谁的值的
    bool missing_dash_i = false;               // 裸路径却没写过 -i
};

/// 扫描到某个词为止时的"待填状态"。它与 AnalyzeCommandStructure 的区别在于
/// **容忍半截**：正在打 `-cr` 时不会因为它不在字典里就放弃后面的判断。
struct ScanState {
    /// 这条命令交给哪个工具。Unknown 在进扫描前就会被落到 Ffmpeg ——
    /// 省略程序名的写法历史上一直是 ffmpeg 语法。
    FfmpegToolKind tool = FfmpegToolKind::Ffmpeg;
    const FfmpegCatalogEntry* pending = nullptr; // 上一个要值、但还没拿到值的选项
    bool pending_is_input = false;               // 那个选项是 -i
    bool has_input = false;
    int input_count = 0;
    int output_count = 0; // 只有 ffmpeg 会递增；ffprobe / ffplay 不产出输出文件
};

void ClassifyWord(const QString& word, ScanState* state, WordScan* scan) {
    scan->text = word;

    if (IsOptionWord(word)) {
        const QString name = OptionNameOf(word);
        // 按工具查：ffprobe 不认 -crf，ffplay 的 -y 也不是 ffmpeg 那个"覆盖"。
        const FfmpegCatalogEntry* entry = FfmpegCommandCatalog::Find(name, state->tool);

        // `-map -0:d` / `-map_metadata -1`：负号值。判据与折行、解析器同源 ——
        // 只有字典标了 value_may_start_with_dash，且这个词本身不是已知选项，才算值。
        if (state->pending != nullptr && state->pending->value_may_start_with_dash && entry == nullptr) {
            scan->role = state->pending_is_input ? WordRole::InputUrl : WordRole::OptionValue;
            scan->owner = state->pending;
            if (state->pending_is_input) {
                state->has_input = true;
                ++state->input_count;
            }
            state->pending = nullptr;
            state->pending_is_input = false;
            return;
        }

        scan->role = WordRole::Option;
        scan->entry = entry;
        // 只有这个工具真的认 -i 时才进入"等输入路径"的状态。
        // ffplay 没有 -i（它与 ffmpeg 的共享表里也不含这条），写上去只会被当成未收录选项。
        if (entry != nullptr && name.compare(QLatin1String("-i"), Qt::CaseInsensitive) == 0) {
            state->pending = entry;
            state->pending_is_input = true;
        } else if (entry != nullptr && entry->takes_value) {
            state->pending = entry;
            state->pending_is_input = false;
        } else {
            state->pending = nullptr;
            state->pending_is_input = false;
        }
        return;
    }

    // 裸词：先看是不是在等值。等输入路径的优先级高于"等某个选项的值"。
    if (state->pending_is_input) {
        scan->role = WordRole::InputUrl;
        scan->owner = state->pending;
        state->has_input = true;
        ++state->input_count;
    } else if (state->pending != nullptr) {
        scan->role = WordRole::OptionValue;
        scan->owner = state->pending;
    } else if (ToolTakesBareInput(state->tool)) {
        // ffprobe / ffplay：裸参数就是输入媒体。它们没有 -i，也没有"忘了写 -i"这回事。
        scan->role = WordRole::InputUrl;
        state->has_input = true;
        ++state->input_count;
    } else if (!state->has_input) {
        // ffmpeg 不接受省略 -i 的输入（`ffmpeg in.mp4 out.mp4` 会被当成 trailing option）。
        // 这里如实标成"缺 -i 的输入"，而不是顺着用户的写法当成输出目标。
        scan->role = WordRole::InputUrl;
        scan->missing_dash_i = true;
    } else {
        scan->role = WordRole::OutputUrl;
        ++state->output_count;
    }
    state->pending = nullptr;
    state->pending_is_input = false;
}

std::vector<WordScan> ScanWords(const QString& text, const std::vector<FfmpegCommandWord>& words, int count,
                                ScanState* state) {
    std::vector<WordScan> scans;
    scans.reserve(static_cast<size_t>(count));
    bool first = true;
    for (int i = 0; i < count; ++i) {
        const QString word = text.mid(words.at(static_cast<size_t>(i)).begin,
                                      words.at(static_cast<size_t>(i)).end - words.at(static_cast<size_t>(i)).begin);
        WordScan scan;
        if (first && ClassifyProgramToken(word) != FfmpegToolKind::Unknown) {
            scan.text = word;
            scan.role = WordRole::Program;
            first = false;
            scans.push_back(scan);
            continue;
        }
        first = false;
        ClassifyWord(word, state, &scan);
        scans.push_back(scan);
    }
    return scans;
}

/// 前缀匹配里挑一条"最可能是用户想写"的：名字最短的那个（`-cr` → `-crf` 而不是 `-crf-params`）。
const FfmpegCatalogEntry* BestPrefixMatch(const QString& prefix, FfmpegToolKind tool) {
    if (prefix.size() < 2 || !prefix.startsWith(QLatin1Char('-'))) {
        return nullptr;
    }
    const FfmpegCatalogEntry* best = nullptr;
    for (const FfmpegCatalogEntry* entry : FfmpegCommandCatalog::EntriesFor(tool)) {
        if (entry->kind != FfmpegEntryKind::Option || !entry->name.startsWith(prefix, Qt::CaseInsensitive)) {
            continue;
        }
        if (best == nullptr || entry->name.size() < best->name.size()) {
            best = entry;
        }
    }
    return best;
}

QString ValueKindPhrase(FfmpegValueKind kind) {
    switch (kind) {
    case FfmpegValueKind::Encoder:
        return QStringLiteral("编码器名；copy 表示不重编码");
    case FfmpegValueKind::Filter:
        return QStringLiteral("滤镜名或滤镜串（scale=1280:-2、fps=30）");
    case FfmpegValueKind::Format:
        return QStringLiteral("封装格式名（mp4 / matroska / flv …）");
    case FfmpegValueKind::None:
    default:
        return QStringLiteral("普通取值");
    }
}

void AddCandidate(FfmpegInputHint* hint, const QString& text, const QString& label, const QString& note,
                  bool expects_value, bool starts_new_word = false) {
    for (const FfmpegInputCandidate& existing : hint->candidates) {
        if (existing.text.compare(text, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    if (static_cast<int>(hint->candidates.size()) >= kMaxCandidates) {
        return;
    }
    hint->candidates.push_back({text, label, note, expects_value, starts_new_word});
}

void AddOptionCandidates(const QString& prefix, bool output_side, FfmpegToolKind tool, FfmpegInputHint* hint) {
    // 打完选项名再补选项名没有意义：这里只处理"正在打选项名"与"准备起一个新选项"。
    if (!prefix.isEmpty() && !prefix.startsWith(QLatin1Char('-'))) {
        return;
    }

    if (prefix.isEmpty()) {
        const QStringList& common = tool == FfmpegToolKind::Ffprobe
                                        ? FfprobeCommonOptions()
                                        : (tool == FfmpegToolKind::Ffplay
                                               ? FfplayCommonOptions()
                                               : (output_side ? OutputSideCommonOptions() : InputSideCommonOptions()));
        for (const QString& name : common) {
            const FfmpegCatalogEntry* entry = FfmpegCommandCatalog::Find(name, tool);
            if (entry != nullptr) {
                AddCandidate(hint, entry->name, entry->name, entry->title, entry->takes_value);
            }
        }
        return;
    }

    std::vector<const FfmpegCatalogEntry*> matches;
    for (const FfmpegCatalogEntry* entry : FfmpegCommandCatalog::EntriesFor(tool)) {
        if (entry->kind == FfmpegEntryKind::Option && entry->name.startsWith(prefix, Qt::CaseInsensitive)) {
            matches.push_back(entry);
        }
    }
    // 短名优先：这是"前缀补全"的心理预期（-cr → -crf），不是因为短名更常用。
    std::stable_sort(matches.begin(), matches.end(),
                     [](const FfmpegCatalogEntry* left, const FfmpegCatalogEntry* right) {
                         return left->name.size() < right->name.size();
                     });
    for (const FfmpegCatalogEntry* entry : matches) {
        AddCandidate(hint, entry->name, entry->name, entry->title, entry->takes_value);
    }
}

void AddValueCandidates(const FfmpegCatalogEntry* owner, const QString& prefix, bool input_side, FfmpegInputHint* hint,
                        bool starts_new_word = false) {
    if (owner == nullptr) {
        return;
    }
    // 用户已经在引号里打字了：候选替换会把引号一起吃掉（路径变成不带引号的两个词），
    // 宁可不出候选，也不悄悄改坏一条能跑的命令。
    if (prefix.startsWith(QLatin1Char('"')) || prefix.startsWith(QLatin1Char('\''))) {
        return;
    }

    const auto& caps = FfmpegCapabilityCache::Instance();
    QStringList pool = owner->typical_values;
    switch (owner->value_kind) {
    case FfmpegValueKind::Encoder:
        if (caps.encoders_known()) {
            pool += caps.Encoders();
        }
        break;
    case FfmpegValueKind::Filter:
        if (caps.filters_known()) {
            pool += caps.Filters();
        } else {
            for (const FfmpegCatalogEntry& entry : FfmpegCommandCatalog::Entries()) {
                if (entry.kind == FfmpegEntryKind::Filter) {
                    pool.append(entry.name);
                }
            }
        }
        break;
    case FfmpegValueKind::Format:
        // `-f` 的方向：还没写过输入时它修饰的是下一个输入（demuxer），否则修饰输出（muxer）。
        pool += input_side ? caps.Demuxers() : caps.Muxers();
        break;
    case FfmpegValueKind::None:
    default:
        break;
    }

    for (const QString& value : pool) {
        if (value.isEmpty()) {
            continue;
        }
        if (!prefix.isEmpty() && !value.startsWith(prefix, Qt::CaseInsensitive)) {
            continue;
        }
        AddCandidate(hint, value, value, QString(), false, starts_new_word);
    }
}

void FillSubject(const WordScan& scan, FfmpegToolKind tool, FfmpegInputHint* hint) {
    hint->has_subject = true;
    hint->subject = scan.text;
    hint->missing_dash_i = scan.missing_dash_i;

    switch (scan.role) {
    case WordRole::Program: {
        const QString program = ToolProgramName(tool);
        hint->subject_title = QStringLiteral("%1 程序").arg(program);
        hint->subject_summary = ToolPurposeText(tool);
        hint->subject_position = QStringLiteral("命令最前面");
        switch (tool) {
        case FfmpegToolKind::Ffprobe:
            hint->subject_detail = QStringLiteral("省略程序名时本页会用设置里那个 ffprobe；写完整路径则优先用它。"
                                                  "ffprobe 的命令行是「选项 + 一个输入文件」，没有输出文件。");
            hint->subject_example = QStringLiteral("ffprobe -show_streams in.mp4");
            break;
        case FfmpegToolKind::Ffplay:
            hint->subject_detail = QStringLiteral("省略程序名时本页会用设置里那个 ffplay；写完整路径则优先用它。"
                                                  "ffplay 是独立窗口播放器，同样只需要一个输入文件。");
            hint->subject_example = QStringLiteral("ffplay -autoexit in.mp4");
            break;
        case FfmpegToolKind::Ffmpeg:
        case FfmpegToolKind::Unknown:
        default:
            hint->subject_detail = QStringLiteral("省略程序名时本页会用设置里那个 ffmpeg；写完整路径则优先用它。");
            hint->subject_example = QStringLiteral("ffmpeg -i in.mp4 out.mp4");
            break;
        }
        hint->subject_known = true;
        break;
    }

    case WordRole::Option: {
        if (scan.entry != nullptr) {
            const FfmpegCatalogEntry* entry = scan.entry;
            hint->subject = entry->name;
            hint->subject_title = entry->title;
            hint->subject_summary = entry->summary;
            hint->subject_detail = entry->detail;
            hint->subject_position = entry->position;
            hint->subject_example = entry->example;
            hint->subject_known = true;
            break;
        }
        const QString name = OptionNameOf(scan.text);
        if (const FfmpegCatalogEntry* guess = BestPrefixMatch(name, tool); guess != nullptr) {
            hint->subject_title = guess->title;
            hint->subject_summary = guess->summary;
            hint->subject_detail = guess->detail;
            hint->subject_position = guess->position;
            hint->subject_example = guess->example;
            hint->subject_known = true;
            hint->subject_is_prefix = true;
            break;
        }
        const QString program = ToolProgramName(tool);
        hint->subject_title = QStringLiteral("字典未收录的参数");
        hint->subject_summary =
            QStringLiteral("认不出来：字典只收高频参数，私有选项（-x264-params 之类）查不到是正常的。");
        hint->subject_detail = QStringLiteral("不影响执行 —— 能把命令跑起来的是 %1 自己，不是这份字典。"
                                              "要确认拼写，可以看 `%1 -h` 的输出。")
                                   .arg(program);
        hint->subject_position =
            tool == FfmpegToolKind::Ffmpeg
                ? QStringLiteral("位置敏感：写在 -i 之前的选项作用于输入，写在输出之前的作用于该输出")
                : QStringLiteral("放在输入文件之前");
        break;
    }

    case WordRole::OptionValue: {
        const FfmpegCatalogEntry* owner = scan.owner;
        if (owner != nullptr) {
            hint->subject_title = QStringLiteral("「%1」的值").arg(owner->title);
            hint->subject_summary = owner->summary;
            hint->subject_detail = QStringLiteral("%1 值是%2。").arg(owner->name, ValueKindPhrase(owner->value_kind));
            if (!owner->typical_values.isEmpty()) {
                hint->subject_detail +=
                    QStringLiteral("常见取值：%1。").arg(owner->typical_values.join(QStringLiteral(" / ")));
            }
            hint->subject_position = owner->position;
            hint->subject_example = owner->example;
            hint->subject_known = true;
        } else {
            hint->subject_title = QStringLiteral("选项的值");
            hint->subject_summary = QStringLiteral("这是上一个选项的值。");
            hint->subject_known = true;
        }
        break;
    }

    case WordRole::InputUrl:
        hint->subject_title = QStringLiteral("输入文件");
        if (tool == FfmpegToolKind::Ffprobe) {
            hint->subject_summary = QStringLiteral("ffprobe 读这个媒体，把流与容器信息打印出来。");
            hint->subject_detail = QStringLiteral("直接写在参数位就行 —— ffprobe **不需要 -i**，也没有输出文件。"
                                                  "选项写在它前面。");
            hint->subject_position = QStringLiteral("所有选项之后");
            hint->subject_example = QStringLiteral("ffprobe -show_streams in.mp4");
        } else if (tool == FfmpegToolKind::Ffplay) {
            hint->subject_summary = QStringLiteral("ffplay 播放这个媒体：本地文件、URL 或设备都可以。");
            hint->subject_detail = QStringLiteral("直接写在参数位就行 —— ffplay **不需要 -i**，也不会产出文件。"
                                                  "想让它播完自动退出，记得加 -autoexit。");
            hint->subject_position = QStringLiteral("所有选项之后");
            hint->subject_example = QStringLiteral("ffplay -autoexit in.mp4");
        } else {
            hint->subject_summary = QStringLiteral("ffmpeg 从这里读取媒体：本地文件、URL 或设备都可以。");
            if (scan.missing_dash_i) {
                hint->subject_summary += QStringLiteral("（注意：这里缺 -i，ffmpeg 不接受省略 -i 的输入。）");
            }
            hint->subject_detail = QStringLiteral("每个输入前面都要有 -i；写在某个 -i 之前的选项作用于这个输入。"
                                                  "含空格的路径要用成对双引号包起来。");
            hint->subject_position = QStringLiteral("-i 之后");
            hint->subject_example = QStringLiteral("-i \"D:/素材/in.mp4\"");
        }
        hint->subject_known = true;
        break;

    case WordRole::OutputUrl:
        hint->subject_title = QStringLiteral("输出文件");
        hint->subject_summary = QStringLiteral("转码结果写到这里；扩展名决定封装格式（除非用 -f 指定）。");
        hint->subject_detail = QStringLiteral("一条命令可以有多个输出，每个输出前面写它自己的编码参数。"
                                              "含空格的路径要用成对双引号包起来。");
        hint->subject_position = QStringLiteral("该输出的编码参数之后、命令的最后");
        hint->subject_example = QStringLiteral("-i in.mp4 -c:v libx264 out.mp4");
        hint->subject_known = true;
        break;
    }
}

} // namespace

FfmpegInputHint BuildInputHint(const QString& text, int cursor) {
    FfmpegInputHint hint;
    cursor = qBound(0, cursor, static_cast<int>(text.size()));

    const std::vector<FfmpegCommandWord> words = SplitCommandWords(text);

    // 先定工具：后面每一处"这个选项归不归我"「裸参数是不是输入」「有没有输出文件」
    // 都要看它。省略程序名（第一个词就是选项）时沿用历史默认 ffmpeg。
    FfmpegToolKind tool = FfmpegToolKind::Ffmpeg;
    if (!words.empty()) {
        const FfmpegCommandWord& first_word = words.front();
        const FfmpegToolKind declared =
            ClassifyProgramToken(text.mid(first_word.begin, first_word.end - first_word.begin));
        if (declared != FfmpegToolKind::Unknown) {
            tool = declared;
        }
    }

    // 引号没闭合时先把话说清楚：此时切词会把剩余全文当成一个词，
    // 提示仍有参考价值，但用户需要知道"路径还没闭合"。
    int quote_count = 0;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('"')) {
            ++quote_count;
        }
    }
    if (quote_count % 2 != 0) {
        hint.problem = QStringLiteral("引号还没闭合：含空格的路径要写成一对双引号。");
    }

    int current = -1;
    for (int i = 0; i < static_cast<int>(words.size()); ++i) {
        const FfmpegCommandWord& word = words.at(static_cast<size_t>(i));
        if (cursor >= word.begin && cursor <= word.end) {
            current = i;
            break;
        }
    }

    // 光标左边已经打完的词：有当前词时是它之前的全部，否则是"结尾早于光标"的全部。
    int ready_count = 0;
    if (current >= 0) {
        ready_count = current;
    } else {
        for (int i = 0; i < static_cast<int>(words.size()); ++i) {
            if (words.at(static_cast<size_t>(i)).end <= cursor) {
                ready_count = i + 1;
            }
        }
    }

    ScanState state;
    state.tool = tool;
    const std::vector<WordScan> scans = ScanWords(text, words, ready_count, &state);

    WordScan current_scan;
    ScanState after_state = state; // 当前词"写完之后"的状态（用来算下一个位置）
    bool has_current_scan = false;
    if (current >= 0) {
        hint.word_begin = words.at(static_cast<size_t>(current)).begin;
        hint.word_end = words.at(static_cast<size_t>(current)).end;
        hint.word = text.mid(hint.word_begin, hint.word_end - hint.word_begin);
        hint.prefix = text.mid(hint.word_begin, cursor - hint.word_begin);

        // 用**副本**判当前词：它的角色不该影响"光标之前"已经确定的状态。
        if (current == 0 && ClassifyProgramToken(hint.word) != FfmpegToolKind::Unknown) {
            // 第一个词单独判：ClassifyWord 只认选项与裸路径，程序名要先拦下来，
            // 否则 `ffmpeg` / `ffprobe` 会被当成"缺 -i 的输入文件"。
            current_scan.text = hint.word;
            current_scan.role = WordRole::Program;
        } else {
            ClassifyWord(hint.word, &after_state, &current_scan);
        }
        has_current_scan = true;
    } else {
        hint.word_begin = cursor;
        hint.word_end = cursor;
    }

    // ---- 光标处是哪个位置 ----
    if (has_current_scan) {
        switch (current_scan.role) {
        case WordRole::Program:
            hint.slot = FfmpegInputSlot::Program;
            break;
        case WordRole::Option:
            hint.slot = FfmpegInputSlot::Option;
            break;
        case WordRole::OptionValue:
            hint.slot = FfmpegInputSlot::OptionValue;
            break;
        case WordRole::InputUrl:
            hint.slot = FfmpegInputSlot::InputUrl;
            break;
        case WordRole::OutputUrl:
            hint.slot = FfmpegInputSlot::OutputUrl;
            break;
        }
    } else if (state.pending_is_input) {
        hint.slot = FfmpegInputSlot::InputUrl;
    } else if (state.pending != nullptr) {
        hint.slot = FfmpegInputSlot::OptionValue;
    } else {
        // 有输入之后起新词：这里既可以写输出侧选项，也可以直接写输出文件名。
        // 两种都归到 Option（候选给选项），next_label 会把"最后一个参数才是文件名"说清楚。
        hint.slot = FfmpegInputSlot::Option;
    }

    // ---- 解释哪个词 ----
    // 光标停在空白处时，解释"刚打完的那个词" —— 这正是"每输一个关键字就给解释"的时刻。
    if (has_current_scan) {
        FillSubject(current_scan, tool, &hint);
    } else if (!scans.empty()) {
        FillSubject(scans.back(), tool, &hint);
    }

    // ---- 下一个位置该写什么 ----
    const bool has_input_now = state.has_input || (has_current_scan && current_scan.role == WordRole::InputUrl &&
                                                   !current_scan.missing_dash_i);
    // 光标在词尾（或词已经完整）时，"下一个位置"要看这个词**消费掉之后**的状态：
    // `-i in.mp4` 之后该写输出选项，而 `-crf` 之后该填值。光标在词中间时用户还在改
    // 这个词，下一步仍然按这个词本身的期望来说。
    //
    // "完整"额外排除了"字典里查不到的选项名"：`-cr` 与 `-crf` 在光标上都表现为
    // prefix == word，但前者明显还没打完，不能按"选项已写完"去要它的值。
    const bool word_complete = has_current_scan && hint.prefix == hint.word &&
                               !(current_scan.role == WordRole::Option && current_scan.entry == nullptr);

    // ffprobe / ffplay 的命令结构只有一段：`[选项…] 输入`。没有输出文件，也不要求 -i，
    // 所以 ffmpeg 那套"输入 → 输出侧选项 → 输出文件名"的推进在这里完全不成立。
    // 必须单独给一套：照抄 ffmpeg 会在用户写完输入后提示"接着写输出文件名"。
    const bool single_input = ToolTakesBareInput(tool);
    const QString program = ToolProgramName(tool);
    const int effective_input_count = word_complete ? after_state.input_count : state.input_count;
    const bool single_input_ready =
        single_input && (effective_input_count > 0 || (has_current_scan && current_scan.role == WordRole::InputUrl));

    if (word_complete && after_state.pending_is_input) {
        hint.next_label = single_input
                              ? QStringLiteral("填输入文件路径（含空格用成对双引号）。")
                              : QStringLiteral("填输入文件路径（含空格用成对双引号）；要加第二个输入就再写一个 -i。");
        hint.next_examples = {QStringLiteral("-i \"\"")};
    } else if (word_complete && after_state.pending != nullptr) {
        const FfmpegCatalogEntry* pending = after_state.pending;
        hint.next_label = QStringLiteral("这是「%1」，接着要填它的值：%2。")
                              .arg(pending->title, ValueKindPhrase(pending->value_kind));
        for (const QString& value : pending->typical_values) {
            if (hint.next_examples.size() >= 3) {
                break;
            }
            hint.next_examples.append(value);
        }
    } else if (single_input_ready) {
        if (tool == FfmpegToolKind::Ffplay) {
            hint.next_label = QStringLiteral("结构完整：%1 会打开窗口播放。没加 -autoexit 的话窗口会一直开着，"
                                             "只能按「停止」结束。")
                                  .arg(program);
            hint.next_examples = {QStringLiteral("-autoexit")};
        } else {
            hint.next_label =
                QStringLiteral("结构完整：%1 可以直接跑了。想换查询项就把选项写在输入文件之前。").arg(program);
            hint.next_examples = {QStringLiteral("-show_format"), QStringLiteral("-of json")};
        }
    } else if (single_input) {
        hint.next_label =
            tool == FfmpegToolKind::Ffplay
                ? QStringLiteral("接着写输入文件路径（ffplay 不需要 -i，文件直接写在参数位）；选项都放在它之前。")
                : QStringLiteral("接着写输入文件路径（ffprobe 不需要 -i，文件直接写在参数位）；选项都放在它之前。");
    } else if (word_complete && !after_state.has_input) {
        hint.next_label = QStringLiteral("还没写输入：先写 -i 加输入文件路径。");
    } else if (word_complete && after_state.output_count > 0) {
        hint.next_label = QStringLiteral("结构完整：%1 个输入 → %2 个输出。可以回车运行，也可以再写一个输出。")
                              .arg(after_state.input_count)
                              .arg(after_state.output_count);
    } else if (word_complete) {
        hint.next_label = QStringLiteral("接着写输出侧选项（-c:v / -crf / -vf …），最后一个参数才是输出文件名。");
    } else if (!has_current_scan && state.pending == nullptr && state.output_count > 0) {
        // 光标停在最后那个输出后面：命令已经能跑了，直说，别再劝用户继续加参数。
        hint.next_label = QStringLiteral("结构完整：%1 个输入 → %2 个输出。可以回车运行，也可以再写一个输出。")
                              .arg(state.input_count)
                              .arg(state.output_count);
    } else {
        switch (hint.slot) {
        case FfmpegInputSlot::Program:
            hint.next_label = single_input
                                  ? QStringLiteral("程序名后面接输入文件；也可以省掉程序名，直接写选项或文件路径。")
                                  : QStringLiteral("程序名后面接 -i 输入文件；也可以省掉程序名，直接从 -i 开始写。");
            break;

        case FfmpegInputSlot::Option: {
            const FfmpegCatalogEntry* entry = has_current_scan ? current_scan.entry : state.pending;
            if (has_current_scan && current_scan.entry == nullptr && !hint.prefix.isEmpty()) {
                hint.next_label = QStringLiteral("把选项名打完（下拉里是前缀匹配到的参数）；按 Tab 接受补全。");
            } else if (entry != nullptr && entry->takes_value) {
                hint.next_label = QStringLiteral("这是「%1」，接着要填它的值：%2。")
                                      .arg(entry->title, ValueKindPhrase(entry->value_kind));
            } else if (entry != nullptr) {
                hint.next_label =
                    single_input ? QStringLiteral("这是开关选项，不需要值；接着写下一个选项，或直接写输入文件。")
                                 : QStringLiteral("这是开关选项，不需要值；接着写下一个选项，或直接写输出文件名。");
            } else if (!has_input_now) {
                hint.next_label = single_input ? QStringLiteral("还没写输入：接着写媒体文件路径（不需要 -i）。")
                                               : QStringLiteral("还没写输入：先写 -i 加输入文件路径。");
            } else {
                hint.next_label =
                    QStringLiteral("接着写输出侧选项（-c:v / -crf / -vf …），最后一个参数才是输出文件名。");
            }
            break;
        }

        case FfmpegInputSlot::OptionValue: {
            const FfmpegCatalogEntry* owner = has_current_scan ? current_scan.owner : state.pending;
            if (owner != nullptr) {
                hint.next_label =
                    QStringLiteral("这里填「%1」的值：%2。").arg(owner->title, ValueKindPhrase(owner->value_kind));
                for (const QString& value : owner->typical_values) {
                    if (hint.next_examples.size() >= 3) {
                        break;
                    }
                    hint.next_examples.append(value);
                }
            } else {
                hint.next_label = QStringLiteral("这里填上一个选项的值。");
            }
            break;
        }

        case FfmpegInputSlot::InputUrl:
            if (single_input) {
                hint.next_label =
                    tool == FfmpegToolKind::Ffplay
                        ? QStringLiteral("填媒体文件路径（不需要 -i）；想让它播完自己退出就加 -autoexit。")
                        : QStringLiteral("填媒体文件路径（不需要 -i）；要换查询项就把选项写在它之前。");
            } else {
                hint.next_label = QStringLiteral("填输入文件路径（含空格用成对双引号）；要加第二个输入就再写一个 -i。");
                hint.next_examples = {QStringLiteral("-i \"\"")};
            }
            break;

        case FfmpegInputSlot::OutputUrl:
            hint.next_label =
                QStringLiteral("写输出文件名；也能继续写输出侧选项（-c:v / -crf …），最后一个参数才是输出文件。");
            break;
        }
    }

    // ---- 候选 ----
    const bool output_side = has_input_now;
    if (word_complete && after_state.pending_is_input) {
        // 路径候选由用户在文件系统里挑，这一层猜不出对的那个；不给比给错好。
    } else if (word_complete && after_state.pending != nullptr) {
        // 刚写完一个吃值的选项：下一步就是填值，候选直接给这个选项的值清单
        // （`-c:v` 写完就能看到本机支持的编码器）。前缀为空是有意的：
        // 此时光标还没进值，按 "-c:v" 当前缀去筛只会得到空列表。
        // starts_new_word：光标还停在 `-c` 这个选项上，值要接在它后面成一个新词，
        // 不能原地替换 —— 否则 "-c" 整段被 "copy" 顶掉，命令直接变意思。
        AddValueCandidates(after_state.pending, QString(), !after_state.has_input, &hint, true);
    } else {
        switch (hint.slot) {
        case FfmpegInputSlot::Option:
            AddOptionCandidates(hint.prefix, output_side, tool, &hint);
            break;

        case FfmpegInputSlot::OptionValue: {
            const FfmpegCatalogEntry* owner = has_current_scan ? current_scan.owner : state.pending;
            AddValueCandidates(owner, hint.prefix, !state.has_input, &hint);
            break;
        }

        case FfmpegInputSlot::OutputUrl:
            // 输出目标位置既可以写选项，也可以直接写文件名：给选项候选，
            // 但**不**给文件系统建议 —— 这一层不知道用户在哪个目录，猜出来的是错的。
            AddOptionCandidates(hint.prefix, true, tool, &hint);
            break;

        case FfmpegInputSlot::InputUrl:
        case FfmpegInputSlot::Program:
            break;
        }
    }

    // 把前几条候选补进"下一步"，让提示行本身就能看出接什么字符 ——
    // 这是"下一个指令字符提示"里最直接的那一半（另一半是下拉）。
    if (hint.next_examples.isEmpty()) {
        for (const FfmpegInputCandidate& candidate : hint.candidates) {
            if (hint.next_examples.size() >= 3) {
                break;
            }
            hint.next_examples.append(candidate.text);
        }
    }

    return hint;
}

FfmpegCompletionEdit ApplyCompletion(const QString& text, int word_begin, int word_end, const QString& replacement) {
    FfmpegCompletionEdit edit;
    edit.text = text;
    const int size = static_cast<int>(text.size());
    const int begin = qBound(0, word_begin, size);
    const int end = qBound(begin, word_end, size);
    edit.cursor = begin;
    if (replacement.isEmpty()) {
        return edit;
    }

    QString base = text;
    base.remove(begin, end - begin);

    // 与 ComposeInsertion 同一套空格规则，但**光标落点不同**：补全之后用户接下去敲的
    // 是下一个词（选项的值或下一个选项），所以光标放在尾随空格之后。
    // 放在空格之前的话，第一下按键会粘成 `-c:vlibx264`。
    const bool need_leading_space = begin > 0 && !base.at(begin - 1).isSpace();
    const bool need_trailing_space = begin >= static_cast<int>(base.size()) || !base.at(begin).isSpace();

    const QString leading = need_leading_space ? QStringLiteral(" ") : QString();
    const QString trailing = need_trailing_space ? QStringLiteral(" ") : QString();
    edit.text = base.left(begin) + leading + replacement + trailing + base.mid(begin);
    edit.cursor = begin + static_cast<int>(leading.size() + replacement.size() + trailing.size());
    return edit;
}

} // namespace ffmpeg
} // namespace videoeye
