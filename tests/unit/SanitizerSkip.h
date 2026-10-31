#pragma once
// ThreadSanitizer 构建下跳过某条用例。
//
// 只用于**被测代码没问题、是 sanitizer 或 Qt 自身不干净**的场合，且只用于
// "把测试进程直接带走"的那种 —— 也就是说, 跳过它不是为了消红灯:
//
//   * QProcess 起子进程 —— TSan 不支持在多线程进程里 fork/exec，一 fork 就在
//     tsan_rtl.cpp:253 CHECK 失败，把测试进程直接带走；
//   * Qt6 的渐变光栅化会在 QThreadPool 里并发跑，Qt 自己的容器/分配器不是
//     TSan-clean 的（报头落在 libQt6Core 里，栈里没有我们自己的共享状态）。
//   * libtsan 自己的线程注册表撞上复用的 pthread 参数地址（
//     sanitizer_thread_registry.cpp:186 "live_.try_emplace(user_id...)"），
//     同样是一个 abort 结束整个二进制，见 test_cancel_and_robustness.cpp 末尾那条。
//
// ❗ 普通的数据竞争**不需要**在这里跳过: preset 用 exitcode=0 让它们不再判红,
// 再由 CI 的 TSan gate（scripts/sanitizer_gate.py）按"访问点落在谁的源码里"决定
// 是不是我们的问题。abort 类的跳不过去（进程已经没了）, 才落在本头文件。
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
