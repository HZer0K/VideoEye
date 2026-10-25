#include "core/player/MediaPlayer.h"
#include "core/analysis/stream/StreamAnalyzer.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/MacroblockInfo.h"
#include "core/exporter/MediaExporter.h"
#include "core/player/AnalysisSession.h"
#include "core/player/ContainerInspectionController.h"
#include "core/player/ExportController.h"
#include "core/player/OpenController.h"
#include "core/player/PlaybackSession.h"
#include "core/player/RealtimeAnalysisController.h"
#include "infrastructure/concurrency/TaskManager.h"
#include "infrastructure/logging/Logger.h"
#include <QByteArray>
#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
}

namespace videoeye {
namespace player {

// 析构时等待后台任务退出的总预算(ms)。受管 std::thread 在 CancelAll 之后会因
// 中断回调及时退出, 这里给一个上限而不是无限等(-1), 避免极端情况下进程退出挂死。
// 注意: 这个预算现在约束的是 WaitForAll **整个函数**(含受管线程回收阶段), 不再是只管
// "等终态"。可能卡住的任务已改用 RunBlockingIo 登记 —— 那种任务超预算会被放弃(detach),
// 不会再拖住退出流程; 剩下的协作式任务有中断机制兜底, 实际不会走到上限。
constexpr int kShutdownWaitMs = 8000;

// MediaPlayer 的实现体。公共头只保留一个不完整类型（见 MediaPlayer.h），
// 会话 / 控制器 / libav* 的全部依赖都收在这个文件里，不再传染给下游 include。
// 门面的全部私有逻辑（打开事务、换媒体复位、分析开关分派）也是本结构的成员 ——
// 所以 MediaPlayer.h 的 private 区只剩一个 impl_，改实现一律不用动公共头。
//
// 成员声明顺序 = 构造顺序，也是依赖顺序：
//   * playback_session_、analysis_session_ 必须早于借用它们的控制器；
//   * task_manager_ 必须早于 ExportController / ContainerInspectionController。
struct MediaPlayer::Impl {
    explicit Impl(MediaPlayer* facade)
        : facade_(facade),
          // 控制器要拿当前 url 判断"排队请求续跑前有没有换过媒体"，用取值回调而不是
          // 拷一份 QString —— 拷的那份在换文件后就是过期的，排队请求会拿新文件往旧目录里导。
          export_controller_(
              task_manager_, [this]() { return current_url_; }, facade),
          container_inspection_(task_manager_, facade),
          // 控制器借用本类的 analysis_session_ / playback_session_（都声明在它之前），
          // 并在构造时把分析 hook 装进播放会话 —— 所以播放机械照旧对分析一无所知。
          realtime_analysis_(analysis_session_, playback_session_, facade),
          // 打开控制器借用本类的播放会话（只持引用），并在提交时把成果一次性写进会话。
          // 取消/中断状态（current_attempt_）由本 Impl 持有，Prepare 时经 OpenPrepareParams 传入。
          open_controller_(playback_session_, facade) {
        // 与下面的 avformat_network_deinit() 配对: 两者都住 Impl，门面构造函数不再碰 FFmpeg。
        avformat_network_init();
    }

    // 收尾顺序与拆分前的门面析构逐条一致：导出 -> 后台任务 -> 播放 -> 会话释放 -> FFmpeg。
    // 放在 Impl 析构里而不是 MediaPlayer::~MediaPlayer：只有这里知道这些成员存在，
    // 门面的析构因此可以 = default。
    ~Impl() {
        // 导出先收: 立关机标志（不再续跑排队请求）+ 取消两类导出 + 限时收线程。
        // 必须在 StopPlayback()/Release() 之前: 导出线程自己持有 FFmpeg 上下文，
        // 让它先退干净再去 avformat_network_deinit()。
        export_controller_.Shutdown(5000);
        // 取消全部后台任务并等待退出。同理: 容器结构分析线程自己持有 AVFormatContext。
        task_manager_.CancelAll();
        task_manager_.WaitForAll(kShutdownWaitMs);
        StopPlayback();
        playback_session_.Release();
        avformat_network_deinit();
    }

