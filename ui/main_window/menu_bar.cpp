#include "ui/main_window/menu_bar.h"

#include <QActionGroup>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QObject>

#include <utility>

namespace videoeye {
namespace ui {
namespace {

// 用回调生成一个可被 Qt 直接连接的触发目标。
// QMenu::addAction 的 functor 重载只接受「无参、receiver 上下文」的可调用体，
// 而 deps 里的回调可能为空（调用方没注入就不该建这个动作），所以统一走一层壳：
// 空回调就地返回，非空才转发。
template <typename Fn>
auto Guard(Fn&& fn) {
    return [fn = std::forward<Fn>(fn)]() {
        if (fn)
            fn();
    };
}

} // namespace

MenuBarActions BuildMenuBar(QMenuBar* bar, const MenuBarDeps& deps) {
    MenuBarActions actions;
    if (!bar)
        return actions;

    // 文件菜单
    QMenu* file_menu = bar->addMenu(QObject::tr("文件"));
    file_menu->addAction(QObject::tr("打开本地文件"), QKeySequence::Open, Guard(deps.open_file));
    file_menu->addAction(QObject::tr("打开URL"), QKeySequence(QStringLiteral("Ctrl+U")), Guard(deps.open_url));
    // 导出 子菜单
    QMenu* export_menu = file_menu->addMenu(QObject::tr("导出"));
    actions.export_frames = export_menu->addAction(QObject::tr("导出视频帧..."), Guard(deps.export_video_frames));
    export_menu->addAction(QObject::tr("导出视频..."), Guard(deps.export_video));
    export_menu->addAction(QObject::tr("导出音频..."), Guard(deps.export_audio));
    file_menu->addSeparator();
    file_menu->addAction(QObject::tr("退出"), QKeySequence::Quit, Guard(deps.exit_app));

    // 帮助菜单 (先声明, 以便在其之前插入"播放设置"与"视图")
    QMenu* help_menu = bar->addMenu(QObject::tr("帮助"));
    help_menu->addAction(QObject::tr("支持的格式与编解码器..."), Guard(deps.show_format_support));
    help_menu->addSeparator();
    help_menu->addAction(QObject::tr("关于"), []() {
        QMessageBox::about(nullptr, QObject::tr("关于"), QObject::tr("VideoEye 2.0\n现代化的视频流分析软件"));
    });

    // 播放设置菜单 (位于 "文件" 与 "帮助" 之间)
    QMenu* playback_menu = new QMenu(QObject::tr("播放设置"), bar);
    bar->insertMenu(help_menu->menuAction(), playback_menu);

    // 视图菜单 (位于 "播放设置" 与 "帮助" 之间)
    QMenu* view_menu = new QMenu(QObject::tr("视图"), bar);
    bar->insertMenu(help_menu->menuAction(), view_menu);

    actions.toggle_player = new QAction(QObject::tr("显示播放区"), bar);
    actions.toggle_player->setCheckable(true);
    actions.toggle_player->setChecked(true);
    actions.toggle_player->setShortcut(QKeySequence(QStringLiteral("F9")));
    actions.toggle_player->setStatusTip(QObject::tr("收起播放区可让分析区占满整个内容区 (VideoEye 以分析为主)"));
    if (deps.toggle_player_area) {
        QObject::connect(actions.toggle_player, &QAction::toggled,
                         [fn = deps.toggle_player_area](bool checked) { fn(checked); });
    }
    view_menu->addAction(actions.toggle_player);

    // 播放区布局预设: 监看 = 播放区 58%; 分析 = 播放区 30% (小屏默认走分析)。
    // 与上面的显隐开关正交 —— 预设管比例, 显隐管在不在。
    QMenu* layout_menu = view_menu->addMenu(QObject::tr("播放区布局"));
    QActionGroup* layout_ag = new QActionGroup(layout_menu);
    layout_ag->setExclusive(true);
    actions.layout_monitor = layout_ag->addAction(QObject::tr("监看布局"));
    actions.layout_monitor->setCheckable(true);
    actions.layout_monitor->setStatusTip(QObject::tr("播放区占大头，适合盯画面 / 逐帧定位"));
    actions.layout_analysis = layout_ag->addAction(QObject::tr("分析布局"));
    actions.layout_analysis->setCheckable(true);
    actions.layout_analysis->setStatusTip(QObject::tr("分析区占大头，适合看表格与扫描结果"));
    actions.layout_monitor->setChecked(true);
    layout_menu->addAction(actions.layout_monitor);
    layout_menu->addAction(actions.layout_analysis);
    // 「分析」那一项要在 lambda 里认回来，而 actions 是本函数的返回值、此刻还在填，
    // 所以先把指针拷出来再捕获（拷的是 QAction*，不是整个返回值）。
    QAction* const analysis_action = actions.layout_analysis;
    QObject::connect(
        layout_ag, &QActionGroup::triggered, [analysis_action, apply = deps.apply_layout_preset](QAction* action) {
            if (!action || !apply)
                return;
            apply(action == analysis_action ? PlayerPanel::LayoutPreset::Analysis : PlayerPanel::LayoutPreset::Monitor);
        });

    QMenu* seek_mode_menu = playback_menu->addMenu(QObject::tr("seek方式"));
    QActionGroup* seek_ag = new QActionGroup(playback_menu);
    seek_ag->setExclusive(true);

    QAction* act_keyframe = seek_ag->addAction(QObject::tr("关键帧"));
    act_keyframe->setCheckable(true);
    act_keyframe->setData(static_cast<int>(model::SeekMode::NearestKeyframe));
    act_keyframe->setChecked(true); // 默认: 关键帧 (与原行为一致)
    seek_mode_menu->addAction(act_keyframe);

    QAction* act_exact = seek_ag->addAction(QObject::tr("精确值"));
    act_exact->setCheckable(true);
    act_exact->setData(static_cast<int>(model::SeekMode::ExactFrame));
    seek_mode_menu->addAction(act_exact);

    QObject::connect(seek_ag, &QActionGroup::triggered, [deps](QAction* action) {
        if (!action || !deps.set_seek_mode)
            return;
        deps.set_seek_mode(static_cast<model::SeekMode>(action->data().toInt()));
    });

    return actions;
}

} // namespace ui
} // namespace videoeye
