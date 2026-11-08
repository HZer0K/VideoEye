#pragma once

// 五个完整性标志的唯一事实源。
//
// 历史坑：CSV 把 5 个标志分开落，JSON 只落 completed / partial / error，
// HTML / TXT / PDF 更把它们压成一个派生标签（if(partial) else if(!completed)）。
// 于是同一个「分析超时」的结论换个格式就成了「分析未跑完」，而「打开失败」
// 与「用户取消」在四种格式里根本分不开 —— 这正是 MEMORY-reporting-detail.md
// 「完整性标志分开落，别合成派生值」要消灭的失效。
//
// 同源约定：HTML / TXT / PDF 直接输出 IntegrityLineText() 这一行
// （PDF 正文本来就是 BuildText 的结果），所以三端只需要一套解析口径。
// JSON / CSV 各用自己的结构化写法，但值一律从 ReadIntegrityFacts() 取。

#include <string>

#include "core/reporting/QcReportExporter.h"

namespace videoeye {
namespace reporting {
namespace detail {

inline constexpr const char* kIntegrityPrefix = "完整性:";
inline constexpr const char* kIntegritySeparator = " ｜ ";

struct IntegrityFacts {
    bool analysis_ok = false;
    bool conclusion_complete = false;
    bool sampled = false;
    bool timed_out = false;
    std::string error;
};

inline IntegrityFacts ReadIntegrityFacts(const QcExportBundle& bundle) {
    IntegrityFacts facts;
    facts.analysis_ok = bundle.run.ok;
    facts.conclusion_complete = bundle.run.report.completed;
    facts.sampled = bundle.run.report.partial;
    facts.timed_out = bundle.run.timed_out;
    facts.error = bundle.run.error;
    return facts;
}

inline const char* YesNo(bool value) {
    return value ? "是" : "否";
}

// 错误原因放最后并用方括号界定：Html() 不转义方括号，PDF 折行之后也能靠
// "[" ... "]" 把整段取回来。会破坏界定的字符换成全角 / 半角替代，信息不丢。
inline std::string SanitizeIntegrityError(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\n' || c == '\r')
            out += ' ';
        else if (c == '｜')
            out += '|';
        else if (c == '[')
            out += "［";
        else if (c == ']')
            out += "］";
        else
            out += c;
    }
    return out;
}

inline std::string IntegrityLineText(const IntegrityFacts& facts) {
    const std::string sep = kIntegritySeparator;
    return std::string(kIntegrityPrefix) + " 分析成功=" + YesNo(facts.analysis_ok) + sep +
           "结论完整=" + YesNo(facts.conclusion_complete) + sep + "抽样扫描=" + YesNo(facts.sampled) + sep +
           "超时放弃=" + YesNo(facts.timed_out) + sep + "错误原因=[" + SanitizeIntegrityError(facts.error) + "]";
}

} // namespace detail
} // namespace reporting
} // namespace videoeye