    // --- 打开链路 ---
    // 换媒体的公共复位（UI 线程）: 作废上一次尝试、取消全部导出、停播放、释放旧会话、
    // 复位分析状态，并为本次打开新建一份独立的取消/中断状态。Open/OpenAsync 共用。
    void BeginMediaSwitch(const QString& url);
    // UI 线程提交: 把 Prepare 的成果交给 OpenController::Commit，成功时接管流信息、
    // 清空错误、派发容器结构分析。Open/OpenAsync 共用。
    bool CommitOpen(const QString& url, OpenResult&& result);
    // 组装 Prepare 的输入（读当前音量 / 解码开关，并带上本次尝试的取消状态）。
    OpenPrepareParams MakeOpenParams(const QString& url, const AVInputFormat* input_format,
                                     AVDictionary* input_options);
    // 同步打开: 换媒体复位 + Prepare + Commit 全在调用线程完成（兼容旧调用与测试）。
    bool OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options);
    // 让当前尝试（打开期后台线程，或已提交会话的播放期阻塞读）立刻退出。
    void RequestCancel();

    // --- 播放收尾 ---
    // Stop() / 换媒体 / 析构共用的"停播放"三件套: 置中断、停会话、闭合未完成的缺陷段。
    void StopPlayback();

    // --- 分析开关分派 ---
    // SetAnalysisFeature() 的实现: 按 AnalysisFeature 落到各维度开关。新增维度只需在
    // domain 枚举加一项 + 这里的 switch 加一行，公共头与 UI 都不用改。
    // 其中几个不是单纯转发 —— VisualDefect / Macroblock / FrameType 还会启停工作线程
    // 或重置分析器状态，所以必须以函数而不是内联赋值的形式存在。
    void ApplyAnalysisFeature(model::AnalysisFeature feature, bool enable);
    void SetFrameTypeAnalysisEnabled(bool enable);
    void SetAudioFrameAnalysisEnabled(bool enable);
    void SetPacketAnalysisEnabled(bool enable);
    void SetEventAnalysisEnabled(bool enable);
    void SetSyncAnalysisEnabled(bool enable);
    void SetTimelineAnalysisEnabled(bool enable);
    void SetContainerStructureEnabled(bool enable);
    void SetMacroblockAnalysisEnabled(bool enable);
    void SetSceneChangeAnalysisEnabled(bool enable);
    void SetVisualDefectAnalysisEnabled(bool enable);

    // 异步打开任务 slot。同一 slot 上只允许一个任务: 换文件时旧任务被取消、结果作废。
    static constexpr const char* kSlotOpen = "player-open";

    // 门面本体。Impl 的生命周期完全落在 MediaPlayer 之内（它就是 impl_ 指向的对象），
    // 裸指针安全；需要它只为"开关实现里回跳公开入口"（Open / Seek / Play / EnableAnalysis），
    // 那些入口的日志与行为契约属于门面，不该复制一份到 Impl。
    MediaPlayer* facade_;

    // 播放会话: demux / 解码 / 音频输出 / 解码线程 / 播放时钟 / 播放状态机
    // 全部住在 PlaybackSession 里; 状态、位置、时长都从它读。
    PlaybackSession playback_session_;

    // 播放信息
    model::StreamInfo stream_info_;
    int volume_ = 100;
    QString current_url_;
    QString last_open_error_; // 最近一次 Open/OpenRawPcm 失败的详细原因

    // 当前打开尝试的中断/取消状态。
    //
    // 为什么是 shared_ptr 而不是普通成员: 后台线程（OpenAsync）持有它做 FFmpeg 打开/
    // 探测，提交后播放会话的解复用阶段仍在用（中断回调被 AVIOContext 复制了一份）——
    // Stop() 靠设置它的取消标志把阻塞的 av_read_frame 打断。每次换媒体新建一份，
    // 使旧后台线程与新尝试各有各的取消标志，互不干扰。生命周期由 shared_ptr 保证，
    // 后台线程即使在播放器析构后才退出也不会踩到已释放的对象。
    std::shared_ptr<OpenAttempt> current_attempt_;

