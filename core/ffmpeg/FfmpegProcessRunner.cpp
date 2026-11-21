#include "core/ffmpeg/FfmpegProcessRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

namespace videoeye {
namespace ffmpeg {
namespace {

// UTF-8 解码失败时 Qt 会填 U+FFFD。出现它就说明这行不是 UTF-8（Windows 上常见的是
// 本地 ANSI 编码的中文路径/报错），退回本地编码再解一次。
constexpr char16_t kReplacementChar = 0xFFFD;

QString DescribeProcessError(QProcess::ProcessError error) {
    switch (error) {
    case QProcess::FailedToStart:
        return QStringLiteral("进程无法启动（程序不存在，或没有执行权限）");
    case QProcess::Crashed:
        return QStringLiteral("进程崩溃");
    case QProcess::Timedout:
        return QStringLiteral("等待进程超时");
    case QProcess::WriteError:
        return QStringLiteral("写入进程失败");
    case QProcess::ReadError:
        return QStringLiteral("读取进程输出失败");
    default:
        return QStringLiteral("未知错误");
    }
}

// ffmpeg 在"输出文件已存在"这件事上有三种收场，全部写在 stderr 上:
//   File 'out.mp4' already exists. Overwrite? [y/N]   —— 等 stdin，回答不是 y 就放弃
//   File 'out.mp4' already exists. Exiting.           —— -nostdin 等无交互场景
//   Not overwriting - exiting
// 命中任意一句就说明它**一个字节都没写**。注意这时退出码常是 0，
// 只看退出码会把它当成"完成"，所以必须在结果里单独标出来。
bool LooksLikeOverwriteRefusal(const QString& line) {
    if (line.contains(QLatin1String("Not overwriting"), Qt::CaseInsensitive)) {
        return true;
    }
    if (line.contains(QLatin1String("already exists"), Qt::CaseInsensitive) &&
        (line.contains(QLatin1String("Overwrite?"), Qt::CaseInsensitive) ||
         line.contains(QLatin1String("Exiting"), Qt::CaseInsensitive))) {
        return true;
    }
    return false;
}

} // namespace

FfmpegProcessRunner::FfmpegProcessRunner(QObject* parent) : QObject(parent) {
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process_, &QProcess::readyReadStandardOutput, this, &FfmpegProcessRunner::OnReadyReadStdout);
    connect(process_, &QProcess::readyReadStandardError, this, &FfmpegProcessRunner::OnReadyReadStderr);
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &FfmpegProcessRunner::OnProcessFinished);
    connect(process_, &QProcess::errorOccurred, this, &FfmpegProcessRunner::OnProcessError);
}

FfmpegProcessRunner::~FfmpegProcessRunner() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        // 析构路径上不能再发信号、也不能等事件循环，直接断开后 kill。
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}

bool FfmpegProcessRunner::CanQuitViaStdin(const QStringList& arguments) {
    // -nostdin 是唯一需要照顾的例外: 它让 ffmpeg 彻底不读 stdin，写 q 进去没人接。
    // （-stdin 是它的反向开关，写了也只是"允许读"，不影响这里的判断。）
    return !arguments.contains(QStringLiteral("-nostdin"));
}

QString FfmpegProcessRunner::BuildDisplayCommand(const QString& program, const QStringList& arguments) {
    // 只用于展示 / 回填输入框，但回填的那一行下次还要被 ParseCommandLine 读回来，
    // 所以含 shell 元字符的 token 也必须加引号 —— 否则一个含 & 的路径回填后
    // 下次运行会被当成 shell 操作符拒掉。
    const auto needs_quote = [](const QString& a) {
        if (a.isEmpty()) {
            return true;
        }
        for (const QChar c : a) {
            if (c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('&') ||
                c == QLatin1Char(';') || c == QLatin1Char('|') || c == QLatin1Char('<') || c == QLatin1Char('>') ||
                c == QLatin1Char('`') || c == QLatin1Char('$') || c == QLatin1Char('(') || c == QLatin1Char(')')) {
                return true;
            }
        }
        return false;
    };

    QString line;
    if (!program.isEmpty()) {
        line = needs_quote(program) ? QLatin1Char('"') + program + QLatin1Char('"') : program;
    }
    for (const QString& a : arguments) {
        if (!line.isEmpty()) {
            line += QLatin1Char(' ');
        }
        line += needs_quote(a) ? QLatin1Char('"') + a + QLatin1Char('"') : a;
    }
    return line;
}

