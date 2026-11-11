#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace model {

const char* ToString(RepairMode value) {
    switch (value) {
    case RepairMode::PreserveBitstream:
        return "preserve_bitstream";
    case RepairMode::RecoverContent:
        return "recover_content";
    }
    return "unknown";
}

const char* ToString(RepairGapPolicy value) {
    switch (value) {
    case RepairGapPolicy::PreserveOriginalTimeline:
        return "preserve_original_timeline";
    case RepairGapPolicy::MergeContiguous:
        return "merge_contiguous";
    }
    return "unknown";
}

const char* ToString(RepairRecoveryEvidence value) {
    switch (value) {
    case RepairRecoveryEvidence::Unknown:
        return "unknown";
    case RepairRecoveryEvidence::AvSeekFrame:
        return "av_seek_frame";
    case RepairRecoveryEvidence::MoofResync:
        return "moof_resync";
    case RepairRecoveryEvidence::BitstreamRap:
        return "bitstream_rap";
    case RepairRecoveryEvidence::TrialDecodeOk:
        return "trial_decode_ok";
    }
    return "unknown";
}

const char* ToString(RepairOutputContainer value) {
    switch (value) {
    case RepairOutputContainer::Auto:
        return "auto";
    case RepairOutputContainer::Mp4:
        return "mp4";
    case RepairOutputContainer::Mkv:
        return "mkv";
    case RepairOutputContainer::Ts:
        return "ts";
    }
    return "unknown";
}

const char* ToString(RepairBusinessStatus value) {
    switch (value) {
    case RepairBusinessStatus::Repaired:
        return "repaired";
    case RepairBusinessStatus::PartiallyRecovered:
        return "partially_recovered";
    case RepairBusinessStatus::NeedsReference:
        return "needs_reference";
    case RepairBusinessStatus::Unsupported:
        return "unsupported";
    case RepairBusinessStatus::NoRecoverableContent:
        return "no_recoverable_content";
    case RepairBusinessStatus::Failed:
        return "failed";
    case RepairBusinessStatus::Canceled:
        return "canceled";
    case RepairBusinessStatus::TimedOut:
        return "timed_out";
    }
    return "unknown";
}

