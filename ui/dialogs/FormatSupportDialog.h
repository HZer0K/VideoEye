#pragma once

// 帮助菜单 ->「支持的格式与编解码器」。
//
// 内容全部来自 FfmpegSupportProbe（现场问本进程链接的 av* 库），这个对话框只负责
// 排版、搜索、过滤与导出 —— 数据怎么来的一行都不写在这里，方便单独测。

#include <QDialog>
#include <QString>

#include "ui/dialogs/FfmpegSupportProbe.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTextEdit;

namespace videoeye {
namespace ui {

class FormatSupportDialog : public QDialog {
    Q_OBJECT

public:
    explicit FormatSupportDialog(QWidget* parent = nullptr);

    // 页签序号是硬契约: 概览 / 封装格式 / 编解码器 / 协议 / 硬件加速。
    // 单测与 CSV 导出都按这个序号取表（概览页没有表，返回 nullptr）。
    enum TabIndex {
        kOverview = 0,
        kContainers,
        kCodecs,
        kProtocols,
        kHwAccel,
        kTabCount
    };

    QTableWidget* TableAt(int tab_index) const;
    /// 当前页签里实际显示（未被搜索/过滤隐藏）的行数
    int VisibleRowCount() const;

private slots:
    void OnTabChanged(int index);
    void OnFilterChanged();
    void OnSearchChanged();
    void OnExportCsv();
    void OnCopyBuildInfo();

private:
    // 行能力位，与过滤下拉的选项一一对应（"能读" / "能写"）。
    // 过滤时不回查数据：能力在建表时就存进第 0 列的 UserRole，改过滤条件只动行隐藏。
    enum RowCap {
        kCapNone = 0,
        kCapRead = 1,   // 解复用 / 解码 / 协议可读 / 有解码器支持
        kCapWrite = 2   // 复用 / 编码 / 协议可写
    };

    void SetupUi();
    QWidget* BuildOverviewTab();
    QTableWidget* MakeTable(const QString& object_name, const QStringList& headers);
    void FillContainers();
    void FillCodecs();
    void FillProtocols();
    void FillHwAccels();

    void RefreshFilterOptions(int tab_index);
    void ApplyFilter();
    void UpdateSummary();
    QTableWidget* CurrentTable() const;

    FfmpegSupportSnapshot snapshot_;

    QTabWidget* tabs_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    QComboBox* filter_combo_ = nullptr;
    QLabel* summary_label_ = nullptr;

    QTableWidget* containers_table_ = nullptr;
    QTableWidget* codecs_table_ = nullptr;
    QTableWidget* protocols_table_ = nullptr;
    QTableWidget* hwaccel_table_ = nullptr;

    QTextEdit* build_text_ = nullptr;
};

} // namespace ui
} // namespace videoeye
