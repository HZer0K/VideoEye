#pragma once

// 主窗口的两条尺寸/布局口径（从 MainWindow 抽出，可单测）。
//
// 这两条原先埋在事件回调里（UpdateMinimumWindowSize / showEvent），属于典型的
// 「改错了不崩、只是窗口变得难用」：
//   * 最小尺寸算漏了某条栏的高度 -> 窗口能被拖到比内容还矮，底部分析页被裁掉；
//   * 小屏阈值判反 -> 1366x768 的笔记本上一打开就超出可用高度，窗口放不下
//     （这条以前真出过：固定 480px 播放区 + 250px 内容区连菜单栏一起超过 768）。
// 抽成纯函数之后，两条口径都能脱离 QWidget 直接断言。

#include <QSize>

namespace videoeye {
namespace ui {

// 播放区布局预设（与 PlayerPanel::LayoutPreset 同义，这里不 include 那个头 ——
// 本文件要给纯逻辑单测用，不该拖进整个播放面板）。
enum class WindowLayoutPreset {
    Monitor,   // 播放区占大头（58%）
    Analysis,  // 播放区占小头（30%）
};

// 可用高度低于该值时按「分析」布局起手。
// 860 是 1366x768 笔记本减掉任务栏之后的典型可用高度：再往下，固定 480px 的播放区
// 加上 250px 的最小内容区就装不下了。
constexpr int kSmallScreenHeightThreshold = 860;

// 最小尺寸的输入：各条栏与两个内容区的实测高度。
struct WindowLayoutMetrics {
    int app_bar_height = 0;      // 顶部应用栏
    int menu_bar_height = 0;     // 菜单栏
    int status_bar_height = 0;   // 状态栏
    int content_min_height = 0;  // 内容栈的最小高度（分析页自己报）
    int player_min_height = 0;   // 播放区的最小高度（收起时为 0）
    int sidebar_width = 0;       // 侧栏当前宽度（0 表示还没有侧栏）
};

// 基础最小宽度：侧栏之外的主内容区。低于这个宽度分析表格就横着滚了。
constexpr int kBaseMinimumWidth = 900;

inline QSize MinimumWindowSize(const WindowLayoutMetrics& metrics) {
    // 高度是「上面几条栏 + 内容区最小 + 播放区最小」的硬加总；播放区收起时
    // player_min_height 为 0，于是窗口可以缩得更矮（这是刻意的）。
    const int min_height = metrics.app_bar_height + metrics.menu_bar_height +
                           metrics.status_bar_height + metrics.content_min_height +
                           metrics.player_min_height;
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

}  // namespace ui
}  // namespace videoeye
