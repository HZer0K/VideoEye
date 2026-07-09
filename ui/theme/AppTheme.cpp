#include "ui/theme/AppTheme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>

namespace videoeye {
namespace ui {
namespace theme {

namespace font {

QFont uiFont(int pointSize, bool bold) {
    QFont font;
    // 尝试使用 Inter，回退到系统默认无衬线字体
    static int interId = QFontDatabase::addApplicationFont("Inter");
    if (interId >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(interId);
        if (!families.isEmpty()) {
            font.setFamily(families.first());
        }
    }
    font.setPointSize(pointSize > 0 ? pointSize : 9);
    font.setBold(bold);
    return font;
}

QFont monoFont(int pointSize, bool bold) {
    QFont font;
    // 尝试使用 JetBrains Mono，回退到系统等宽字体
    static int jbmId = QFontDatabase::addApplicationFont("JetBrains Mono");
    if (jbmId >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(jbmId);
        if (!families.isEmpty()) {
            font.setFamily(families.first());
        }
    } else {
        font.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    }
    font.setPointSize(pointSize > 0 ? pointSize : 9);
    font.setBold(bold);
    return font;
}

QString formatTime(int ms) {
    int seconds = ms / 1000;
    int minutes = seconds / 60;
    int hours = minutes / 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes % 60, 2, 10, QChar('0'))
        .arg(seconds % 60, 2, 10, QChar('0'));
}

} // namespace font

// ============================================================================
// QSS Stylesheet
// ============================================================================
QString getDarkStylesheet() {
    return QStringLiteral(R"(
/* ==== Global ==== */
QWidget {
    background-color: #0D1117;
    color: #F0F6FC;
    font-size: 13px;
}

QMainWindow {
    background-color: #0D1117;
}

/* ==== Top App Bar ==== */
QWidget#AppBar {
    background-color: #161B22;
    border-bottom: 1px solid #30363D;
}

QLabel#AppTitle {
    color: #F0F6FC;
    font-size: 15px;
    font-weight: 600;
}

QLabel#VersionBadge {
    color: #58A6FF;
    background-color: rgba(88, 166, 255, 0.15);
    border: 1px solid rgba(88, 166, 255, 0.3);
    border-radius: 4px;
    padding: 1px 6px;
    font-size: 11px;
    font-weight: 600;
}

QLabel#MediaPath {
    color: #8B949E;
    font-family: "JetBrains Mono", "Cascadia Code", Consolas, monospace;
    font-size: 12px;
}

/* ==== Buttons ==== */
QPushButton {
    background-color: #21262D;
    color: #F0F6FC;
    border: 1px solid #30363D;
    border-radius: 6px;
    padding: 6px 16px;
    font-size: 13px;
}

QPushButton:hover {
    background-color: #30363D;
    border-color: #8B949E;
}

QPushButton:pressed {
    background-color: #484F58;
}

QPushButton:disabled {
    color: #484F58;
    background-color: #161B22;
    border-color: #21262D;
}

QPushButton#PrimaryButton {
    background-color: rgba(88, 166, 255, 0.12);
    color: #58A6FF;
    border: 1px solid rgba(88, 166, 255, 0.4);
    border-radius: 6px;
}

QPushButton#PrimaryButton:hover {
    background-color: rgba(88, 166, 255, 0.2);
    border-color: #58A6FF;
}

QPushButton#PrimaryButton:pressed {
    background-color: rgba(88, 166, 255, 0.3);
}

/* Play button - circular accent */
QPushButton#playPauseButton {
    background-color: #58A6FF;
    color: #0D1117;
    border: none;
    border-radius: 16px;
    min-width: 32px;
    min-height: 32px;
    max-width: 32px;
    max-height: 32px;
    font-size: 14px;
}

QPushButton#playPauseButton:hover {
    background-color: #79B8FF;
}

QPushButton#playPauseButton:pressed {
    background-color: #4493F1;
}

QPushButton#stopButton {
    background-color: #21262D;
    color: #F0F6FC;
    border: 1px solid #30363D;
    border-radius: 6px;
    min-width: 32px;
    min-height: 32px;
    max-width: 32px;
    max-height: 32px;
}

/* ==== Sidebar (QListWidget) ==== */
QListWidget#Sidebar {
    background-color: #0D1117;
    border: none;
    border-right: 1px solid #30363D;
    outline: none;
    padding: 8px 0px;
    font-size: 13px;
}

QListWidget#Sidebar::item {
    color: #8B949E;
    padding: 8px 16px;
    border-left: 3px solid transparent;
}

QListWidget#Sidebar::item:hover {
    background-color: #161B22;
    color: #F0F6FC;
}

QListWidget#Sidebar::item:selected {
    background-color: rgba(88, 166, 255, 0.08);
    color: #F0F6FC;
    border-left: 3px solid #58A6FF;
}

QLabel#SidebarGroupLabel {
    color: #484F58;
    font-size: 11px;
    font-weight: 600;
    padding: 8px 16px 4px 16px;
    background-color: transparent;
}