const char* ToString(RepairCheckStatus value) {
    switch (value) {
    case RepairCheckStatus::NotRun:
        return "not_run";
    case RepairCheckStatus::Passed:
        return "passed";
    case RepairCheckStatus::Failed:
        return "failed";
    case RepairCheckStatus::Inconclusive:
        return "inconclusive";
    case RepairCheckStatus::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

const char* ToString(DamageLayer value) {
    switch (value) {
    case DamageLayer::File:
        return "file";
    case DamageLayer::Container:
        return "container";
    case DamageLayer::Track:
        return "track";
    case DamageLayer::Codec:
        return "codec";
    case DamageLayer::Decode:
        return "decode";
    case DamageLayer::Timing:
        return "timing";
    case DamageLayer::Redundancy:
        return "redundancy";
    }
    return "unknown";
}

const char* ToString(DamageConfidence value) {
    switch (value) {
    case DamageConfidence::Confirmed:
        return "confirmed";
    case DamageConfidence::Probable:
        return "probable";
    case DamageConfidence::Possible:
        return "possible";
    case DamageConfidence::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* ToString(DamagePrecision value) {
    switch (value) {
    case DamagePrecision::Exact:
        return "exact";
    case DamagePrecision::Bounded:
        return "bounded";
    case DamagePrecision::Estimated:
        return "estimated";
    case DamagePrecision::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* ToString(RepairAction value) {
    switch (value) {
    case RepairAction::S01_Remux:
        return "S01_remux";
    case RepairAction::S02_TailSalvage:
        return "S02_tail_salvage";
    case RepairAction::S03_TimelineRepair:
        return "S03_timeline_repair";
    case RepairAction::S04_SegmentSalvage:
        return "S04_segment_salvage";
    case RepairAction::S08_TolerantTranscode:
        return "S08_tolerant_transcode";
    case RepairAction::S09_SingleTrack:
        return "S09_single_track";
    }
    return "unknown";
}

const char* ToString(RepairVerificationLevel value) {
    switch (value) {
    case RepairVerificationLevel::V0_Container:
        return "V0";
    case RepairVerificationLevel::V1_FrameSample:
        return "V1";
    case RepairVerificationLevel::V2_FullDecode:
        return "V2";
    }
    return "unknown";
}

const char* ToString(RepairFidelityStatus value) {
    switch (value) {
    case RepairFidelityStatus::VerifiedMatch:
        return "verified_match";
    case RepairFidelityStatus::PartiallyVerified:
        return "partially_verified";
    case RepairFidelityStatus::NotVerified:
        return "not_verified";
    case RepairFidelityStatus::Failed:
        return "failed";
    }
    return "unknown";
}

const char* DisplayName(RepairBusinessStatus value) {
    switch (value) {
    case RepairBusinessStatus::Repaired:
        return "修复完成";
    case RepairBusinessStatus::PartiallyRecovered:
        return "部分恢复";
    case RepairBusinessStatus::NeedsReference:
        return "需要参考样片";
    case RepairBusinessStatus::Unsupported:
        return "当前能力不支持";
    case RepairBusinessStatus::NoRecoverableContent:
        return "无可恢复内容";
    case RepairBusinessStatus::Failed:
        return "修复失败";
    case RepairBusinessStatus::Canceled:
        return "已取消";
    case RepairBusinessStatus::TimedOut:
        return "超出预算";
    }
    return "未知";
}

const char* DisplayName(RepairAction value) {
    switch (value) {
    case RepairAction::S01_Remux:
        return "保留码流重封装";
    case RepairAction::S02_TailSalvage:
        return "尾部抢救";
    case RepairAction::S03_TimelineRepair:
        return "时间轴修复";
    case RepairAction::S04_SegmentSalvage:
        return "多段恢复";
    case RepairAction::S08_TolerantTranscode:
        return "容忍式转码";
    case RepairAction::S09_SingleTrack:
        return "单轨降级";
    }
    return "未知策略";
}

const char* DisplayName(RepairGapPolicy value) {
    switch (value) {
    case RepairGapPolicy::PreserveOriginalTimeline:
        return "保留原时间间隔";
    case RepairGapPolicy::MergeContiguous:
        return "删除损坏区间，连续播放";
    }
    return "未知";
}

const char* DisplayName(RepairRecoveryEvidence value) {
    switch (value) {
    case RepairRecoveryEvidence::Unknown:
        return "未标注来源";
    case RepairRecoveryEvidence::AvSeekFrame:
        return "demuxer 索引定位";
    case RepairRecoveryEvidence::MoofResync:
        return "容器级重同步（moof/trun）";
    case RepairRecoveryEvidence::BitstreamRap:
        return "码流级重同步（NAL/AU 扫描）";
    case RepairRecoveryEvidence::TrialDecodeOk:
        return "试解码确认";
    }
    return "未知";
}

const char* DisplayName(RepairOutputContainer value) {
    switch (value) {
    case RepairOutputContainer::Auto:
        return "自动（沿用源容器）";
    case RepairOutputContainer::Mp4:
        return "MP4";
    case RepairOutputContainer::Mkv:
        return "Matroska (MKV)";
    case RepairOutputContainer::Ts:
        return "MPEG-TS";
    }
    return "未知";
}

const char* DisplayName(RepairVerificationLevel value) {
    switch (value) {
    case RepairVerificationLevel::V0_Container:
        return "V0 容器可读";
    case RepairVerificationLevel::V1_FrameSample:
        return "V1 抽帧验证";
    case RepairVerificationLevel::V2_FullDecode:
        return "V2 全量解码";
    }
    return "未知";
}

} // namespace model
} // namespace videoeye
