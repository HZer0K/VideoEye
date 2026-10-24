#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <memory>
#include <string>

#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/FrameData.h" // model::PlayerState / model::StreamInfo
#include "core/domain/model/SeekMode.h"
#include "core/domain/model/StreamStats.h"         // GetCurrentStats() 的返回值
#include "core/domain/model/VisualDefectOptions.h" // Set/GetVisualDefectOptions() 的值类型

// 以下类型只以指针/引用出现在签名里，定义由用到它们的 .cpp 自己 include：
// 公共头不再把 libav*、播放会话、分析会话、导出控制器拖给所有下游。
extern "C" {
struct AVInputFormat;
struct AVDictionary;
}

namespace videoeye {

class StreamAnalyzer;

namespace exporter {
struct ExportOptions;
}

namespace player {

class AnalysisBus;
class ContainerBus;
class ExportBus;
struct OpenPrepareParams;
struct OpenResult;

// 媒体播放器类 - 使用Qt信号槽机制
class MediaPlayer : public QObject {
    Q_OBJECT

public:
    explicit MediaPlayer(QObject* parent = nullptr);
    ~MediaPlayer();

    // 播放控制
    //
    // Open(): 同步打开。在**调用线程**上完成"建上下文 + 探测 + 解码器"（事务的
    // Prepare + Commit 都在本线程）—— 兼容旧调用与测试；在 UI 线程上它会阻塞界面。
    bool Open(const QString& url);
    // OpenAsync(): 事务式异步打开。UI 线程只做"换媒体复位"，把耗时的打开/探测丢到
    // 后台线程（TaskKind::BlockingIo），完成后排回 UI 线程一次性提交进播放会话。
    // 结果经 OpenFinished(bool) 通知；过期结果（期间又开了新文件）会被丢弃且不发信号。
    void OpenAsync(const QString& url);
    bool OpenRawPcm(const QString& url, const QString& demuxer_name, int sample_rate, int channels);
    void Play();
    void Pause();
    void Stop();
    void Seek(int position_ms, model::SeekMode mode = model::SeekMode::NearestKeyframe);

    // 定位方式 (进度条拖动策略)
    void SetSeekMode(model::SeekMode mode);
    model::SeekMode GetSeekMode() const;
    // 拖动进度条期间调用: 抑制音频输出, 避免关键帧预览时从多个位置传出杂音
    void SetSeekDragging(bool dragging);

    // 状态查询
    model::PlayerState GetState() const;
    model::StreamInfo GetStreamInfo() const;
    int GetDuration() const;
    int GetCurrentPosition() const;

    // 最近一次打开失败的详细原因 (成功打开后清空)。用于"打开异常文件仍进入分析模式"的
    // 场景: MainWindow 据此在状态栏/分析模块展示失败原因, 而不是弹模态框阻断。
    QString GetLastError() const;
    // 播放器 Open 失败后手动触发容器结构分析 (后台线程, 结果经 ContainerStructureReady 发出)。
    // 正常 Open 成功路径内部已自动触发, 无需调用此方法。
    void RequestContainerStructureAnalysis(const QString& url);

    // 导出信号的对外出口（抽帧 5 条 + 音视频 5 条，见 core/player/ExportBus.h）。
    //
    // 这十条信号此前挂在门面身上，门面因此要替它们背十条"同名转发"连接。现在由
    // ExportController 直接在总线上发射，UI（ExportCoordinator）连这根总线：
    // 信号名与参数一个字节没变，门面少一层无意义的转发。
    // 返回引用而非指针: 总线与播放器同生灭，不存在"取不到"的情况。
    ExportBus& export_bus();

    // 容器结构分析信号的对外出口（成功 / 失败二选一，见 core/player/ContainerBus.h）。
    // 与导出同理: 由 ContainerInspectionController 直接在总线上发射，门面不再转发。
    ContainerBus& container_bus();

    // 播放期实时分析信号的对外出口（23 条，见 core/player/AnalysisBus.h）。
    // 这是原先挂在门面上最大的一块（占了一半以上的信号声明与转发连接），现在由
    // RealtimeAnalysisController 直接在总线上发射。
    AnalysisBus& analysis_bus();

