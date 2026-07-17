#ifndef XMIN_SERVER_PROPERTY_DATA_HPP
#define XMIN_SERVER_PROPERTY_DATA_HPP

#include "xmin/server/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace xmin::server {

std::optional<std::vector<std::uint8_t>>
canonical_property_data(const std::uint8_t *data, std::size_t size,
                        std::uint8_t format, ByteOrder order);

std::vector<std::uint8_t>
wire_property_data(const std::uint8_t *data, std::size_t size,
                   std::uint8_t format, ByteOrder order);

} // namespace xmin::server

#endif
