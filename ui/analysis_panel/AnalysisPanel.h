#pragma once

#include <QWidget>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QCategoryAxis>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QTableWidget>
#include <QTimer>
#include <QComboBox>
#include <QtCharts/QValueAxis>
#include <deque>
#include <vector>

#include "core/analyzer/StreamAnalyzer.h"
#include "core/analyzer/FrameAnalyzer.h"
#include "core/analyzer/FaceDetector.h"

namespace videoeye {
namespace ui {

// 分析面板类
class AnalysisPanel : public QWidget {
    Q_OBJECT
    
public:
    explicit AnalysisPanel(QWidget* parent = nullptr);
    ~AnalysisPanel();
    
public slots:
    // 更新统计数据
    void UpdateStreamStats(const analyzer::StreamStats& stats);
    void UpdateHistogram(const analyzer::HistogramData& hist);
    void UpdateFaceDetection(const std::vector<analyzer::FaceInfo>& faces);
    void ResetVideoFrameList();
    void AppendVideoFrameInfo(int index, int frame_type, bool is_key_frame, qint64 pts, double timestamp_seconds);
    void ResetAudioFrameList();
    void AppendAudioFrameInfo(int index, qint64 pts, double timestamp_seconds,
                              int sample_count, int sample_rate, int channels, int byte_count);
    
    // 导出报告
    void OnExportReport();
    
private:
    struct VideoFrameRecord {
        int index = 0;
        int frame_type = 0;
        bool is_key_frame = false;
        qint64 pts = 0;
        double timestamp_seconds = 0.0;
        int gop_index = 0;
        int gop_position = 0;
    };

    struct GopSummary {
        int gop_index = 0;
        int start_frame = 0;
        int end_frame = 0;
        double start_ts = 0.0;
        double end_ts = 0.0;
        int total_frames = 0;
        int i_count = 0;
        int p_count = 0;
        int b_count = 0;
        int key_count = 0;
    };

    struct AudioFrameRecord {
        int index = 0;
        qint64 pts = 0;
        double timestamp_seconds = 0.0;
        int sample_count = 0;
        int sample_rate = 0;
        int channels = 0;
        int byte_count = 0;
    };

    // 初始化UI
    void SetupUI();
    void SetupStreamTab();
    void SetupFrameTab();
    void SetupAudioFrameTab();
    void SetupHistogramTab();
    void SetupFaceTab();
    void RebuildFrameTable();
    void RebuildGopTable();
    void RebuildAudioFrameTable();
    void UpdateFrameSummary();
    void UpdateAudioFrameSummary();
    QString FrameTypeToString(int frame_type) const;
    bool MatchesFrameFilter(const VideoFrameRecord& record) const;
    void FlushPendingUiUpdates();
    void FlushPendingFrameTableUpdates();
    void FlushPendingGopTableUpdates();
    void FlushPendingAudioFrameTableUpdates();
    void RefreshStreamStatsUi(const analyzer::StreamStats& stats);
    void SetTableItemText(QTableWidget* table, int row, int column, const QString& text);
    void AppendFrameRowToTable(const VideoFrameRecord& record);
    void AppendAudioFrameRowToTable(const AudioFrameRecord& record);
    void UpdateGopRowInTable(int row, const GopSummary& summary);
    void OnExportFrameCsv();
    void OnExportAudioFrameCsv();
    void OnFrameFilterChanged();
    
    // 更新图表
    void UpdateBitrateChart(const analyzer::StreamStats& stats);
    void UpdateFPSChart(const analyzer::StreamStats& stats);
    void UpdateGOPChart(const analyzer::StreamStats& stats);
    void UpdateHistogramChart(const analyzer::HistogramData& hist);
    
    // 成员变量
    QTabWidget* tab_widget_;
    
    // 流分析标签页
    QWidget* stream_tab_;
    QTableWidget* stats_table_;
    QChartView* bitrate_chart_;
    QChartView* fps_chart_;
    QChartView* gop_chart_;

    QWidget* frame_tab_;
    QComboBox* frame_filter_combo_;
    QLabel* frame_summary_label_;
    QTableWidget* frame_table_;
    QPushButton* export_frame_csv_button_;
    QTableWidget* gop_table_;

    QWidget* audio_frame_tab_;
    QLabel* audio_frame_summary_label_;
    QTableWidget* audio_frame_table_;
    QPushButton* export_audio_frame_csv_button_;
    
    // 直方图标签页
    QWidget* histogram_tab_;
    QChartView* histogram_chart_;
    
    // 人脸检测标签页
    QWidget* face_tab_;
    QLabel* face_count_label_;
    QTableWidget* face_table_;
    QLabel* face_image_label_;
    
    // 控制按钮
    QPushButton* export_button_;
    
    // 图表数据系列
    QChart* bitrate_chart_object_;
    QChart* fps_chart_object_;
    QLineSeries* bitrate_series_;
    QLineSeries* fps_series_;
    QValueAxis* bitrate_axis_x_;
    QValueAxis* bitrate_axis_y_;
    QValueAxis* fps_axis_x_;
    QValueAxis* fps_axis_y_;
    
    // 定时器
    QTimer* update_timer_;
    
    // 当前数据
    analyzer::StreamStats current_stats_;
    analyzer::HistogramData current_hist_;
    std::vector<analyzer::FaceInfo> current_faces_;
    std::vector<VideoFrameRecord> frame_records_;
    std::vector<AudioFrameRecord> audio_frame_records_;
    std::vector<GopSummary> gop_summaries_;
    analyzer::StreamStats pending_stream_stats_;
    bool has_pending_stream_stats_ = false;
    bool frame_table_dirty_ = false;
    bool gop_table_dirty_ = false;
    bool frame_summary_dirty_ = false;
    bool audio_frame_table_dirty_ = false;
    bool audio_frame_summary_dirty_ = false;
    size_t frame_table_synced_record_count_ = 0;
    size_t gop_table_synced_count_ = 0;
    size_t audio_frame_table_synced_record_count_ = 0;
    int stream_chart_sample_index_ = 0;
    std::deque<qreal> bitrate_chart_values_;
    std::deque<qreal> fps_chart_values_;
};

} // namespace ui
} // namespace videoeye
