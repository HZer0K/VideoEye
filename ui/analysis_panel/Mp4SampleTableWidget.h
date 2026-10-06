#pragma once

// MP4 / fMP4 样本表子页：轨道选择 + 样本级展开表（含结构树联动高亮）
// + 一致性问题表 + 分片(moof)表 + CSV 导出。
//
// 从 ContainerStructurePage 拆出来的独立控件：只负责「样本表」这一块，
// 对外接缝：
//   - SetSamples()       容器解析 / 全文件诊断扫描喂样本表结果
//   - SetFocusBox()      结构树点击样本相关 box 时的列高亮联动
//   - ShowFragmentTab()  分片类 box 联动时切到底部「分片 (moof)」tab
// 页面本体负责把本控件放进 mp4_detail_tabs_，并按需切 detail_stack_ / 外层 tab。

#include <QWidget>
#include <QString>

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/Mp4ConsistencyIssue.h"

namespace videoeye {
namespace ui {

class Mp4SampleTableWidget : public QWidget {
    Q_OBJECT

public:
    explicit Mp4SampleTableWidget(QWidget* parent = nullptr);

    // 喂入样本表结果：填充轨道下拉 + 概要 + 三张表（样本/问题/分片）
    void SetSamples(const model::Mp4SampleTableResult& samples);

    // 结构树点击样本相关 box 时的联动：设置高亮列 + 重绘样本表
    void SetFocusBox(const QString& box);

    // 分片类 box 联动时，把底部 tab 切到「分片 (moof)」
    void ShowFragmentTab();

    int TrackCount() const { return track_combo_->count(); }
    void SetTrackIndex(int index) { track_combo_->setCurrentIndex(index); }

private:
    void SetupUi();
    void UpdateSummary();
    void RebuildSampleTable();
    void RebuildIssueTable();
    void RebuildFragmentTable();
    void OnTrackChanged(int index);
    void OnExportCsv();

    QComboBox* track_combo_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QLabel* focus_label_ = nullptr;
    QTableWidget* sample_table_ = nullptr;
    QTableWidget* issue_table_ = nullptr;
    QTableWidget* fragment_table_ = nullptr;
    QPushButton* export_button_ = nullptr;

    model::Mp4SampleTableResult samples_;
    QString focus_box_;  // 结构树点击的 box 名（"" = 不过滤）
};

} // namespace ui
} // namespace videoeye
