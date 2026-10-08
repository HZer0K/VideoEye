#include "ui/analysis_panel/ContainerStructurePage.h"

#include <QAbstractItemView>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include "infrastructure/logging/ScopedTimer.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/analysis_panel/Mp4SampleTableWidget.h"

#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/Mp4ConsistencyIssue.h"

#include <QColor>
#include <QDateTime>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTreeWidget>

namespace videoeye {
namespace ui {

ContainerStructurePage::ContainerStructurePage(bool feature_checked, QWidget* parent)
    : QWidget(parent), feature_checked_(feature_checked) {
    SetupUi();
}

void ContainerStructurePage::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 4);
    layout->setSpacing(2);

    // 动态标题 + 启用分析开关 (同一行)
    {
        QWidget* titleRow = new QWidget(this);
        QHBoxLayout* trl = new QHBoxLayout(titleRow);
        trl->setContentsMargins(0, 0, 0, 0);
        title_label_ = new QLabel(tr("未加载文件"), titleRow);
        title_label_->setStyleSheet("font-size: 13px; font-weight: bold; color: #F0F6FC; padding: 2px 4px;");
        title_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        trl->addWidget(title_label_);
        trl->addStretch();
        QCheckBox* toggle = new QCheckBox(tr("启用分析（打开文件时解析）"), titleRow);
        toggle->setChecked(feature_checked_);
        toggle->setToolTip(tr("打开文件时解析容器结构，结果同时供「流媒体包」页使用，默认开启。"));
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_checked_ = checked;
            emit FeatureToggled(checked);
        });
        trl->addWidget(toggle);
        layout->addWidget(titleRow);
    }

    // 概要标签
    summary_label_ = new QLabel(tr("打开媒体文件后将自动分析容器结构"), this);
    summary_label_->setStyleSheet("font-size: 12px; color: #8B949E; padding: 2px 4px;");
    summary_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(summary_label_);

    // 分割布局: 左侧结构树 + 右侧详情
    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);

    // --- 左侧: 通用结构树 ---
    QWidget* leftPanel = new QWidget(splitter);
    QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    QLabel* treeLabel = new QLabel(tr("容器结构树:"), leftPanel);
    treeLabel->setStyleSheet("font-weight: bold; font-size: 11px; padding: 2px;");
    leftLayout->addWidget(treeLabel);
    tree_ = new QTreeWidget(leftPanel);
    tree_->setHeaderLabels({tr("名称"), tr("类型"), tr("大小"), tr("偏移"), tr("值/属性")});
    tree_->setColumnWidth(0, 160);
    tree_->setColumnWidth(1, 70);
    tree_->setColumnWidth(2, 80);
    tree_->setColumnWidth(3, 70);
    tree_->header()->setStretchLastSection(true);
    tree_->setAlternatingRowColors(true);
    tree_->setAnimated(true);
    tree_->setIndentation(16);
    tree_->setStyleSheet(
        "QTreeWidget { background-color: #0D1117; border: 1px solid #30363D; border-radius: 4px; color: #F0F6FC; font-size: 11px; }"
        "QTreeWidget::item { padding: 2px 4px; color: #F0F6FC; }"
        "QTreeWidget::item:hover { background-color: #161B22; }"
        "QTreeWidget::item:selected { background-color: rgba(88, 166, 255, 0.15); color: #F0F6FC; }"
        "QHeaderView::section { background-color: #161B22; color: #8B949E; padding: 3px; border: none; border-bottom: 1px solid #30363D; font-weight: bold; }"
    );
    leftLayout->addWidget(tree_);
    splitter->addWidget(leftPanel);

    // --- 右侧: 详情区 (QStackedWidget) ---
    QWidget* rightPanel = new QWidget(splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    detail_stack_ = new QStackedWidget(rightPanel);

    // ===== Page 0: 通用信息 (流信息 + 元数据) =====
    QWidget* generic_page = new QWidget();
    QVBoxLayout* generic_layout = new QVBoxLayout(generic_page);
    generic_layout->setContentsMargins(0, 0, 0, 0);

    QLabel* stream_label = new QLabel(tr("流信息:"), generic_page);
    stream_label->setStyleSheet("font-weight: bold; font-size: 11px; padding: 2px;");
    generic_layout->addWidget(stream_label);
    stream_table_ = new QTableWidget(0, 4, generic_page);
    stream_table_->setHorizontalHeaderLabels({tr("#"), tr("类型"), tr("编码"), tr("详情")});
    stream_table_->horizontalHeader()->setStretchLastSection(true);
    stream_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stream_table_->setAlternatingRowColors(true);
    generic_layout->addWidget(stream_table_);

    QLabel* meta_label = new QLabel(tr("元数据:"), generic_page);
    meta_label->setStyleSheet("font-weight: bold; font-size: 11px; padding: 2px;");
    generic_layout->addWidget(meta_label);
    metadata_table_ = new QTableWidget(0, 2, generic_page);
    metadata_table_->setHorizontalHeaderLabels({tr("键"), tr("值")});
    metadata_table_->horizontalHeader()->setStretchLastSection(true);
    metadata_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    metadata_table_->setAlternatingRowColors(true);
    generic_layout->addWidget(metadata_table_);

    detail_stack_->addWidget(generic_page);

    // ===== Page 1: MP4 专用详情表 =====
    mp4_detail_tabs_ = new QTabWidget();

    QWidget* stts_w = new QWidget();
    QVBoxLayout* stts_l = new QVBoxLayout(stts_w);
    stts_table_ = new QTableWidget(0, 3, stts_w);
    stts_table_->setHorizontalHeaderLabels({tr("索引"), tr("样本计数"), tr("样本增量")});
    stts_table_->horizontalHeader()->setStretchLastSection(true);
    stts_table_->setAlternatingRowColors(true);
    stts_l->addWidget(stts_table_);
    mp4_detail_tabs_->addTab(stts_w, "stts");

    QWidget* stco_w = new QWidget();
    QVBoxLayout* stco_l = new QVBoxLayout(stco_w);
    stco_table_ = new QTableWidget(0, 2, stco_w);
    stco_table_->setHorizontalHeaderLabels({tr("索引"), tr("Chunk 偏移")});
    stco_table_->horizontalHeader()->setStretchLastSection(true);
    stco_table_->setAlternatingRowColors(true);
    stco_l->addWidget(stco_table_);
    mp4_detail_tabs_->addTab(stco_w, "stco");

    QWidget* stsc_w = new QWidget();
    QVBoxLayout* stsc_l = new QVBoxLayout(stsc_w);
    stsc_table_ = new QTableWidget(0, 4, stsc_w);
    stsc_table_->setHorizontalHeaderLabels({tr("索引"), tr("首个Chunk"), tr("样本/Chunk"), tr("描述索引")});
    stsc_table_->horizontalHeader()->setStretchLastSection(true);
    stsc_table_->setAlternatingRowColors(true);
    stsc_l->addWidget(stsc_table_);
    mp4_detail_tabs_->addTab(stsc_w, "stsc");

    QWidget* stsz_w = new QWidget();
    QVBoxLayout* stsz_l = new QVBoxLayout(stsz_w);
    stsz_table_ = new QTableWidget(0, 3, stsz_w);
    stsz_table_->setHorizontalHeaderLabels({tr("索引"), tr("样本大小"), tr("备注")});
    stsz_table_->horizontalHeader()->setStretchLastSection(true);
    stsz_table_->setAlternatingRowColors(true);
    stsz_l->addWidget(stsz_table_);
    mp4_detail_tabs_->addTab(stsz_w, "stsz");

    QWidget* stss_w = new QWidget();
    QVBoxLayout* stss_l = new QVBoxLayout(stss_w);
    stss_table_ = new QTableWidget(0, 2, stss_w);
    stss_table_->setHorizontalHeaderLabels({tr("索引"), tr("关键帧样本号")});
    stss_table_->horizontalHeader()->setStretchLastSection(true);
    stss_table_->setAlternatingRowColors(true);
    stss_l->addWidget(stss_table_);
    mp4_detail_tabs_->addTab(stss_w, "stss");

    QWidget* co64_w = new QWidget();
    QVBoxLayout* co64_l = new QVBoxLayout(co64_w);
    co64_table_ = new QTableWidget(0, 2, co64_w);
    co64_table_->setHorizontalHeaderLabels({tr("索引"), tr("Chunk偏移(64位)")});
    co64_table_->horizontalHeader()->setStretchLastSection(true);
    co64_table_->setAlternatingRowColors(true);
    co64_l->addWidget(co64_table_);
    mp4_detail_tabs_->addTab(co64_w, "co64");

    // Sample Table 子页: 样本级展开 + 一致性问题 + 分片列表
    mp4_sample_widget_ = new Mp4SampleTableWidget();
    mp4_detail_tabs_->addTab(mp4_sample_widget_, tr("Sample Table"));

    detail_stack_->addWidget(mp4_detail_tabs_);

    // ===== Page 2: EBML 专用详情表 =====
    ebml_detail_tabs_ = new QTabWidget();

    ebml_track_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_track_table_->setColumnCount(10);
    ebml_track_table_->setHorizontalHeaderLabels({
        tr("#"), tr("类型"), tr("编码"), tr("CodecID"),
        tr("分辨率/采样率"), tr("声道"), tr("语言"),
        tr("帧率"), tr("默认"), tr("强制")
    });
    ebml_track_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_track_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_track_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_track_table_, tr("轨道"));

    ebml_cue_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_cue_table_->setColumnCount(4);
    ebml_cue_table_->setHorizontalHeaderLabels({tr("#"), tr("时间"), tr("轨道"), tr("Cluster偏移")});
    ebml_cue_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_cue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_cue_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_cue_table_, tr("Cues"));

    ebml_block_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_block_table_->setColumnCount(5);
    ebml_block_table_->setHorizontalHeaderLabels({tr("#"), tr("轨道"), tr("Timecode"), tr("关键帧"), tr("大小")});
    ebml_block_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_block_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_block_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_block_table_, tr("Block"));

    detail_stack_->addWidget(ebml_detail_tabs_);

    rightLayout->addWidget(detail_stack_);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setMinimumWidth(540);
    splitter->setChildrenCollapsible(false);

    layout->addWidget(splitter, 1);

    // 导出按钮
    export_button_ = new QPushButton(tr("导出结构数据"), this);
    export_button_->setToolTip(tr("将容器结构树和表格数据导出为文本文件"));
    layout->addWidget(export_button_, 0, Qt::AlignRight);


    connect(export_button_, &QPushButton::clicked, this, &ContainerStructurePage::OnExportContainerStructure);
    // 结构树点击 stts/ctts/stss/stco/stsz/elst/moof 等样本表相关 box 时联动到 Sample Table
    connect(tree_, &QTreeWidget::itemSelectionChanged,
            this, &ContainerStructurePage::OnContainerTreeSelectionChanged);
}


