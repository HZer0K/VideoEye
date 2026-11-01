// ============================================================
// 预编译头（纯标准库部分）—— 给不链 Qt 的模块用
//
// 为什么单独拆一份：仓库里有明确的"domain 层零 Qt"约定，core/domain、
// infrastructure 等模块编译时不应把 Qt 头拖进来；而 Qt 模块的 PCH 里带
// <QtWidgets> 会让"有没有偷偷依赖 Qt"这件事再也看不出来。所以按目标是否
// 真的依赖 Qt 来选 PCH 头，而不是一股脑共用一份。
//
// 只放"几乎每个 TU 都会用到、且自身很重"的头。放冷门头只会让每个 TU 都
// 被迫解析一遍用不上的声明，反而变慢。
//
// 启用方式：-DVIDEOEYE_USE_PCH=ON（默认关闭，收益需要实测确认）
// 详见顶层 CMakeLists.txt 里 VIDEOEYE_USE_PCH 的注释。
// ============================================================
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
