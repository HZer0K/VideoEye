#pragma once

#include <QString>
#include <QColor>
#include <QFont>

namespace videoeye {
namespace ui {
namespace theme {

// ============================================================================
// GitHub Dark Color Palette
// ============================================================================
namespace color {

// Backgrounds
constexpr const char* kBg          = "#0D1117";  // 最底层画布
constexpr const char* kBgCard      = "#161B22";  // 卡片/面板
constexpr const char* kBgSurface   = "#21262D";  // 略亮的卡片背景
constexpr const char* kBgOverlay   = "#01060E";  // 视频区背景

// Borders
constexpr const char* kBorder      = "#30363D";  // 所有 1px 边框
constexpr const char* kBorderLight = "#484F58";  // 弱化边框

// Text
constexpr const char* kTextPrimary   = "#F0F6FC";  // 主文本
constexpr const char* kTextSecondary = "#8B949E";  // 次文本
constexpr const char* kTextMuted     = "#484F58";  // 弱化文本

// Accents
constexpr const char* kAccent       = "#58A6FF";  // 主强调色 (蓝)
constexpr const char* kSuccess      = "#3FB950";  // 成功色 (绿)
constexpr const char* kDanger       = "#F85149";  // 警示色 (红)
constexpr const char* kWarning      = "#D29922";  // 警告色 (橙)

} // namespace color

// ============================================================================
// Font Helpers
// ============================================================================
namespace font {

/// 获取界面字体 (Inter 或系统默认)
QFont uiFont(int pointSize = 9, bool bold = false);

/// 获取等宽字体 (JetBrains Mono 或系统等宽字体)
QFont monoFont(int pointSize = 9, bool bold = false);

/// 格式化时间为 HH:MM:SS
QString formatTime(int ms);

} // namespace font

// ============================================================================
// QSS Stylesheet
// ============================================================================

/// 生成完整的深色主题 QSS 样式表
QString getDarkStylesheet();

/// 应用深色主题到 QApplication
void applyDarkTheme();

} // namespace theme
} // namespace ui
} // namespace videoeye
