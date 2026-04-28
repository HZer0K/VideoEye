#pragma once

#include <QMetaType>
#include <QVector>

namespace videoeye {
namespace model {

struct AudioVisualizationFrame {
    int index = 0;
    double timestamp_seconds = 0.0;
    double level = 0.0;
    int sample_rate = 0;
    int channels = 0;
    QVector<double> waveform_points;
    QVector<double> spectrum_bins;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::AudioVisualizationFrame)
