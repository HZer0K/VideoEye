#include "core/domain/model/AnalysisTypes.h"

namespace videoeye {
namespace model {

const char* ToString(AnalysisStatus status) {
    switch (status) {
        case AnalysisStatus::Complete:  return "complete";
        case AnalysisStatus::Sampled:   return "sampled";
        case AnalysisStatus::Cancelled: return "cancelled";
        case AnalysisStatus::Failed:    return "failed";
    }
    return "unknown";
}

} // namespace model
} // namespace videoeye
