#pragma once

// 「这条命令把素材改成了什么」—— 把三份事实合到一张表里：
//   1) 输入文件的实测参数（FfmpegMediaProbe，来自 `ffmpeg -i <输入>`）
//   2) 命令行声明的工序（编码器 / 滤镜链 / copy / 丢弃 / 裁剪）
//   3) 输出文件的实测参数（跑完之后 `ffmpeg -i <输出>`）
//
// ⭐ 核心口径：**声明与实测分开，绝不互相补台。**
//   * 声明层只回答"命令让它做什么"。它不知道 scale=1280:-2 算出来的高度是多少、
//     也不知道 `-c copy` 之后容器会保留哪些 tag —— 所以声明层**不产出字段级变化**，
//     只产出工序节点（滤镜 / 编码 / copy / 丢弃）。
//   * 字段级变化（编码 1920×1080 → 1280×720）**只能**来自两侧实测。没探测到就写
//     Unknown，不拿"声明"顶上去假装对比过。
//   * 配对（哪条输入流变成哪条输出流）优先按 `-map` 解；没写 -map 就按类型顺序配，
//     并把"按类型顺序配对"这句话写进 pairing_note —— 它是个约定，不是事实。
//
// 为什么值得单独成层：多输出、-map、-vn、封面图、滤镜自动生成新流这几件事混在一起时，
// "凭印象画一张图"必然出错。做成纯函数才能被单测钉住，界面只负责摆矩形。

#include <QString>
#include <QStringList>

#include <vector>

#include "core/ffmpeg/FfmpegMediaProbe.h"

namespace videoeye {
namespace ffmpeg {

enum class ChangeState {
    Unchanged, // 两侧都读到且相同
    Changed,   // 两侧都读到且不同
    Added,     // 只有输出侧有（滤镜/lavfi 新生成的流）
    Removed,   // 只有输入侧有（-vn / -an / 只映射了部分流）
    Unknown,   // 至少一侧没读到 —— 不知道，不是"没变"
};

enum class ChangeSource {
    Measured, // 两侧实测对比出来的
    Declared, // 命令声明的（目前只用于工序节点，不参与字段级判定）
};

struct FieldChange {
    QString key;   // "codec" / "size" / "fps" / "bitrate" / "duration" / "container" …
    QString label; // "编码" / "分辨率" / "帧率" …
    QString before;
    QString after;
    ChangeState state = ChangeState::Unknown;
    ChangeSource source = ChangeSource::Measured;
    bool numeric = false;
    double before_value = 0.0; // 仅 numeric：用于画升降
    double after_value = 0.0;
    QString note; // 空 = 无需补充；例如"扩展名 .mov → .mp4，同一 demuxer"
};

/// 一道工序（滤镜 / 编码 / copy / 丢弃）属于哪一类。工序**全部**来自命令声明。
enum class ProcessKind {
    Filter,  // scale / unsharp / fps …
    Encoder, // libx265 / aac …
    Copy,    // -c copy
    Drop,    // -vn / -an / -sn
    Default, // 命令没指定，由 ffmpeg 按容器选
    Unknown,
};

/// 图上的一行：一条输入流 → 若干工序 → 一条输出流。
struct GraphRow {
    MediaStreamKind kind = MediaStreamKind::Unknown;
    int input_index = -1;  // 输入快照 streams 的下标；-1 = 没有对应输入流
    int output_index = -1; // 输出快照 streams 的下标；-1 = 没有对应输出流
    QString input_label;   // "0:v" / "0:a"；空 = 无输入
    QString input_summary; // "h264 High · 1920×1080 · 29.97 fps"
    QString output_label;  // "0:v"；空 = 未探测 / 无输出
    QString output_summary;
    QStringList filter_labels; // 该行走过的滤镜（按声明顺序）
    QString encoder_label;     // "libx265" / "copy" / "丢弃"
    QString encoder_detail;
    ProcessKind encoder_kind = ProcessKind::Default;
    ChangeState state = ChangeState::Unknown;
    std::vector<FieldChange> changes; // 只在这一行有实测两侧时才非空
    /// 这一行被判成 Unknown 的原因（"-vn 丢弃" / "输出未探测" / "command 无此路流"）
    QString note;
};

struct ChangeGraph {
    bool ok = false;
    QString note;   // 整体降级说明（输入未探测 / 结构不明 / 信息类命令 …）
    QString target; // 输出目标（可能为空：命令里没写输出）
    bool output_measured = false;
    QString output_error; // 探测失败的原因（output_measured=false 且尝试过）

    QString container_before;
    QString container_after;
    std::vector<FieldChange> container_changes; // 容器 / 时长 / 体积 / 流数

    std::vector<GraphRow> rows;
    QString pairing_note; // "按 -map 配对" / "按类型顺序配对（命令里没有 -map）"
    /// 命令里有 -filter_complex：工序无法逐路归属，图上只给一条整体说明。
    bool has_complex_filter = false;

    int CountRows(ChangeState state) const;
    int CountChanges(ChangeState state) const; // 含 container_changes
};

/// 生成每个输出一份图谱。measured 与命令里的输出顺序一一对应；没探测的那一项
/// 传 default-constructed（parsed=false）即可 —— 它会退化成"只有工序、没有对比"。
/// 信息类命令（-version / -encoders …）或命令里没有输出时返回空。
std::vector<ChangeGraph> BuildChangeGraphs(const MediaProbeSnapshot& input, const QStringList& arguments,
                                           const std::vector<MediaProbeSnapshot>& measured);

// ---- 文案 ----
QString ChangeStateName(ChangeState state);        // "未变" / "已变" / "新增" / "移除" / "未知"
QString ChangeStateColor(ChangeState state);       // "#d9a441" 之类（图与详情共用一份）
QString DescribeChange(const FieldChange& change); // "编码：h264 High → hevc Main（实测）"
/// 一行结论：只数"已变 / 新增 / 移除"，未变与未知单独给数 —— 别合成一个"共 N 项"。
QString DescribeChangeSummary(const ChangeGraph& graph);

} // namespace ffmpeg
} // namespace videoeye
