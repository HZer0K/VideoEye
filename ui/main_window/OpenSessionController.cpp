#include "ui/main_window/OpenSessionController.h"

#include "core/player/MediaPlayer.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/ffmpeg_panel/FfmpegPanel.h"
#include "ui/main_window/MediaInfoCoordinator.h"
#include "ui/main_window/OverviewPage.h"
#include "ui/player/PlayerPanel.h"

namespace videoeye {
namespace ui {

OpenSessionController::OpenSessionController(const Deps& deps, QObject* parent)
    : QObject(parent), deps_(deps) {}

bool OpenSessionController::Open(const QString& source, bool autoplay) {
    if (source.isEmpty() || !deps_.player) {
        return false;
    }

    // 任何一次新打开都先作废上一次异步打开的收尾回调：只有真正走 OpenAsync 的路径
    // 会在下面重新登记。这样裸图像 / PCM 这类同步分支期间若有旧的异步结果迟到，
    // OnPlayerOpenFinished 会因"没有待收尾登记"而直接忽略。
    ClearOpenTicket(ticket_);

    // 换媒体复位。注意这里**只**管界面那部分 —— "终止进行中的导出"是媒体生命周期
    // 的保证，归 MediaPlayer::CancelAllExports()，由打开链路自己负责（打开媒体的
    // 入口不止这一个）。以前这里只调了 CancelVideoFrameExport()：媒体转码导出既不
    // 会被取消、排队请求也没清，旧任务还能把完成信号串回新媒体的界面。
    if (deps_.hide_export_progress) {
        deps_.hide_export_progress();
    }
    // 停止当前播放并清理播放模块状态（Raw / 音频可视化 / 进度条等）
    if (deps_.player_panel) {
        deps_.player_panel->StopPlayback();
    }

    switch (ClassifyOpenTarget(source)) {
        case OpenTargetKind::RawImage: return OpenRawImage(source);
        case OpenTargetKind::Pcm: return OpenPcm(source, autoplay);
        case OpenTargetKind::Media: break;
    }
    return OpenMediaAsync(source, autoplay);
}

bool OpenSessionController::OpenRawImage(const QString& source) {
    if (!deps_.player_panel) return false;

    // 裸图像不走 player_->Open()，拿不到打开链路里那次统一的导出取消 ——
    // 这里显式补上，否则旧媒体的抽帧 / 转码导出会继续在后台跑。
    deps_.player->CancelAllExports();
    deps_.player_panel->SetRawImageMode(true);
    deps_.player_panel->SetCurrentSource(QString());
    current_media_url_.clear();

    if (!deps_.player_panel->LoadRawImageFile(source)) {
        emit StatusMessage(tr("打开失败: %1").arg(source));
        return false;
    }
    emit StatusMessage(tr("已打开图像: %1").arg(source));
    emit CurrentMediaLabelChanged(source);

    // 首页只登记文件名（裸图像没有 StreamInfo 可摘要）；媒体信息框保留
    // LoadRawImageFile 通过 RawImageInfoReady 写入的 Raw 详情
    // （旧行为：此处会用"无 MediaInfo 数据"覆盖掉，导致 Raw 信息一闪而过）。
    if (deps_.overview_page) {
        deps_.overview_page->SetOpenedFileWithoutSummary(source);
    }
    return true;
}

bool OpenSessionController::OpenPcm(const QString& source, bool autoplay) {
    if (!deps_.player_panel || !deps_.analysis_panel) return false;

    QString demuxer_name;
    int sample_rate = 44100;
    int channels = 2;
    // 裸流无法自动探测，格式参数由用户现场指定。弹窗是 UI 交互，注入进来，
    // 控制器不认识对话框；没注入就当作用户取消。
    if (!deps_.prompt_pcm || !deps_.prompt_pcm(demuxer_name, sample_rate, channels)) {
        emit StatusMessage(tr("已取消打开 PCM: %1").arg(source));
        return false;
    }

    deps_.player_panel->SetRawImageMode(false);
    deps_.player_panel->SetCurrentSource(source);
    if (!deps_.player->OpenRawPcm(source, demuxer_name, sample_rate, channels)) {
        emit StatusMessage(tr("打开 PCM 失败: %1").arg(source));
        return false;
    }

    emit StatusMessage(tr("已打开 PCM: %1").arg(source));
    emit CurrentMediaLabelChanged(tr("%1 [PCM %2, %3 Hz, %4 ch]")
                                      .arg(source, demuxer_name)
                                      .arg(sample_rate)
                                      .arg(channels));

    current_media_url_ = source;
    deps_.analysis_panel->SetCurrentVideoPath(source);
    if (deps_.ffmpeg_panel) {
        deps_.ffmpeg_panel->SetCurrentFile(source);
    }
    if (deps_.overview_page) {
        deps_.overview_page->SetOpenedFileWithoutSummary(source);
        // 媒体信息解析 PCM（裸流无法自动探测，需带上用户选择的格式参数）
        if (deps_.media_info) {
            deps_.overview_page->SetMediaInfoText(
                deps_.media_info->DescribeRawPcm(source, demuxer_name, sample_rate, channels));
        }
    }

    if (autoplay) {
        deps_.player->Play();
    }
    return true;
}

bool OpenSessionController::OpenMediaAsync(const QString& source, bool autoplay) {
    if (!deps_.player_panel || !deps_.analysis_panel) return false;

    deps_.player_panel->SetRawImageMode(false);
    deps_.player_panel->SetCurrentSource(source);
    current_media_url_ = source;
    deps_.analysis_panel->SetCurrentVideoPath(source);
    if (deps_.ffmpeg_panel) {
        deps_.ffmpeg_panel->SetCurrentFile(source);
    }

    // 事务式异步打开：UI 线程只做"换媒体复位"并登记收尾信息，耗时的
    // avformat 打开 / 探测被派到后台线程；完成后经 OpenFinished 排回 UI 线程提交。
    // 收尾（状态栏 / 标签 / 自动播放 / 诊断扫描）见 OnPlayerOpenFinished。
    // 打开失败也不再直接中止：仍把文件加载到分析模块，由媒体信息 / 文件结构 /
    // 诊断扫描给出错误原因。
    ArmOpenTicket(ticket_, source, autoplay);
    deps_.player->OpenAsync(source);

    // 同步已启用的分析功能到播放器（复选框默认勾选但未触发信号）。
    // 须放在 OpenAsync 之后：它内部先做了换媒体复位。
    deps_.analysis_panel->EmitInitialFeatureStates();
    // 宏块分析不在 EmitInitialFeatureStates 里重发（见其注释），换文件后在此按
    // "用户启用 || 叠加需求"重新下发，同时校准宏块页勾选框与播放器特征开关。
    if (deps_.apply_macroblock_state) {
        deps_.apply_macroblock_state();
    }

    // 媒体信息解析：不再独立探测 —— 打开链路在后台探测时已顺带把同一个上下文
    // 格式化好，提交成功后经 MediaInfoTextReady 贴进首页；这里只放占位文本。
    // 打开失败的"分析模式"由 OnPlayerOpenFinished 退回协调器做尽力而为的独立解析
    // （此时没有可复用的上下文）。
    if (deps_.overview_page) {
        deps_.overview_page->SetMediaInfoText(tr("(正在解析媒体信息…)"));
    }
    return true;
}

void OpenSessionController::OnPlayerOpenFinished(bool ok) {
    QString source;
    bool autoplay = false;
    // 过期 / 无关的回调（对应请求已被后续打开取代，或这次打开根本没走异步）
    // 直接忽略：TakeOpenTicket 取不到登记就说明没有人在等它。
    if (!TakeOpenTicket(ticket_, &source, &autoplay)) {
        return;
    }
    if (!deps_.player) {
        return;
    }

    if (ok) {
        emit StatusMessage(tr("已打开: %1").arg(source));
    } else {
        // 打开失败不弹错误框：文件仍然进了分析模块（分析模式），状态栏只做提示。
        // timeout=0 表示常驻，否则这条原因会被下一次统计刷新冲掉。
        emit StatusMessage(tr("无法播放 (已进入分析模式): %1").arg(deps_.player->GetLastError()), 0);
    }
    emit CurrentMediaLabelChanged(ok ? source : tr("%1 (无法播放)").arg(source));

    // ---- 首页文件摘要 ----
    // 打开成功时贴 StreamInfo；失败（分析模式）至少要把文件名留在首页上。
    if (deps_.overview_page) {
        if (ok) {
            deps_.overview_page->SetFileSummary(deps_.player->GetStreamInfo());
        } else {
            deps_.overview_page->SetOpenedFileWithoutSummary(source);
        }
    }

    if (ok) {
        if (autoplay) {
            deps_.player->Play();
        }
        // 打开即分析：后台自动跑一次全文件诊断扫描（扩展名 / 容器一致性、GOP、
        // 时间戳、音频 QC 等规则在「诊断与报告」页直接给出原因与修复建议；
        // 大文件可在该页取消）。
        deps_.analysis_panel->StartDiagnosticsScanForCurrentFile();
    } else {
        // 分析模式：补跑文件结构分析与全文件诊断扫描，让「文件结构」
        // 「诊断与报告」「码率与 GOP」页展示该文件的具体错误。
        deps_.player->RequestContainerStructureAnalysis(source);
        if (deps_.analysis_panel) {
            deps_.analysis_panel->StartDiagnosticsScanForCurrentFile();
        }
        // 打开失败时没有可复用的 AVFormatContext（提交被整体丢弃），媒体信息
        // 退回协调器做一次尽力而为的独立解析（异常文件也可能部分解析成功）。
        if (deps_.media_info) {
            deps_.media_info->StartAsync(source);
        }
    }
}

}  // namespace ui
}  // namespace videoeye
