#ifndef XMIN_NEXT_SERVER_STATE_HPP
#define XMIN_NEXT_SERVER_STATE_HPP

#include "xmin/next/atom_table.hpp"
#include "xmin/next/resource_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace xmin::next {

constexpr std::uint32_t root_window_id = 1;
constexpr std::uint32_t default_colormap_id = 2;
constexpr std::uint32_t root_visual_id = 3;
constexpr std::uint32_t client_resource_mask = 0x001fffff;
constexpr std::size_t maximum_client_resources = 4096;
constexpr std::size_t maximum_server_resources = 65536;

enum class WindowClass : std::uint16_t {
    input_output = 1,
    input_only = 2,
};

struct WindowRecord {
    std::uint32_t id = 0;
    std::uint32_t parent = 0;
    std::vector<std::uint32_t> children;
    std::unordered_map<std::uint32_t, std::uint32_t> event_masks;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t border_width = 0;
    std::uint16_t do_not_propagate_mask = 0;
    std::uint32_t visual = root_visual_id;
    std::uint32_t colormap = default_colormap_id;
    std::uint32_t background_pixel = 0;
    std::uint32_t border_pixel = 0;
    std::uint32_t backing_planes = 0xffffffffU;
    std::uint32_t backing_pixel = 0;
    std::uint8_t depth = 24;
    WindowClass window_class = WindowClass::input_output;
    std::uint8_t bit_gravity = 0;
    std::uint8_t window_gravity = 1;
    std::uint8_t backing_store = 0;
    bool override_redirect = false;
    bool save_under = false;
    bool mapped = false;
};

class ServerState {
public:
    ServerState(std::uint16_t width, std::uint16_t height);

    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }

    [[nodiscard]] AtomTable &atoms() noexcept { return atoms_; }
    [[nodiscard]] const AtomTable &atoms() const noexcept { return atoms_; }

    [[nodiscard]] WindowRecord *window(std::uint32_t id);
    [[nodiscard]] const WindowRecord *window(std::uint32_t id) const;
    [[nodiscard]] bool resource_exists(std::uint32_t id) const;
    [[nodiscard]] bool valid_client_resource(std::uint32_t id,
                                             std::uint32_t base) const;
    [[nodiscard]] bool resource_limit_reached(std::uint32_t owner) const;
    [[nodiscard]] bool add_window(WindowRecord window, std::uint32_t owner);
    void destroy_window(std::uint32_t id);
    void disconnect_client(std::uint32_t owner);
    [[nodiscard]] std::uint8_t map_state(std::uint32_t id) const;
    [[nodiscard]] std::uint32_t all_event_masks(const WindowRecord &window) const;

private:
    AtomTable atoms_;
    ResourceRegistry resources_;
    std::unordered_map<std::uint32_t, WindowRecord> windows_;
    std::uint16_t width_;
    std::uint16_t height_;
};

} // namespace xmin::next

#endif
