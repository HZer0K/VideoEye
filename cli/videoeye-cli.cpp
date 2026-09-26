// videoeye-cli —— VideoEye 的命令行入口（报告 / 批量 QC / 对比 / CI）
//
// 存在的意义：GUI 能做的事这里都能脚本化，从而可以挂到转码流水线或 nightly 任务里。
// 输出走 stdout / 落盘文件，退出码是唯一的机器判定依据（见 PrintUsage 的退出码说明）。
//
// 用法见 PrintUsage()。要点：
//   * analyze 单文件；batch 目录；compare 双文件；profiles 列出内置模板
//   * --profile 既可以是内置模板 id，也可以是一份 JSON 模板文件的路径
//   * --fail-on 决定达到哪个级别的问题才返回非 0，CI 按这个数决定构建是否失败

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcComparator.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcRunner.h"
#include "utils/Json.h"
#include "utils/QcReportExporter.h"

using namespace videoeye;

namespace {

// ---- 退出码 ----
enum class ExitCode {
    Ok = 0,
    IssuesFound = 1,   // 分析成功，但问题达到 --fail-on 设定的级别
    Usage = 2,         // 命令行写错了
    AnalysisFailed = 3,
    OutputFailed = 4,
    Cancelled = 5,
};

struct Options {
    std::vector<std::string> positional;
    std::string command;
    std::string profile_arg = "general";
    std::string depth_arg;
    std::vector<std::string> extensions;
    std::vector<qc::QcReportFormat> formats;
    std::string out_dir;
    std::string summary_path;
    std::string json_path;
    std::string csv_path;
    std::string html_path;
    std::string text_path;
    std::string pdf_path;
    int jobs = 4;
    bool recursive = true;
    bool no_progress = false;
    bool verbose = false;
    bool quiet = false;
    // 触发非 0 退出的最低严重度；none 表示永远返回 0
    int fail_on = 2;  // 0=info 1=warning 2=error 3=critical 4=none
};

void PrintUsage() {
    std::cout
        << "VideoEye CLI - 视频 QC 命令行工具\n"
        << "\n"
        << "用法:\n"
        << "  videoeye-cli analyze <文件>   [选项]   单个文件 QC\n"
        << "  videoeye-cli batch   <目录>   [选项]   批量目录扫描\n"
        << "  videoeye-cli compare <A> <B> [选项]   两个文件对比（转码前后）\n"
        << "  videoeye-cli profiles          列出内置 QC 模板\n"
        << "\n"
        << "通用选项:\n"
        << "  --profile <id|路径>   内置模板 id 或 JSON 模板文件 (默认 general)\n"
        << "  --depth <档位>        覆盖模板的分析强度: fast | standard | deep\n"
        << "  --json <路径>         导出 JSON 报告（完整 schema，机器读取）\n"
        << "  --csv  <路径>         导出 CSV（每个问题一行）\n"
        << "  --html <路径>         导出 HTML（人工审核）\n"
        << "  --txt  <路径>         导出纯文本\n"
        << "  --pdf  <路径>         导出 PDF（仅拉丁字符，中文请用 HTML）\n"
        << "  --fail-on <级别>      达到该级别就返回非 0: info|warning|error|critical|none\n"
        << "                        (默认 error)\n"
        << "  --no-progress         不打印进度条（CI 日志更干净）\n"
        << "  -v, --verbose         打印每个文件的单行摘要\n"
        << "  -q, --quiet           只保留错误输出\n"
        << "\n"
        << "batch 专有:\n"
        << "  --out <目录>          报告输出目录（默认不落盘）\n"
        << "  --format <列表>       批量导出格式，逗号分隔: json,csv,html,txt,pdf (默认 json)\n"
        << "  --ext <列表>          扩展名过滤，逗号分隔: mp4,mov,mxf (默认全部文件)\n"
        << "  --jobs <N>            并发数 (默认 4，上限 16)\n"
        << "  --no-recursive        只扫描给定目录，不递归子目录\n"
        << "  --summary <路径>      额外导出一份批量汇总 json/csv/html\n"
        << "\n"
        << "退出码:\n"
        << "  0  无阻断问题        1  存在 --fail-on 指定级别以上的问题\n"
        << "  2  命令行错误        3  分析失败   4  写文件失败   5  被取消\n"
        << "\n"
        << "示例:\n"
        << "  videoeye-cli analyze input.mp4 --profile hls-vod --json report.json\n"
        << "  videoeye-cli batch D:\\media --profile broadcast --out reports --summary reports\\summary.csv\n"
        << "  videoeye-cli compare source.mov transcoded.mp4 --csv diff.csv\n";
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ',' || c == ';') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

bool ParseFailOn(const std::string& text, int& out) {
    if (text == "info")     { out = 0; return true; }
    if (text == "warning")  { out = 1; return true; }
    if (text == "error")    { out = 2; return true; }
    if (text == "critical") { out = 3; return true; }
    if (text == "none")     { out = 4; return true; }
    return false;
}

// 严重度 -> 顺序号（越大越严重），与 Options::fail_on 同一套编号
int SeverityRank(model::IssueSeverity severity) {
    switch (severity) {
        case model::IssueSeverity::Info:     return 0;
        case model::IssueSeverity::Warning:  return 1;
        case model::IssueSeverity::Error:    return 2;
        case model::IssueSeverity::Critical: return 3;
    }
    return 0;
}

bool HasFailLevelIssue(const model::QcReport& report, int fail_on) {
    if (fail_on >= 4) return false;
    for (const auto& issue : report.issues) {
        if (SeverityRank(issue.severity) >= fail_on) return true;
    }
    return false;
}

std::string BaseName(const std::string& path) {
    std::filesystem::path file(path);
    return file.stem().string();
}

std::string EnsureDirectory(const std::string& directory, bool& ok) {
    if (directory.empty()) return directory;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "无法创建输出目录: " << directory << " (" << error.message() << ")\n";
        ok = false;
        return std::string();
    }
    return directory;
}

