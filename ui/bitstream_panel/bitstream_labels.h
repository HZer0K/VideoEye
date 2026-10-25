#pragma once

// 码流页共用的展示口径：布尔值 / chroma 格式 / CICP 枚举（ISO/IEC 23001-8）-> 可读名。
//
// 摘要、结构树、对比表三个组件都要用同一套名称（VUI 里写的 primaries=9 在同一页
// 任何位置都该显示成 BT.2020），抽成 header-only 避免各抄一份后口径漂移。

#include <QObject>
#include <QString>

namespace videoeye {
namespace ui {

inline QString BitstreamFlagText(bool value) {
    return value ? QObject::tr("是") : QObject::tr("否");
}

inline QString BitstreamChromaName(int chroma_format_idc) {
    switch (chroma_format_idc) {
    case 0:
        return "4:0:0";
    case 1:
        return "4:2:0";
    case 2:
        return "4:2:2";
    case 3:
        return "4:4:4";
    default:
        return QObject::tr("未知");
    }
}

inline QString BitstreamPrimariesName(int value) {
    switch (value) {
    case 1:
        return "BT.709";
    case 4:
        return "BT.470M";
    case 5:
        return "BT.470BG";
    case 6:
        return "BT.601";
    case 7:
        return "SMPTE 240M";
    case 8:
        return "Generic film";
    case 9:
        return "BT.2020";
    case 10:
        return "SMPTE 428";
    case 11:
        return "SMPTE 431 (DCI-P3)";
    case 12:
        return "SMPTE 432 (Display P3)";
    case 22:
        return "EBU 3213";
    default:
        return QObject::tr("未标注");
    }
}

inline QString BitstreamTransferName(int value) {
    switch (value) {
    case 1:
        return "BT.709";
    case 4:
        return "Gamma 2.2";
    case 5:
        return "Gamma 2.8";
    case 6:
        return "BT.601";
    case 7:
        return "SMPTE 240M";
    case 8:
        return "Linear";
    case 9:
        return "Log";
    case 10:
        return "Log Sqrt";
    case 11:
        return "IEC 61966-2-4";
    case 12:
        return "BT.1361";
    case 13:
        return "sRGB/sYCC";
    case 14:
        return "BT.2020 (10bit)";
    case 15:
        return "BT.2020 (12bit)";
    case 16:
        return "SMPTE 2084 (PQ)";
    case 17:
        return "SMPTE 428";
    case 18:
        return "HLG (ARIB B67)";
    default:
        return QObject::tr("未标注");
    }
}

inline QString BitstreamMatrixName(int value) {
    switch (value) {
    case 0:
        return "GBR";
    case 1:
        return "BT.709";
    case 4:
        return "FCC";
    case 5:
        return "BT.470BG";
    case 6:
        return "BT.601";
    case 7:
        return "SMPTE 240M";
    case 8:
        return "YCgCo";
    case 9:
        return "BT.2020 NCL";
    case 10:
        return "BT.2020 CL";
    case 11:
        return "SMPTE 2085";
    case 12:
        return "Chroma-derived NCL";
    case 13:
        return "Chroma-derived CL";
    case 14:
        return "ICtCp";
    default:
        return QObject::tr("未标注");
    }
}

} // namespace ui
} // namespace videoeye
