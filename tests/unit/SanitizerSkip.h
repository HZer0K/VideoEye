#pragma once
// ThreadSanitizer 构建下跳过某条用例。
//
// 只用于**被测代码没问题、是 sanitizer 或 Qt 自身不干净**的场合，例如：
//   * QProcess 起子进程 —— TSan 不支持在多线程进程里 fork/exec，一 fork 就在
//     tsan_rtl.cpp:253 CHECK 失败，把测试进程直接带走；
//   * Qt6 的渐变光栅化会在 QThreadPool 里并发跑，Qt 自己的容器/分配器不是
//     TSan-clean 的（报头落在 libQt6Core 里，栈里没有我们自己的共享状态）。
//
// ⚠️ 我们自己代码里的数据竞争一律不许用这个宏盖过去 —— 那几处都在代码里修掉了
// （AvInterruptState::deadline_us、AudioOutput 的 open_thread_、QtWorkerOwner 的
// QThread 收尾），本宏不是它们的替代品。
//
// 为什么不用 TSAN_OPTIONS 的 suppressions 文件：那条路要按函数名精确匹配
// （实测 `race:QWaitCondition::~QWaitCondition` 匹配不上 Qt 的 C++ 符号），
// 而且压 QArrayData / operator delete / memmove 会把真正的竞争一起压掉，太宽。
// 显式跳过哪几条用例、为什么跳过，写在这里反而一眼可读。

#include <gtest/gtest.h>

#if defined(__SANITIZE_THREAD__)
#define SKIP_UNDER_TSAN(reason)                                                                                        \
    do {                                                                                                               \
        GTEST_SKIP() << "ThreadSanitizer 构建下跳过: " << (reason) << "（asan / 普通 Debug 构建仍会跑这条用例）";      \
    } while (0)
#else
#define SKIP_UNDER_TSAN(reason)                                                                                        \
    do {                                                                                                               \
    } while (0)
#endif
