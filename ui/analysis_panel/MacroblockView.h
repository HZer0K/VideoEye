#pragma once

// 宏块分析页：运动矢量列表 + 运动矢量可视化预览 + 块大小分布 + 运动幅度分布。
//
// 从 AnalysisPanel 拆出来的独立页面组件。这一页的数据只在播放期由解码器回吐
// （MediaPlayer → MainWindow → 面板），不参与全文件扫描，所以接缝很窄：
//   - 数据进来：SetAnalysis()
//   - 开关出去：面板通过 SetFeatureHooks 注入，视图只认自己的编号 0 = 宏块分析
// 页面本体就是 QWidget（外部 QStackedWidget 的一页），不需要再包一层 tab widget。
//
// 术语随编码自适应（HEVC→CTU，其余→宏块），判断依赖模型里的 CodecType 辅助函数。

#include <QWidget>
#include <QString>

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <functional>

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
    void SetFeatureHooks(std::function<bool(int)> is_enabled,
                         std::function<void(int, bool)> set_enabled);

    // 播放期逐帧回吐：与面板的批量刷新节拍一致，攒到 FlushPending() 再画
    void SetAnalysis(const model::MacroblockFrameAnalysis& analysis);

    bool HasPending() const { return dirty_; }
    void FlushPending();

private:
    void SetupUi();
    void SyncToggleFromHooks();
    void RefreshUi();
    void OnExportCsv();

    std::function<bool(int)> is_enabled_;
    std::function<void(int, bool)> set_enabled_;

    QLabel* summary_label_ = nullptr;
    QPushButton* export_csv_button_ = nullptr;
    // 必须留成员：SetFeatureHooks 注入钩子后要拿它把真实开关状态回写到界面
    QCheckBox* toggle_ = nullptr;

    QTableWidget* mv_table_ = nullptr;          // 运动矢量列表
    QLabel* viz_label_ = nullptr;               // 运动矢量可视化预览
    QTableWidget* blocksize_table_ = nullptr;   // 块大小分布
    QTableWidget* mag_table_ = nullptr;         // 运动幅度分布

    model::MacroblockFrameAnalysis analysis_;
    bool dirty_ = false;
};

}  // namespace ui
}  // namespace videoeye