    // 音量控制 (0-100)
    void SetVolume(int volume);
    int GetVolume() const;

    // 分析控制
    void EnableAnalysis(bool enable);
    bool IsAnalysisEnabled() const;
    // 播放期分析功能的统一开关。UI 侧只走这一个入口。
    //
    // 以前每个维度一个 SetXxxAnalysisEnabled()，新增一个播放期分析维度要同时改三处：
    // domain 的 AnalysisFeature、MediaPlayer 的 setter、MainWindow 里那个把枚举翻译
    // 成 setter 调用的 switch。现在 MediaPlayer 直接认 model::AnalysisFeature，UI 只做
    // 转发，新增维度只需在枚举里加一项 + 在本函数的实现里加一行。
    //
    // 语义上要留意两点：
    //   * Master 与 StreamStats 都映射到 EnableAnalysis()（总开关）；
    //   * Diagnostics（全文件扫描 + QC）不在播放会话里，这里对它不做任何事 ——
    //     扫描由面板侧的 DiagnosticsPage 单独驱动。
    void SetAnalysisFeature(model::AnalysisFeature feature, bool enable);

    // 画面质量 / 视觉缺陷（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边）。
    // 开关会顺带启停分析用的工作线程；换采样档位用 SetVisualDefectOptions()。
    // 开关请经 SetAnalysisFeature(AnalysisFeature::VisualDefect, ...) 调用。
    bool IsVisualDefectAnalysisEnabled() const;
    void SetVisualDefectOptions(const model::VisualDefectOptions& options);
    // 线程安全地取视觉缺陷采样选项副本: 解码线程逐帧读取, UI 线程写入,
    // 普通值类型直接跨线程访问存在数据竞争, 故加锁后返回副本。
    model::VisualDefectOptions GetVisualDefectOptions() const;
    // 播完 / 停止时把还开着的缺陷段闭合（否则最后一段要等下一次播放才显示）
    void FlushVisualDefectSegments(double end_timestamp_seconds);

    // 硬件解码
    void SetHardwareDecodingEnabled(bool enable);
    bool IsHardwareDecoding() const;
    std::string GetHwDeviceName() const;

    // 视频帧导出
    //
    // 若上一次抽帧的线程还没退出来，本次请求**排队**而不是立即启动：两次任务写的是
    // 同一个输出目录、文件名又只由帧序号决定，并行跑会互相覆盖产物。排队请求会在旧
    // 线程真正结束的 finished 回调里自动续跑（用户不必再点一次）。
    void StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality = 90,
                               int frame_interval = 1);
    // 只置取消标志并请求线程退出，**不等待**：调用方在 UI 线程，绝不能被卡住。
    void CancelVideoFrameExport();

    // 音视频导出 (remux / transcode)。排队语义同 StartVideoFrameExport。
    void StartMediaExport(const exporter::ExportOptions& opt);
    // 同 CancelVideoFrameExport: 请求停止但不等待。
    void CancelMediaExport();

    // 取消**全部**导出（抽帧 + 音视频），并作废各自的排队请求与代际号。
    //
    // 谁该调它: 任何"媒体上下文要换了"的时刻 —— 打开新文件（OpenInternal 会自己调）、
    // 关闭/析构。以前这件事散在 UI 里做（MainWindow::OpenMedia 只调了抽帧那一半），
    // 于是"媒体导出进行中打开新文件"会留下一个仍在跑、且终态信号还能串回界面的旧任务。
    // 生命周期保证必须住在播放器里，UI 只负责重置自己的进度框。
    // 代际号一并推进: 旧任务迟到的进度/完成/错误信号会被代际校验丢弃，
    // 不会覆盖新媒体的界面状态。
    void CancelAllExports();

    // 渲染抑制: 播放区被隐藏时跳过画面输出 (sws_scale + FrameReady),
    // 解码线程、实时分析与音频照常运行; 重新展开播放区即恢复画面。
    void SetRenderingSuppressed(bool suppressed);
    bool IsRenderingSuppressed() const;

