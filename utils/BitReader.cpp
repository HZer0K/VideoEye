#include "utils/BitReader.h"

namespace videoeye {
namespace utils {

void BitReader::Reset(const uint8_t* data, size_t size) {
    ResetFromData(data, size);
}

void BitReader::ResetFromData(const uint8_t* data, size_t size) {
    data_ = data;
    total_bytes_ = size;
    byte_pos_ = 0;
    bit_pos_ = 0;
    error_ = false;
    error_message_.clear();
}

uint32_t BitReader::ReadBits(int count) {
    if (count < 0 || count > 32) {
        error_ = true;
        error_message_ = "Invalid bit count: " + std::to_string(count);
        return 0;
    }
    if (count == 0) {
        return 0;
    }

    const int available = AvailableBits();
    if (available < count) {
        error_ = true;
        error_message_ = "Not enough bits available: need " +
                         std::to_string(count) + ", have " +
                         std::to_string(available);
        return 0;
    }

    // MSB-first：每读一位把结果左移一位，新位补到最低位。
    uint32_t result = 0;
    for (int i = 0; i < count; ++i) {
        result = (result << 1) | static_cast<uint32_t>(ReadBitUnchecked());
    }
    return result;
}

bool BitReader::ReadBit() {
    if (AvailableBits() < 1) {
        error_ = true;
        error_message_ = "Not enough bits available";
        return false;
    }
    return ReadBitUnchecked();
}

bool BitReader::ReadBitUnchecked() {
    const bool bit = (data_[byte_pos_] >> (7 - bit_pos_)) & 1U;
    if (++bit_pos_ == 8) {
        bit_pos_ = 0;
        ++byte_pos_;
    }
    return bit;
}

uint32_t BitReader::ReadUWord() {
    return ReadUE();
}

uint32_t BitReader::ReadUE() {
    // ue(v)：leading_zeros 个 0，后跟一个 1（该 1 由下面的循环消费掉），
    // 再跟 leading_zeros 位后缀。值 = (1 << leading_zeros) - 1 + suffix。
    int leading_zeros = 0;
    while (true) {
        if (AvailableBits() < 1) {
            error_ = true;
            error_message_ = "Not enough bits available for ue(v) prefix";
            return 0;
        }
        if (ReadBitUnchecked()) {
            break;
        }
        if (++leading_zeros > 31) {
            error_ = true;
            error_message_ = "ue(v) prefix too long (>31 leading zeros)";
            return 0;
        }
    }

    if (leading_zeros == 0) {
        return 0;
    }

    const uint32_t suffix = ReadBits(leading_zeros);
    return suffix + (1U << leading_zeros) - 1U;
}

int32_t BitReader::ReadSE() {
    // se(v)：按 k = ue(v) 解码后做交错映射
    //   k:  0  1  2  3  4  5 ...
    //  se:  0  1 -1  2 -2  3 ...
    const uint32_t k = ReadUE();
    if (HasError()) {
        return 0;
    }
    if (k & 1U) {
        return static_cast<int32_t>((k + 1U) / 2U);
    }
    return -static_cast<int32_t>(k / 2U);
}

void BitReader::SkipBits(int count) {
    if (count <= 0) {
        return;
    }

    const int available = AvailableBits();
    if (available < count) {
        error_ = true;
        error_message_ = "Not enough bits to skip: need " +
                         std::to_string(count) + ", have " +
                         std::to_string(available);
        return;
    }

    bit_pos_ += count;
    byte_pos_ += static_cast<size_t>(bit_pos_ / 8);
    bit_pos_ %= 8;
}

void BitReader::AlignToByte() {
    if (bit_pos_ != 0) {
        bit_pos_ = 0;
        if (byte_pos_ < total_bytes_) {
            ++byte_pos_;
        }
    }
}

int BitReader::AvailableBits() const {
    if (!data_ || byte_pos_ > total_bytes_) {
        return 0;
    }
    return static_cast<int>((total_bytes_ - byte_pos_) * 8) - bit_pos_;
}

int BitReader::AvailableBytes() const {
    if (!data_ || byte_pos_ > total_bytes_) {
        return 0;
    }
    return static_cast<int>(total_bytes_ - byte_pos_);
}

std::vector<uint8_t> UnescapeRbsp(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);

    int zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        // emulation_prevention_three_byte：连续两个 0x00 后的 0x03 要丢弃
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }
    return out;
}

} // namespace utils
} // namespace videoeye