bool FfmpegProcessRunner::Start(const QString& program, const QStringList& arguments,
                                const QString& working_directory) {
    if (running_) {
        return false;
    }

    // 新一代开始。此后上一代遗留的停止定时器会因为序号不匹配而自动失效。
    ++run_id_;

    last_result_ = FfmpegRunResult();
    last_result_.command_line = BuildDisplayCommand(program, arguments);
    pending_stdout_.clear();
    pending_stderr_.clear();
    stop_requested_ = false;
    stop_stage_ = FfmpegStopStage::None;
    arguments_ = arguments;

    const QFileInfo info(program);
    if (!info.exists() || !info.isFile()) {
        last_result_.status = FfmpegRunStatus::StartError;
        last_result_.error_message = QStringLiteral("ffmpeg 可执行程序不存在: %1").arg(program);
        emit Finished(last_result_);
        return false;
    }

    if (!working_directory.isEmpty() && !QDir(working_directory).exists()) {
        last_result_.status = FfmpegRunStatus::StartError;
        last_result_.error_message = QStringLiteral("工作目录不存在: %1").arg(working_directory);
        emit Finished(last_result_);
        return false;
    }

    process_->setProgram(program);
    process_->setArguments(arguments);
    // 每次都显式设置: 空串 = 回到调用进程的工作目录。只在非空时设置的话，
    // 上一次运行指定的目录会残留到下一次（QProcess 对象是被复用的）。
    process_->setWorkingDirectory(working_directory);

    timer_.start();
    // ReadWrite: 要留着可写的 stdin —— 停止时往里写一个 "q" 是让 ffmpeg
    // 正常收尾（写出 moov box）的唯一可靠办法，只读通道做不到这件事。
    process_->start(QIODevice::ReadWrite);
    // start() 返回后进程可能还没真正起来（Windows 上 FailedToStart 是异步报的）。
    // 这里只标"正在跑"，真正的失败由 errorOccurred 兜住。
    running_ = true;
    emit Started(last_result_.command_line);
    return true;
}

void FfmpegProcessRunner::Stop(int quit_grace_ms, int terminate_grace_ms) {
    if (!running_ || !process_) {
        return;
    }
    if (stop_stage_ != FfmpegStopStage::None) {
        return; // 已经在停了：再点一次不该把宽限期往前提
    }
    stop_requested_ = true;

    // 只对**发起停止时那一代**进程负责：期间任务结束又重跑了新命令的话，
    // 下面这两个定时器再动手就是误杀新任务。
    const quint64 generation = run_id_;

    auto advance = [this, generation](FfmpegStopStage stage) {
        if (generation != run_id_) {
            return false;
        }
        if (!process_ || process_->state() == QProcess::NotRunning) {
            return false;
        }
        stop_stage_ = stage;
        emit StopStageChanged(stage);
        return true;
    };

    // 第一档: 写 "q" 让它自己收尾 —— 只有这一档能保证输出文件完整。
    // -nostdin 时跳过（写了也没人接），直接进第二档。
    const bool can_quit = CanQuitViaStdin(arguments_);
    int delay_to_terminate = 0;
    if (can_quit && advance(FfmpegStopStage::QuitSent)) {
        process_->write("q\n");
        process_->closeWriteChannel(); // 写完就关，避免 ffmpeg 等一个不会来的输入
        delay_to_terminate = quit_grace_ms;
    }

    QTimer::singleShot(delay_to_terminate, this, [this, generation, advance, terminate_grace_ms]() {
        if (!advance(FfmpegStopStage::Terminated)) {
            return;
        }
        // 第二档: Windows 控制台程序收不到这个（Qt 发的是 WM_CLOSE），
        // 但 Unix 上 SIGTERM 会被 ffmpeg 接住并正常收尾，所以不能直接跳到 kill。
        process_->terminate();
        QTimer::singleShot(terminate_grace_ms, this, [this, generation, advance]() {
            if (!advance(FfmpegStopStage::Killed)) {
                return;
            }
            // 第三档: 强杀。走到这里输出文件多半已经不完整了，界面必须说清楚。
            process_->kill();
        });
    });
}

bool FfmpegProcessRunner::IsRunning() const {
    return running_;
}

bool FfmpegProcessRunner::StopAndWait(int quit_grace_ms) {
    if (!running_ || process_ == nullptr || process_->state() == QProcess::NotRunning) {
        return true;
    }
    // 不设这个标记的话，收尾时会被判成"退出码 0 = 完成"—— 而这一次是我们中途掐断的。
    stop_requested_ = true;

    const auto set_stage = [this](FfmpegStopStage stage) {
        if (stop_stage_ != stage) {
            stop_stage_ = stage;
            emit StopStageChanged(stage);
        }
    };

    // 第一档：写 q，让 ffmpeg 自己写 trailer（唯一能保证输出文件完整的一档）
    if (CanQuitViaStdin(arguments_)) {
        set_stage(FfmpegStopStage::QuitSent);
        process_->write("q\n");
        process_->closeWriteChannel();
        if (process_->waitForFinished(quit_grace_ms)) {
            return true;
        }
    }

    // 第二档：Unix 上 SIGTERM 会被 ffmpeg 接住并正常收尾（Windows 控制台程序收不到）
    set_stage(FfmpegStopStage::Terminated);
    process_->terminate();
    if (process_->waitForFinished(1500)) {
        return true;
    }

    // 第三档：强杀。走到这里输出文件多半已经不完整，界面必须说清楚。
    set_stage(FfmpegStopStage::Killed);
    process_->kill();
    process_->waitForFinished(2000);
    return process_->state() == QProcess::NotRunning;
}

