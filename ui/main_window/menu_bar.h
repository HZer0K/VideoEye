#pragma once

// 菜单栏的构建（从 MainWindow::SetupMenuBar 搬出）。
//
// 菜单本身是纯粹的「动作清单」：谁在第几位、快捷键是什么、勾选项归哪个互斥组。
// 原先它和 MainWindow 挤在一起，于是主窗口既要知道菜单结构，又要知道每个动作背后
// 该调谁；搬出来之后菜单只认回调，MainWindow 只认自己拿到的那几个 QAction
// （视图菜单的两个勾选项需要在别处同步勾选状态）。
//
// 刻意不做成 QObject / 不做成控制器：菜单建完就是一堆挂在 QMenuBar 上的 QAction，
// 没有后续状态机，一个自由函数足够。

#include <QAction>
#include <QMenuBar>

#include <functional>

#include "core/domain/model/SeekMode.h"
#include "ui/player/PlayerPanel.h"

namespace videoeye {
namespace ui {

// 建完之后 MainWindow 还需要直接操作的动作。
// 其余动作（打开/退出/导出/seek）建好即自洽，不必留句柄。
struct MenuBarActions {
    QAction* export_frames = nullptr;   // 需要按当前是否有 raw 序列启用/禁用
    QAction* toggle_player = nullptr;   // 视图菜单：显示播放区，勾选态跟播放区显隐
    QAction* layout_monitor = nullptr;  // 视图菜单：监看布局
    QAction* layout_analysis = nullptr; // 视图菜单：分析布局
};

// 菜单动作背后该干什么。全部以回调注入 —— 菜单不认识 ExportCoordinator、
// 也不认识 MediaPlayer，于是这整块可以脱离它们单独构建。
struct MenuBarDeps {
    std::function<void()> open_file;
    std::function<void()> open_url;
    std::function<void()> exit_app;
    std::function<void()> show_format_support;
    std::function<void()> export_video_frames;
    std::function<void()> export_video;
    std::function<void()> export_audio;
    std::function<void(bool checked)> toggle_player_area;
    std::function<void(PlayerPanel::LayoutPreset)> apply_layout_preset;
    std::function<void(model::SeekMode)> set_seek_mode;
};

// 建好整条菜单栏。bar 通常是 MainWindow::menuBar()；重复调用会追加一组菜单
// （调用方负责先清空），目前只在构造期调一次。
MenuBarActions BuildMenuBar(QMenuBar* bar, const MenuBarDeps& deps);

} // namespace ui
} // namespace videoeye
