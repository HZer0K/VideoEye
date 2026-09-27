#pragma once

// 找到「FFmpeg 命令工作台」要执行的那个 ffmpeg 程序，并在找不到时给出可执行的补救步骤。
//
// 它和 cmake/deps/FFmpegPrebuilt.cmake 找的**不是同一个东西**:
//   那边找的是开发包（include/ + lib/），用来链接 libav* 做分析；
//   这边找的是可执行程序。两者版本可以不一样 —— 所以页面必须把实际执行的
//   程序路径和 `-version` 输出显示出来，别让人以为跑的是链接进去的那份库。
//
// 查找顺序（前者优先）:
//   1. 用户在设置里指定的路径      —— 唯一能覆盖一切的入口（Linux/macOS 尤其需要）
//   2. 随包分发的那份              —— Windows 上构建时拷到可执行文件旁的 ffmpeg.exe
//   3. 构建期探测到的绝对路径      —— 只在 Debug 构建下排在这里（见下）
//   4. PATH                        —— 最常见的安装方式
//   5. 各平台的常见安装目录        —— scoop / Chocolatey / Homebrew / 手工解压目录等
//   6. 构建期探测到的绝对路径      —— 兜底：本机哪儿都找不到时的最后一次尝试
//
// 为什么构建期路径要分两处: 它是**构建机**的事实。开发机自己跑它最合适（Debug 下
// 优先）；但安装版如果还认它，用户在自己机器上装的新版 ffmpeg 就永远用不上，
// 安装包还要凭空依赖一台它不该知道的开发机。所以 Release 下它降级为兜底。
//
// 关于"用户没装 ffmpeg":
//   工作台干的活就是**调用原生 ffmpeg 程序**，这件事没法用链接进来的 libav* 替代
//   （ffmpeg 命令行是几万个选项的庞大前端，重实现一遍既不现实也不该承诺）。
//   所以正确的做法不是假装能跑，而是把"缺程序"做成一等状态:
//     * 打包时尽量带上（Windows 随包分发 ffmpeg.exe）；
//     * 运行时尽力找（含不在 PATH 里的常见安装位置）；
//     * 真找不到就给平台化的安装指引 + 手动指定入口；
//     * 这一页之外的功能（分析 / QC / 播放）一个都不受影响 —— 它们用的是内置库。

#include <QString>
#include <QStringList>

#include "videoeye/FfmpegToolConfig.h"

namespace videoeye {
namespace ffmpegtool {

struct FfmpegToolInfo {
    QString path;                // 解析出的路径（可能为空 = 没找到）
    bool exists = false;         // 文件是否真的在
    QString origin;              // "用户设置" / "随包分发" / "构建期探测" / "PATH" / "常见安装位置" / "未找到"
    QString bundled_name;        // 随包分发时的文件名
    bool bundled = false;        // 本项目是否随包分发了 ffmpeg
};

/// 平台相关的安装指引。内容全部来自本文件，不查网络、不依赖任何外部资源。
struct FfmpegInstallGuide {
    QString platform;               // "Windows" / "macOS" / "Linux" / "其他"
    QString headline;               // 一句话结论
    QStringList package_commands;   // 包管理器命令（可能为空）
    QString download_url;           // 官方下载页
    QStringList manual_steps;       // 手动安装步骤
    QString note;                   // 额外提示（作用域 / 许可证 / 架构差异）
};

class FfmpegToolLocator {
public:
    /// 按优先级解析一个可用路径。configured_path 为空表示"用户没指定"。
    static FfmpegToolInfo Resolve(const QString& configured_path = QString());

    /// 是否可执行（存在 + 是可执行文件）。Linux/macOS 会顺带看可执行位。
    static bool IsExecutable(const QString& path);

    /// 从 `ffmpeg -version` 的输出里摘出版本号；失败返回空串。
    static QString ParseVersionLine(const QString& version_output);

    /// 当前平台的安装指引（离线内置，不联网）。
    static FfmpegInstallGuide InstallGuide();

    /// 当前平台名，用于界面文案。
    static QString PlatformName();

    /// "装了但没进 PATH" 时的候选目录。Windows 上会展开环境变量。
    static QStringList CommonSearchDirs();

    /// 在给定目录里找 ffmpeg（按平台候选文件名逐个试），找不到返回空串。
    static QString FindInDirectory(const QString& dir);
};

}  // namespace ffmpegtool
}  // namespace videoeye
