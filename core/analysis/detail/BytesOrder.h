#pragma once
//
// analysis 层内部的最小大端读取助手，替掉 Qt 的 qFromBigEndian<T>(const void*)。
//
// 为什么用逐字节拼而不是 memcpy + 字节交换
// ----------------------------------------
// 分析层读的是容器字节流（EBML / FLV / ASF 头部），字节序是格式规范的一部分，
// 逐字节拼是最直白也最不会错的做法；顺带避开对 uint8_t* 做 16/32/64 位对齐的假设。
//
// 对应 Qt：qFromBigEndian<uint16_t>(p) → LoadBE16(p)，其余同理。
//
#include <cstdint>

namespace videoeye {

inline uint16_t LoadBE16(const unsigned char* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t LoadBE24(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

inline uint32_t LoadBE32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t LoadBE64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

}  // namespace videoeye
