// ============================================================
// 预编译头（Qt Widgets）—— 只给依赖闭包里含 Qt6::Widgets 的目标用
//
// ui 层单 TU 均值 11.8s、core 7.1s，差值大头就是这批 Widgets 头。
// 这份头只有在真的链了 Qt6::Widgets 时才能用 —— 否则目标拿不到 Widgets 的
// include 目录，会在编译 PCH 这一步就失败，不会等到后期才炸。
// ============================================================
#pragma once

#include "videoeye_pch_qt.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>
