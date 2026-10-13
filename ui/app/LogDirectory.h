#pragma once

// 调试日志文件的位置选择
//
// 为什么要有这个文件：以前日志写死在 exe 同目录（main.cpp 里一行拼路径）。
// 开发机上无所谓，装到 `C:\Program Files\VideoEye` 或 `/opt/videoeye` 之后，
// 安装目录对普通用户是只读的 —— 要么日志直接写不进去（最需要日志的场景恰好
// 拿不到日志），要么触发 UAC 虚拟化把文件重定向到别处，用户按文档路径去找会扑空。
//
// 所以改成：优先写系统给应用的用户级数据目录，写不了才回退 exe 同目录，
// 并且把"回退了"这件事明确返回给调用方（main.cpp 会把它打进日志里）。
//
// 依赖边界：这是 ui 层，可以直接用 Qt；infrastructure/logging/Logger 不碰 Qt，
// 所以路径选择放在这里而不是塞进 Logger。

#include <QString>

namespace videoeye {
namespace ui {

// 选路结果。调用方要把 fell_back 的情况告知用户（至少写进日志），
// 否则"日志到底写哪去了"又变成一个需要猜的问题。
struct LogFileLocation {
    QString path;                  // 最终采用的日志文件绝对路径
    bool fell_back_to_app_dir = false; // true = 用户目录不可用，退回了 exe 同目录
    QString reason;                // 回退原因（用户目录不可写 / 无法创建 / 为空）
};

// 日志文件的体积上限，超过就滚动一次。Debug 级别下扫一个大文件几秒钟就能
// 写出几十 MB，不设上限的话它会悄悄吃满磁盘 —— 而日志是"排查时才有用的东西"，
// 不值得为它保留全部历史。
constexpr qint64 kMaxLogFileBytes = 16 * 1024 * 1024;  // 16 MiB
// 滚动保留的备份个数（.1 ~ .N）
constexpr int kLogBackupCount = 2;

// 解析出可用的日志文件路径。
// app_dir 是 exe 所在目录（回退用）；app_name 用于组织子目录，避免与其它应用混淆。
LogFileLocation ResolveDebugLogPath(const QString& app_dir, const QString& app_name = "VideoEye");

// 把超过上限的日志滚动成 .1 / .2（旧的往后推，末尾丢弃）。
// 只在启动前调用一次，不参与运行时 —— 滚动要挪文件，放在热路径上会卡顿。
void RotateLogFileIfOversized(const QString& path, qint64 max_bytes = kMaxLogFileBytes,
                              int backup_count = kLogBackupCount);

} // namespace ui
} // namespace videoeye