// 批量场景的输出路径：<out>/<相对目录>/<文件名>.<扩展名>
std::string RelativeReportPath(const std::string& root, const std::string& source_path,
                               const std::string& out_dir, qc::QcReportFormat format) {
    std::error_code error;
    const auto relative = std::filesystem::relative(source_path, root, error);
    const auto rel_path = error ? std::filesystem::path(source_path).filename() : relative;
    auto target = std::filesystem::path(out_dir) / rel_path.parent_path() / rel_path.stem();
    target += qc::QcReportExtension(format);
    return target.string();
}

bool WriteBundleToPath(const std::string& path, const utils::QcExportBundle& bundle,
                       qc::QcReportFormat format) {
    std::error_code error;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) {
        std::cerr << "无法创建目录 " << parent.string() << ": " << error.message() << "\n";
        return false;
    }
    if (format == qc::QcReportFormat::Pdf) {
        const auto result = utils::QcReportExporter::ExportPdf(path, bundle);
        if (result.text_loss && result.ok) {
            std::cerr << "提示: PDF 无法嵌入中文字形，相关文字已替换为 '?'，建议改用 --html ("
                      << path << ")\n";
        }
        return result.ok;
    }
    return utils::QcReportExporter::Export(path, bundle, format);
}

void PrintProgress(double percent, const std::string& stage, bool enabled) {
    if (!enabled) return;
    const int width = 24;
    const int filled = static_cast<int>(percent / 100.0 * width);
    std::string bar;
    for (int i = 0; i < width; ++i) bar += (i < filled ? '#' : '-');
    std::cout << "\r  [" << bar << "] " << static_cast<int>(percent) << "% " << stage
              << "          " << std::flush;
}

void ClearProgressLine(bool enabled) {
    if (!enabled) return;
    std::cout << "\r                                                                    \r"
              << std::flush;
}

bool ResolveProfile(const std::string& argument, qc::QcProfile& profile, std::string& error) {
    if (const qc::QcProfile* builtin = qc::FindBuiltinQcProfile(argument)) {
        profile = *builtin;
        return true;
    }
    if (qc::LoadQcProfileFromFile(argument, profile, error)) return true;
    error = "无法通过内置模板 id 或文件路径加载模板 '" + argument + "': " + error;
    return false;
}

int CountIssues(const model::QcReport& report, model::IssueSeverity severity) {
    return report.CountBySeverity(severity);
}

