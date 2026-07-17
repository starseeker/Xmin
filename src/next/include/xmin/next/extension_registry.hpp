#ifndef XMIN_NEXT_EXTENSION_REGISTRY_HPP
#define XMIN_NEXT_EXTENSION_REGISTRY_HPP

#include <array>
#include <cstdint>
#include <string_view>

namespace xmin::next {

enum class ExtensionKind : std::uint8_t {
    big_requests,
    xc_misc,
    generic_event,
    xtest,
    shape,
    sync,
    render,
    xfixes,
    randr,
    damage,
    composite,
    present,
    xkb,
};

struct ExtensionInfo {
    std::string_view name;
    std::uint8_t major_opcode;
    std::uint8_t first_event;
    std::uint8_t first_error;
    std::uint8_t major_version;
    std::uint16_t minor_version;
    ExtensionKind kind;
};

inline constexpr ExtensionInfo big_requests_extension{
    "BIG-REQUESTS", 128, 0, 0, 0, 0, ExtensionKind::big_requests};
inline constexpr ExtensionInfo xc_misc_extension{
    "XC-MISC", 129, 0, 0, 1, 1, ExtensionKind::xc_misc};
inline constexpr ExtensionInfo generic_event_extension{
    "Generic Event Extension", 130, 0, 0, 1, 0,
    ExtensionKind::generic_event};
inline constexpr ExtensionInfo xtest_extension{
    "XTEST", 131, 0, 0, 2, 2, ExtensionKind::xtest};
inline constexpr ExtensionInfo shape_extension{
    "SHAPE", 132, 64, 0, 1, 1, ExtensionKind::shape};
inline constexpr ExtensionInfo sync_extension{
    "SYNC", 133, 65, 128, 3, 1, ExtensionKind::sync};
inline constexpr ExtensionInfo render_extension{
    "RENDER", 134, 0, 131, 0, 11, ExtensionKind::render};
inline constexpr ExtensionInfo xfixes_extension{
    "XFIXES", 135, 67, 136, 6, 0, ExtensionKind::xfixes};
inline constexpr ExtensionInfo randr_extension{
    "RANDR", 136, 69, 138, 1, 6, ExtensionKind::randr};
inline constexpr ExtensionInfo damage_extension{
    "DAMAGE", 137, 71, 142, 1, 1, ExtensionKind::damage};
inline constexpr ExtensionInfo composite_extension{
    "Composite", 138, 0, 0, 0, 4, ExtensionKind::composite};
inline constexpr ExtensionInfo present_extension{
    "Present", 139, 0, 0, 1, 4, ExtensionKind::present};
inline constexpr ExtensionInfo xkb_extension{
    "XKEYBOARD", 140, 72, 143, 1, 0, ExtensionKind::xkb};

inline constexpr std::array<ExtensionInfo, 13> extension_registry{{
    big_requests_extension,
    xc_misc_extension,
    generic_event_extension,
    xtest_extension,
    shape_extension,
    sync_extension,
    render_extension,
    xfixes_extension,
    randr_extension,
    damage_extension,
    composite_extension,
    present_extension,
    xkb_extension,
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
