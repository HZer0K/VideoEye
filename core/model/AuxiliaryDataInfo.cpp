#include "core/model/AuxiliaryDataInfo.h"

namespace videoeye {
namespace model {

const char* Scte35Cue::NetworkIndicatorText() const {
    if (!valid || cancel_indicator) return "-";
    return out_of_network ? "OUT" : "IN";
}

const char* ToString(AuxStreamKind kind) {
    switch (kind) {
        case AuxStreamKind::Scte35:       return "SCTE-35 cue";
        case AuxStreamKind::Timecode:     return "时码轨 (tmcd)";
        case AuxStreamKind::Klv:          return "KLV 元数据";
        case AuxStreamKind::Teletext:     return "图文电视";
        case AuxStreamKind::Vbi:          return "VBI 数据";
        case AuxStreamKind::SubtitleData: return "字幕数据";
        case AuxStreamKind::GenericData:  return "通用 data 流";
        case AuxStreamKind::Unknown:
        default:                          return "未知 data 流";
    }
}

const char* ToString(Scte35Command command) {
    switch (command) {
        case Scte35Command::SpliceNull:           return "splice_null";
        case Scte35Command::SpliceSchedule:       return "splice_schedule";
        case Scte35Command::SpliceInsert:         return "splice_insert";
        case Scte35Command::TimeSignal:           return "time_signal";
        case Scte35Command::BandwidthReservation: return "bandwidth_reservation";
        case Scte35Command::PrivateCommand:       return "private_command";
        case Scte35Command::Unknown:
        default:                                  return "unknown";
    }
}

const char* Scte35CommandCode(Scte35Command command) {
    switch (command) {
        case Scte35Command::SpliceNull:           return "splice_null";
        case Scte35Command::SpliceSchedule:       return "splice_schedule";
        case Scte35Command::SpliceInsert:         return "splice_insert";
        case Scte35Command::TimeSignal:           return "time_signal";
        case Scte35Command::BandwidthReservation: return "bandwidth_reservation";
        case Scte35Command::PrivateCommand:       return "private_command";
        case Scte35Command::Unknown:
        default:                                  return "unknown";
    }
}

const char* Scte35SegmentationTypeName(uint32_t type_id) {
    switch (type_id) {
        case 0x10: return "节目开始 (Program Start)";
        case 0x11: return "节目结束 (Program End)";
        case 0x12: return "节目提前终止 (Program Early Termination)";
        case 0x13: return "节目中断脱离 (Program Breakaway)";
        case 0x14: return "节目恢复 (Program Resumption)";
        case 0x15: return "计划内超时 (Program Runover Planned)";
        case 0x16: return "非计划超时 (Program Runover Unplanned)";
        case 0x17: return "节目重叠开始 (Program Overlap Start)";
        case 0x18: return "节目黑屏覆盖 (Program Blackout Override)";
        case 0x19: return "节目接入 (Program Join)";
        case 0x20: return "章节开始 (Chapter Start)";
        case 0x21: return "章节结束 (Chapter End)";
        case 0x22: return "广告位开始 (Break Start)";
        case 0x23: return "广告位结束 (Break End)";
        case 0x24: return "片头开始 (Opening Credit Start)";
        case 0x25: return "片头结束 (Opening Credit End)";
        case 0x26: return "片尾开始 (Closing Credit Start)";
        case 0x27: return "片尾结束 (Closing Credit End)";
        case 0x30: return "提供方广告开始 (Provider Advertisement Start)";
        case 0x31: return "提供方广告结束 (Provider Advertisement End)";
        case 0x32: return "分发方广告开始 (Distributor Advertisement Start)";
        case 0x33: return "分发方广告结束 (Distributor Advertisement End)";
        case 0x34: return "提供方广告位开始 (Provider Placement Opportunity Start)";
        case 0x35: return "提供方广告位结束 (Provider Placement Opportunity End)";
        case 0x36: return "分发方广告位开始 (Distributor Placement Opportunity Start)";
        case 0x37: return "分发方广告位结束 (Distributor Placement Opportunity End)";
        case 0x38: return "非计划事件开始 (Unscheduled Event Start)";
        case 0x39: return "非计划事件结束 (Unscheduled Event End)";
        case 0x3A: return "网络开始 (Network Start)";
        case 0x3B: return "网络结束 (Network End)";
        default:   return nullptr;
    }
}

}  // namespace model
}  // namespace videoeye
