#pragma once

#include <QObject>
#include <QString>

#include <functional>

#include "core/player/MediaPlayer.h"

class QProgressDialog;
class QWidget;

namespace videoeye {
namespace ui {

// 导出协调器: 把 MainWindow 从「导出」职责里抽出来。
//
// 三个导出入口 (视频帧 / 视频 / 音频) 与「播放器导出信号 -> 进度框 / 提示」的
// 十条接线全部内聚在本类; MainWindow 只注入两个状态查询 (当前媒体 URL / 是否
// 裸图像模式) 并把 StatusMessage 转发给状态栏 —— 它不再持有任何导出状态。
//
// 与 MediaInfoCoordinator 同约定: 只发文本, 不直接碰主窗口的控件。对话框
// (文件选择 / 输入框 / 消息框 / 进度框) 的 parent 统一用构造时注入的 dialog_parent。
class ExportCoordinator : public QObject {
    Q_OBJECT

public:
    ExportCoordinator(player::MediaPlayer* player, QWidget* dialog_parent,
                      QObject* parent = nullptr);

    // 两个状态查询在导出入口被调用时读取, 不缓存 (返回值随 MainWindow 状态变)。
    using SourceQuery = std::function<QString()>;
    using RawImageQuery = std::function<bool()>;
    void SetSourceQueries(SourceQuery source_query, RawImageQuery raw_image_query);

    // 收起进度框 (打开新媒体的换媒体复位用; 无框时安全)。
    void HideProgress();

signals:
    // 状态栏文本 (MainWindow 转发给 statusBar()->showMessage)。
    void StatusMessage(const QString& text, int timeout = 0);

public slots:
    // 导出入口 (菜单动作接收者)
    void OnExportVideoFrames();
    void OnExportVideo();
    void OnExportAudio();

private slots:
    // 播放器导出信号处理器 (视频帧 / 音视频各五条)
    void OnVideoFrameExportStarted(int total_frames);
    void OnVideoFrameExportProgress(int exported_frames);
    void OnVideoFrameExportFinished(const QString& output_dir);
    void OnVideoFrameExportCanceled(int exported_frames, const QString& output_dir);
    void OnVideoFrameExportError(const QString& message);

    void OnMediaExportStarted(qint64 duration_ms);
    void OnMediaExportProgress(int percent);
    void OnMediaExportFinished(const QString& output_path);
    void OnMediaExportCanceled(const QString& output_path);
    void OnMediaExportError(const QString& message);

    void OnProgressCanceled();  // 进度框取消按钮

private:
    void ShowProgress(const QString& title, int maximum, const QString& label);

    player::MediaPlayer* player_ = nullptr;  // 不拥有
    QWidget* dialog_parent_ = nullptr;       // 对话框 parent (MainWindow); 不拥有

    // 导出进度框与其状态 (原 MainWindow 成员整体迁入)
    QProgressDialog* export_progress_dialog_ = nullptr;
    int export_total_frames_ = 0;

    // 导出类型跟踪 (用于进度对话框取消时调用正确的取消接口)
    enum class ActiveExport { None, Frames, Media };
    ActiveExport active_export_ = ActiveExport::None;

    SourceQuery source_query_;
    RawImageQuery raw_image_query_;
};

} // namespace ui
} // namespace videoeye