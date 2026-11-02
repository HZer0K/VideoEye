#pragma once

// 页头「启用分析」聚合开关。
//
// 为什么要有它
// ------------
// 「播放统计与帧包」与「事件与时间轴」两页各自包含多个采集项（流统计 / 视频帧 /
// 音频帧 / 数据包；异常事件 / 同步样本 / 时间轴）。原先每一项在各自子页的工具栏里
// 各挂一个**同名**「启用分析」勾选框，于是同一个页面里出现 3~4 个一模一样的控件，
// 而其它页面把唯一的开关放在页头 —— 位置、个数、层级三处都不一致，用户既分不清
// 哪个管什么，也很难判断"这页到底开没开"。
//
// 现在每页页头只有一个聚合开关：
//   * 全部采集项开启 -> 勾选；全部关闭 -> 未勾选；部分开启 -> 半选（如实反映现状）；
//   * 点击 = 全开或全关（当前只要还有一项开着，就整体关掉；否则整体打开）。
// 逐项开关不再暴露 —— 需要单独控制时请另设计入口，不要靠"同名勾选框"凑数。
//
// 三态只用于展示：半选不是用户能点出来的状态，点击后立刻按钩子的真实结果回写。

#include <QCheckBox>
#include <QCoreApplication>
#include <QSignalBlocker>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <utility>

namespace videoeye {
namespace ui {

class AggregateFeatureToggle {
public:
    AggregateFeatureToggle() = default;
    ~AggregateFeatureToggle() {
        // 勾选框是页面的子对象，可能比本成员活得久（页面析构顺序：成员先析构，
        // 子对象随后由 QObject 析构）。断开信号，避免析构窗口里回调进已释放的成员。
        if (box_) {
            box_->blockSignals(true);
        }
    }
    AggregateFeatureToggle(const AggregateFeatureToggle&) = delete;
    AggregateFeatureToggle& operator=(const AggregateFeatureToggle&) = delete;

    // 建出勾选框并挂好点击处理。调用方负责把它 addWidget 到页头动作区
    // （顺序：分析入口在「导出」左侧，与其它页一致）。
    // feature_count = 本页参与聚合的采集项个数（>= 1）。
    QCheckBox* Create(QWidget* parent, const QString& tooltip, int feature_count) {
        feature_count_ = std::max(1, feature_count);
        box_ = new QCheckBox(QCoreApplication::translate("AggregateFeatureToggle", "启用分析"), parent);
        // 统一的 objectName：页头一致性回归靠它定位（见 test_analysis_page_layout）。
        box_->setObjectName(QStringLiteral("AnalysisFeatureToggle"));
        box_->setToolTip(tooltip);
        // 半选态要 tristate 才画得出来；用户点击时的三态循环由 OnClicked 兜住，
        // 点完立刻按钩子真实状态回写，不会停在半选。
        box_->setTristate(true);
        QObject::connect(box_, &QCheckBox::clicked, box_, [this]() { OnClicked(); });
        return box_;
    }

    // 注入读写钩子并立即回写控件状态。控件是在页面构造函数里建的，那时钩子还不存在。
    // 回写过程屏蔽信号：那是"初始化同步"，不是用户操作，不能伪造一次 FeatureToggled。
    void SetHooks(std::function<bool(int)> is_enabled, std::function<void(int, bool)> set_enabled, int feature_count) {
        is_enabled_ = std::move(is_enabled);
        set_enabled_ = std::move(set_enabled);
        feature_count_ = std::max(1, feature_count);
        SyncFromHooks();
    }

    // 按钩子的真实状态刷新三态（屏蔽信号）。
    void SyncFromHooks() {
        if (!box_ || !is_enabled_)
            return;
        int on = 0;
        for (int i = 0; i < feature_count_; ++i) {
            if (is_enabled_(i))
                ++on;
        }
        const Qt::CheckState state =
            (on == 0) ? Qt::Unchecked : ((on == feature_count_) ? Qt::Checked : Qt::PartiallyChecked);
        if (box_->checkState() == state)
            return;
        const QSignalBlocker blocker(box_);
        box_->setCheckState(state);
    }

    // 当前开启的采集项个数（半选判定的唯一依据，测试也读它）。
    int EnabledCount() const {
        int on = 0;
        if (!is_enabled_)
            return on;
        for (int i = 0; i < feature_count_; ++i) {
            if (is_enabled_(i))
                ++on;
        }
        return on;
    }

    int FeatureCount() const {
        return feature_count_;
    }

    QCheckBox* Box() const {
        return box_;
    }

private:
    void OnClicked() {
        // clicked 到达时 Qt 已经改过勾选框状态，目标状态要以钩子的真实状态为准：
        // 还有任一项开着 -> 全关；全部关着 -> 全开。
        const bool target = (EnabledCount() == 0);
        for (int i = 0; i < feature_count_; ++i) {
            if (set_enabled_)
                set_enabled_(i, target);
        }
        // 钩子可能夹住某些项（如宏块的"叠加强制开启"），以回读结果为准再落一次界面。
        SyncFromHooks();
    }

    QCheckBox* box_ = nullptr;
    int feature_count_ = 1;
    std::function<bool(int)> is_enabled_;
    std::function<void(int, bool)> set_enabled_;
};

} // namespace ui
} // namespace videoeye
