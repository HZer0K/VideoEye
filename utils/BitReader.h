#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace videoeye {
namespace utils {

// ==========================================================================
// 位流读取器（Big-Endian / MSB-first）
//
// 用于解析码流中的变长字段（ue(v)/se(v)/u(n) 等）。
// 提供：
//   - ReadBits(n) : 读 n 位无符号整数（u(n)）
//   - ReadUE()    : 读无符号 Exp-Golomb（ue(v)）
//   - ReadSE()    : 读有符号 Exp-Golomb（se(v)）
//   - ReadBit()   : 读 1 位
//   - SkipBits(n) / AlignToByte()
//   - AvailableBits() / AvailableBytes()
//
// 位序：H.264 / HEVC / AV1 的 RBSP 与 OBU 都是 **MSB-first**，
// 即每个字节的最高位最先被读出。旧实现按 LSB-first 读取，会把
// profile/level/chroma 全部读错，这里已修正。
//
// 调用方需要先把 NAL payload 转成 RBSP（去掉 emulation prevention 的 0x03），
// 可直接使用 UnescapeRbsp()。
// ==========================================================================
class BitReader {
public:
    // 初始化位流
    void Reset(const uint8_t* data, size_t size);

    // 从现有数据重置
    void ResetFromData(const uint8_t* data, size_t size);

    // 基本读取操作
    uint32_t ReadBits(int count);           // u(n)，最多 32 位
    uint32_t ReadUWord();                   // 兼容旧名，等价于 ReadUE()
    uint32_t ReadUE();                      // ue(v)
    int32_t ReadSE();                       // se(v)
    bool ReadBit();                         // u(1)

    // 跳过操作
    void SkipBits(int count);               // 跳过指定位数
    void AlignToByte();                     // 对齐到下一个字节边界

    // 状态查询
    int AvailableBits() const;              // 剩余可用位数
    int AvailableBytes() const;             // 剩余可用字节数
    bool HasError() const { return error_; }
    std::string GetError() const { return error_message_; }

    // 当前位置（字节偏移）
    size_t CurrentBytePos() const { return byte_pos_; }

    // 当前字节内的位偏移（0 = 该字节最高位）
    int CurrentBitPos() const { return bit_pos_; }

    // 总长度
    size_t TotalBytes() const { return total_bytes_; }

private:
    // 不做越界检查的取位（调用方必须先确认 AvailableBits() 足够）
    bool ReadBitUnchecked();

    const uint8_t* data_ = nullptr;         // 输入数据指针
    size_t total_bytes_ = 0;                // 总字节数
    size_t byte_pos_ = 0;                   // 当前字节索引
    int bit_pos_ = 0;                       // 当前字节内位偏移，0 = MSB
    bool error_ = false;                    // 错误标志
    std::string error_message_;             // 错误信息
};

// 去掉 RBSP 中的 emulation_prevention_three_byte（00 00 03 中的 03）。
// 输入应指向 NAL payload（不含起始码；可含 1 字节 NAL header）。
std::vector<uint8_t> UnescapeRbsp(const uint8_t* data, size_t size);

// 辅助函数：将大端序字节数组转换为 uint32（用于 avcC/hvcC 配置记录）
inline uint32_t BytesToUint32BE(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

// 辅助函数：将大端序字节数组转换为 uint16（用于 avcC/hvcC 配置记录）
inline uint16_t BytesToUint16BE(const uint8_t* bytes) {
    return (static_cast<uint16_t>(bytes[0]) << 8) |
           static_cast<uint16_t>(bytes[1]);
}

// 辅助函数：读取长度前缀（length-prefix，通常为 1/2/4 字节）
inline uint32_t ReadLengthPrefix(const uint8_t*& ptr, size_t remaining, int prefix_bytes) {
    if (prefix_bytes == 1) {
        if (remaining < 1) return 0;
        uint32_t value = *ptr++;
        return value;
    } else if (prefix_bytes == 2) {
        if (remaining < 2) return 0;
        uint32_t value = (static_cast<uint32_t>(ptr[0]) << 8) | ptr[1];
        ptr += 2;
        return value;
    } else if (prefix_bytes == 4) {
        if (remaining < 4) return 0;
        uint32_t value = (static_cast<uint32_t>(ptr[0]) << 24) |
                        (static_cast<uint32_t>(ptr[1]) << 16) |
                        (static_cast<uint32_t>(ptr[2]) << 8) |
                        static_cast<uint32_t>(ptr[3]);
        ptr += 4;
        return value;
    }
    return 0;
}

} // namespace utils
} // namespace videoeye