static void PopulateMp4BoxTablesInContainer(const model::Mp4BoxAnalysisResult& result,
                                       QTableWidget* stts_table, QTableWidget* stco_table,
                                       QTableWidget* stsc_table, QTableWidget* stsz_table,
                                       QTableWidget* co64_table, QTableWidget* stss_table) {
    constexpr int kMaxBoxTableRows = 2000;   // 每张表每个 track 最多显示的行数

    for (auto* table : {stts_table, stco_table, stsc_table, stsz_table, co64_table, stss_table}) {
        table->setUpdatesEnabled(false);
        table->setRowCount(0);
    }

    // 追加一个 track 段：标题行 + min(total, cap) 条 + 可选"已截断"提示行
    // kMaxBoxTableRows 必须显式捕获：std::min 走 const& 形参，属于 odr-use，
    // C++17 的"constexpr 变量免捕获"豁免不适用（MSVC 不检查，GCC/Clang 会报错）。
    auto append_section = [kMaxBoxTableRows](QTableWidget* table, const QString& header, int total,
                                             const std::function<void(int row, int index)>& fill_row) {
        if (total <= 0) return;
        const int shown = std::min(total, kMaxBoxTableRows);
        const bool truncated = total > shown;
        const int first = table->rowCount();
        table->setRowCount(first + 1 + shown + (truncated ? 1 : 0));

        auto* head = new QTableWidgetItem(header);
        QFont hf = head->font();
        hf.setBold(true);
        head->setFont(hf);
        table->setItem(first, 0, head);

        for (int i = 0; i < shown; ++i) {
            fill_row(first + 1 + i, i);
        }

        if (truncated) {
            auto* tail = new QTableWidgetItem(
                QObject::tr("… 仅显示前 %1 条（共 %2 条，完整数据请导出查看）").arg(shown).arg(total));
            QFont tf = tail->font();
            tf.setItalic(true);
            tail->setFont(tf);
            table->setItem(first + 1 + shown, 0, tail);
        }
    };
    auto cell = [](QTableWidget* table, int row, int col, const QString& text) {
        table->setItem(row, col, new QTableWidgetItem(text));
    };

    for (const auto& track : result.track_tables) {
        const QString header = QString("Track %1 (%2)").arg(track.track_id).arg(QString::fromStdString(track.track_type));

        append_section(stts_table, header, track.stts_entries.size(), [&](int row, int i) {
            cell(stts_table, row, 0, QString::number(i));
            cell(stts_table, row, 1, QString::number(track.stts_entries[i].sample_count));
            cell(stts_table, row, 2, QString::number(track.stts_entries[i].sample_delta));
        });
        append_section(stco_table, header, track.stco_entries.size(), [&](int row, int i) {
            cell(stco_table, row, 0, QString::number(i));
            cell(stco_table, row, 1, QString::number(track.stco_entries[i].chunk_offset));
        });
        append_section(co64_table, header, track.co64_entries.size(), [&](int row, int i) {
            cell(co64_table, row, 0, QString::number(i));
            cell(co64_table, row, 1, QString::number(track.co64_entries[i].chunk_offset));
        });
        append_section(stsc_table, header, track.stsc_entries.size(), [&](int row, int i) {
            cell(stsc_table, row, 0, QString::number(i));
            cell(stsc_table, row, 1, QString::number(track.stsc_entries[i].first_chunk));
            cell(stsc_table, row, 2, QString::number(track.stsc_entries[i].samples_per_chunk));
            cell(stsc_table, row, 3, QString::number(track.stsc_entries[i].sample_description_index));
        });
        if (!track.stsz_entries.empty()) {
            append_section(stsz_table, header, track.stsz_entries.size(), [&](int row, int i) {
                cell(stsz_table, row, 0, QString::number(i));
                cell(stsz_table, row, 1, QString::number(track.stsz_entries[i].sample_size));
            });
        } else if (track.stsz_default_size > 0) {
            append_section(stsz_table, header, 1, [&](int row, int) {
                cell(stsz_table, row, 0, "0");
                cell(stsz_table, row, 1, QString::number(track.stsz_default_size));
                cell(stsz_table, row, 2, QString("default x%1").arg(track.stsz_sample_count));
            });
        }
        append_section(stss_table, header, track.stss_entries.size(), [&](int row, int i) {
            cell(stss_table, row, 0, QString::number(i));
            cell(stss_table, row, 1, QString::number(track.stss_entries[i].sample_number));
        });
    }

    for (auto* table : {stts_table, stco_table, stsc_table, stsz_table, co64_table, stss_table}) {
        table->setUpdatesEnabled(true);
        table->resizeColumnsToContents();
    }
}

