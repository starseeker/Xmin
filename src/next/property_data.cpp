#include "xmin/next/property_data.hpp"

namespace xmin::next {

std::optional<std::vector<std::uint8_t>>
canonical_property_data(const std::uint8_t *data, std::size_t size,
                        std::uint8_t format, ByteOrder order)
{
    if (format == 8)
        return std::vector<std::uint8_t>(data, data + size);

    WireReader reader(data, size, order);
    std::vector<std::uint8_t> canonical;
    canonical.reserve(size);
    while (reader.remaining() != 0) {
        if (format == 16) {
            const auto value = reader.u16();
            if (!value)
                return std::nullopt;
            canonical.push_back(static_cast<std::uint8_t>(*value));
            canonical.push_back(static_cast<std::uint8_t>(*value >> 8));
        }
        else {
            const auto value = reader.u32();
            if (!value)
                return std::nullopt;
            for (unsigned shift = 0; shift < 32; shift += 8)
                canonical.push_back(static_cast<std::uint8_t>(*value >> shift));
        }
    }
    return canonical;
}

std::vector<std::uint8_t>
wire_property_data(const std::uint8_t *data, std::size_t size,
                   std::uint8_t format, ByteOrder order)
{
    if (format == 8)
        return std::vector<std::uint8_t>(data, data + size);

    WireWriter writer(order);
    const std::size_t unit = format / 8;
    for (std::size_t offset = 0; offset < size; offset += unit) {
        if (format == 16) {
            const auto value = static_cast<std::uint16_t>(data[offset]) |
                (static_cast<std::uint16_t>(data[offset + 1]) << 8);
            writer.u16(value);
        }
        else {
            std::uint32_t value = 0;
            for (unsigned index = 0; index < 4; ++index) {
                value |= static_cast<std::uint32_t>(data[offset + index])
                    << (index * 8);
            }
            writer.u32(value);
        }
    }
    return writer.data();
}

} // namespace xmin::next
