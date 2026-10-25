#pragma once

// 流媒体包各子视图共用的格式化小工具：秒 / kbps / 分辨率文案。
//
// 结构树、码率阶梯、分片时间轴三处都会把同一批数字写成同一种格式。以前它们
// 放在 StreamingPanel.cpp 的匿名命名空间里，拆分后必须挪到能被多个视图共享的
// 位置。只做字符串格式化、不碰任何状态，所以留成头文件里的 inline。

#include <QString>

#include <cstdint>

namespace videoeye {
namespace ui {

// 三处小数位统一保留：时间轴要对齐比较，列宽也靠这个保持稳定
inline QString StreamingSeconds(double seconds) {
    return QString::number(seconds, 'f', 3);
}

inline QString StreamingKbps(int64_t bps) {
    return QString::number(bps / 1000);
}

// 宽或高任一为 0 都写成 "-"：清单没声明分辨率时写 "0x0" 会被当成真实分辨率读。
inline QString StreamingResolution(int width, int height) {
    if (width > 0 && height > 0)
        return QString("%1x%2").arg(width).arg(height);
    return QStringLiteral("-");
}

} // namespace ui
} // namespace videoeye
