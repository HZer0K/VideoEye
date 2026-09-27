#pragma once

// ffmpeg 子进程执行器。
//
// 三件事:
//   1. 用 (program, QStringList) 启动进程 —— 参数数组直接给 QProcess, 不经过任何 shell;
//   2. 异步接收 stdout / stderr, 按行回吐给 UI, 长任务不阻塞界面;
//   3. 记录退出码、耗时、停止/失败状态, 让"跑完了没、为什么没跑完"在界面上说得清。
//
// 停止节奏: 请求停止 → terminate()（让 ffmpeg 自己收尾、写出合法的 moov box）
// → 宽限期到点还没退出才 kill()。那个宽限定时器**必须绑定本次运行的序号**
// (run_id_): 旧任务先结束、用户立刻跑下一个时, 残留的定时器会去杀新一代的进程。
// 进程自己结束也会把 run_id_ 推进, 让在飞的定时器自动作废。
//
// ffmpeg 的进度信息（frame= ... fps= ...）是**不换行**的: 它靠 \r 反复覆盖同一行。
// 所以这里不能简单地"一行一发", 必须按 \r 也分行, 否则日志框会攒出一条几十 KB 的长行。
// 同理, 输出编码不保证是 UTF-8（Windows 中文环境下 ffmpeg 可能吐本地编码）,
// 解码时 UTF-8 失败会退到 QString::fromLocal8Bit。

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace videoeye {
namespace ffmpegtool {

enum class FfmpegRunStatus {
    Idle,
    Running,
    Finished,   // 正常退出（exitCode == 0）
    Failed,     // 退出码非 0
    Stopped,    // 被用户停止
    StartError  // 进程没起来（程序不存在 / 无权限 / 工作目录不存在）
};

struct FfmpegRunResult {
    FfmpegRunStatus status = FfmpegRunStatus::Idle;
    int exit_code = -1;
    qint64 elapsed_ms = 0;
    QString error_message;   // StartError / Failed 时的补充说明
    QString command_line;    // 实际执行的命令（用于日志抬头）
};

class FfmpegProcessRunner : public QObject {
    Q_OBJECT

public:
    explicit FfmpegProcessRunner(QObject* parent = nullptr);
    ~FfmpegProcessRunner() override;

    /// 启动。成功返回 true（此后 IsRunning() 为 true）。
    /// 失败时通过 Finished() 抛出一个 status=StartError 的结果，便于调用方统一处理。
    bool Start(const QString& program, const QStringList& arguments,
               const QString& working_directory = QString());

    /// 请求停止。先尝试优雅退出（ffmpeg 会收尾并写出合法的 mp4 moov box），
    /// 超时后 kill。
    void Stop(int grace_ms = 3000);

    bool IsRunning() const;

    /// 拼一个给人看的命令行（含引号），只用于展示，不参与执行。
    static QString BuildDisplayCommand(const QString& program, const QStringList& arguments);

signals:
    /// 一行输出。is_error=true 表示来自 stderr。
    void OutputLine(const QString& text, bool is_error);
    void Started(const QString& command_line);
    void Finished(const FfmpegRunResult& result);

private slots:
    void OnReadyReadStdout();
    void OnReadyReadStderr();
    void OnProcessFinished(int exit_code, QProcess::ExitStatus exit_status);
    void OnProcessError(QProcess::ProcessError error);

private:
    void EmitPending(const QString& text, bool is_error);
    void Drain(QByteArray& pending, const QByteArray& chunk, bool is_error);
    static QString DecodeLine(const QByteArray& raw);

    QProcess* process_ = nullptr;
    QByteArray pending_stdout_;
    QByteArray pending_stderr_;
    QElapsedTimer timer_;
    FfmpegRunResult last_result_;
    bool running_ = false;
    bool stop_requested_ = false;
    // 本次运行的序号。每 Start() 一次 +1、每次进程收尾 +1 —— 用来让"上一代"
    // 遗留的停止定时器自动失效，避免它杀掉新一代进程。
    quint64 run_id_ = 0;
};

}  // namespace ffmpegtool
}  // namespace videoeye