    // 用户选择的定位方式 (菜单设置)。实际执行在 PlaybackSession::Seek()。
    std::atomic<model::SeekMode> seek_mode_{model::SeekMode::NearestKeyframe};

    // 分析会话: 12 个分析开关 + StreamAnalyzer + 视觉缺陷采样选项 (见 AnalysisSession.h)。
    // MediaPlayer 经它转发开关/统计, 自身不再持有这些散落成员。
    AnalysisSession analysis_session_;

    // 播放期实时分析的全部编排（六个 hook / 19 个逐帧计数器 / 逐帧分析器 / 视觉缺陷
    // 采样与工作线程）住在 RealtimeAnalysisController 里，门面只转发入口与信号。
    RealtimeAnalysisController realtime_analysis_;

    // 后台任务统一调度: 任务 ID / 取消标志 / 终态 / 过期结果丢弃。
    // 容器结构分析走它的受管线程; 抽帧与媒体导出的 worker 是 QObject(要发进度信号),
    // 仍留在 QThread 上, 但生命周期(Begin/End/Cancel)也登记在这里。
    task::TaskManager task_manager_;

    // 抽帧 / 音视频导出的全部编排（发起、排队、代际、取消、worker 线程生命周期）
    // 住在 ExportController 里，门面只转发入口与信号。
    ExportController export_controller_;

    // 容器结构分析的后台编排（派发 / 作废 / 过期结果丢弃）住在
    // ContainerInspectionController 里，门面只负责"该不该分析"和转发两条信号。
    ContainerInspectionController container_inspection_;

