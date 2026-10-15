#pragma once

// 打开会话控制器: 把 MainWindow 从「打开一个媒体源」这件事里抽出来。
//
// MainWindow 原先在这里混了四类职责: 分派(裸图像 / PCM / 通用媒体)、换媒体复位
// (停播放 / 收进度框 / 作废上一次待收尾登记)、异步收尾(状态栏 + 标签 + 自动播放 +
// 失败时退回分析模式)、以及把新文件登记进分析/FFmpeg/首页三个下游。约 150 行，
// 且 pending_open_source_ / pending_open_autoplay_ / current_media_url_ 三个状态
// 只有这一段在用，却挂在主窗口上。
//
// 与 ExportCoordinator / MediaInfoCoordinator 同约定:
//   * 只发文本与信号，不直接碰主窗口的控件（顶部标签、状态栏由 MainWindow 自己更新）；
//   * 依赖在构造时注入，控制器不拥有它们；
//   * UI 交互（PCM 参数弹窗）以 std::function 注入，控制器不认识对话框。

#include <QObject>
#include <QString>

#include <functional>

// MediaPlayer 直接 include（与 ExportCoordinator.h 同做法）：头里要用
// player::MediaPlayer*，而 audit_namespace_layout 的 R3 不允许在 ui/ 下开具名
// 的 videoeye::player 子命名空间（命名空间必须跟目录走）。
#include "core/player/MediaPlayer.h"
#include "ui/main_window/open_session.h"

namespace videoeye {
namespace ui {

class AnalysisPanel;
class FfmpegPanel;
class MediaInfoCoordinator;
class OverviewPage;
class PlayerPanel;

class OpenSessionController : public QObject {
    Q_OBJECT

public:
    // 注入的依赖，全部由 MainWindow 拥有（控制器只借用，构造时都已就绪）。
    struct Deps {
        player::MediaPlayer* player = nullptr;
        PlayerPanel* player_panel = nullptr;
        AnalysisPanel* analysis_panel = nullptr;
        FfmpegPanel* ffmpeg_panel = nullptr;      // 可为 null（面板未创建）
        OverviewPage* overview_page = nullptr;
        MediaInfoCoordinator* media_info = nullptr;
        // 打开 PCM 前的参数弹窗（demuxer / 采样率 / 声道）。未注入时视为用户取消。
        std::function<bool(QString& /*demuxer*/, int& /*sample_rate*/, int& /*channels*/)>
            prompt_pcm;
        // 打开新文件前收掉导出进度框（导出协调器的界面部分）。
        // "终止进行中的导出"归 MediaPlayer::CancelAllExports()，不在这里。
        std::function<void()> hide_export_progress;
        // 换媒体后重新下发宏块分析的采集状态（"用户启用 || MV 叠加需要"的协调规则
        // 在 MainWindow / MacroblockCoordination 手里，这里只是到点回调一次）。
        std::function<void()> apply_macroblock_state;
    };

    explicit OpenSessionController(const Deps& deps, QObject* parent = nullptr);

    // 打开一个媒体源。返回 false 表示没打开（空路径 / 没播放器 / 取消了 PCM 参数 /
    // 打开失败），失败原因已经用 StatusMessage 报出去了 —— 调用方只据此决定继续与否。
    bool Open(const QString& source, bool autoplay);

    // 当前登记的媒体源（导出入口的「当前文件」查询读它）。
    // 裸图像模式下为空：那一类没有可导出的媒体流。
    const QString& current_media_url() const { return current_media_url_; }

signals:
    // 状态栏文本（MainWindow 转发给 statusBar()->showMessage）。
    void StatusMessage(const QString& text, int timeout = 0);
    // 顶部媒体路径标签的新文本（控制器不碰控件，交给 MainWindow 更新）。
    void CurrentMediaLabelChanged(const QString& text);

public slots:
    // MediaPlayer::OpenFinished 的收尾（UI 线程）。过期结果在这里被丢弃。
    void OnPlayerOpenFinished(bool ok);

private:
    bool OpenRawImage(const QString& source);
    bool OpenPcm(const QString& source, bool autoplay);
    bool OpenMediaAsync(const QString& source, bool autoplay);

    Deps deps_;
    QString current_media_url_;
    // 待收尾的异步打开登记（见 open_session.h 的口径说明）。
    OpenTicket ticket_;
};

}  // namespace ui
}  // namespace videoeye
