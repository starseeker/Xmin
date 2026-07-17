#ifndef XMIN_SERVER_XAUTHORITY_HPP
#define XMIN_SERVER_XAUTHORITY_HPP

#include "xmin/server/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace xmin::server {

Result<std::vector<std::uint8_t>>
load_xauthority_cookie(const std::string &path, unsigned display);

Result<std::vector<std::uint8_t>> secure_random_bytes(std::size_t size);

Result<void> write_xauthority_cookie(
    const std::string &path, const std::vector<std::uint8_t> &cookie);

} // namespace xmin::server

#endif
