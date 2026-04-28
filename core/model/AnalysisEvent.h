#pragma once

#include <QMetaType>
#include <QString>

namespace videoeye {
namespace model {

struct AnalysisEvent {
    int index = 0;
    QString severity;
    QString type;
    int stream_index = -1;
    qint64 pts = 0;
    double timestamp_seconds = 0.0;
    QString summary;
    QString detail;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::AnalysisEvent)