// 汇总级别的问题数：level 0=info 及以上, 1=warning 及以上, 2=error 及以上, 3=critical
int CountAtLevel(const qc::BatchQcSummary& summary, int level) {
    if (level <= 0) {
        return summary.critical_count + summary.error_count + summary.warning_count +
               summary.info_count;
    }
    if (level == 1) return summary.critical_count + summary.error_count + summary.warning_count;
    if (level == 2) return summary.critical_count + summary.error_count;
    return summary.critical_count;
}

int RunProfiles(const Options& options) {
    const auto profiles = qc::BuiltinQcProfiles();
    if (options.json_path.empty()) {
        std::cout << "内置模板 " << profiles.size() << " 个:\n\n";
        for (const auto& profile : profiles) {
            std::cout << "  " << profile.id << "  [" << qc::ToString(profile.depth) << "]  "
                      << profile.name << "\n      " << profile.description << "\n"
                      << "      覆盖规则 " << profile.overrides.size() << " 条\n\n";
        }
        std::cout << "用 --profile <id> 选用，或导出后自行修改:\n"
                  << "  videoeye-cli profiles --json profiles.json\n";
        return static_cast<int>(ExitCode::Ok);
    }
    utils::JsonValue root = utils::JsonValue::MakeArray();
    for (const auto& profile : profiles) {
        utils::JsonValue node;
        utils::JsonParse(qc::SerializeQcProfile(profile, false), node, nullptr);
        root.PushBack(node);
    }
    std::ofstream file(options.json_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "无法写入: " << options.json_path << "\n";
        return static_cast<int>(ExitCode::OutputFailed);
    }
    file << root.ToPrettyString(2) << "\n";
    std::cout << "模板已导出到 " << options.json_path << "\n";
    return static_cast<int>(ExitCode::Ok);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 控制台默认是 936 代码页，UTF-8 输出会乱码
    SetConsoleOutputCP(CP_UTF8);
#endif

    Options options;
    bool parse_error = false;

    auto next_value = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "缺少参数: " << flag << "\n";
            parse_error = true;
            return std::string();
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            PrintUsage();
            return static_cast<int>(ExitCode::Ok);
        }
        if (argument == "--version") {
            std::cout << "videoeye-cli 2.0.0\n";
            return static_cast<int>(ExitCode::Ok);
        }
        if (argument == "--profile") {
            options.profile_arg = next_value(i, "--profile");
        } else if (argument == "--depth") {
            options.depth_arg = next_value(i, "--depth");
        } else if (argument == "--json") {
            options.json_path = next_value(i, "--json");
        } else if (argument == "--csv") {
            options.csv_path = next_value(i, "--csv");
        } else if (argument == "--html") {
            options.html_path = next_value(i, "--html");
        } else if (argument == "--txt") {
            options.text_path = next_value(i, "--txt");
        } else if (argument == "--pdf") {
            options.pdf_path = next_value(i, "--pdf");
        } else if (argument == "--fail-on") {
            const std::string value = next_value(i, "--fail-on");
            if (!parse_error && !ParseFailOn(value, options.fail_on)) {
                std::cerr << "--fail-on 取值非法: " << value << "\n";
                parse_error = true;
            }
        } else if (argument == "--out") {
            options.out_dir = next_value(i, "--out");
        } else if (argument == "--format") {
            const auto parts = SplitList(next_value(i, "--format"));
            for (const auto& part : parts) {
                qc::QcReportFormat format = qc::QcReportFormat::Json;
                if (!qc::ParseQcReportFormat(part, format)) {
                    std::cerr << "未知导出格式: " << part << "\n";
                    parse_error = true;
                } else {
                    options.formats.push_back(format);
                }
            }
        } else if (argument == "--ext") {
            options.extensions = SplitList(next_value(i, "--ext"));
        } else if (argument == "--jobs") {
            try {
                options.jobs = std::stoi(next_value(i, "--jobs"));
            } catch (const std::exception&) {
                std::cerr << "--jobs 需要整数\n";
                parse_error = true;
            }
        } else if (argument == "--summary") {
            options.summary_path = next_value(i, "--summary");
        } else if (argument == "--no-recursive") {
            options.recursive = false;
        } else if (argument == "--no-progress") {
            options.no_progress = true;
        } else if (argument == "-v" || argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "-q" || argument == "--quiet") {
            options.quiet = true;
        } else if (!argument.empty() && argument.front() == '-') {
            std::cerr << "未知选项: " << argument << "\n";
            parse_error = true;
        } else {
            options.positional.push_back(argument);
        }
    }

    if (parse_error) {
        PrintUsage();
        return static_cast<int>(ExitCode::Usage);
    }

    if (options.positional.empty()) {
        PrintUsage();
        return static_cast<int>(ExitCode::Usage);
    }

    options.command = options.positional.front();
    if (options.formats.empty()) options.formats.push_back(qc::QcReportFormat::Json);

    // Ctrl+C 之类我们不做处理（进程被杀即视为取消），仅保留接口上的取消能力给批量任务
    std::atomic<bool> cancelled{false};

    // ---- profiles ----
    if (options.command == "profiles") {
        return RunProfiles(options);
    }

    qc::QcProfile profile;
    std::string profile_error;
    if (!ResolveProfile(options.profile_arg, profile, profile_error)) {
        std::cerr << profile_error << "\n";
        return static_cast<int>(ExitCode::Usage);
    }
    analyzer::AnalysisOptions analysis_options = qc::OptionsForDepth(profile.depth);
    if (!options.depth_arg.empty()) {
        if (!qc::ParseQcAnalysisDepth(options.depth_arg, profile.depth)) {
            std::cerr << "--depth 取值非法: " << options.depth_arg << "\n";
            return static_cast<int>(ExitCode::Usage);
        }
        analysis_options = qc::OptionsForDepth(profile.depth);
    }
    const auto unknown_ids = qc::UnknownRuleIds(profile);
    if (!unknown_ids.empty()) {
        for (const auto& id : unknown_ids) {
            std::cerr << "警告: 模板引用了未知规则 id '" << id << "'，已忽略\n";
        }
    }

    const bool show_progress = !options.no_progress && !options.quiet;

    // ---- analyze ----
    if (options.command == "analyze") {
        if (options.positional.size() < 2) {
            std::cerr << "analyze 需要一个输入文件路径\n";
            return static_cast<int>(ExitCode::Usage);
        }
        const std::string input = options.positional[1];

        qc::QcRunner runner;
        qc::QcRunCallbacks callbacks;
        callbacks.progress = [&](double percent, const std::string& stage) {
            PrintProgress(percent, stage, show_progress);
        };
        callbacks.should_cancel = [&]() { return cancelled.load(); };

        const auto result = runner.AnalyzeFile(input, profile, analysis_options, callbacks);
        ClearProgressLine(show_progress);

        if (!result.ok) {
            std::cerr << "分析失败: " << result.error << "\n";
            if (result.error == "分析被取消") return static_cast<int>(ExitCode::Cancelled);
            return static_cast<int>(ExitCode::AnalysisFailed);
        }

        utils::QcExportBundle bundle;
        bundle.profile_id = profile.id;
        bundle.profile_name = profile.name;
        bundle.run = result;

        bool io_ok = true;
        if (!options.json_path.empty()) {
            io_ok &= utils::QcReportExporter::ExportJson(options.json_path, bundle);
        }
        if (!options.csv_path.empty()) {
            io_ok &= utils::QcReportExporter::ExportCsv(options.csv_path, bundle);
        }
        if (!options.html_path.empty()) {
            io_ok &= utils::QcReportExporter::ExportHtml(options.html_path, bundle);
        }
        if (!options.text_path.empty()) {
            io_ok &= utils::QcReportExporter::ExportText(options.text_path, bundle);
        }
        if (!options.pdf_path.empty()) {
            const auto pdf = utils::QcReportExporter::ExportPdf(options.pdf_path, bundle);
            io_ok &= pdf.ok;
            if (pdf.text_loss) {
                std::cerr << "提示: PDF 无法嵌入中文字形，相关文字已替换为 '?'，"
                          << "建议改用 --html\n";
            }
        }
        if (!io_ok) return static_cast<int>(ExitCode::OutputFailed);

        if (!options.quiet) {
            const auto& report = result.report;
            std::cout << report.file_name << "  " << report.score << "/100  " << report.verdict
                      << "  (致命 " << CountIssues(report, model::IssueSeverity::Critical)
                      << " / 错误 " << CountIssues(report, model::IssueSeverity::Error)
                      << " / 警告 " << CountIssues(report, model::IssueSeverity::Warning)
                      << " / 提示 " << CountIssues(report, model::IssueSeverity::Info) << ")\n";
            if (options.verbose) {
                for (const auto& issue : report.issues) {
                    std::cout << "    " << issue.SeverityText() << "  " << issue.rule_id
                              << "  " << issue.title << "\n";
                }
            }
        }

        return HasFailLevelIssue(result.report, options.fail_on)
                   ? static_cast<int>(ExitCode::IssuesFound)
                   : static_cast<int>(ExitCode::Ok);
    }

    // ---- batch ----
    if (options.command == "batch") {
        if (options.positional.size() < 2) {
            std::cerr << "batch 需要一个目录路径\n";
            return static_cast<int>(ExitCode::Usage);
        }
        const std::string root = options.positional[1];
        std::error_code error;
        if (!std::filesystem::exists(root, error)) {
            std::cerr << "目录不存在: " << root << "\n";
            return static_cast<int>(ExitCode::Usage);
        }

        bool dir_ok = true;
        const std::string out_dir = EnsureDirectory(options.out_dir, dir_ok);
        if (!dir_ok) return static_cast<int>(ExitCode::OutputFailed);

        auto items = qc::BatchQcRunner::Discover(root, options.recursive, options.extensions);
        if (items.empty()) {
            std::cout << "目录下没有匹配的文件: " << root << "\n";
            return static_cast<int>(ExitCode::Ok);
        }
        if (!options.quiet) {
            std::cout << "发现 " << items.size() << " 个文件，模板 " << profile.name
                      << "，并发 " << options.jobs << "\n";
        }

        qc::BatchQcOptions batch_options;
        batch_options.recursive = options.recursive;
        batch_options.extensions = options.extensions;
        batch_options.max_parallel = options.jobs;
        batch_options.keep_reports = false;  // 只留计数，报告随即落盘，内存占用与文件数无关
        if (!out_dir.empty()) {
            batch_options.output_path_factory =
                [root, out_dir, &options](const std::string& path) {
                    return RelativeReportPath(root, path, out_dir, options.formats.front());
                };
        }

        int finished = 0;
        std::atomic<bool> io_failed{false};
        qc::BatchQcCallbacks callbacks;
        callbacks.progress = [&](int done, int total) {
            if (!show_progress) return;
            std::cout << "\r  已完成 " << done << " / " << total << "            " << std::flush;
        };
        callbacks.item_finished = [&](const qc::BatchQcItemResult& item) {
            ++finished;
            if (item.status == qc::BatchItemStatus::Failed && !options.quiet) {
                std::cout << "\n  ! " << item.path << " -> " << item.error << "\n";
            }
        };

        // 每个 worker 拿到的是"分析 + 立刻落盘"的组合闭包。
        // 之所以不在全部跑完后统一写：批量 runner 已经把 keep_reports 关了，
        // 报告本体没有留在内存里。
        const auto base_analyze = qc::QcRunner::MakeAnalyzeFunction(profile, analysis_options);
        const qc::QcAnalyzeFn analyze = [base_analyze, &options, &profile, &root, out_dir,
                                         &io_failed](const qc::QcAnalyzeRequest& request) {
            qc::QcRunResult result = base_analyze(request);
            if (out_dir.empty() || !result.ok) return result;

            utils::QcExportBundle bundle;
            bundle.profile_id = profile.id;
            bundle.profile_name = profile.name;
            bundle.run = result;

            for (const auto format : options.formats) {
                const std::string target = RelativeReportPath(root, request.path, out_dir, format);
                if (!WriteBundleToPath(target, bundle, format)) io_failed.store(true);
            }
            return result;
        };

        qc::BatchQcRunner runner;
        const auto batch_run = runner.Run(items, batch_options, analyze, callbacks);
        ClearProgressLine(show_progress);

        if (!options.quiet) {
            std::cout << "完成 " << batch_run.summary.succeeded << " / 失败 "
                      << batch_run.summary.failed << " / 取消 " << batch_run.summary.cancelled
                      << " / 跳过 " << batch_run.summary.skipped << "，总耗时 "
                      << static_cast<int>(batch_run.summary.elapsed_ms) << " ms\n";
            std::cout << "致命 " << batch_run.summary.critical_count << " / 错误 "
                      << batch_run.summary.error_count << " / 警告 "
                      << batch_run.summary.warning_count << " / 提示 "
                      << batch_run.summary.info_count << "\n";
        }

        if (!options.summary_path.empty()) {
            const auto summary = utils::QcReportExporter::MakeBatchSummary(root, batch_run, profile);
            const bool written = utils::QcReportExporter::ExportBatchSummaryAuto(
                options.summary_path, summary);
            if (!written) io_failed.store(true);
        }
        if (io_failed.load()) return static_cast<int>(ExitCode::OutputFailed);

        if (batch_run.summary.failed > 0) return static_cast<int>(ExitCode::AnalysisFailed);
        if (!batch_run.summary.completed) return static_cast<int>(ExitCode::Cancelled);
        if (options.fail_on < 4 && CountAtLevel(batch_run.summary, options.fail_on) > 0) {
            return static_cast<int>(ExitCode::IssuesFound);
        }
        return static_cast<int>(ExitCode::Ok);
    }

    // ---- compare ----
    if (options.command == "compare") {
        if (options.positional.size() < 3) {
            std::cerr << "compare 需要两个文件路径\n";
            return static_cast<int>(ExitCode::Usage);
        }
        const std::string left_path = options.positional[1];
        const std::string right_path = options.positional[2];

        qc::QcRunner runner_left;
        qc::QcRunner runner_right;
        auto run_one = [&](const std::string& path, qc::QcRunner& runner) -> qc::QcRunResult {
            qc::QcRunCallbacks callbacks;
            const bool enabled = show_progress;
            callbacks.progress = [&](double percent, const std::string& stage) {
                PrintProgress(percent, stage, enabled);
            };
            callbacks.should_cancel = [&]() { return cancelled.load(); };
            auto result = runner.AnalyzeFile(path, profile, analysis_options, callbacks);
            ClearProgressLine(enabled);
            return result;
        };

        if (!options.quiet) std::cout << "分析 " << left_path << "\n";
        const auto left = run_one(left_path, runner_left);
        if (!left.ok) {
            std::cerr << "分析失败: " << left.error << "\n";
            return static_cast<int>(ExitCode::AnalysisFailed);
        }
        if (!options.quiet) std::cout << "分析 " << right_path << "\n";
        const auto right = run_one(right_path, runner_right);
        if (!right.ok) {
            std::cerr << "分析失败: " << right.error << "\n";
            return static_cast<int>(ExitCode::AnalysisFailed);
        }

        const auto comparison = qc::CompareRuns(left, right);

        bool io_ok = true;
        if (!options.json_path.empty()) {
            io_ok &= utils::QcReportExporter::ExportComparisonJson(options.json_path, comparison);
        }
        if (!options.csv_path.empty()) {
            io_ok &= utils::QcReportExporter::ExportComparisonCsv(options.csv_path, comparison);
        }
        if (!options.html_path.empty()) {
            io_ok &= utils::QcReportExporter::ExportComparisonHtml(options.html_path, comparison);
        }
        const std::string compare_text =
            options.text_path.empty() && options.pdf_path.empty()
                ? std::string()
                : utils::QcReportExporter::BuildComparisonText(comparison);
        if (!options.text_path.empty()) {
            std::ofstream file(options.text_path, std::ios::binary);
            if (!file.is_open()) {
                io_ok = false;
            } else {
                file << compare_text;
            }
        }
        if (!options.pdf_path.empty()) {
            std::cerr << "提示: 对比结果不支持 PDF，已改导文本 -> " << options.pdf_path << "\n";
            std::ofstream file(options.pdf_path, std::ios::binary);
            if (!file.is_open()) {
                io_ok = false;
            } else {
                file << compare_text;
            }
        }
        if (!io_ok) return static_cast<int>(ExitCode::OutputFailed);

        if (!options.quiet) {
            std::cout << utils::QcReportExporter::BuildComparisonText(comparison);
        }

        // 对比模式下：任一侧存在 fail_on 级别以上问题即返回非 0
        if (HasFailLevelIssue(left.report, options.fail_on) ||
            HasFailLevelIssue(right.report, options.fail_on)) {
            return static_cast<int>(ExitCode::IssuesFound);
        }
        return static_cast<int>(ExitCode::Ok);
    }

    std::cerr << "未知子命令: " << options.command << "\n\n";
    PrintUsage();
    return static_cast<int>(ExitCode::Usage);
}
