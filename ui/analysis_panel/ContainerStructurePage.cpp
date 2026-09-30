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
        QCheckBox* toggle = new QCheckBox(tr("启用分析"), titleRow);
        toggle->setChecked(feature_checked_);
        toggle->setToolTip(tr("启用或禁用容器结构分析，关闭可跳过打开文件时的结构解析"));
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
    mp4_sample_sub_ = new QWidget();
    SetupMp4SampleTableSubPage(mp4_sample_sub_);
    mp4_detail_tabs_->addTab(mp4_sample_sub_, tr("Sample Table"));

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

void ContainerStructurePage::SetupMp4SampleTableSubPage(QWidget* parent) {
    QVBoxLayout* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    // 顶部: 轨道选择 + 联动提示
    QHBoxLayout* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(new QLabel(tr("轨道:"), parent));
    mp4_sample_track_combo_ = new QComboBox(parent);
    mp4_sample_track_combo_->setMinimumWidth(240);
    mp4_sample_track_combo_->setToolTip(tr("选择要展开样本的轨道（视频/音频/字幕等）"));
    top->addWidget(mp4_sample_track_combo_);
    top->addStretch();
    mp4_sample_focus_label_ = new QLabel(tr("点击左侧结构树的 stts/ctts/stss/stco/stsz 可联动本表"), parent);
    mp4_sample_focus_label_->setStyleSheet("font-size: 11px; color: #8B949E;");
    top->addWidget(mp4_sample_focus_label_);
    export_mp4_sample_button_ = new QPushButton(tr("导出样本 CSV"), parent);
    export_mp4_sample_button_->setToolTip(tr("把当前轨道的样本表导出为 CSV（含偏移/DTS/PTS/大小/关键帧）"));
    top->addWidget(export_mp4_sample_button_);
    layout->addLayout(top);

    mp4_sample_summary_label_ = new QLabel(tr("样本表：未分析"), parent);
    mp4_sample_summary_label_->setStyleSheet("font-size: 11px; color: #8B949E; padding: 2px;");
    mp4_sample_summary_label_->setWordWrap(true);
    layout->addWidget(mp4_sample_summary_label_);

    mp4_sample_table_ = new QTableWidget(0, 9, parent);
    mp4_sample_table_->setHorizontalHeaderLabels({
        tr("#"), tr("偏移"), tr("DTS(s)"), tr("PTS(s)"), tr("ΔCTS(ms)"),
        tr("时长(ms)"), tr("大小(B)"), tr("Chunk"), tr("关键帧")});
    mp4_sample_table_->horizontalHeader()->setStretchLastSection(true);
    mp4_sample_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mp4_sample_table_->setAlternatingRowColors(true);
    mp4_sample_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mp4_sample_table_->setStyleSheet(
        "QTableWidget { background-color: #0D1117; alternate-background-color: #12181F; color: #F0F6FC; }"
        "QHeaderView::section { background-color: #161B22; color: #8B949E; padding: 3px; border: none; }");

    // 底部: 一致性问题 + 分片列表
    QTabWidget* bottom = new QTabWidget(parent);
    mp4_issue_table_ = new QTableWidget(0, 5, bottom);
    mp4_issue_table_->setHorizontalHeaderLabels({tr("级别"), tr("位置"), tr("问题"), tr("说明"), tr("建议")});
    mp4_issue_table_->horizontalHeader()->setStretchLastSection(true);
    mp4_issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mp4_issue_table_->setAlternatingRowColors(true);
    mp4_issue_table_->verticalHeader()->setVisible(false);
    bottom->addTab(mp4_issue_table_, tr("一致性问题"));

    mp4_fragment_table_ = new QTableWidget(0, 8, bottom);
    mp4_fragment_table_->setHorizontalHeaderLabels({
        tr("#"), tr("序号"), tr("Track"), tr("偏移"), tr("tfdt"),
        tr("时长"), tr("样本数"), tr("字节数")});
    mp4_fragment_table_->horizontalHeader()->setStretchLastSection(true);
    mp4_fragment_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mp4_fragment_table_->setAlternatingRowColors(true);
    mp4_fragment_table_->verticalHeader()->setVisible(false);
    bottom->addTab(mp4_fragment_table_, tr("分片 (moof)"));

    QSplitter* splitter = new QSplitter(Qt::Vertical, parent);
    splitter->addWidget(mp4_sample_table_);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

    connect(mp4_sample_track_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ContainerStructurePage::OnMp4SampleTrackChanged);
    connect(export_mp4_sample_button_, &QPushButton::clicked,
            this, &ContainerStructurePage::OnExportMp4SampleCsv);
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
        const QString header = QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type);

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
        if (!track.stsz_entries.isEmpty()) {
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

void ContainerStructurePage::SetResult(const model::ContainerStructureResult& result) {
    VE_PERF("ContainerStructurePage::SetResult 总计");
    result_ = result;

    // 更新动态标题
    title_label_->setText(result.valid ? result.format_name + " " + tr("结构分析") : tr("文件结构"));
    title_label_->setStyleSheet(
        result.valid ? "font-size: 13px; font-weight: bold; color: #1a73e8; padding: 2px 4px;"
                     : "font-size: 13px; font-weight: bold; color: #8B949E; padding: 2px 4px;");

    if (!result.valid) {
        summary_label_->setText(result.error_message.isEmpty() ? tr("无法分析该格式") : result.error_message);
        summary_label_->setStyleSheet("font-size: 12px; color: #8B949E; padding: 2px 4px;");
        tree_->clear();
        detail_stack_->setCurrentIndex(0);
        return;
    }

    summary_label_->setText(result.summary);
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

    std::function<void(QTreeWidgetItem*, const QVector<model::ContainerElement>&)> addNodes;
    addNodes = [&](QTreeWidgetItem* parent, const QVector<model::ContainerElement>& nodes) {
        for (const auto& n : nodes) {
            auto* item = new QTreeWidgetItem();
            item->setText(0, n.name);
            item->setText(1, n.type);
            item->setText(2, QString::number(n.size));
            item->setText(3, QString("0x%1").arg(n.offset, 0, 16));
            item->setText(4, n.value);
            if (!n.extra.isEmpty()) {
                item->setToolTip(0, n.extra);
                item->setToolTip(4, n.extra);
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
        top->setText(0, n.name);
        top->setText(1, n.type);
        top->setText(2, QString::number(n.size));
        top->setText(3, QString("0x%1").arg(n.offset, 0, 16));
        top->setText(4, n.value);
        if (!n.extra.isEmpty()) {
            top->setToolTip(0, n.extra);
            top->setToolTip(4, n.extra);
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
        stream_table_->setItem(i, 1, new QTableWidgetItem(s.type));
        stream_table_->setItem(i, 2, new QTableWidgetItem(s.codec));
        stream_table_->setItem(i, 3, new QTableWidgetItem(s.details));
    }
    // 元数据
    metadata_table_->setRowCount(result.metadata.size());
    int row = 0;
    for (auto it = result.metadata.begin(); it != result.metadata.end(); ++it, ++row) {
        metadata_table_->setItem(row, 0, new QTableWidgetItem(it.key()));
        metadata_table_->setItem(row, 1, new QTableWidgetItem(it.value()));
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
        mp4_samples_ = result.mp4_samples;
        mp4_sample_focus_box_.clear();
        {
            const QSignalBlocker blocker(mp4_sample_track_combo_);
            mp4_sample_track_combo_->clear();
            for (const auto& t : mp4_samples_.tracks) {
                QString label = QString("Track %1 (%2").arg(t.track_id)
                                    .arg(QString::fromStdString(t.type));
                if (!t.codec.empty()) {
                    label += " / " + QString::fromStdString(t.codec);
                }
                label += QString(") · %1 样本").arg(t.stsz_sample_count);
                mp4_sample_track_combo_->addItem(label, static_cast<quint32>(t.track_id));
            }
        }
        UpdateMp4SampleSummary();
        {
            VE_PERF("RebuildMp4SampleTable(主线程)");
            RebuildMp4SampleTable();
        }
        RebuildMp4IssueTable();
        RebuildMp4FragmentTable();
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
                set(1, t.track_type_name);
                set(2, t.codec_name);
                set(3, t.codec_id);
                if (t.track_type == 1) {
                    set(4, QString("%1x%2").arg(t.pixel_width).arg(t.pixel_height));
                    set(5, "-"); set(7, t.frame_rate > 0 ? QString("%1 fps").arg(t.frame_rate, 0, 'f', 2) : "-");
                } else if (t.track_type == 2) {
                    set(4, t.sampling_frequency > 0 ? QString("%1 Hz").arg(t.sampling_frequency, 0, 'f', 0) : "-");
                    set(5, t.channels > 0 ? QString::number(t.channels) : "-"); set(7, "-");
                } else { set(4, "-"); set(5, "-"); set(7, "-"); }
                set(6, t.language);
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
            int bn = qMin(er.blocks.size(), 500);
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

void ContainerStructurePage::UpdateMp4SampleSummary() {
    const auto& r = mp4_samples_;
    if (!r.valid) {
        mp4_sample_summary_label_->setText(
            tr("样本表：未分析（文件不是 MP4/MOV，或容器解析失败）"));
        mp4_sample_summary_label_->setStyleSheet("font-size: 11px; color: #8B949E; padding: 2px;");
        return;
    }

    QString order;
    for (const auto& box : r.top_level_order) {
        if (!order.isEmpty()) order += " → ";
        order += QString::fromStdString(box);
    }
    const int errors = r.CountIssues(model::IssueSeverity::Error) +
                       r.CountIssues(model::IssueSeverity::Critical);
    const int warnings = r.CountIssues(model::IssueSeverity::Warning);
    const int infos = r.CountIssues(model::IssueSeverity::Info);

    mp4_sample_summary_label_->setText(
        QString("文件 %1 字节 | 顶层: %2 | moov: %3 | faststart: %4 | 分片: %5 (sidx %6 / styp %7) | "
                "问题: 错误 %8 / 警告 %9 / 提示 %10")
            .arg(QString::number(static_cast<quint64>(r.file_size)))
            .arg(order.isEmpty() ? "-" : order)
            .arg(r.moov_size > 0 ? QString("0x%1 (%2 B)")
                                       .arg(static_cast<quint64>(r.moov_offset), 0, 16)
                                       .arg(QString::number(static_cast<quint64>(r.moov_size)))
                                 : tr("无"))
            .arg(r.IsFastStart() ? tr("是") : tr("否"))
            .arg(r.fragments.size())
            .arg(r.sidx_count)
            .arg(r.styp_count)
            .arg(errors)
            .arg(warnings)
            .arg(infos));
    mp4_sample_summary_label_->setStyleSheet(
        errors > 0 ? "font-size: 11px; color: #F85149; padding: 2px;"
                   : (warnings > 0 ? "font-size: 11px; color: #D29922; padding: 2px;"
                                   : "font-size: 11px; color: #3FB950; padding: 2px;"));
}

void ContainerStructurePage::RebuildMp4SampleTable() {
    mp4_sample_table_->setRowCount(0);
    if (!mp4_samples_.valid) return;

    const int track_index = mp4_sample_track_combo_->currentIndex();
    if (track_index < 0 || track_index >= static_cast<int>(mp4_samples_.tracks.size())) return;
    const model::Mp4TrackSampleTable& track = mp4_samples_.tracks[track_index];
    if (track.samples.empty()) return;

    // 结构树点了 stss 时只看关键帧
    const bool keyframe_only = (mp4_sample_focus_box_ == "stss");
    // 结构树联动的列高亮
    QVector<int> focus_cols;
    if (mp4_sample_focus_box_ == "stts")       focus_cols = {2, 5};
    else if (mp4_sample_focus_box_ == "ctts")  focus_cols = {3, 4};
    else if (mp4_sample_focus_box_ == "stsz")  focus_cols = {6};
    else if (mp4_sample_focus_box_ == "stsc")  focus_cols = {7};
    else if (mp4_sample_focus_box_ == "stco" ||
             mp4_sample_focus_box_ == "co64")  focus_cols = {1, 7};
    else if (mp4_sample_focus_box_ == "stss")  focus_cols = {8};

    constexpr int kMaxSampleRows = 5000;  // UI 安全上限（样本本身可按 options 截断）

    // 先算出实际要显示多少行，一次性 setRowCount：
    // 逐行 insertRow 在几千行时是 O(n^2)，实测 5000 行要 200ms+。
    int visible = 0;
    for (const auto& s : track.samples) {
        if (keyframe_only && !s.keyframe) continue;
        if (visible >= kMaxSampleRows) break;
        ++visible;
    }
    TableBatch table(mp4_sample_table_);
    table.SetRowCount(visible);

    int row = 0;
    for (const auto& s : track.samples) {
        if (keyframe_only && !s.keyframe) continue;
        if (row >= kMaxSampleRows) break;

        auto set = [&](int col, const QString& text) {
            return table.SetText(row, col, text);
        };
        set(0, QString::number(s.index));
        set(1, QString("0x%1").arg(static_cast<quint64>(s.offset), 0, 16));
        set(2, QString::number(s.DtsSeconds(track.media_timescale), 'f', 4));
        set(3, QString::number(s.CtsSeconds(track.media_timescale), 'f', 4));
        set(4, track.media_timescale > 0
                   ? QString::number(static_cast<double>(s.cts_delta) * 1000.0 /
                                         track.media_timescale, 'f', 2)
                   : QString::number(s.cts_delta));
        set(5, track.media_timescale > 0
                   ? QString::number(static_cast<double>(s.duration) * 1000.0 /
                                         track.media_timescale, 'f', 2)
                   : QString::number(s.duration));
        set(6, QString::number(static_cast<quint64>(s.size)));
        set(7, QString("%1.%2").arg(s.chunk_index).arg(s.index_in_chunk));
        set(8, s.keyframe ? QString::fromUtf8("\u2713") : QString());

        // 异常行着色: 红=会导致读不到数据/解码错乱，黄=可能影响兼容与体验
        QStringList reasons;
        bool error_row = false, warn_row = false;
        auto note = [&reasons](const char* text) { reasons << QString::fromUtf8(text); };
        if (s.HasFlag(model::Mp4SampleFlags::kOffsetOutOfRange)) {
            error_row = true; note("偏移越过文件末尾");
        }
        if (s.HasFlag(model::Mp4SampleFlags::kDtsNotMonotonic)) {
            error_row = true; note("DTS 回退");
        }
        if (s.HasFlag(model::Mp4SampleFlags::kNegativeCts)) { warn_row = true; note("合成时间为负"); }
        if (s.HasFlag(model::Mp4SampleFlags::kZeroSize)) { warn_row = true; note("大小为 0"); }
        if (s.HasFlag(model::Mp4SampleFlags::kZeroDuration)) { warn_row = true; note("时长为 0"); }
        if (s.HasFlag(model::Mp4SampleFlags::kChunkDiscontinuity)) {
            warn_row = true; note("chunk 内偏移不连续");
        }

        if (error_row || warn_row) {
            const QColor bg = error_row ? QColor("#5C1F1F") : QColor("#5C4A1F");
            for (int c = 0; c < mp4_sample_table_->columnCount(); ++c) {
                auto* item = mp4_sample_table_->item(row, c);
                if (item) item->setBackground(bg);
            }
            for (int c = 0; c < mp4_sample_table_->columnCount(); ++c) {
                auto* item = mp4_sample_table_->item(row, c);
                if (item) item->setToolTip(reasons.join(" / "));
            }
        }
        // 结构树联动的高亮列
        for (int col : focus_cols) {
            auto* item = mp4_sample_table_->item(row, col);
            if (item) item->setForeground(QColor("#58A6FF"));
        }
        ++row;
    }

    // resizeColumnsToContents 要逐行算文本宽度 (5000 行 × 9 列 ≈ 300ms),
    // 行数多时改用固定列宽, 把主线程还给用户。
    if (mp4_sample_table_->rowCount() <= 1000) {
        mp4_sample_table_->resizeColumnsToContents();
    } else {
        static const int kColumnWidths[] = {70, 110, 90, 90, 80, 80, 80, 90, 60};
        for (int c = 0; c < mp4_sample_table_->columnCount() && c < 9; ++c) {
            mp4_sample_table_->setColumnWidth(c, kColumnWidths[c]);
        }
    }
}

void ContainerStructurePage::RebuildMp4IssueTable() {
    mp4_issue_table_->setRowCount(0);
    if (!mp4_samples_.valid) return;

    int row = 0;
    for (const auto& issue : mp4_samples_.issues) {
        mp4_issue_table_->insertRow(row);
        auto set = [&](int col, const QString& text) {
            mp4_issue_table_->setItem(row, col, new QTableWidgetItem(text));
        };
        set(0, QString::fromUtf8(model::ToString(issue.severity)));

        QString where = tr("文件级");
        if (issue.track_id >= 0) {
            where = QString("Track %1").arg(issue.track_id);
        } else if (issue.fragment_index >= 0) {
            where = QString("分片 #%1").arg(issue.fragment_index);
        }
        if (issue.has_sample_index) where += QString(" @样本#%1").arg(issue.sample_index);
        if (issue.occurrence_count > 1) where += QString(" ×%1").arg(issue.occurrence_count);
        set(1, where);

        set(2, QString::fromStdString(issue.title));
        set(3, QString::fromStdString(issue.detail));
        set(4, QString::fromStdString(issue.suggestion));

        const QColor fg = (issue.severity == model::IssueSeverity::Error ||
                           issue.severity == model::IssueSeverity::Critical)
                              ? QColor("#F85149")
                              : (issue.severity == model::IssueSeverity::Warning
                                     ? QColor("#D29922")
                                     : QColor("#8B949E"));
        for (int c = 0; c < mp4_issue_table_->columnCount(); ++c) {
            if (auto* item = mp4_issue_table_->item(row, c)) item->setForeground(fg);
        }
        ++row;
    }
    mp4_issue_table_->resizeColumnsToContents();
}

void ContainerStructurePage::RebuildMp4FragmentTable() {
    mp4_fragment_table_->setRowCount(0);
    if (!mp4_samples_.valid || mp4_samples_.fragments.empty()) return;

    int row = 0;
    for (const auto& f : mp4_samples_.fragments) {
        mp4_fragment_table_->insertRow(row);
        auto set = [&](int col, const QString& text) {
            mp4_fragment_table_->setItem(row, col, new QTableWidgetItem(text));
        };
        const model::Mp4TrackSampleTable* track = mp4_samples_.FindTrack(f.track_id);
        const uint32_t ts = track ? track->media_timescale : mp4_samples_.movie_timescale;
        set(0, QString::number(f.index));
        set(1, QString::number(f.sequence_number));
        set(2, QString::number(f.track_id));
        set(3, QString("0x%1").arg(static_cast<quint64>(f.offset), 0, 16));
        set(4, f.has_tfdt ? QString::number(static_cast<quint64>(f.base_media_decode_time))
                          : tr("缺失"));
        set(5, ts > 0 ? QString("%1 (%2 s)")
                            .arg(QString::number(static_cast<quint64>(f.duration)))
                            .arg(QString::number(static_cast<double>(f.duration) / ts, 'f', 3))
                      : QString::number(static_cast<quint64>(f.duration)));
        set(6, QString::number(f.sample_count));
        set(7, QString::number(static_cast<quint64>(f.total_size)));
        if (!f.has_tfdt || (!f.base_data_offset_present && !f.default_base_is_moof &&
                            !f.trun_data_offset_present)) {
            for (int c = 0; c < mp4_fragment_table_->columnCount(); ++c) {
                if (auto* item = mp4_fragment_table_->item(row, c)) {
                    item->setBackground(QColor("#5C4A1F"));
                }
            }
        }
        ++row;
    }
    mp4_fragment_table_->resizeColumnsToContents();
}

void ContainerStructurePage::OnMp4SampleTrackChanged(int) {
    RebuildMp4SampleTable();
}

void ContainerStructurePage::OnContainerTreeSelectionChanged() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (!item) return;
    const QString box = item->text(0).trimmed();
    static const QStringList kSampleBoxes = {
        "stts", "ctts", "stss", "stsz", "stz2", "stsc", "stco", "co64",
        "elst", "moof", "traf", "tfhd", "tfdt", "trun", "mfhd"};
    if (!kSampleBoxes.contains(box)) return;

    mp4_sample_focus_box_ = box;

    // moof 系列: 直接跳到分片表
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
        if (trak_index >= 0 && trak_index < mp4_sample_track_combo_->count()) {
            mp4_sample_track_combo_->setCurrentIndex(trak_index);
        }
    }

    detail_stack_->setCurrentIndex(1);        // MP4 详情页
    mp4_detail_tabs_->setCurrentWidget(mp4_sample_sub_);  // Sample Table 子页
    if (is_fragment_box) {
        // 底部切到分片表
        if (auto* bottom = qobject_cast<QTabWidget*>(mp4_fragment_table_->parentWidget())) {
            bottom->setCurrentWidget(mp4_fragment_table_);
        }
        mp4_sample_focus_label_->setText(tr("已联动：%1（分片信息见下方「分片 (moof)」）").arg(box));
    } else {
        mp4_sample_focus_label_->setText(tr("已联动：%1（高亮关联列）").arg(box));
    }
    RebuildMp4SampleTable();
}

void ContainerStructurePage::OnExportMp4SampleCsv() {
    if (!mp4_samples_.valid || mp4_samples_.tracks.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 MP4 样本数据。"));
        return;
    }
    const int track_index = mp4_sample_track_combo_->currentIndex();
    if (track_index < 0 || track_index >= static_cast<int>(mp4_samples_.tracks.size())) return;
    const model::Mp4TrackSampleTable& track = mp4_samples_.tracks[track_index];

    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出 MP4 样本表 CSV"),
        QString("videoeye_mp4_samples_track%1_%2.csv")
            .arg(track.track_id)
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("CSV 文件 (*.csv);;所有文件 (*)"));
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "index,offset,dts,pts,cts_delta,duration,size,chunk,index_in_chunk,keyframe,flags\n";
    const double ts = static_cast<double>(track.media_timescale);
    for (const auto& s : track.samples) {
        out << s.index << ','
            << s.offset << ','
            << QString::number(ts > 0 ? s.dts / ts : 0.0, 'f', 6) << ','
            << QString::number(ts > 0 ? s.cts / ts : 0.0, 'f', 6) << ','
            << s.cts_delta << ','
            << s.duration << ','
            << s.size << ','
            << s.chunk_index << ','
            << s.index_in_chunk << ','
            << (s.keyframe ? 1 : 0) << ','
            << s.flags << '\n';
    }
    file.close();
    QMessageBox::information(this, tr("导出完成"),
                             tr("已导出 %1 个样本到:\n%2").arg(track.samples.size()).arg(filename));
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
    out << "Format: " << result_.format_name << "\n";
    out << "File: " << result_.file_path << "\n";
    out << "Summary: " << result_.summary << "\n\n";

    std::function<void(const QVector<model::ContainerElement>&, int)> printTree;
    printTree = [&](const QVector<model::ContainerElement>& nodes, int d) {
        for (const auto& n : nodes) {
            QString indent(d * 2, ' ');
            out << indent << n.name << " [" << n.type << "]  size=" << n.size
                << "  offset=0x" << Qt::hex << n.offset << Qt::dec;
            if (!n.value.isEmpty()) out << "  value=" << n.value;
            out << "\n";
            printTree(n.children, d + 1);
        }
    };
    printTree(result_.element_tree, 0);

    if (!result_.metadata.isEmpty()) {
        out << "\n--- Metadata ---\n";
        for (auto it = result_.metadata.begin();
             it != result_.metadata.end(); ++it) {
            out << it.key() << " = " << it.value() << "\n";
        }
    }

    // 流信息表
    if (!result_.streams.isEmpty()) {
        out << "\n--- Streams ---\n";
        for (const auto& s : result_.streams) {
            out << "  #" << s.index << "  " << s.type << "  " << s.codec;
            if (!s.details.isEmpty()) out << "  (" << s.details << ")";
            out << "\n";
        }
    }

    // MP4/MOV 详细 Box 表
    const auto fmt = result_.format;
    if (fmt == model::ContainerFormat::MP4 || fmt == model::ContainerFormat::MOV) {
        const auto& mp4 = result_.mp4_detail;
        if (mp4.valid && !mp4.track_tables.isEmpty()) {
            out << "\n========== MP4 Sample Tables ==========\n";
            for (const auto& t : mp4.track_tables) {
                out << "\n--- Track " << t.track_id << " (" << t.track_type << ") ---\n";

                if (!t.stts_entries.isEmpty()) {
                    out << "  [stts] Time-to-Sample (" << t.stts_entries.size() << " entries)\n";
                    out << "    idx\tsample_count\tsample_delta\n";
                    int i = 0;
                    for (const auto& e : t.stts_entries)
                        out << "    " << i++ << "\t" << e.sample_count << "\t\t" << e.sample_delta << "\n";
                }
                if (!t.stsc_entries.isEmpty()) {
                    out << "  [stsc] Sample-to-Chunk (" << t.stsc_entries.size() << " entries)\n";
                    out << "    idx\tfirst_chunk\tsamples_per_chunk\tsample_desc_idx\n";
                    int i = 0;
                    for (const auto& e : t.stsc_entries)
                        out << "    " << i++ << "\t" << e.first_chunk << "\t\t" << e.samples_per_chunk
                            << "\t\t\t" << e.sample_description_index << "\n";
                }
                if (!t.stco_entries.isEmpty()) {
                    out << "  [stco] Chunk Offset (" << t.stco_entries.size() << " entries)\n";
                    out << "    idx\tchunk_offset\n";
                    int i = 0;
                    for (const auto& e : t.stco_entries)
                        out << "    " << i++ << "\t0x" << Qt::hex << e.chunk_offset << Qt::dec << "\n";
                }
                if (!t.co64_entries.isEmpty()) {
                    out << "  [co64] 64-bit Chunk Offset (" << t.co64_entries.size() << " entries)\n";
                    out << "    idx\tchunk_offset\n";
                    int i = 0;
                    for (const auto& e : t.co64_entries)
                        out << "    " << i++ << "\t0x" << Qt::hex << e.chunk_offset << Qt::dec << "\n";
                }
                if (!t.stsz_entries.isEmpty() || t.stsz_default_size > 0) {
                    out << "  [stsz] Sample Size (count=" << t.stsz_sample_count
                        << ", default=" << t.stsz_default_size << ")\n";
                    if (!t.stsz_entries.isEmpty()) {
                        out << "    idx\tsample_size\n";
                        int i = 0;
                        for (const auto& e : t.stsz_entries)
                            out << "    " << i++ << "\t" << e.sample_size << "\n";
                    }
                }
                if (!t.stss_entries.isEmpty()) {
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
            out << "DocType: " << ebml.doc_type << " v" << ebml.doc_type_version << "\n";
            out << "TimestampScale: " << ebml.timestamp_scale << " ns\n";
            out << "Duration: " << ebml.duration_seconds << " s\n";
            out << "Clusters: " << ebml.total_clusters
                << "  BlockGroups: " << ebml.total_blockgroups
                << "  SimpleBlocks: " << ebml.total_simpleblocks << "\n";

            if (!ebml.tracks.isEmpty()) {
                out << "\n--- Tracks (" << ebml.tracks.size() << ") ---\n";
                for (const auto& tr : ebml.tracks) {
                    out << "  Track #" << tr.track_number << "  " << tr.track_type_name
                        << "  codec=" << tr.codec_id;
                    if (!tr.codec_name.isEmpty()) out << " (" << tr.codec_name << ")";
                    if (!tr.language.isEmpty()) out << "  lang=" << tr.language;
                    if (tr.pixel_width > 0)
                        out << "  " << tr.pixel_width << "x" << tr.pixel_height;
                    if (tr.sampling_frequency > 0)
                        out << "  " << tr.sampling_frequency << "Hz " << tr.channels << "ch";
                    out << "\n";
                }
            }
            if (!ebml.cues.isEmpty()) {
                out << "\n--- Cues (" << ebml.cues.size() << " entries) ---\n";
                out << "    time\ttrack\tcluster_pos\tblock_no\n";
                for (const auto& c : ebml.cues)
                    out << "    " << c.time << "\t" << c.track_number
                        << "\t0x" << Qt::hex << c.cluster_position << Qt::dec
                        << "\t" << c.block_number << "\n";
            }
            if (!ebml.blocks.isEmpty()) {
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
    mp4_samples_ = samples;
    UpdateMp4SampleSummary();
    RebuildMp4SampleTable();
    RebuildMp4IssueTable();
    RebuildMp4FragmentTable();
}

} // namespace ui
} // namespace videoeye
