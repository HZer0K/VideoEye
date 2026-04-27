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
    void AppendVideoFrameInfo(int index, int frame_type, qint64 pts, double timestamp_seconds);
    
    // 导出报告
    void OnExportReport();
    
private:
    // 初始化UI
    void SetupUI();
    void SetupStreamTab();
    void SetupFrameTab();
    void SetupHistogramTab();
    void SetupFaceTab();
    
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
    QTableWidget* frame_table_;
    
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
    QLineSeries* bitrate_series_;
    QLineSeries* fps_series_;
    
    // 定时器
    QTimer* update_timer_;
    
    // 当前数据
    analyzer::StreamStats current_stats_;
    analyzer::HistogramData current_hist_;
    std::vector<analyzer::FaceInfo> current_faces_;
};

} // namespace ui
} // namespace videoeye