    // 获取分析器
    //
    // 不再对外暴露可变的 StreamAnalyzer&: 它的内部状态由解码线程更新,
    // 外部拿可变引用等于开了一条绕锁的读写通道(历史曲线接口尤其明显)。
    // 需要统计就用 GetCurrentStats()/下面这些快照。
    const videoeye::StreamAnalyzer& stream_analyzer() const;
    model::StreamStats GetCurrentStats() const;

signals:
    void StateChanged(model::PlayerState state);
    void FrameReady(const QImage& frame);
    void PositionChanged(int position_ms, int duration_ms);
    void Error(const QString& message);
    // 打开阶段失败 (avformat_open_input / find_stream_info / 无可播放流 / 解码器初始化失败)。
    // 与 Error 的区别: 不弹模态框, 只在状态栏提示, 文件仍会加载到分析模块。
    void OpenFailed(const QString& message);
    // OpenAsync() 完成时发出: ok=true 表示已提交进播放会话, false 表示打开失败
    // （失败原因见 GetLastError()）。过期结果（期间又开了新文件）不会发这条信号。
    void OpenFinished(bool ok);
    // 媒体信息文本: 打开链路在后台探测时顺带格式化（不再二次探测），提交成功后发出。
    // 空文本不发；顺序上先于 OpenFinished（收尾逻辑能拿到文本再决定回退）。
    void MediaInfoTextReady(const QString& text);
    void PlaybackFinished();
    void MediaModeChanged(bool has_video);

    // 实时分析的 23 条信号不在门面上了 —— 走 analysis_bus()
    // （见 core/player/AnalysisBus.h），UI 连总线而不是连门面。
    // 容器结构分析的两条信号同理走 container_bus()；导出的十条走 export_bus()。
    // 门面现在只背"播放 + 打开"这两件真正属于它的契约。

private:
    // --- SetAnalysisFeature() 的分派目标 ---
    //
    // 各维度开关的实际实现。UI 不要直接调这些，一律走 SetAnalysisFeature()：
    // 那才是"按 AnalysisFeature 设开关"的唯一入口，也是新增维度时唯一要改的地方。
    // 其中几个不是单纯转发 —— VisualDefect / Macroblock / FrameType 还会启停工作
    // 线程或重置分析器状态，所以必须以函数而不是内联赋值的形式存在。
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

    bool OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options);

    // 换媒体的公共复位（UI 线程）: 作废上一次尝试、取消全部导出、停播放、释放旧会话、
    // 复位分析状态，并为本次打开新建一份独立的取消/中断状态。Open/OpenAsync 共用。
    void BeginMediaSwitch(const QString& url);
    // UI 线程提交: 把 Prepare 的成果交给 OpenController::Commit，成功时接管流信息、
    // 清空错误、派发容器结构分析。Open/OpenAsync 共用。
    bool CommitOpen(const QString& url, OpenResult&& result);
    // 组装 Prepare 的输入（读当前音量 / 解码开关，并带上本次尝试的取消状态）。
    OpenPrepareParams MakeOpenParams(const QString& url, const AVInputFormat* input_format,
                                     AVDictionary* input_options);
    // 让当前尝试（打开期后台线程，或已提交会话的播放期阻塞读）立刻退出。
    void RequestCancel();

    // 异步打开任务 slot。同一 slot 上只允许一个任务: 换文件时旧任务被取消、结果作废。
    static constexpr const char* kSlotOpen = "player-open";

    // 导出的实际启动 / 排队 / 代际 / 取消全部在 ExportController 里
    // （见 core/player/ExportController.h），本类不保留任何导出状态。

    // 实现体：播放信息 + 播放会话 + 分析会话 + 实时分析 + 任务调度 + 三类控制器。
    //
    // 为什么指针化：这些成员的类型头会把 libav*、分析器与导出编排全部拉进
    // MediaPlayer.h 的 include 闭包 —— 任何 include 本头的 UI 文件都被迫重编一遍
    // FFmpeg 与整个分析层。Impl 的完整定义在 MediaPlayer.cpp 里，公共头只剩一个
    // 不完整类型；析构必须留在 .cpp（unique_ptr 的惯例）。
    //
    // 依赖顺序（构造顺序）约束写在 Impl 的定义处：会话在前、借用它们的控制器在后。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace player
} // namespace videoeye