QString FfmpegProcessRunner::DecodeLine(const QByteArray& raw) {
    QString text = QString::fromUtf8(raw);
    if (text.contains(QChar(kReplacementChar))) {
        const QString local = QString::fromLocal8Bit(raw);
        if (!local.contains(QChar(kReplacementChar))) {
            return local;
        }
    }
    return text;
}

// ffmpeg 的进度行用 \r 反复覆盖同一行，日志里必须当成行分隔，否则会攒出巨长一行。
void FfmpegProcessRunner::Drain(QByteArray& pending, const QByteArray& chunk, bool is_error) {
    pending.append(chunk);

    int start = 0;
    while (start < pending.size()) {
        int nl = -1;
        int cr = -1;
        for (int i = start; i < pending.size(); ++i) {
            const char c = pending.at(i);
            if (c == '\n') {
                nl = i;
                break;
            }
            if (c == '\r') {
                cr = i;
                break;
            }
        }
        const int cut = (nl >= 0) ? nl : cr;
        if (cut < 0) {
            break;
        }
        QByteArray line = pending.mid(start, cut - start);
        // \r\n 连在一起时把 \n 一起吃掉
        int advance = cut + 1;
        if (nl < 0 && cr >= 0 && cr + 1 < pending.size() && pending.at(cr + 1) == '\n') {
            advance = cr + 2;
        }
        start = advance;
        EmitPending(DecodeLine(line), is_error);
    }

    if (start > 0) {
        pending = pending.mid(start);
    }
}

void FfmpegProcessRunner::EmitPending(const QString& text, bool is_error) {
    if (text.trimmed().isEmpty()) {
        return; // ffmpeg 的进度行会刷出大量空行，不进日志
    }
    if (is_error && !last_result_.overwrite_refused && LooksLikeOverwriteRefusal(text)) {
        // 只认标记，不改状态: 状态仍由退出码/停止阶段决定，但结果里要带上这一条，
        // 否则"输出没写"会被显示成"完成"或"已停止（正常收尾）"。
        last_result_.overwrite_refused = true;
    }
    emit OutputLine(text, is_error);
}

void FfmpegProcessRunner::OnReadyReadStdout() {
    Drain(pending_stdout_, process_->readAllStandardOutput(), false);
}

void FfmpegProcessRunner::OnReadyReadStderr() {
    Drain(pending_stderr_, process_->readAllStandardError(), true);
}

void FfmpegProcessRunner::OnProcessFinished(int exit_code, QProcess::ExitStatus exit_status) {
    // 收尾: 进程退出后缓冲区里可能还有半行
    if (!pending_stdout_.isEmpty()) {
        EmitPending(DecodeLine(pending_stdout_), false);
        pending_stdout_.clear();
    }
    if (!pending_stderr_.isEmpty()) {
        EmitPending(DecodeLine(pending_stderr_), true);
        pending_stderr_.clear();
    }

    running_ = false;
    // 这一代到此为止：在飞的宽限定时器（如果有）就此作废，不会碰到下一次运行。
    ++run_id_;
    last_result_.exit_code = exit_code;
    last_result_.elapsed_ms = timer_.elapsed();
    // Stopped 只是"用户点过停止"，收尾干不干净要看当时走到哪一档
    last_result_.stop_stage = stop_stage_;

    if (stop_requested_) {
        last_result_.status = FfmpegRunStatus::Stopped;
    } else if (exit_status == QProcess::CrashExit) {
        last_result_.status = FfmpegRunStatus::Failed;
        last_result_.error_message = QStringLiteral("进程异常退出（崩溃或被系统终止）");
    } else if (exit_code == 0) {
        if (last_result_.overwrite_refused) {
            // 退出码 0，但 ffmpeg 其实什么都没写（输出文件已存在，它放弃了覆盖）。
            // 这不是成功 —— 报"完成"会让用户以为产出已经更新。
            last_result_.status = FfmpegRunStatus::Failed;
            last_result_.error_message = QStringLiteral("输出文件已存在，ffmpeg 拒绝了覆盖 —— 没有写入任何数据。");
        } else {
            last_result_.status = FfmpegRunStatus::Finished;
        }
    } else {
        last_result_.status = FfmpegRunStatus::Failed;
    }
    emit Finished(last_result_);
}

void FfmpegProcessRunner::OnProcessError(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
        running_ = false;
        ++run_id_; // 同上：让这一代遗留的停止定时器失效
        last_result_.status = FfmpegRunStatus::StartError;
        last_result_.elapsed_ms = timer_.elapsed();
        last_result_.error_message = DescribeProcessError(error);
        emit Finished(last_result_);
    }
    // 其余错误（读/写失败、崩溃）都以 finished() 收尾，这里不重复发 Finished。
}

} // namespace ffmpeg
} // namespace videoeye
