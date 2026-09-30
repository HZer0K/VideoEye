#pragma once

#include <QMetaType>
#include <QString>

namespace videoeye {
namespace model {

struct TimelineEvent {
    int index = 0;
    QString category;
    double timestamp_seconds = 0.0;
    QString label;
    QString detail;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::TimelineEvent)
