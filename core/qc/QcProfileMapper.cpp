#include "core/qc/QcProfileMapper.h"

namespace videoeye {
namespace qc {

analyzer::AnalysisOptions OptionsForDepth(QcAnalysisDepth depth) {
    analyzer::AnalysisOptions options;
    // 强度只动"要不要解码"的开关，各分析维度本身仍保持默认开启。
    switch (depth) {
        case QcAnalysisDepth::Fast:
            options.analyze_audio_qc = false;  // 响度要靠解码，是最大的一块开销
            options.decode_frame_types = false;
            options.analyze_bitstream = true;      // 只读 extradata，几百字节
            options.analyze_mp4_sample_table = true;
            options.sample_interval_seconds = 2.0;  // 抽稀一点，序列点数减半
            break;
        case QcAnalysisDepth::Standard:
            options.analyze_audio_qc = true;
            options.decode_frame_types = false;
            break;
        case QcAnalysisDepth::Deep:
            options.analyze_audio_qc = true;
            options.decode_frame_types = true;
            options.sample_interval_seconds = 0.5;
            break;
    }
    return options;
}

}  // namespace qc
}  // namespace videoeye
