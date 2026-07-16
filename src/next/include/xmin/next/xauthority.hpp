#ifndef XMIN_NEXT_XAUTHORITY_HPP
#define XMIN_NEXT_XAUTHORITY_HPP

#include "xmin/next/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace xmin::next {

Result<std::vector<std::uint8_t>>
load_xauthority_cookie(const std::string &path, unsigned display);

} // namespace xmin::next

#endif
