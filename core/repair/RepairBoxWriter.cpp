#include "core/repair/RepairBoxWriter.h"

#include <cstring>

namespace videoeye {
namespace repair {

void RepairBoxWriter::U8(uint8_t value) {
    bytes_.push_back(value);
}

void RepairBoxWriter::U16(uint16_t value) {
    bytes_.push_back(static_cast<uint8_t>(value >> 8));
    bytes_.push_back(static_cast<uint8_t>(value));
}

void RepairBoxWriter::U24(uint32_t value) {
    bytes_.push_back(static_cast<uint8_t>(value >> 16));
    bytes_.push_back(static_cast<uint8_t>(value >> 8));
    bytes_.push_back(static_cast<uint8_t>(value));
}

void RepairBoxWriter::U32(uint32_t value) {
    bytes_.push_back(static_cast<uint8_t>(value >> 24));
    bytes_.push_back(static_cast<uint8_t>(value >> 16));
    bytes_.push_back(static_cast<uint8_t>(value >> 8));
    bytes_.push_back(static_cast<uint8_t>(value));
}

void RepairBoxWriter::U64(uint64_t value) {
    U32(static_cast<uint32_t>(value >> 32));
    U32(static_cast<uint32_t>(value));
}

void RepairBoxWriter::FourCC(const char* type) {
    bytes_.insert(bytes_.end(), type, type + 4);
}

void RepairBoxWriter::Zeros(std::size_t count) {
    bytes_.insert(bytes_.end(), count, 0);
}

void RepairBoxWriter::Raw(const std::vector<uint8_t>& data) {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
}

void RepairBoxWriter::Raw(const uint8_t* data, std::size_t size) {
    bytes_.insert(bytes_.end(), data, data + size);
}

std::size_t RepairBoxWriter::BeginBox(const char* type) {
    const std::size_t start = bytes_.size();
    U32(0);
    FourCC(type);
    return start;
}

std::size_t RepairBoxWriter::BeginFullBox(const char* type, uint8_t version, uint32_t flags) {
    const std::size_t start = BeginBox(type);
    U8(version);
    U24(flags);
    return start;
}

void RepairBoxWriter::EndBox(std::size_t start) {
    if (start + 4 > bytes_.size())
        return;
    const uint32_t size = static_cast<uint32_t>(bytes_.size() - start);
    bytes_[start] = static_cast<uint8_t>(size >> 24);
    bytes_[start + 1] = static_cast<uint8_t>(size >> 16);
    bytes_[start + 2] = static_cast<uint8_t>(size >> 8);
    bytes_[start + 3] = static_cast<uint8_t>(size);
}

} // namespace repair
} // namespace videoeye
