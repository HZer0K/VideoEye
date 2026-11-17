#pragma once

// 宏块分析页：运动矢量列表 + 运动矢量可视化预览 + 块大小分布 + 运动幅度分布。
//
// 从 AnalysisPanel 拆出来的独立页面组件。这一页的数据只在播放期由解码器回吐
// （RealtimeAnalysisController → AnalysisBus → MainWindow → 面板），不参与全文件扫描，所以接缝很窄：
//   - 数据进来：SetAnalysis()
//   - 开关出去：面板通过 SetFeatureHooks 注入，视图只认自己的编号 0 = 宏块分析
// 页面本体就是 QWidget（外部 QStackedWidget 的一页），不需要再包一层 tab widget。
//
// 术语随编码自适应（HEVC→CTU，其余→宏块），判断依赖模型里的 CodecType 辅助函数。
// 注意 HEVC / VP9 / AV1 的解码器不导出运动矢量（见 CodecType::SupportsMotionVectorExport），
// 这些编码的 P/B 帧只能显示「不导出运动矢量」，只有 HEVC I 帧还能给出 CTU 网格估算。
// 数据只在播放期产生：换文件 / 关闭分析时必须 Reset()，否则上一份素材的表格和
// 箭头会留在页面上被当成当前文件的分析结果。

#include <QString>
#include <QWidget>

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <functional>
#include <vector>

#include "core/domain/model/MacroblockInfo.h"

class QCheckBox;

namespace videoeye {
namespace ui {

class MacroblockView : public QWidget {
    Q_OBJECT

public:
    explicit MacroblockView(QWidget* parent = nullptr);

    // 页面只认自己的开关编号（0 = 宏块分析）。编号到 AnalysisFeature 的映射、
    // 以及 AnalysisFeatureToggled 的转发都由面板做，视图不反向认识面板。
    // 注入时会立即把真实开关状态回写到「启用分析」勾选框：控件是在构造函数里建的，
    // 那时钩子还不存在，只能先按默认值创建；不同步的话界面会显示"启用分析"，
    // 而面板里的 feature_enabled_ 其实是关的。
    void SetFeatureHooks(std::function<bool(int)> is_enabled, std::function<void(int, bool)> set_enabled);

    // 播放期约 10Hz 合并回吐（控制器侧 100ms 窗口只留最新一帧）：攒到 FlushPending() 再画
    void SetAnalysis(const model::MacroblockFrameAnalysis& analysis);

    // 清空本页数据并回到「等待视频播放」态。换文件、关闭分析时调用。
    void Reset();

    // 依据当前钩子（is_enabled_）把「启用分析」勾选框回写一次。协调层在叠加开关
    // 改变实际分析状态后调用它，让页面显示与实际采集状态保持一致；内部用
    // QSignalBlocker，不会反过来触发 set_enabled_ 回调造成循环。
    void SyncToggleFromHooks();

    bool HasPending() const {
        return dirty_;
    }
    void FlushPending();

    // 记录一个场景切换时间点（秒）。曲线在该位置画竖线并在切镜处断线，
    // 避免把两个镜头的幅度点连成一段，也便于判断幅度峰值是不是切镜造成的。
    void AppendSceneChangeTimestamp(double seconds);

signals:
    // 点击运动强度曲线: 请求跳到曲线上对应的时间点（秒）
    void SeekRequested(double seconds);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void SetupUi();
    void RefreshUi();
    void OnExportCsv();
    // 导出曲线历史（时间 / 平均幅度 / 最大幅度 / 帧内块 / 总块数）
    void OnExportMotionHistoryCsv();
    // 只重绘右侧可视化预览（表格选中行变化时也走这里，不重建整页数据）
    void RedrawVisualization();
    // 运动强度时间曲线：记录每个分析帧的 avg/max 幅度，点击可定位
    void AppendMotionHistory(const model::MacroblockFrameAnalysis& analysis);
    void RedrawMotionChart();

    // 一个采样点 = 一个带运动矢量的帧
    struct MotionSample {
        double timestamp = 0.0;
        double avg_magnitude = 0.0;
        double max_magnitude = 0.0;
        int intra_count = 0;
        int total_blocks = 0;
    };

    std::function<bool(int)> is_enabled_;
    std::function<void(int, bool)> set_enabled_;

    QLabel* summary_label_ = nullptr;
    // 必须留成员：SetFeatureHooks 注入钩子后要拿它把真实开关状态回写到界面
    QCheckBox* toggle_ = nullptr;

    QTableWidget* mv_table_ = nullptr;        // 运动矢量列表
    QLabel* viz_label_ = nullptr;             // 运动矢量可视化预览
    QLabel* chart_label_ = nullptr;           // 运动强度时间曲线
    QTableWidget* blocksize_table_ = nullptr; // 块大小分布
    QTableWidget* mag_table_ = nullptr;       // 运动幅度分布

    model::MacroblockFrameAnalysis analysis_;
    // 表格里选中的运动矢量行（-1 = 未选中）；只影响预览高亮，不参与统计
    int highlighted_row_ = -1;
    bool dirty_ = false;

    // 运动强度历史（滚动窗口，约 10Hz × 10 分钟）与曲线坐标范围
    std::vector<MotionSample> motion_history_;
    // 播放期场景切换时间点（升序）；曲线画竖线并在切镜处断线
    std::vector<double> scene_change_timestamps_;
    double last_sample_timestamp_ = -1.0;
    double chart_t0_ = 0.0;
    double chart_t1_ = 1.0;
    int chart_pixel_width_ = 0;
    int chart_pixel_height_ = 0;
};

} // namespace ui
} // namespace videoeye
