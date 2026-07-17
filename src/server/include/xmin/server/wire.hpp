#ifndef XMIN_SERVER_WIRE_HPP
#define XMIN_SERVER_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace xmin::server {

enum class ByteOrder : std::uint8_t {
    little,
    big,
};

ByteOrder host_byte_order() noexcept;

class WireReader {
public:
    WireReader(const std::uint8_t *data, std::size_t size, ByteOrder order)
        : data_(data), size_(size), order_(order)
    {
    }

    explicit WireReader(const std::vector<std::uint8_t> &bytes, ByteOrder order)
        : WireReader(bytes.data(), bytes.size(), order)
    {
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return size_ - offset_;
    }

    std::optional<std::uint8_t> u8() noexcept;
    std::optional<std::uint16_t> u16() noexcept;
    std::optional<std::uint32_t> u32() noexcept;
    std::optional<std::uint64_t> u64() noexcept;
    bool skip(std::size_t count) noexcept;

private:
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t offset_ = 0;
    ByteOrder order_;
};

class WireWriter {
public:
    explicit WireWriter(ByteOrder order) : order_(order) {}

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void i16(std::int16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void bytes(const std::vector<std::uint8_t> &value);
    void bytes(std::string_view value);
    void pad(std::size_t count);
    void pad_to_four();

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t> &data() const noexcept
    {
        return bytes_;
    }

private:
    ByteOrder order_;
    std::vector<std::uint8_t> bytes_;
};

} // namespace xmin::server

#endif