    // 打开媒体的全部编排（事务式: Prepare 建独立上下文 / Commit 一次性提交）住在
    // OpenController 里，门面只做"换媒体前的复位"、"后台派发"与"提交后派发容器分析"。
    OpenController open_controller_;
};

MediaPlayer::MediaPlayer(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {
    qRegisterMetaType<model::Mp4BoxAnalysisResult>("model::Mp4BoxAnalysisResult");
    qRegisterMetaType<model::ContainerStructureResult>("model::ContainerStructureResult");
    qRegisterMetaType<model::MacroblockFrameAnalysis>("model::MacroblockFrameAnalysis");
    qRegisterMetaType<model::MacroblockFrameAnalysis>("videoeye::model::MacroblockFrameAnalysis");
    // 画面质量 / 视觉缺陷（解码线程 -> UI 线程，必须注册元类型才能排队投递）
    qRegisterMetaType<model::FrameQualityMetric>("model::FrameQualityMetric");
    qRegisterMetaType<model::FrameQualityMetric>("videoeye::model::FrameQualityMetric");
    qRegisterMetaType<model::VisualDefect>("model::VisualDefect");
    qRegisterMetaType<model::VisualDefect>("videoeye::model::VisualDefect");
    qRegisterMetaType<model::ActivePictureArea>("model::ActivePictureArea");
    qRegisterMetaType<model::ActivePictureArea>("videoeye::model::ActivePictureArea");

    // 播放会话的信号转发到 MediaPlayer 的同名信号: 播放机械住在 PlaybackSession 里,
    // 但对外(UI)的信号契约必须保持原样, 一个字节都不许变。
    // 这些连接都是跨线程的(sender 在解码线程), Qt 会自动判成 QueuedConnection。
    connect(&impl_->playback_session_, &PlaybackSession::StateChanged, this, &MediaPlayer::StateChanged);
    connect(&impl_->playback_session_, &PlaybackSession::FrameReady, this, &MediaPlayer::FrameReady);
    connect(&impl_->playback_session_, &PlaybackSession::PositionChanged, this, &MediaPlayer::PositionChanged);
    connect(&impl_->playback_session_, &PlaybackSession::Error, this, &MediaPlayer::Error);
    connect(&impl_->playback_session_, &PlaybackSession::PlaybackFinished, this, &MediaPlayer::PlaybackFinished);

    // 导出的十条信号不再经门面转发: ExportController 直接在自己的 ExportBus 上发射，
    // UI 连的是 export_bus()（见 core/player/ExportBus.h），契约一个字节都没变。

    // 容器结构分析的两条信号也不再经门面转发: ContainerInspectionController 直接在
    // 自己的 ContainerBus 上发射，UI 连 container_bus()（见 core/player/ContainerBus.h）。

    // 实时分析的 23 条信号也不再经门面转发: RealtimeAnalysisController 直接在
    // 自己的 AnalysisBus 上发射，UI 连 analysis_bus()（见 core/player/AnalysisBus.h）。

    // 打开链路的四条信号原样转发: 封面图 / 媒体模式 / 媒体信息文本 / 打开失败。
    connect(&impl_->open_controller_, &OpenController::FrameReady, this, &MediaPlayer::FrameReady);
    connect(&impl_->open_controller_, &OpenController::MediaModeChanged, this, &MediaPlayer::MediaModeChanged);
    connect(&impl_->open_controller_, &OpenController::MediaInfoTextReady, this, &MediaPlayer::MediaInfoTextReady);
    connect(&impl_->open_controller_, &OpenController::OpenFailed, this, &MediaPlayer::OpenFailed);
}

// 析构体在 Impl 里：关机顺序（导出 -> 后台任务 -> 播放 -> 会话 -> FFmpeg）只有它知道。
MediaPlayer::~MediaPlayer() = default;

// --- 轻量访问器 ---
//
// 这些以前是头文件里的内联转发。成员搬进 Impl 后，公共头只有不完整类型，
// 于是统一收进 .cpp —— 顺带让公共头不再暴露播放/分析会话的实现细节。

void MediaPlayer::SetSeekMode(model::SeekMode mode) {
    impl_->seek_mode_.store(mode);
}

model::SeekMode MediaPlayer::GetSeekMode() const {
    return impl_->seek_mode_.load();
}

void MediaPlayer::SetSeekDragging(bool dragging) {
    impl_->playback_session_.SetSeekDragging(dragging);
}

model::PlayerState MediaPlayer::GetState() const {
    return impl_->playback_session_.state();
}

model::StreamInfo MediaPlayer::GetStreamInfo() const {
    return impl_->stream_info_;
}

int MediaPlayer::GetDuration() const {
    return impl_->playback_session_.duration_ms();
}

int MediaPlayer::GetCurrentPosition() const {
    return impl_->playback_session_.current_position_ms();
}

QString MediaPlayer::GetLastError() const {
    return impl_->last_open_error_;
}

ExportBus& MediaPlayer::export_bus() {
    return impl_->export_controller_.bus();
}

ContainerBus& MediaPlayer::container_bus() {
    return impl_->container_inspection_.bus();
}

AnalysisBus& MediaPlayer::analysis_bus() {
    return impl_->realtime_analysis_.bus();
}

int MediaPlayer::GetVolume() const {
    return impl_->volume_;
}

bool MediaPlayer::IsAnalysisEnabled() const {
    return impl_->analysis_session_.IsAnalysisEnabled();
}

bool MediaPlayer::IsVisualDefectAnalysisEnabled() const {
    return impl_->analysis_session_.IsVisualDefectAnalysisEnabled();
}

void MediaPlayer::SetHardwareDecodingEnabled(bool enable) {
    impl_->analysis_session_.SetHardwareDecodingEnabled(enable);
}

void MediaPlayer::SetRenderingSuppressed(bool suppressed) {
    impl_->playback_session_.SetRenderingSuppressed(suppressed);
}

bool MediaPlayer::IsRenderingSuppressed() const {
    return impl_->playback_session_.IsRenderingSuppressed();
}

const videoeye::StreamAnalyzer& MediaPlayer::stream_analyzer() const {
    return impl_->analysis_session_.stream_analyzer();
}

// --- 播放控制 ---

bool MediaPlayer::Open(const QString& url) {
    LOG_INFO("Open ENTER: " + url.toStdString());
    bool ok = impl_->OpenInternal(url, nullptr, nullptr);
    LOG_INFO("Open EXIT: result=" + std::string(ok ? "true" : "false"));
    return ok;
}

bool MediaPlayer::OpenRawPcm(const QString& url, const QString& demuxer_name, int sample_rate, int channels) {
    const AVInputFormat* input_format = av_find_input_format(demuxer_name.toUtf8().constData());
    if (!input_format) {
        impl_->last_open_error_ = QString("不支持的 PCM 采样格式: %1").arg(demuxer_name);
        emit OpenFailed(impl_->last_open_error_);
        return false;
    }
    AVDictionary* input_options = nullptr;
    av_dict_set(&input_options, "sample_rate", QByteArray::number(sample_rate).constData(), 0);
    av_dict_set(&input_options, "channels", QByteArray::number(channels).constData(), 0);
    return impl_->OpenInternal(url, input_format, input_options);
}

// --- Impl: 打开链路 / 播放收尾 / 分析开关分派 ---

void MediaPlayer::Impl::RequestCancel() {
    // 置位后卡在 avformat_open_input / av_read_frame 上的阻塞 IO 会立刻退出 ——
    // deadline 只会"等到超时"，只有它能让阻塞的 read 马上返回 AVERROR_EXIT。
    if (current_attempt_) {
        current_attempt_->cancel.store(true, std::memory_order_release);
        current_attempt_->interrupt.deadline_us = 0;
    }
}

void MediaPlayer::Impl::BeginMediaSwitch(const QString& url) {
    // 上一次尝试立即作废: 它可能还卡在后台打开，也可能是已提交会话在播放期读取。
    RequestCancel();

    // 换媒体 = 换上下文: 两类导出(抽帧 + 音视频转码)都必须在这里统一终止。
    // 放在这里而不是 UI 里, 是因为打开媒体的入口不止一个（Open / OpenAsync / OpenRawPcm /
    // 播放列表切换），任何一个入口漏掉"取消导出"都会留下旧任务写旧文件、
    // 并把终态信号串回新媒体界面的问题。
    facade_->CancelAllExports();

    facade_->Stop();
    playback_session_.Release();

    current_url_ = url;
    // 换文件了: 上一次的容器结构分析结果作废(任务体投递前会查 IsCurrent)
    container_inspection_.Cancel();
    // 实时分析的复位（19 个逐帧计数器 / "已报告过"标志 / 逐帧分析器 / 清空 UI 列表）
    // 整体在控制器里 —— 逐个留在调用方等于把它的内部状态摊开在别人家。
    realtime_analysis_.ResetForNewMedia();
    analysis_session_.stream_analyzer().Reset();
    playback_session_.ResetSyncTimestamps();

    // 新的一次打开尝试: 独立的取消标志/中断状态 —— 不与上一次共享。旧后台线程仍按
    // 自己的标志退出（不会被本次复位掉的标志误放行）；提交后播放会话也继续用它取消。
    current_attempt_ = std::make_shared<OpenAttempt>();
}

OpenPrepareParams MediaPlayer::Impl::MakeOpenParams(const QString& url, const AVInputFormat* input_format,
                                                    AVDictionary* input_options) {
    OpenPrepareParams params;
    params.url = url;
    params.input_format = input_format;
    params.input_options = input_options;
    params.volume_percent = volume_;
    params.prefer_hw_decoding = analysis_session_.IsHardwareDecodingEnabled();
    params.macroblock_analysis = analysis_session_.IsMacroblockAnalysisEnabled();
    params.attempt = current_attempt_;
    return params;
}

bool MediaPlayer::Impl::CommitOpen(const QString& url, OpenResult&& result) {
    model::StreamInfo info;
    QString error;
    const bool ok = open_controller_.Commit(std::move(result), info, error);
    if (!ok) {
        last_open_error_ = error;
        return false;
    }
    stream_info_ = info;
    last_open_error_.clear();
    // 容器结构分析 (统一调度, 后台线程) — 纯展示信息 (面板里看 Box 树 / 样本表),
    // 不该阻塞打开; 之前同步跑在 UI 线程, 大文件的 sample 表展开会让界面冻结数秒~数十秒。
    if (analysis_session_.IsContainerStructureEnabled()) {
        container_inspection_.Start(url);
    }
    return true;
}

bool MediaPlayer::Impl::OpenInternal(const QString& url, const AVInputFormat* input_format,
                                     AVDictionary* input_options) {
    LOG_INFO("OpenInternal(同步): " + url.toStdString());
    // 同步路径: 换媒体复位 + Prepare/Commit 都在**调用线程**上完成（兼容旧调用与测试）。
    BeginMediaSwitch(url);
    const OpenPrepareParams params = MakeOpenParams(url, input_format, input_options);
    OpenResult result = OpenController::Prepare(params);
    return CommitOpen(url, std::move(result));
}

void MediaPlayer::Impl::StopPlayback() {
    // 先置中断、再停会话：网络源此刻可能正卡在 avformat_open_input / av_read_frame 上，
    // 置位后那些阻塞 IO 会立刻退出，playback_session_.Stop() 的 join 才等得到返回。
    RequestCancel();
    playback_session_.Stop();
    // 停止播放时闭合未结束的缺陷段（UI 立刻能看到最后一条）
    realtime_analysis_.FlushOnStop();
}

void MediaPlayer::Impl::ApplyAnalysisFeature(model::AnalysisFeature feature, bool enable) {
    switch (feature) {
    case model::AnalysisFeature::Master:
    case model::AnalysisFeature::StreamStats:
        facade_->EnableAnalysis(enable);
        break;
    case model::AnalysisFeature::VideoFrame:
        SetFrameTypeAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::AudioFrame:
        SetAudioFrameAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Packet:
        SetPacketAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Event:
        SetEventAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::SyncSample:
        SetSyncAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Timeline:
        SetTimelineAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::ContainerStructure:
        SetContainerStructureEnabled(enable);
        break;
    case model::AnalysisFeature::Macroblock:
        SetMacroblockAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::SceneChange:
        SetSceneChangeAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::VisualDefect:
        SetVisualDefectAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Diagnostics:
        // 全文件扫描 + QC 不在播放会话里，由面板侧的 DiagnosticsPage 单独驱动，
        // 这里刻意什么都不做（以前 MainWindow 的 switch 也是落到 default 分支）。
        break;
    }
}

void MediaPlayer::Impl::SetFrameTypeAnalysisEnabled(bool enable) {
    analysis_session_.SetFrameTypeAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetAudioFrameAnalysisEnabled(bool enable) {
    analysis_session_.SetAudioFrameAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetPacketAnalysisEnabled(bool enable) {
    analysis_session_.SetPacketAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetEventAnalysisEnabled(bool enable) {
    analysis_session_.SetEventAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetSyncAnalysisEnabled(bool enable) {
    analysis_session_.SetSyncAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetTimelineAnalysisEnabled(bool enable) {
    analysis_session_.SetTimelineAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetContainerStructureEnabled(bool enable) {
    analysis_session_.SetContainerStructureEnabled(enable);
}

void MediaPlayer::Impl::SetSceneChangeAnalysisEnabled(bool enable) {
    analysis_session_.SetSceneChangeAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetVisualDefectAnalysisEnabled(bool enable) {
    realtime_analysis_.SetVisualDefectAnalysisEnabled(enable);
}

void MediaPlayer::Impl::SetMacroblockAnalysisEnabled(bool enable) {
    bool was_enabled = analysis_session_.IsMacroblockAnalysisEnabled();
    analysis_session_.SetMacroblockAnalysisEnabled(enable);
    if (enable && !was_enabled) {
        LOG_INFO("宏块分析已启用");
        // 运动矢量仅在软件解码下由 FFmpeg 导出; 硬件解码 (Vulkan/D3D11/CUDA) 不产出
        // AV_FRAME_DATA_MOTION_VECTORS side data。若当前正在使用硬件解码, 则重新以
        // 软件解码打开当前文件并恢复播放位置, 否则宏块分析面板将始终为空。
        if (playback_session_.video_decoder() && playback_session_.video_decoder()->IsHardwareDecoding() &&
            !current_url_.isEmpty()) {
            LOG_WARN("当前为硬件解码, 无法导出运动矢量。自动切换为软件解码以启用宏块分析...");
            int resume_ms = playback_session_.current_position_ms();
            bool was_playing = (playback_session_.state() == model::PlayerState::Playing);
            QString url = current_url_;
            if (facade_->Open(url)) {
                if (resume_ms > 0)
                    facade_->Seek(resume_ms);
                if (was_playing)
                    facade_->Play();
            }
        }
    } else if (!enable && was_enabled) {
        LOG_INFO("宏块分析已禁用");
    }
}

void MediaPlayer::OpenAsync(const QString& url) {
    LOG_INFO("OpenAsync ENTER: " + url.toStdString());
    // UI 线程只做换媒体复位；耗时的打开/探测丢到后台线程。
    impl_->BeginMediaSwitch(url);
    OpenPrepareParams params = impl_->MakeOpenParams(url, nullptr, nullptr);
    const std::string url_copy = url.toStdString();

    QPointer<MediaPlayer> self = this;
    // 用 RunBlockingIoWithResult: 任务体要走 FFmpeg 的 avformat_open_input / find_stream_info，
    // 网络源/异常设备上即使装了中断回调也可能不响应，关闭时不能 join 到底（超预算放弃）。
    // 代价是必须遵守 BlockingIo 生命周期约定: 任务体只按值捕获依赖，不持有裸引用 ——
    // 这里只捕获 QPointer self + 按值/共享指针的 params，且 Prepare 是 static 的，不碰本对象。
    impl_->task_manager_.RunBlockingIoWithResult(
        Impl::kSlotOpen, [self, url_copy, params](task::TaskId id, task::CancelToken token) -> task::TaskState {
            if (!self)
                return task::TaskState::Canceled;

            OpenResult result = OpenController::Prepare(params);
            if (token.IsCanceled())
                return task::TaskState::Canceled;
            if (!self)
                return task::TaskState::Canceled;

            const bool ok = result.ok;
            // 结果排回 UI 线程一次性提交。落地时复查 current id: 排进 UI 队列之后可能
            // 已经换了文件/又开了一次，那条结果属于上一个任务，必须丢弃且不发 OpenFinished。
            QMetaObject::invokeMethod(
                self.data(),
                [self, id, url_copy, result = std::move(result)]() mutable {
                    if (!self)
                        return;
                    if (self->impl_->task_manager_.CurrentId(Impl::kSlotOpen) != id) {
                        LOG_INFO("播放器打开结果已过期, 丢弃");
                        return;
                    }
                    const bool committed = self->impl_->CommitOpen(QString::fromStdString(url_copy), std::move(result));
                    emit self->OpenFinished(committed);
                },
                Qt::QueuedConnection);
            return ok ? task::TaskState::Succeeded : task::TaskState::Failed;
        });
}

void MediaPlayer::RequestContainerStructureAnalysis(const QString& url) {
    // 供播放器 Open 失败后的"分析模式"使用: 文件打不开/播不了也要尽量给出文件级结构信息。
    // 不需要手工回收旧线程: TaskManager 在 Begin 时自动取代同 slot 的旧任务。
    if (impl_->analysis_session_.IsContainerStructureEnabled()) {
        impl_->container_inspection_.Start(url);
    }
}

void MediaPlayer::Play() {
    LOG_INFO("Play ENTER: state=" + std::to_string(static_cast<int>(impl_->playback_session_.state())));
    if (!impl_->playback_session_.format_ctx()) {
        emit Error("No media opened");
        return;
    }
    // 起停解码线程、播放状态机、音频设备都在 PlaybackSession 里
    impl_->playback_session_.Play();
}

void MediaPlayer::Pause() {
    LOG_INFO("Pause");
    impl_->playback_session_.Pause();
}

void MediaPlayer::Stop() {
    LOG_INFO("Stop");
    // 置中断 / 停会话 / 闭合缺陷段三件套在 Impl 里：析构与换媒体也走同一条路径。
    impl_->StopPlayback();
}

void MediaPlayer::Seek(int position_ms, model::SeekMode mode) {
    LOG_INFO("Seek: target_ms=" + std::to_string(position_ms) + " mode=" + std::to_string(static_cast<int>(mode)));
    // 只投递 seek 请求: AVFormatContext 由解码线程独占调用 av_seek_frame / av_read_frame,
    // 避免 UI 线程与解码线程并发访问同一个 FFmpeg 上下文。
    impl_->playback_session_.Seek(position_ms, mode);
}

void MediaPlayer::SetVolume(int volume) {
    impl_->volume_ = std::max(0, std::min(100, volume));
    if (impl_->playback_session_.audio_output())
        impl_->playback_session_.audio_output()->SetVolume(impl_->volume_ / 100.0);
}

void MediaPlayer::EnableAnalysis(bool enable) {
    impl_->analysis_session_.SetAnalysisEnabled(enable);
    if (enable) {
        impl_->analysis_session_.Start();
        LOG_INFO("已启用视频分析");
    } else {
        impl_->analysis_session_.Stop();
        LOG_INFO("已禁用视频分析");
    }
}

// 各维度开关的实现已收进 Impl::ApplyAnalysisFeature()（见上），门面只留这一个公开入口：
// 新增一个分析维度 = domain 枚举加一项 + Impl 里的 switch 加一行，公共头与 UI 都不用改。
void MediaPlayer::SetAnalysisFeature(model::AnalysisFeature feature, bool enable) {
    impl_->ApplyAnalysisFeature(feature, enable);
}

// --- 画面质量 / 视觉缺陷 ---

// 画面质量 / 视觉缺陷: 开关、采样档位、收尾全部在 RealtimeAnalysisController 里
// （见 core/player/RealtimeAnalysisController.h），本类只保留对外入口与信号转发。

model::VisualDefectOptions MediaPlayer::GetVisualDefectOptions() const {
    return impl_->realtime_analysis_.GetVisualDefectOptions();
}

void MediaPlayer::SetVisualDefectOptions(const model::VisualDefectOptions& options) {
    impl_->realtime_analysis_.SetVisualDefectOptions(options);
}

void MediaPlayer::FlushVisualDefectSegments(double end_timestamp_seconds) {
    impl_->realtime_analysis_.FlushVisualDefectSegments(end_timestamp_seconds);
}

model::StreamStats MediaPlayer::GetCurrentStats() const {
    return impl_->analysis_session_.stream_analyzer().GetStats();
}

// --- 导出: 全部委托给 ExportController ---
//
// 抽帧 / 音视频导出的发起、排队、代际过滤、取消与 worker 线程生命周期都在
// core/player/ExportController.cpp，本类只保留对外入口与信号转发
// （连接见构造函数）。拆分的理由与"绝不能改"的几条约定都写在那个文件头。

void MediaPlayer::StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality,
                                        int frame_interval) {
    impl_->export_controller_.StartVideoFrameExport(output_dir, format, jpg_quality, frame_interval);
}

void MediaPlayer::CancelVideoFrameExport() {
    impl_->export_controller_.CancelVideoFrameExport();
}

void MediaPlayer::StartMediaExport(const exporter::ExportOptions& opt) {
    impl_->export_controller_.StartMediaExport(opt);
}

void MediaPlayer::CancelMediaExport() {
    impl_->export_controller_.CancelMediaExport();
}

void MediaPlayer::CancelAllExports() {
    impl_->export_controller_.CancelAllExports();
}

// --- 解码线程 ---

// --- 播放回调 (由 PlaybackSession 的解码线程调用) ---
//
// 切分后解码循环只做 demux / 解码 / pacing / 画面输出; "要不要发信号、要不要计数"
// 这类分析语义全部留在下面这些 hook 里, 与分析器、计数器放在一起。
// 调用线程与改造前一致(解码线程), 因此信号仍然由 Qt 自动排队到 UI 线程。

bool MediaPlayer::IsHardwareDecoding() const {
    const VideoDecoder* decoder = impl_->playback_session_.video_decoder();
    return decoder && decoder->IsHardwareDecoding();
}

std::string MediaPlayer::GetHwDeviceName() const {
    const VideoDecoder* decoder = impl_->playback_session_.video_decoder();
    if (!decoder || !decoder->IsHardwareDecoding())
        return "none";
    return av_hwdevice_get_type_name(decoder->GetHwDeviceType());
}

} // namespace player
} // namespace videoeye