/* ==== Control Bar ==== */
QWidget#ControlBar {
    background-color: #161B22;
    border-top: 1px solid #30363D;
}

QLabel#TimeLabel {
    color: #8B949E;
    font-family: "JetBrains Mono", "Cascadia Code", Consolas, monospace;
    font-size: 12px;
}

/* ==== Slider ==== */
QSlider::groove:horizontal {
    border: none;
    height: 4px;
    background: #30363D;
    border-radius: 2px;
}

QSlider::sub-page:horizontal {
    background: #58A6FF;
    border-radius: 2px;
}

QSlider::add-page:horizontal {
    background: #30363D;
    border-radius: 2px;
}

QSlider::handle:horizontal {
    background: #F0F6FC;
    border: none;
    border-radius: 6px;
    width: 12px;
    height: 12px;
    margin: -4px 0;
}

QSlider::handle:horizontal:hover {
    background: #58A6FF;
}

QSlider::handle:horizontal:pressed {
    background: #4493F1;
}

/* ==== QSplitter handle ==== */
QSplitter::handle {
    background-color: #30363D;
}

QSplitter::handle:horizontal {
    width: 1px;
}

QSplitter::handle:vertical {
    height: 1px;
}

/* ==== QGroupBox ==== */
QGroupBox {
    border: 1px solid #30363D;
    border-radius: 6px;
    margin-top: 12px;
    padding-top: 8px;
    color: #F0F6FC;
    font-weight: 600;
    font-size: 12px;
}

QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: #8B949E;
}

/* ==== Table ==== */
QTableWidget {
    background-color: #0D1117;
    alternate-background-color: #161B22;
    border: 1px solid #30363D;
    border-radius: 6px;
    gridline-color: #21262D;
    color: #F0F6FC;
    font-size: 12px;
}

QTableWidget::item {
    padding: 4px 8px;
    color: #F0F6FC;
}

QTableWidget::item:selected {
    background-color: rgba(88, 166, 255, 0.15);
    color: #F0F6FC;
}

QHeaderView::section {
    background-color: #161B22;
    color: #8B949E;
    border: none;
    border-right: 1px solid #30363D;
    border-bottom: 1px solid #30363D;
    padding: 6px 8px;
    font-size: 11px;
    font-weight: 600;
}

QTableCornerButton::section {
    background-color: #161B22;
    border: none;
    border-bottom: 1px solid #30363D;
}

/* ==== Tree Widget ==== */
QTreeWidget {
    background-color: #0D1117;
    border: 1px solid #30363D;
    border-radius: 6px;
    color: #F0F6FC;
    font-size: 12px;
    outline: none;
}

QTreeWidget::item {
    padding: 3px 0px;
    color: #F0F6FC;
}

QTreeWidget::item:hover {
    background-color: #161B22;
}

QTreeWidget::item:selected {
    background-color: rgba(88, 166, 255, 0.12);
    color: #F0F6FC;
}

QTreeWidget::branch:has-children:!has-siblings:closed,
QTreeWidget::branch:closed:has-children:has-siblings {
    image: none;
    border-image: none;
}

QTreeWidget::branch:open:has-children:!has-siblings,
QTreeWidget::branch:open:has-children:has-siblings {
    image: none;
    border-image: none;
}

QHeaderView::section {
    background-color: #161B22;
}

/* ==== QTextEdit ==== */
QTextEdit {
    background-color: #0D1117;
    color: #F0F6FC;
    border: 1px solid #30363D;
    border-radius: 6px;
    font-family: "JetBrains Mono", "Cascadia Code", Consolas, monospace;
    font-size: 12px;
    padding: 4px;
}

/* ==== QCheckBox ==== */
QCheckBox {
    color: #F0F6FC;
    font-size: 12px;
    spacing: 6px;
}

QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 3px;
    border: 1px solid #484F58;
    background-color: #0D1117;
}

QCheckBox::indicator:hover {
    border-color: #58A6FF;
}

QCheckBox::indicator:checked {
    background-color: #58A6FF;
    border-color: #58A6FF;
    image: none;
}

/* ==== ComboBox ==== */
QComboBox {
    background-color: #21262D;
    color: #F0F6FC;
    border: 1px solid #30363D;
    border-radius: 6px;
    padding: 4px 10px;
    font-size: 12px;
    min-height: 22px;
}

QComboBox:hover {
    border-color: #8B949E;
}

QComboBox::drop-down {
    border: none;
    width: 24px;
}

QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #8B949E;
    width: 0;
    height: 0;
    margin-right: 8px;
}

QComboBox QAbstractItemView {
    background-color: #161B22;
    color: #F0F6FC;
    border: 1px solid #30363D;
    border-radius: 4px;
    padding: 4px;
    selection-background-color: rgba(88, 166, 255, 0.15);
    outline: none;
}

/* ==== ScrollBar ==== */
QScrollBar:vertical {
    background: #0D1117;
    width: 10px;
    margin: 0;
    border: none;
}

