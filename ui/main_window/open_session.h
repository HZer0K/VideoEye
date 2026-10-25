#pragma once

// 打开会话的两条判定口径（纯函数 + 纯结构，不依赖 QWidget）。
//
// 从 MainWindow::OpenMedia / OnPlayerOpenFinished 里抽出来，因为它们**错了不会崩、
// 只会静默走错分支**，而原来的宿主是 UI 回调，测不到：
//
//   1. 一个 source 该走哪条打开路径。三条路径的收尾方式完全不同（裸图像直接成功、
//      PCM 同步打开、通用媒体异步 + 回调），判错就等于让收尾信号无人认领，
//      或让一个根本没有 OpenFinished 的路径干等回调。
//   2. 异步回调是不是「过期的」。任何一次新打开都要先作废上一次的待收尾登记，
//      否则同步分支（Raw / PCM）期间迟到的旧回调会拿着旧路径去刷界面 ——
//      用户看到的是"打开了 B，界面显示的是 A 的结果"。

#include <QFileInfo>
#include <QString>

namespace videoeye {
namespace ui {

// 打开路径的种类。
enum class OpenTargetKind {
    Media,    // 通用媒体：事务式异步打开，收尾靠 OpenFinished
    RawImage, // 裸图像（yuv/nv12/rgb/bgr/yuy2/raw）：不进播放器，没有异步收尾
    Pcm,      // 裸 PCM：同步打开，参数由用户现场指定
};

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
};

inline bool IsOpenTicketArmed(const OpenTicket& ticket) {
    return !ticket.source.isEmpty();
}

inline void ClearOpenTicket(OpenTicket& ticket) {
    ticket.source.clear();
    ticket.autoplay = false;
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

} // namespace ui
} // namespace videoeye
