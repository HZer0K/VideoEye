#pragma once

// ISOBMFF box 序列化工具。
//
// 修复链路的其余部分只读盒；这里补上唯一缺失的"写"能力，用于：
//   * 从 in-band 参数集自建 ftyp+moov 初始化段（RepairInitSegmentBuilder）；
//   * 重建带样本表的 moov（RepairIndexRepair）。
//
// 只实现写 32 位 size 的普通盒（moov 不会超过 4 GiB）；写 largesize 只会
// 让下游解析器多一条分支，收益为零。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace repair {

class RepairBoxWriter {
public:
    const std::vector<uint8_t>& Bytes() const {
        return bytes_;
    }
    std::vector<uint8_t> TakeBytes() {
        return std::move(bytes_);
    }
    std::size_t Position() const {
        return bytes_.size();
    }

    void U8(uint8_t value);
    void U16(uint16_t value);
    void U24(uint32_t value);
    void U32(uint32_t value);
    void U64(uint64_t value);
    void FourCC(const char* type);
    void Zeros(std::size_t count);
    void Raw(const std::vector<uint8_t>& data);
    void Raw(const uint8_t* data, std::size_t size);

    // 开始一个盒并占位 4 字节 size；写完后用 EndBox 回填实际长度。
    std::size_t BeginBox(const char* type);
    // full box：在 box header 后追加 version(8) + flags(24)。
    std::size_t BeginFullBox(const char* type, uint8_t version, uint32_t flags);
    void EndBox(std::size_t start);

private:
    std::vector<uint8_t> bytes_;
};

} // namespace repair
} // namespace videoeye
