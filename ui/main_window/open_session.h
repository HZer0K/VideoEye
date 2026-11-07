#pragma once

// 打开会话的两条判定口径（纯函数 + 纯结构，不依赖 QWidget）。
//
// 从 MainWindow::OpenMedia / OnPlayerOpenFinished 里抽出来，因为它们**错了不会崩、
// 只会静默走错分支**，而原来的宿主是 UI 回调，测不到：
//
//   1. 一个 source 该走哪条打开路径。两条路径的收尾方式完全不同（裸图像直接成功、
//      PCM 与通用媒体都异步 + 回调），判错就等于让收尾信号无人认领，
//      或让一个根本没有 OpenFinished 的路径干等回调。
//   2. 异步回调是不是「过期的」。任何一次新打开都要先作废上一次的待收尾登记，
//      否则同步分支（Raw）期间迟到的旧回调会拿着旧路径去刷界面 ——
//      用户看到的是"打开了 B，界面显示的是 A 的结果"。

#include <QFileInfo>
#include <QString>

namespace videoeye {
namespace ui {

// 打开路径的种类。
enum class OpenTargetKind {
    Media,    // 通用媒体：事务式异步打开，收尾靠 OpenFinished
    RawImage, // 裸图像（yuv/nv12/rgb/bgr/yuy2/raw）：不进播放器，没有异步收尾
    Pcm,      // 裸 PCM：异步打开（同走 OpenFinished），参数由用户现场指定
};

// `.raw` 是唯一一个内容二义的扩展名：既可能是原始像素序列，也可能是裸 PCM 音频。
// 分类仍按历史口径落到 RawImage（测过的判定不在这里推翻），由打开入口在真正加载
// 之前再问一次用户；其余后缀没有歧义。
enum class RawOpenChoice {
    RawImage, // 原始像素序列（历史默认）
    Pcm,      // 裸 PCM 音频
};

inline bool IsAmbiguousRawSuffix(const QString& source) {
    return QFileInfo(source).suffix().toLower() == "raw";
}

inline OpenTargetKind ClassifyOpenTarget(const QString& source) {
    // 判据与抽出来之前一致：QFileInfo 的扩展名（不含点）转小写。
    // 注意 URL（rtmp://…、http://…/x.m3u8）也走这里：没有扩展名时 suffix() 为空，
    // 落到 Media 分支，与历史行为相同。
    const QString suffix = QFileInfo(source).suffix().toLower();
    if (suffix == "yuv" || suffix == "nv12" || suffix == "rgb" || suffix == "bgr" || suffix == "yuy2" ||
        suffix == "raw") {
        return OpenTargetKind::RawImage;
    }
    if (suffix == "pcm") {
        return OpenTargetKind::Pcm;
    }
    return OpenTargetKind::Media;
}

// 一次「异步打开」的待收尾登记。
// source 为空表示**当前没有**待收尾请求 —— 空串不是合法的打开目标，
// 正好拿它当"已作废"的哨兵。
struct OpenTicket {
    QString source;
    bool autoplay = false;

    // 裸 PCM：demuxer 非空表示这次异步打开由用户的 PCM 参数驱动，收尾要按
    // PCM 处理（标签要带参数、不跑视频诊断扫描、失败不退回通用媒体重探）。
    QString pcm_demuxer;
    int pcm_sample_rate = 0;
    int pcm_channels = 0;

    bool IsPcm() const {
        return !pcm_demuxer.isEmpty();
    }
};

inline bool IsOpenTicketArmed(const OpenTicket& ticket) {
    return !ticket.source.isEmpty();
}

inline void ClearOpenTicket(OpenTicket& ticket) {
    ticket = OpenTicket{};
}

// 登记一次待收尾请求。空 source 不会登记（当作"这次打开不走异步"）。
inline void ArmOpenTicket(OpenTicket& ticket, const QString& source, bool autoplay) {
    if (source.isEmpty()) {
        ClearOpenTicket(ticket);
        return;
    }
    ticket.source = source;
    ticket.autoplay = autoplay;
}

// 登记一次裸 PCM 的待收尾请求：除路径/自动播放外还要记住用户现场选的参数，
// 收尾时标签与媒体信息都靠它（打开链路本身不会把这些写回 StreamInfo）。
inline void ArmPcmOpenTicket(OpenTicket& ticket, const QString& source, bool autoplay, const QString& demuxer,
                             int sample_rate, int channels) {
    ArmOpenTicket(ticket, source, autoplay);
    if (!IsOpenTicketArmed(ticket)) {
        return;
    }
    ticket.pcm_demuxer = demuxer;
    ticket.pcm_sample_rate = sample_rate;
    ticket.pcm_channels = channels;
}

// 取走待收尾信息并作废登记。返回 false 表示这次回调没有对应的登记
// （被后续打开取代 / 同步分支期间迟到的旧结果）——调用方应当直接忽略。
inline bool TakeOpenTicket(OpenTicket& ticket, QString* source, bool* autoplay) {
    if (!IsOpenTicketArmed(ticket)) {
        return false;
    }
    if (source)
        *source = ticket.source;
    if (autoplay)
        *autoplay = ticket.autoplay;
    ClearOpenTicket(ticket);
    return true;
}

// 同上，但把整张票（含 PCM 参数）交给调用方，由它决定按哪条路径收尾。
inline bool TakeOpenTicket(OpenTicket& ticket, OpenTicket* out) {
    if (!IsOpenTicketArmed(ticket)) {
        return false;
    }
    if (out)
        *out = ticket;
    ClearOpenTicket(ticket);
    return true;
}

} // namespace ui
} // namespace videoeye
