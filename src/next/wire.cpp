#include "xmin/next/wire.hpp"

namespace xmin::next {

ByteOrder
host_byte_order() noexcept
{
    const std::uint16_t value = 1;
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    return bytes[0] == 1 ? ByteOrder::little : ByteOrder::big;
}

std::optional<std::uint8_t>
WireReader::u8() noexcept
{
    if (remaining() < 1)
        return std::nullopt;
    return data_[offset_++];
}

std::optional<std::uint16_t>
WireReader::u16() noexcept
{
    if (remaining() < 2)
        return std::nullopt;
    const auto first = static_cast<std::uint16_t>(data_[offset_]);
    const auto second = static_cast<std::uint16_t>(data_[offset_ + 1]);
    offset_ += 2;
    if (order_ == ByteOrder::little)
        return static_cast<std::uint16_t>(first | (second << 8));
    return static_cast<std::uint16_t>((first << 8) | second);
}

std::optional<std::uint32_t>
WireReader::u32() noexcept
{
    if (remaining() < 4)
        return std::nullopt;
    std::uint32_t result = 0;
    if (order_ == ByteOrder::little) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            result |= static_cast<std::uint32_t>(data_[offset_++]) << shift;
    }
    else {
        for (unsigned shift = 24;; shift -= 8) {
            result |= static_cast<std::uint32_t>(data_[offset_++]) << shift;
            if (shift == 0)
                break;
        }
    }
    return result;
}

std::optional<std::uint64_t>
WireReader::u64() noexcept
{
    if (remaining() < 8)
        return std::nullopt;
    std::uint64_t result = 0;
    if (order_ == ByteOrder::little) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            result |= static_cast<std::uint64_t>(data_[offset_++]) << shift;
    }
    else {
        for (unsigned shift = 56;; shift -= 8) {
            result |= static_cast<std::uint64_t>(data_[offset_++]) << shift;
            if (shift == 0)
                break;
        }
    }
    return result;
}

bool
WireReader::skip(std::size_t count) noexcept
{
    if (remaining() < count)
        return false;
    offset_ += count;
    return true;
}

void
WireWriter::u8(std::uint8_t value)
{
    bytes_.push_back(value);
}

void
WireWriter::u16(std::uint16_t value)
{
    if (order_ == ByteOrder::little) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    else {
        u8(static_cast<std::uint8_t>(value >> 8));
        u8(static_cast<std::uint8_t>(value));
    }
}

void
WireWriter::i16(std::int16_t value)
{
    u16(static_cast<std::uint16_t>(value));
}

void
WireWriter::u32(std::uint32_t value)
{
    if (order_ == ByteOrder::little) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    else {
        for (unsigned shift = 24;; shift -= 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
            if (shift == 0)
                break;
        }
    }
}

void
WireWriter::u64(std::uint64_t value)
{
    if (order_ == ByteOrder::little) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    else {
        for (unsigned shift = 56;; shift -= 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
            if (shift == 0)
                break;
        }
    }
}

void
WireWriter::bytes(const std::vector<std::uint8_t> &value)
{
    bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void
WireWriter::bytes(std::string_view value)
{
    for (const char byte : value)
        u8(static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
}

void
WireWriter::pad(std::size_t count)
{
    bytes_.insert(bytes_.end(), count, std::uint8_t{0});
}

void
WireWriter::pad_to_four()
{
    pad((4 - (size() & 3)) & 3);
}

} // namespace xmin::next