void ContainerStructurePage::SetError(const QString& message) {
    // 与 SetResult 里 !valid 那条分支同一套表现：不画结构树，只留一条原因。
    // 少了这条，分析器抛异常时页面会一直停在"打开媒体文件后将自动分析"，
    // 用户分不清是还没分析还是分析崩了。
    title_label_->setText(tr("文件结构"));
    title_label_->setStyleSheet("font-size: 13px; font-weight: bold; color: #8B949E; padding: 2px 4px;");

    summary_label_->setText(message.isEmpty() ? tr("无法分析该格式") : message);
    summary_label_->setStyleSheet("font-size: 12px; color: #8B949E; padding: 2px 4px;");
    // 一并清掉上一段分析留下的结构树 / 详情表，别让旧内容冒充新结果
    tree_->clear();
    detail_stack_->setCurrentIndex(0);
    result_ = model::ContainerStructureResult{};
}

void ContainerStructurePage::SetResult(const model::ContainerStructureResult& result) {
    VE_PERF("ContainerStructurePage::SetResult 总计");
    result_ = result;

    // 更新动态标题
    title_label_->setText(result.valid ? QString::fromStdString(result.format_name) + " " + tr("结构分析") : tr("文件结构"));
    title_label_->setStyleSheet(
        result.valid ? "font-size: 13px; font-weight: bold; color: #1a73e8; padding: 2px 4px;"
                     : "font-size: 13px; font-weight: bold; color: #8B949E; padding: 2px 4px;");

    if (!result.valid) {
        summary_label_->setText(result.error_message.empty()
                                    ? tr("无法分析该格式")
                                    : QString::fromStdString(result.error_message));
        summary_label_->setStyleSheet("font-size: 12px; color: #8B949E; padding: 2px 4px;");
        tree_->clear();
        detail_stack_->setCurrentIndex(0);
        return;
    }

    summary_label_->setText(QString::fromStdString(result.summary));
    summary_label_->setStyleSheet("font-size: 12px; color: #F0F6FC; font-weight: bold; padding: 2px 4px;");

    // 注：流媒体清单页的联动不再由本页做 —— AnalysisPanel::OnContainerStructureReady
    // 收到同一份结果后会自己调 UpdateStreamingUi()，避免页面之间互相知道对方。

    // 填充通用结构树
    tree_->clear();
    static const QColor kDepthColors[] = {
        QColor("#161B22"),  // level 0 — card background
        QColor("#0D1117"),  // level 1 — base background
        QColor("#131A21"),  // level 2
        QColor("#111920"),  // level 3
        QColor("#141C24"),  // level 4
        QColor("#10171D"),  // level 5
        QColor("#151D25"),  // level 6
    };

    std::function<void(QTreeWidgetItem*, const std::vector<model::ContainerElement>&)> addNodes;
    addNodes = [&](QTreeWidgetItem* parent, const std::vector<model::ContainerElement>& nodes) {
        for (const auto& n : nodes) {
            auto* item = new QTreeWidgetItem();
            item->setText(0, QString::fromStdString(n.name));
            item->setText(1, QString::fromStdString(n.type));
            item->setText(2, QString::number(n.size));
            item->setText(3, QString("0x%1").arg(n.offset, 0, 16));
            item->setText(4, QString::fromStdString(n.value));
            if (!n.extra.empty()) {
                item->setToolTip(0, QString::fromStdString(n.extra));
                item->setToolTip(4, QString::fromStdString(n.extra));
            }
            int d = n.depth;
            QColor bg = kDepthColors[d % 7];
            for (int c = 0; c < 5; ++c) item->setBackground(c, bg);
            if (parent) parent->addChild(item);
            else tree_->addTopLevelItem(item);
            addNodes(item, n.children);
        }
    };
    {
    VE_PERF("容器结构树填充");
    for (const auto& n : result.element_tree) {
        auto* top = new QTreeWidgetItem();
        top->setText(0, QString::fromStdString(n.name));
        top->setText(1, QString::fromStdString(n.type));
        top->setText(2, QString::number(n.size));
        top->setText(3, QString("0x%1").arg(n.offset, 0, 16));
        top->setText(4, QString::fromStdString(n.value));
        if (!n.extra.empty()) {
            top->setToolTip(0, QString::fromStdString(n.extra));
            top->setToolTip(4, QString::fromStdString(n.extra));
        }
        tree_->addTopLevelItem(top);
        addNodes(top, n.children);
    }
    }
    {
        VE_PERF("容器结构树 expandAll");
        tree_->expandAll();
    }

    // 填充通用信息表 (Page 0)
    // 流信息
    stream_table_->setRowCount(result.streams.size());
    for (int i = 0; i < result.streams.size(); ++i) {
        const auto& s = result.streams[i];
        stream_table_->setItem(i, 0, new QTableWidgetItem(QString::number(s.index)));
        stream_table_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(s.type)));
        stream_table_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(s.codec)));
        stream_table_->setItem(i, 3, new QTableWidgetItem(QString::fromStdString(s.details)));
    }
    // 元数据
    metadata_table_->setRowCount(result.metadata.size());
    int row = 0;
    for (auto it = result.metadata.begin(); it != result.metadata.end(); ++it, ++row) {
        metadata_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(it->first)));
        metadata_table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(it->second)));
    }

    // 根据格式切换详情页面
    using CF = model::ContainerFormat;
    switch (result.format) {
    case CF::MP4:
    case CF::MOV:
        detail_stack_->setCurrentIndex(1);  // MP4 详情页
        // 填充 MP4 详细表
        if (result.mp4_detail.valid) {
            PopulateMp4BoxTablesInContainer(result.mp4_detail, stts_table_, stco_table_,
                                            stsc_table_, stsz_table_, co64_table_, stss_table_);
        }
        // 填充 Sample Table 子页（样本级展开 + 一致性问题 + 分片）
        {
            VE_PERF("RebuildMp4SampleTable(主线程)");
            mp4_sample_widget_->SetSamples(result.mp4_samples);
        }
        break;
    case CF::MKV:
    case CF::WebM:
        detail_stack_->setCurrentIndex(2);  // EBML 详情页
        // 填充 EBML 详细表
        if (result.ebml_detail.valid) {
            const auto& er = result.ebml_detail;
            // 轨道表
            ebml_track_table_->setRowCount(er.tracks.size());
            for (int i = 0; i < er.tracks.size(); ++i) {
                const auto& t = er.tracks[i];
                auto set = [&](int col, const QString& v) {
                    ebml_track_table_->setItem(i, col, new QTableWidgetItem(v));
                };
                set(0, QString::number(t.track_number));
                set(1, QString::fromStdString(t.track_type_name));
                set(2, QString::fromStdString(t.codec_name));
                set(3, QString::fromStdString(t.codec_id));
                if (t.track_type == 1) {
                    set(4, QString("%1x%2").arg(t.pixel_width).arg(t.pixel_height));
                    set(5, "-"); set(7, t.frame_rate > 0 ? QString("%1 fps").arg(t.frame_rate, 0, 'f', 2) : "-");
                } else if (t.track_type == 2) {
                    set(4, t.sampling_frequency > 0 ? QString("%1 Hz").arg(t.sampling_frequency, 0, 'f', 0) : "-");
                    set(5, t.channels > 0 ? QString::number(t.channels) : "-"); set(7, "-");
                } else { set(4, "-"); set(5, "-"); set(7, "-"); }
                set(6, QString::fromStdString(t.language));
                set(8, t.default_track ? QString::fromUtf8("\u2713") : "");
                set(9, t.forced ? QString::fromUtf8("\u2713") : "");
            }
            ebml_detail_tabs_->setTabText(0, tr("轨道 (%1)").arg(er.tracks.size()));
            // Cues 表
            ebml_cue_table_->setRowCount(er.cues.size());
            for (int i = 0; i < er.cues.size(); ++i) {
                const auto& c = er.cues[i];
                ebml_cue_table_->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
                ebml_cue_table_->setItem(i, 1, new QTableWidgetItem(QString::number(c.time)));
                ebml_cue_table_->setItem(i, 2, new QTableWidgetItem(QString::number(c.track_number)));
                ebml_cue_table_->setItem(i, 3, new QTableWidgetItem(QString("0x%1").arg(c.cluster_position, 0, 16)));
            }
            ebml_detail_tabs_->setTabText(1, tr("Cues (%1)").arg(er.cues.size()));
            // Block 表
            int bn = qMin(static_cast<int>(er.blocks.size()), 500);
            ebml_block_table_->setRowCount(bn);
            for (int i = 0; i < bn; ++i) {
                const auto& b = er.blocks[i];
                ebml_block_table_->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
                ebml_block_table_->setItem(i, 1, new QTableWidgetItem(QString::number(b.track_number)));
                ebml_block_table_->setItem(i, 2, new QTableWidgetItem(QString::number(b.timecode)));
                ebml_block_table_->setItem(i, 3, new QTableWidgetItem(b.keyframe ? "KEY" : ""));
                ebml_block_table_->setItem(i, 4, new QTableWidgetItem(QString::number(b.data_size)));
            }
            ebml_detail_tabs_->setTabText(2, tr("Block (%1/%2)").arg(bn).arg(er.blocks.size()));
        }
        break;
    default:
        detail_stack_->setCurrentIndex(0);  // 通用信息页
        break;
    }
}






