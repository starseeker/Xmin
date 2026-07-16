#ifndef XMIN_NEXT_EXTENSION_REGISTRY_HPP
#define XMIN_NEXT_EXTENSION_REGISTRY_HPP

#include <array>
#include <cstdint>
#include <string_view>

namespace xmin::next {

struct ExtensionInfo {
    std::string_view name;
    std::uint8_t major_opcode;
    std::uint8_t major_version;
    std::uint16_t minor_version;
};

inline constexpr ExtensionInfo xtest_extension{"XTEST", 128, 2, 2};

inline constexpr std::array<ExtensionInfo, 1> extension_registry{{
    xtest_extension,
}};

[[nodiscard]] constexpr const ExtensionInfo *
extension_by_name(std::string_view name) noexcept
{
    for (const auto &extension : extension_registry) {
        if (extension.name == name)
            return &extension;
    }
    return nullptr;
}

[[nodiscard]] constexpr const ExtensionInfo *
extension_by_opcode(std::uint8_t opcode) noexcept
{
    for (const auto &extension : extension_registry) {
        if (extension.major_opcode == opcode)
            return &extension;
    }
    return nullptr;
}

} // namespace xmin::next

#endif
