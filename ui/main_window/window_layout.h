#pragma once

// 主窗口的两条尺寸/布局口径（从 MainWindow 抽出，可单测）。
//
// 这两条原先埋在事件回调里（UpdateMinimumWindowSize / showEvent），属于典型的
// 「改错了不崩、只是窗口变得难用」：
//   * 最小尺寸算漏了某条栏的高度 -> 窗口能被拖到比内容还矮，底部分析页被裁掉；
//   * 小屏阈值判反 -> 1366x768 的笔记本上一打开就超出可用高度，窗口放不下
//     （这条以前真出过：固定 480px 播放区 + 250px 内容区连菜单栏一起超过 768）。
// 抽成纯函数之后，两条口径都能脱离 QWidget 直接断言。
//
// 第三组口径（PreferredLayoutForPage / ShouldAutoApplyLayout）同样在这里：
// 「切到哪一页该给分析区多少空间」是产品口径，不是控件操作 —— 埋在
// OnSidebarChanged 里的话，只能靠人肉点着看，改错了也只会被当成"排版就这样"。

#include <QSize>
#include <QString>

namespace videoeye {
namespace ui {

// 播放区布局预设（与 PlayerPanel::LayoutPreset 同义，这里不 include 那个头 ——
// 本文件要给纯逻辑单测用，不该拖进整个播放面板）。
//
// ⚠️ 三个值必须与 PlayerPanel::LayoutPreset 一一对应（MainWindow 在两处之间做
// 手工映射），增删一个值时两边一起改。
enum class WindowLayoutPreset {
    Monitor,    // 上下分割，播放区占大头（58%）：盯画面 / 逐帧定位
    Analysis,   // 上下分割，分析区占大头（播放区 30%）：看表与分析结果
    SideBySide, // 左右分割，预览在右、分析在左：宽屏同时看两边
};

// 可用高度低于该值时按「分析」布局起手。
// 860 是 1366x768 笔记本减掉任务栏之后的典型可用高度：再往下，固定 480px 的播放区
// 加上 250px 的最小内容区就装不下了。
constexpr int kSmallScreenHeightThreshold = 860;

// 最小尺寸的输入：各条栏与两个内容区的实测高度。
struct WindowLayoutMetrics {
    int app_bar_height = 0;     // 顶部应用栏
    int menu_bar_height = 0;    // 菜单栏
    int status_bar_height = 0;  // 状态栏
    int scan_bar_height = 0;    // 扫描任务条（未在扫描时为 0 —— 它与内容同生共死）
    int content_min_height = 0; // 内容栈的最小高度（分析页自己报）
    int player_min_height = 0;  // 播放区的最小高度（收起时为 0）
    int sidebar_width = 0;      // 侧栏当前宽度（0 表示还没有侧栏）
};

// 基础最小宽度：侧栏之外的主内容区。低于这个宽度分析表格就横着滚了。
constexpr int kBaseMinimumWidth = 900;

inline QSize MinimumWindowSize(const WindowLayoutMetrics& metrics) {
    // 高度是「上面几条栏 + 内容区最小 + 播放区最小」的硬加总；播放区收起时
    // player_min_height 为 0，于是窗口可以缩得更矮（这是刻意的）。
    // 任务条按可见高度计入：漏了它，扫描一起窗口就比内容矮一截，底部分析页被裁掉。
    const int min_height = metrics.app_bar_height + metrics.menu_bar_height + metrics.status_bar_height +
                           metrics.scan_bar_height + metrics.content_min_height + metrics.player_min_height;
    return QSize(kBaseMinimumWidth + metrics.sidebar_width, min_height);
}

// 首次显示时用哪个布局预设。
// available_height <= 0 表示拿不到屏幕信息（离屏/无屏），退回默认的「监看」——
// 宁可按常规尺寸起手，也不要因为读不到屏幕就把播放区压成一条。
inline WindowLayoutPreset InitialLayoutPreset(int available_height) {
    if (available_height > 0 && available_height < kSmallScreenHeightThreshold) {
        return WindowLayoutPreset::Analysis;
    }
    return WindowLayoutPreset::Monitor;
}

// 窗口当前尺寸是否已经低于最小尺寸（resizeEvent 里据此强制拉回）。
inline bool IsBelowMinimum(const QSize& current, const QSize& minimum) {
    return current.width() < minimum.width() || current.height() < minimum.height();
}

// ---------------------------------------------------------------------------
// 「切到这一页，播放区该给多少空间」
// ---------------------------------------------------------------------------
//
// VideoEye 的主语是分析，播放只是定位手段。但页面之间的空间诉求差别很大：
//   * 「总览」是落地页，用户先想确认素材长什么样 —— 播放区该大一点；
//   * 「质量诊断」「文件结构」这类页面是表格 + 图表，纵向空间本来就紧张
//     （诊断页里图表最小 220、问题表最小 200，再压就只剩一行）；
//   * 「FFmpeg 命令」自己内部就有 splitter 与滚动条，播放区抢高度会让输出区
//     拿不到剩余空间。
// 所以按 pageId 给一个默认取向，换页时用一次；用户随时可以在应用栏里改。
inline WindowLayoutPreset PreferredLayoutForPage(const QString& page_id) {
    // 只有"落地页"和"自带分栏的工具页"保留大播放区；其余都是分析内容优先。
    if (page_id == QLatin1String("overview"))
        return WindowLayoutPreset::Monitor;
    if (page_id == QLatin1String("ffmpeg_command"))
        return WindowLayoutPreset::Monitor;
    return WindowLayoutPreset::Analysis;
}

// 换页时是否真的动手改比例。
//
//   * 并排（SideBySide）是用户为宽屏显式选的模式，换页不去动它 —— 否则宽屏用户
//     每点一个页面就被掰回上下分割，等于这个模式没法用；
//   * 已经是目标预设时不动：拖动过分隔条的用户不该因为"点了同一页"被重置。
inline bool ShouldAutoApplyLayout(WindowLayoutPreset current, WindowLayoutPreset preferred) {
    if (current == WindowLayoutPreset::SideBySide)
        return false;
    return current != preferred;
}

} // namespace ui
} // namespace videoeye