void ContainerStructurePage::OnContainerTreeSelectionChanged() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (!item) return;
    const QString box = item->text(0).trimmed();
    static const QStringList kSampleBoxes = {
        "stts", "ctts", "stss", "stsz", "stz2", "stsc", "stco", "co64",
        "elst", "moof", "traf", "tfhd", "tfdt", "trun", "mfhd"};
    if (!kSampleBoxes.contains(box)) return;

    const bool is_fragment_box = (box == "moof" || box == "traf" || box == "tfhd" ||
                                  box == "tfdt" || box == "trun" || box == "mfhd");

    // 顺着父链找 trak，用它在同级 trak 中的序号选对应轨道
    QTreeWidgetItem* node = item;
    while (node && node->text(0).trimmed() != "trak") node = node->parent();
    if (node && node->parent()) {
        int trak_index = -1, found = 0;
        QTreeWidgetItem* parent = node->parent();
        for (int i = 0; i < parent->childCount(); ++i) {
            if (parent->child(i)->text(0).trimmed() != "trak") continue;
            if (parent->child(i) == node) { trak_index = found; break; }
            ++found;
        }
        if (trak_index >= 0 && trak_index < mp4_sample_widget_->TrackCount()) {
            mp4_sample_widget_->SetTrackIndex(trak_index);
        }
    }

    detail_stack_->setCurrentIndex(1);        // MP4 详情页
    mp4_detail_tabs_->setCurrentWidget(mp4_sample_widget_);  // Sample Table 子页
    mp4_sample_widget_->SetFocusBox(box);
    if (is_fragment_box) {
        mp4_sample_widget_->ShowFragmentTab();
    }
}


