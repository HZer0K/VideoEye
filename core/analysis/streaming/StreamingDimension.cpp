#include "core/analysis/streaming/StreamingDimension.h"

namespace videoeye {

model::DimensionStatus BuildStreamingDimensionStatus(const model::StreamingPackageResult& package) {
    model::DimensionStatus status;
    status.dimension = model::AnalysisDimension::Streaming;
    status.executed = true;
    status.capability = !package.valid      ? model::AnalysisCapability::Failed
                        : package.truncated ? model::AnalysisCapability::Partial
                                            : model::AnalysisCapability::Supported;
    status.scope_unit = "码率层";
    status.units_scanned = static_cast<int64_t>(package.ladder.size());
    status.units_total = status.units_scanned;
    status.has_coverage = true;
    status.coverage_ratio = 1.0;
    status.failure_reason = package.error_message;
    if (package.truncated)
        status.notes.push_back("清单读取被截断（事件/分片列表不完整）");
    return status;
}

} // namespace videoeye