QScrollBar::handle:vertical {
    background: #30363D;
    min-height: 30px;
    border-radius: 5px;
    margin: 2px;
}

QScrollBar::handle:vertical:hover {
    background: #484F58;
}

QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0;
}

QScrollBar::add-page:vertical,
QScrollBar::sub-page:vertical {
    background: none;
}

QScrollBar:horizontal {
    background: #0D1117;
    height: 10px;
    margin: 0;
    border: none;
}

QScrollBar::handle:horizontal {
    background: #30363D;
    min-width: 30px;
    border-radius: 5px;
    margin: 2px;
}

QScrollBar::handle:horizontal:hover {
    background: #484F58;
}

QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal {
    width: 0;
}

QScrollBar::add-page:horizontal,
QScrollBar::sub-page:horizontal {
    background: none;
}

/* ==== ScrollArea ==== */
QScrollArea {
    border: none;
    background-color: transparent;
}

/* ==== QTabWidget (for MP4/EBML detail sub-tabs) ==== */
QTabWidget::pane {
    border: 1px solid #30363D;
    border-radius: 6px;
    background-color: #0D1117;
    top: -1px;
}

QTabBar::tab {
    background-color: #161B22;
    color: #8B949E;
    border: 1px solid #30363D;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 6px 14px;
    margin-right: 2px;
    font-size: 12px;
}

QTabBar::tab:selected {
    background-color: #0D1117;
    color: #F0F6FC;
    border-bottom: 2px solid #58A6FF;
}

QTabBar::tab:hover:!selected {
    background-color: #21262D;
    color: #F0F6FC;
}

/* ==== QLabel ==== */
QLabel {
    color: #F0F6FC;
    background-color: transparent;
}

/* ==== Status Badge ==== */
QLabel#StatusBadge {
    color: #3FB950;
    background-color: rgba(63, 185, 80, 0.12);
    border: 1px solid rgba(63, 185, 80, 0.3);
    border-radius: 4px;
    padding: 2px 8px;
    font-size: 11px;
    font-weight: 600;
}

/* ==== QProgressBar ==== */
QProgressBar {
    background-color: #161B22;
    border: 1px solid #30363D;
    border-radius: 4px;
    text-align: center;
    color: #F0F6FC;
    font-size: 11px;
    height: 18px;
}

QProgressBar::chunk {
    background-color: #58A6FF;
    border-radius: 3px;
}

/* ==== QMenu ==== */
QMenu {
    background-color: #161B22;
    color: #F0F6FC;
    border: 1px solid #30363D;
    border-radius: 6px;
    padding: 4px;
}

QMenu::item {
    padding: 6px 24px;
    border-radius: 4px;
}

QMenu::item:selected {
    background-color: rgba(88, 166, 255, 0.15);
}

QMenu::separator {
    height: 1px;
    background-color: #30363D;
    margin: 4px 8px;
}

/* ==== QMenuBar ==== */
QMenuBar {
    background-color: #0D1117;
    color: #F0F6FC;
    border-bottom: 1px solid #30363D;
    padding: 2px;
}

QMenuBar::item {
    background-color: transparent;
    padding: 4px 12px;
    border-radius: 4px;
}

QMenuBar::item:selected {
    background-color: #21262D;
}

QStatusBar {
    background-color: #161B22;
    color: #8B949E;
    border-top: 1px solid #30363D;
    font-size: 12px;
}

/* ==== QToolTip ==== */
QToolTip {
    background-color: #161B22;
    color: #F0F6FC;
    border: 1px solid #30363D;
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 12px;
}

/* ==== QChartView background ==== */
QChartView {
    background-color: #0D1117;
    border: 1px solid #30363D;
    border-radius: 6px;
}
    )");
}

void applyDarkTheme() {
    // 设置 QPalette 作为基础
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(color::kBg));
    palette.setColor(QPalette::WindowText, QColor(color::kTextPrimary));
    palette.setColor(QPalette::Base, QColor(color::kBg));
    palette.setColor(QPalette::AlternateBase, QColor(color::kBgCard));
    palette.setColor(QPalette::Text, QColor(color::kTextPrimary));
    palette.setColor(QPalette::Button, QColor(color::kBgCard));
    palette.setColor(QPalette::ButtonText, QColor(color::kTextPrimary));
    palette.setColor(QPalette::BrightText, QColor(color::kDanger));
    palette.setColor(QPalette::Highlight, QColor(88, 166, 255, 40));
    palette.setColor(QPalette::HighlightedText, QColor(color::kTextPrimary));
    palette.setColor(QPalette::ToolTipBase, QColor(color::kBgCard));
    palette.setColor(QPalette::ToolTipText, QColor(color::kTextPrimary));

    if (qApp) {
        qApp->setPalette(palette);
        qApp->setStyleSheet(getDarkStylesheet());
    }
}

} // namespace theme
} // namespace ui
} // namespace videoeye
