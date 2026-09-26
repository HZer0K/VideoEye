#pragma once

// 两个文件的对比（转码前后 / 归档前后 / 上游与自研转码对比）
//
// 对比的数据源是 analyzer::AnalysisResult 而不是 QcReport —— 后者只有问题清单，
// 拿不到"第二个 GOP 多长"这类对判断是否同一次重编码很有用的原始指标。
//
// 输出的最小单元是 QcCompareRow：一行一个字段，含左右两侧的字符串展示值、
// 数值与差值。导出器（JSON/CSV/HTML）与 UI 表格都直接消费这个结构，
// 加一个新对比项只要在 CompareRuns() 里加一行 AddXxx()。

#include <string>
#include <vector>

#include "core/qc/QcAnalyzeRequest.h"

namespace videoeye {
namespace qc {

enum class QcFieldDiff {
    Same,           // 两侧一致（数值类按容差判定）
    Different,      // 不一致
    OnlyLeft,       // 只有左文件有（右侧缺失，如转码后丢了时码轨）
    OnlyRight,
    Unavailable,    // 两侧都没有 / 没采集到（不算差异）
};

const char* ToString(QcFieldDiff diff);

struct QcCompareRow {
    std::string group;    // 分组名（容器 / 视频 / 码率与 GOP / 音频 / 色彩与 HDR）
    std::string field;
    std::string left;
    std::string right;
    QcFieldDiff diff = QcFieldDiff::Same;
    bool numeric = false;
    double left_value = 0.0;
    double right_value = 0.0;
    double delta = 0.0;   // right - left
    std::string unit;
};

struct QcComparison {
    std::string left_path;
    std::string right_path;
    std::string left_container;
    std::string right_container;
    double left_score = 0.0;
    double right_score = 0.0;
    std::string left_verdict;
    std::string right_verdict;

    std::vector<QcCompareRow> rows;

    int DifferentCount() const;
    int AvailableRowCount() const;
    // 各分组的行下标区间（导出/UI 分节展示用）
    std::vector<std::string> GroupNames() const;
};

// 数值判定用的相对/绝对容差：时长差 3 毫秒不算差异（容器时间基不同必然有舍入差），
// 响度差 0.1 LU 也不该提示"变了"。
struct QcCompareTolerance {
    double absolute = 0.0;   // abs(right-left) <= absolute 视为一致
    double relative = 0.0;   // 或 abs(delta) <= relative * max(|left|,1)
};

QcComparison CompareRuns(const QcRunResult& left, const QcRunResult& right,
                         const QcCompareTolerance& tolerance = QcCompareTolerance{});

}  // namespace qc
}  // namespace videoeye
