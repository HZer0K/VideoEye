#include "core/ffmpeg/FfmpegProcessRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

namespace videoeye {
namespace ffmpegtool {
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

}  // namespace

FfmpegProcessRunner::FfmpegProcessRunner(QObject* parent)
    : QObject(parent) {
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process_, &QProcess::readyReadStandardOutput,
            this, &FfmpegProcessRunner::OnReadyReadStdout);
    connect(process_, &QProcess::readyReadStandardError,
            this, &FfmpegProcessRunner::OnReadyReadStderr);
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &FfmpegProcessRunner::OnProcessFinished);
    connect(process_, &QProcess::errorOccurred,
            this, &FfmpegProcessRunner::OnProcessError);
}

FfmpegProcessRunner::~FfmpegProcessRunner() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        // 析构路径上不能再发信号、也不能等事件循环，直接断开后 kill。
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}

QString FfmpegProcessRunner::BuildDisplayCommand(const QString& program, const QStringList& arguments) {
    QString line = program;
    for (const QString& a : arguments) {
        line += QLatin1Char(' ');
        if (a.isEmpty() || a.contains(QLatin1Char(' ')) || a.contains(QLatin1Char('\t'))) {
            line += QLatin1Char('"') + a + QLatin1Char('"');
        } else {
            line += a;
        }
    }
    return line;
}

bool FfmpegProcessRunner::Start(const QString& program, const QStringList& arguments,
                                const QString& working_directory) {
    if (running_) {
        return false;
    }

    last_result_ = FfmpegRunResult();
    last_result_.command_line = BuildDisplayCommand(program, arguments);
    pending_stdout_.clear();
    pending_stderr_.clear();
    stop_requested_ = false;

    const QFileInfo info(program);
    if (!info.exists() || !info.isFile()) {
        last_result_.status = FfmpegRunStatus::StartError;
        last_result_.error_message =
            QStringLiteral("ffmpeg 可执行程序不存在: %1").arg(program);
        emit Finished(last_result_);
        return false;
    }

    if (!working_directory.isEmpty() && !QDir(working_directory).exists()) {
        last_result_.status = FfmpegRunStatus::StartError;
        last_result_.error_message =
            QStringLiteral("工作目录不存在: %1").arg(working_directory);
        emit Finished(last_result_);
        return false;
    }

    process_->setProgram(program);
    process_->setArguments(arguments);
    if (!working_directory.isEmpty()) {
        process_->setWorkingDirectory(working_directory);
    }

    timer_.start();
    process_->start(QIODevice::ReadOnly);
    // start() 返回后进程可能还没真正起来（Windows 上 FailedToStart 是异步报的）。
    // 这里只标"正在跑"，真正的失败由 errorOccurred 兜住。
    running_ = true;
    emit Started(last_result_.command_line);
    return true;
}

void FfmpegProcessRunner::Stop(int grace_ms) {
    if (!running_ || !process_) {
        return;
    }
    stop_requested_ = true;
    process_->terminate();
    // Windows 上 ffmpeg 是控制台程序，terminate() 发过去的 WM_CLOSE 没有人接；
    // 宽限一段时间让它自己收尾（写 moov box），到点还没退出就强杀。
    QTimer::singleShot(grace_ms, this, [this]() {
        if (process_ && process_->state() != QProcess::NotRunning) {
            process_->kill();
        }
    });
}

bool FfmpegProcessRunner::IsRunning() const {
    return running_;
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
            if (c == '\n') { nl = i; break; }
            if (c == '\r') { cr = i; break; }
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
        return;   // ffmpeg 的进度行会刷出大量空行，不进日志
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
    last_result_.exit_code = exit_code;
    last_result_.elapsed_ms = timer_.elapsed();

    if (stop_requested_) {
        last_result_.status = FfmpegRunStatus::Stopped;
    } else if (exit_status == QProcess::CrashExit) {
        last_result_.status = FfmpegRunStatus::Failed;
        last_result_.error_message = QStringLiteral("进程异常退出（崩溃或被系统终止）");
    } else if (exit_code == 0) {
        last_result_.status = FfmpegRunStatus::Finished;
    } else {
        last_result_.status = FfmpegRunStatus::Failed;
    }
    emit Finished(last_result_);
}

void FfmpegProcessRunner::OnProcessError(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
        running_ = false;
        last_result_.status = FfmpegRunStatus::StartError;
        last_result_.elapsed_ms = timer_.elapsed();
        last_result_.error_message = DescribeProcessError(error);
        emit Finished(last_result_);
    }
    // 其余错误（读/写失败、崩溃）都以 finished() 收尾，这里不重复发 Finished。
}

}  // namespace ffmpegtool
}  // namespace videoeye