void ContainerStructurePage::OnExportContainerStructure() {
    if (!result_.valid) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的结构数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出文件结构"),
        QString("videoeye_structure_%1.txt")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("文本文件 (*.txt);;所有文件 (*)"));
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "Format: " << QString::fromStdString(result_.format_name) << "\n";
    out << "File: " << QString::fromStdString(result_.file_path) << "\n";
    out << "Summary: " << QString::fromStdString(result_.summary) << "\n\n";

    std::function<void(const std::vector<model::ContainerElement>&, int)> printTree;
    printTree = [&](const std::vector<model::ContainerElement>& nodes, int d) {
        for (const auto& n : nodes) {
            QString indent(d * 2, ' ');
            out << indent << QString::fromStdString(n.name) << " [" << QString::fromStdString(n.type) << "]  size=" << n.size
                << "  offset=0x" << Qt::hex << n.offset << Qt::dec;
            if (!n.value.empty()) out << "  value=" << QString::fromStdString(n.value);
            out << "\n";
            printTree(n.children, d + 1);
        }
    };
    printTree(result_.element_tree, 0);

    if (!result_.metadata.empty()) {
        out << "\n--- Metadata ---\n";
        for (auto it = result_.metadata.begin();
             it != result_.metadata.end(); ++it) {
            out << QString::fromStdString(it->first) << " = "
                << QString::fromStdString(it->second) << "\n";
        }
    }

    // 流信息表
    if (!result_.streams.empty()) {
        out << "\n--- Streams ---\n";
        for (const auto& s : result_.streams) {
            out << "  #" << s.index << "  " << QString::fromStdString(s.type) << "  " << QString::fromStdString(s.codec);
            if (!s.details.empty()) out << "  (" << QString::fromStdString(s.details) << ")";
            out << "\n";
        }
    }

    // MP4/MOV 详细 Box 表
    const auto fmt = result_.format;
    if (fmt == model::ContainerFormat::MP4 || fmt == model::ContainerFormat::MOV) {
        const auto& mp4 = result_.mp4_detail;
        if (mp4.valid && !mp4.track_tables.empty()) {
            out << "\n========== MP4 Sample Tables ==========\n";
            for (const auto& t : mp4.track_tables) {
                out << "\n--- Track " << t.track_id << " (" << QString::fromStdString(t.track_type) << ") ---\n";

                if (!t.stts_entries.empty()) {
                    out << "  [stts] Time-to-Sample (" << t.stts_entries.size() << " entries)\n";
                    out << "    idx\tsample_count\tsample_delta\n";
                    int i = 0;
                    for (const auto& e : t.stts_entries)
                        out << "    " << i++ << "\t" << e.sample_count << "\t\t" << e.sample_delta << "\n";
                }
                if (!t.stsc_entries.empty()) {
                    out << "  [stsc] Sample-to-Chunk (" << t.stsc_entries.size() << " entries)\n";
                    out << "    idx\tfirst_chunk\tsamples_per_chunk\tsample_desc_idx\n";
                    int i = 0;
                    for (const auto& e : t.stsc_entries)
                        out << "    " << i++ << "\t" << e.first_chunk << "\t\t" << e.samples_per_chunk
                            << "\t\t\t" << e.sample_description_index << "\n";
                }
                if (!t.stco_entries.empty()) {
                    out << "  [stco] Chunk Offset (" << t.stco_entries.size() << " entries)\n";
                    out << "    idx\tchunk_offset\n";
                    int i = 0;
                    for (const auto& e : t.stco_entries)
                        out << "    " << i++ << "\t0x" << Qt::hex << e.chunk_offset << Qt::dec << "\n";
                }
                if (!t.co64_entries.empty()) {
                    out << "  [co64] 64-bit Chunk Offset (" << t.co64_entries.size() << " entries)\n";
                    out << "    idx\tchunk_offset\n";
                    int i = 0;
                    for (const auto& e : t.co64_entries)
                        out << "    " << i++ << "\t0x" << Qt::hex << e.chunk_offset << Qt::dec << "\n";
                }
                if (!t.stsz_entries.empty() || t.stsz_default_size > 0) {
                    out << "  [stsz] Sample Size (count=" << t.stsz_sample_count
                        << ", default=" << t.stsz_default_size << ")\n";
                    if (!t.stsz_entries.empty()) {
                        out << "    idx\tsample_size\n";
                        int i = 0;
                        for (const auto& e : t.stsz_entries)
                            out << "    " << i++ << "\t" << e.sample_size << "\n";
                    }
                }
                if (!t.stss_entries.empty()) {
                    out << "  [stss] Sync Sample / Keyframes (" << t.stss_entries.size() << " entries)\n";
                    out << "    idx\tsample_number\n";
                    int i = 0;
                    for (const auto& e : t.stss_entries)
                        out << "    " << i++ << "\t" << e.sample_number << "\n";
                }
            }
        }
    }

    // MKV/WebM 详细表 (轨道 / Cues / Blocks)
    if (fmt == model::ContainerFormat::MKV || fmt == model::ContainerFormat::WebM) {
        const auto& ebml = result_.ebml_detail;
        if (ebml.valid) {
            out << "\n========== EBML/Matroska Detail ==========\n";
            out << "DocType: " << QString::fromStdString(ebml.doc_type) << " v" << ebml.doc_type_version << "\n";
            out << "TimestampScale: " << ebml.timestamp_scale << " ns\n";
            out << "Duration: " << ebml.duration_seconds << " s\n";
            out << "Clusters: " << ebml.total_clusters
                << "  BlockGroups: " << ebml.total_blockgroups
                << "  SimpleBlocks: " << ebml.total_simpleblocks << "\n";

            if (!ebml.tracks.empty()) {
                out << "\n--- Tracks (" << ebml.tracks.size() << ") ---\n";
                for (const auto& tr : ebml.tracks) {
                    out << "  Track #" << tr.track_number << "  " << QString::fromStdString(tr.track_type_name)
                        << "  codec=" << QString::fromStdString(tr.codec_id);
                    if (!tr.codec_name.empty()) out << " (" << QString::fromStdString(tr.codec_name) << ")";
                    if (!tr.language.empty()) out << "  lang=" << QString::fromStdString(tr.language);
                    if (tr.pixel_width > 0)
                        out << "  " << tr.pixel_width << "x" << tr.pixel_height;
                    if (tr.sampling_frequency > 0)
                        out << "  " << tr.sampling_frequency << "Hz " << tr.channels << "ch";
                    out << "\n";
                }
            }
            if (!ebml.cues.empty()) {
                out << "\n--- Cues (" << ebml.cues.size() << " entries) ---\n";
                out << "    time\ttrack\tcluster_pos\tblock_no\n";
                for (const auto& c : ebml.cues)
                    out << "    " << c.time << "\t" << c.track_number
                        << "\t0x" << Qt::hex << c.cluster_position << Qt::dec
                        << "\t" << c.block_number << "\n";
            }
            if (!ebml.blocks.empty()) {
                out << "\n--- Blocks (" << ebml.blocks.size() << " entries, may be truncated) ---\n";
                out << "    track\ttimecode\tkeyframe\tsize\tcluster_off\tblock_off\n";
                for (const auto& b : ebml.blocks)
                    out << "    " << b.track_number << "\t" << b.timecode
                        << "\t\t" << (b.keyframe ? "K" : "-")
                        << "\t" << b.data_size
                        << "\t0x" << Qt::hex << b.cluster_offset
                        << "\t0x" << b.block_offset << Qt::dec << "\n";
            }
        }
    }

    QMessageBox::information(this, tr("导出成功"), tr("已导出到:\n%1").arg(filename));
}

void ContainerStructurePage::ApplySampleTable(const model::Mp4SampleTableResult& samples) {
    mp4_sample_widget_->SetSamples(samples);
}

} // namespace ui
} // namespace videoeye
