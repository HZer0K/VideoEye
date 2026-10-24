#pragma once

// MediaPlayer 的前向声明头。
//
// 有些 UI 头只需要 `player::MediaPlayer*`（MainWindow / PlayerPanel / 导出与打开
// 控制器的注入点），但它们所在目录不是 ui/player，不能按 R3 直接写
// `namespace videoeye::player`。直接 include MediaPlayer.h 会把 libav*、分析会话、
// 导出控制器全部拖进公共头闭包，导致 VideoEyeUi 也不得不 PUBLIC 依赖 FFmpeg。
// 这里只保留一个声明，完整定义留给 .cpp。

namespace videoeye {
namespace player {

class MediaPlayer;

} // namespace player
} // namespace videoeye
