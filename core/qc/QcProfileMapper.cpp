#include "core/qc/QcProfileMapper.h"

namespace videoeye {
namespace qc {

videoeye::AnalysisOptions OptionsForDepth(QcAnalysisDepth depth) {
    videoeye::AnalysisOptions options;
    // 强度只动"要不要解码"的开关，各分析维度本身仍保持默认开启。
    switch (depth) {
    case QcAnalysisDepth::Fast:
        options.analyze_audio_qc = false; // 响度要靠解码，是最大的一块开销
        options.decode_frame_types = false;
        options.analyze_bitstream = true; // 只读 extradata，几百字节
        options.analyze_mp4_sample_table = true;
        options.sample_interval_seconds = 2.0; // 抽稀一点，序列点数减半
        // 快速档不碰画面解码：视觉 QC 要完整解码视频一遍，会直接毁掉"批量初筛"
        // 的成本模型。需要画面结论时用标准/深度档。
        options.analyze_visual_qc = false;
        break;
    case QcAnalysisDepth::Standard:
        options.analyze_audio_qc = true;
        options.decode_frame_types = false;
        // 离线 QC 默认档就走逐帧：抽帧档的覆盖率必然远低于 99%，维度会恒为
        // Partial（见 BuildVisualQualityStatus），"没检出缺陷"就没法当通过用。
        // 逐帧分析宽度 256 也正是阈值标定的那档，和实时"标准档"口径一致。
        options.analyze_visual_qc = true;
        options.visual_qc_options.preset = model::VisualSamplingPreset::OfflineFull;
        break;
    case QcAnalysisDepth::Deep:
        options.analyze_audio_qc = true;
        options.decode_frame_types = true;
        options.sample_interval_seconds = 0.5;
        options.analyze_visual_qc = true;
        options.visual_qc_options.preset = model::VisualSamplingPreset::OfflineFull;
        break;
    }
    return options;
}

} // namespace qc
} // namespace videoeye
