// ============================================================
// 预编译头（Qt Core）
//
// 用于闭包里链了 Qt6::* 但不链 Qt6::Widgets 的目标。
// <QWidget> 之类需要 Widgets 的 include 目录，只有真的链了 Qt6::Widgets 的
// 目标才拿得到 —— 所以 Widgets 头单独放一份，别混进来，否则 Core-only 目标
// 会直接编译失败。
//
// 注意这里#include "videoeye_pch_std.h"用的是引号形式：CMake 生成的
// cmake_pch.hxx 里会依次 #include 我们给的 PCH 头，按文件自身位置去找相对
// 路径的兄弟头文件才是稳的（用 <> 会依赖 include 路径里有 cmake/pch/）。
// ============================================================
#pragma once

#include "videoeye_pch_std.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
