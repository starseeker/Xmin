#ifndef XMIN_NEXT_XAUTHORITY_HPP
#define XMIN_NEXT_XAUTHORITY_HPP

#include "xmin/next/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace xmin::next {

Result<std::vector<std::uint8_t>>
load_xauthority_cookie(const std::string &path, unsigned display);

Result<std::vector<std::uint8_t>> secure_random_bytes(std::size_t size);

Result<void> write_xauthority_cookie(
    const std::string &path, const std::vector<std::uint8_t> &cookie);

} // namespace xmin::next

#endif
