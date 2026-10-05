#pragma once

// 一次全文件分析的**输入侧**: FFmpeg 上下文的创建 / 打开 / 探测 / 中断与取消，
// 以及打开成功后立刻就能算出来的那些容器级事实（容器格式、时长、体积、码率、
// moov 顺序、MP4 sample table、流摘要）。
//
// 为什么从 AnalysisEngine::Run() 里拆出来: 以前"打开文件"和"逐包扫描"写在同一个函数里，
// 于是
//   * 上下文的生命周期由函数的控制流决定 —— 6 条失败出口各自 close 一次，漏一处就是句柄泄漏；
//   * 中断状态挂的是栈上对象，靠"函数还没返回"来保证不悬垂，谁都不敢把它挪走；
//   * 想单独验证"打开/探测阶段的取消行为"，必须把整个扫描跑完。
// 收进来之后这三件事分别由构造/析构与 Open() 的返回值表达。
//
// ⚠️ 两条搬动时最容易踩的约束（与 core/ffmpeg_io/FfmpegInterrupt.h 的约定同源）:
//   1) AvInterruptState 必须是**成员**: 装到 AVFormatContext 上的回调会被它派生出的
//      AVIOContext / URLContext 各复制一份指针，栈上状态一返回就悬垂。
//   2) 上下文**只能由 Close() 关闭**，Open() 末尾绝不顺手关掉 —— 色彩 HDR 的
//      UpdateFromStream 必须在关闭之前跑（部分容器的 HDR 元数据要读完包才补进 codecpar），
//      字幕 / 时码 / 辅助数据的 Finish() 又必须在关闭之后、发信号之前跑。
//      所以"什么时候关"仍然由引擎决定，本类只提供这个动作，且重复调用安全。
//
// 本类不拥有取消标志: 引擎那颗（可能是外部后台任务令牌）由 CancelSourceScope 管生命周期，
// 本会话只在一次 Run 内部存在，因此只存裸指针。

#include <atomic>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/ffmpeg_io/FfmpegInterrupt.h"

namespace videoeye {
namespace analyzer {

class AnalysisInputSession {
public:
    // 打开的结果。Failed / Cancelled 时 result 里的终态已经写好
    // （scan_status + error_message + scan_error_code），调用方只负责发对应的回调。
    enum class Outcome {
        Ok,
        Failed,     // result 已被 MarkFailed 写过；回调文案取 result.error_message
        Cancelled,  // 打开/探测阶段被取消；调用方发 NotifyCancelled
    };

    // file_path / options 必须活过本对象（同一次 Run 内的形参即可）。
    // cancel_source: 本次 Run 实际使用的那颗取消标志，可为空（那时只靠超时）。
    AnalysisInputSession(const std::string& file_path, const AnalysisOptions& options,
                         std::atomic<bool>* cancel_source);
    ~AnalysisInputSession();

    AnalysisInputSession(const AnalysisInputSession&) = delete;
    AnalysisInputSession& operator=(const AnalysisInputSession&) = delete;

    // 分配上下文 -> 打开 -> 探测 -> 容器级事实 -> moov 顺序 -> MP4 sample table -> 流摘要。
    // 打开成功后把中断回调的绝对截止时间清零（进入扫描阶段后只响应取消，
    // 否则长本地文件的正常读取会被打开阶段那 15 秒超时误杀）。
    Outcome Open(model::AnalysisResult& result);

    // 关闭上下文并把指针置空。重复调用安全。
    void Close();

    AVFormatContext* context() const { return fmt_; }

private:
    // 打开/探测阶段被打断 = 用户取消。判据与引擎原先的 IsCancelledExit 完全一致：
    // 取消标记已置位 **且** FFmpeg 确实以 AVERROR_EXIT 收场 —— 只看 ret 会把
    // "取消前恰好撞上一次真的 IO 错误"误判成取消，只看标记又会把"刚点取消、
    // FFmpeg 还没来得及响应"的失败误判成完成。
    bool IsCancelledExit(int ret) const;

    // 容器级事实：格式名 / 时长 / 体积 / 码率 / 可定位 / moov 顺序 / MP4 sample table。
    // 这几个都只在打开成功后才有意义，且都不依赖逐包扫描，所以归在输入侧。
    void FillContainerFacts(model::AnalysisResult& result);
    void BuildStreamDigests(model::AnalysisResult& result);

    const std::string& file_path_;
    const AnalysisOptions& options_;
    std::atomic<bool>* cancel_source_;
    AVFormatContext* fmt_ = nullptr;
    // 必须是成员：见文件头约束 1)
    ffmpeg_io::AvInterruptState interrupt_;
};

}  // namespace analyzer
}  // namespace videoeye
