#ifndef XMIN_NEXT_SERVER_STATE_HPP
#define XMIN_NEXT_SERVER_STATE_HPP

#include "xmin/next/atom_table.hpp"
#include "xmin/next/client_event.hpp"
#include "xmin/next/generated/core_keymap.hpp"
#include "xmin/next/resource_registry.hpp"
#include "xmin/next/surface.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xmin::next {

constexpr std::uint32_t pointer_root_id = 1;
constexpr std::uint32_t root_window_id = 0x00000100;
constexpr std::uint32_t default_colormap_id = 2;
constexpr std::uint32_t root_visual_id = 3;
constexpr std::uint32_t client_resource_mask = 0x001fffff;
constexpr std::size_t maximum_client_resources = 4096;
constexpr std::size_t maximum_server_resources = 65536;
constexpr std::size_t maximum_property_bytes = 1024U * 1024U;
constexpr std::size_t maximum_server_property_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_pending_events_per_client = 256;
constexpr std::size_t maximum_pending_events = 4096;
constexpr std::size_t maximum_passive_grabs_per_client = 256;
constexpr std::size_t maximum_passive_grabs = 4096;
constexpr std::uint16_t any_modifier = 0x8000;
constexpr std::uint16_t all_modifiers_mask = 0x00ff;

enum class WindowClass : std::uint16_t {
    input_output = 1,
    input_only = 2,
};

struct PropertyValue {
    AtomId type = 0;
    std::uint8_t format = 0;
    std::vector<std::uint8_t> data;
};

struct WindowRecord {
    std::uint32_t id = 0;
    std::uint32_t owner = 0;
    std::uint32_t parent = 0;
    std::vector<std::uint32_t> children;
    std::unordered_map<std::uint32_t, std::uint32_t> event_masks;
    std::unordered_map<AtomId, PropertyValue> properties;
    std::optional<Surface> surface;
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

struct PixmapRecord {
    std::uint32_t id = 0;
    Surface surface;
};

struct GraphicsContextRecord {
    std::uint32_t id = 0;
    std::uint8_t depth = 0;
    std::uint8_t function = 3;
    std::uint32_t plane_mask = 0xffffffffU;
    std::uint32_t foreground = 0;
    std::uint32_t background = 1;
    std::int32_t clip_x_origin = 0;
    std::int32_t clip_y_origin = 0;
    std::optional<Region> clip_region;

    [[nodiscard]] ClipView clip() const noexcept
    {
        return ClipView{clip_region ? &*clip_region : nullptr,
                        clip_x_origin, clip_y_origin};
    }
};

struct SelectionRecord {
    std::uint32_t window = 0;
    std::uint32_t client = 0;
    std::uint32_t changed_at = 0;
};

enum class FocusKind : std::uint8_t {
    none,
    pointer_root,
    window,
};

struct FocusState {
    FocusKind kind = FocusKind::pointer_root;
    std::uint32_t window = 0;
    std::uint32_t changed_at = 1;
    std::uint8_t revert_to = 0;

    [[nodiscard]] std::uint32_t wire_id() const noexcept
    {
        if (kind == FocusKind::none)
            return 0;
        if (kind == FocusKind::pointer_root)
            return pointer_root_id;
        return window;
    }
};

struct ActiveGrab {
    std::uint32_t owner = 0;
    std::uint32_t window = 0;
    std::uint32_t confine_to = 0;
    std::uint32_t activated_at = 0;
    std::uint32_t event_mask = 0;
    std::uint8_t pointer_mode = 1;
    std::uint8_t keyboard_mode = 1;
    bool owner_events = false;
};

enum class PassiveGrabKind : std::uint8_t {
    button,
    key,
};

using PassiveGrabDomain = std::bitset<256>;

struct PassiveGrab {
    PassiveGrabKind kind = PassiveGrabKind::key;
    PassiveGrabDomain details;
    PassiveGrabDomain modifiers;
    std::uint32_t owner = 0;
    std::uint32_t window = 0;
    std::uint32_t confine_to = 0;
    std::uint16_t event_mask = 0;
    std::uint8_t pointer_mode = 1;
    std::uint8_t keyboard_mode = 1;
    bool owner_events = false;
};

[[nodiscard]] PassiveGrabDomain passive_grab_details(
    PassiveGrabKind kind, std::uint8_t detail) noexcept;
[[nodiscard]] PassiveGrabDomain passive_grab_modifiers(
    std::uint16_t modifiers) noexcept;

struct InputState {
    std::array<std::array<std::uint32_t, keysyms_per_keycode>, 256> keymap =
        core_keymap;
    std::array<std::uint8_t, 32> modifier_map = core_modifier_map;
    std::array<std::uint8_t, 32> auto_repeats = default_auto_repeats;
    std::array<std::uint8_t, 10> pointer_map = default_pointer_map;
    std::int32_t pointer_x = 0;
    std::int32_t pointer_y = 0;
    std::uint16_t modifier_button_mask = 0;
    std::uint32_t led_mask = 0;
    std::int16_t pointer_acceleration_numerator =
        default_pointer_acceleration_numerator;
    std::int16_t pointer_acceleration_denominator =
        default_pointer_acceleration_denominator;
    std::int16_t pointer_threshold = default_pointer_threshold;
    std::uint16_t bell_pitch = default_bell_pitch;
    std::uint16_t bell_duration = default_bell_duration;
    std::uint8_t key_click_percent = default_key_click_percent;
    std::uint8_t bell_percent = default_bell_percent;
    std::array<std::uint8_t, 32> pressed_keys{};
    FocusState focus;
    std::optional<ActiveGrab> pointer_grab;
    std::optional<ActiveGrab> keyboard_grab;
    std::uint32_t pointer_grab_time = 1;
    std::uint32_t keyboard_grab_time = 1;
    bool global_auto_repeat = default_global_auto_repeat;
};

enum class SelectionUpdate {
    updated,
    ignored,
    event_queue_full,
};

enum class EventDelivery {
    delivered,
    no_recipient,
    queue_full,
};

enum class FocusUpdate {
    updated,
    ignored,
};

enum class PassiveGrabUpdate {
    updated,
    access_denied,
    resource_exhausted,
};

class ServerState {
public:
    ServerState(std::uint16_t width, std::uint16_t height);

    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] AtomTable &atoms() noexcept { return atoms_; }
    [[nodiscard]] const AtomTable &atoms() const noexcept { return atoms_; }

    [[nodiscard]] WindowRecord *window(std::uint32_t id);
    [[nodiscard]] const WindowRecord *window(std::uint32_t id) const;
    [[nodiscard]] bool resource_exists(std::uint32_t id) const;
    [[nodiscard]] bool valid_client_resource(std::uint32_t id,
                                             std::uint32_t base) const;
    [[nodiscard]] bool resource_limit_reached(std::uint32_t owner) const;
    [[nodiscard]] bool add_window(WindowRecord window, std::uint32_t owner);
    [[nodiscard]] bool resize_window_surface(WindowRecord &window,
                                             std::uint16_t width,
                                             std::uint16_t height);
    [[nodiscard]] PixmapRecord *pixmap(std::uint32_t id);
    [[nodiscard]] const PixmapRecord *pixmap(std::uint32_t id) const;
    [[nodiscard]] bool add_pixmap(PixmapRecord pixmap, std::uint32_t owner);
    [[nodiscard]] bool erase_pixmap(std::uint32_t id);
    [[nodiscard]] GraphicsContextRecord *graphics_context(std::uint32_t id);
    [[nodiscard]] const GraphicsContextRecord *
    graphics_context(std::uint32_t id) const;
    [[nodiscard]] bool add_graphics_context(GraphicsContextRecord context,
                                            std::uint32_t owner);
    [[nodiscard]] bool erase_graphics_context(std::uint32_t id);
    [[nodiscard]] bool colormap_exists(std::uint32_t id) const;
    [[nodiscard]] bool add_colormap(std::uint32_t id, std::uint32_t owner);
    [[nodiscard]] bool erase_colormap(std::uint32_t id);
    void install_colormap(std::uint32_t id) noexcept
    {
        installed_colormap_ = id;
    }
    [[nodiscard]] std::uint32_t installed_colormap() const noexcept
    {
        return installed_colormap_;
    }
    [[nodiscard]] Surface *drawable_surface(std::uint32_t id);
    [[nodiscard]] const Surface *drawable_surface(std::uint32_t id) const;
    [[nodiscard]] Surface *readable_surface(std::uint32_t id);
    [[nodiscard]] std::uint8_t drawable_depth(std::uint32_t id) const;
    void invalidate_scene() noexcept { scene_dirty_ = true; }
    void set_window_mapped(WindowRecord &window, bool mapped) noexcept;
    void advance_time() noexcept;
    [[nodiscard]] std::uint32_t current_time() const noexcept
    {
        return current_time_;
    }
    [[nodiscard]] std::uint32_t selection_owner(AtomId selection) const;
    [[nodiscard]] SelectionUpdate set_selection_owner(
        AtomId selection, std::uint32_t window, std::uint32_t client,
        std::uint32_t time);
    [[nodiscard]] EventDelivery deliver_client_message(
        std::uint32_t destination, std::uint32_t event_mask, bool propagate,
        const ClientMessageEvent &event);
    [[nodiscard]] bool has_pending_event(std::uint32_t client) const;
    [[nodiscard]] const ClientEvent *next_event(std::uint32_t client) const;
    void pop_event(std::uint32_t client);
    [[nodiscard]] bool set_property(WindowRecord &window, AtomId property,
                                    PropertyValue value);
    void delete_property(WindowRecord &window, AtomId property);
    void destroy_window(std::uint32_t id);
    void destroy_subwindows(std::uint32_t id);
    [[nodiscard]] bool reparent_window(std::uint32_t id,
                                       std::uint32_t new_parent,
                                       std::int16_t x, std::int16_t y);
    void set_subwindows_mapped(std::uint32_t id, bool mapped);
    void grab_server(std::uint32_t owner) noexcept
    {
        if (server_grab_owner_ == 0 || server_grab_owner_ == owner)
            server_grab_owner_ = owner;
    }
    void ungrab_server(std::uint32_t owner) noexcept
    {
        if (server_grab_owner_ == owner)
            server_grab_owner_ = 0;
    }
    [[nodiscard]] std::uint32_t server_grab_owner() const noexcept
    {
        return server_grab_owner_;
    }
    [[nodiscard]] InputState &input() noexcept { return input_; }
    [[nodiscard]] const InputState &input() const noexcept { return input_; }
    [[nodiscard]] const std::vector<PassiveGrab> &passive_grabs() const noexcept
    {
        return passive_grabs_;
    }
    [[nodiscard]] PassiveGrabUpdate add_passive_grab(PassiveGrab grab);
    [[nodiscard]] PassiveGrabUpdate remove_passive_grab(
        PassiveGrabKind kind, std::uint32_t owner, std::uint32_t window,
        const PassiveGrabDomain &details,
        const PassiveGrabDomain &modifiers);
    [[nodiscard]] FocusUpdate set_input_focus(
        FocusKind kind, std::uint32_t window, std::uint8_t revert_to,
        std::uint32_t time) noexcept;
    void disconnect_client(std::uint32_t owner);
    [[nodiscard]] std::uint8_t map_state(std::uint32_t id) const;
    [[nodiscard]] std::uint32_t all_event_masks(const WindowRecord &window) const;
    [[nodiscard]] std::pair<std::int32_t, std::int32_t>
    absolute_position(std::uint32_t id) const;

private:
    AtomTable atoms_;
    ResourceRegistry resources_;
    std::unordered_map<std::uint32_t, WindowRecord> windows_;
    std::unordered_map<std::uint32_t, PixmapRecord> pixmaps_;
    std::unordered_map<std::uint32_t, GraphicsContextRecord>
        graphics_contexts_;
    std::unordered_map<AtomId, SelectionRecord> selections_;
    std::unordered_map<std::uint32_t, std::deque<ClientEvent>> event_queues_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::optional<Surface> composited_root_;
    std::size_t property_bytes_ = 0;
    std::size_t surface_bytes_ = 0;
    std::size_t pending_events_ = 0;
    std::uint32_t current_time_ = 1;
    std::uint32_t installed_colormap_ = default_colormap_id;
    std::uint32_t server_grab_owner_ = 0;
    InputState input_;
    std::vector<PassiveGrab> passive_grabs_;
    bool scene_dirty_ = true;

    [[nodiscard]] bool can_queue_event(std::uint32_t client) const;
    [[nodiscard]] bool queue_event(std::uint32_t client, ClientEvent event);
    void clear_selections_for_window(std::uint32_t window);
    void revert_focus_from(std::uint32_t window) noexcept;
    void composite_scene();
    void composite_window(std::uint32_t id, std::int64_t parent_x,
                          std::int64_t parent_y, std::int64_t clip_left,
                          std::int64_t clip_top, std::int64_t clip_right,
                          std::int64_t clip_bottom);
    [[nodiscard]] bool is_descendant(std::uint32_t candidate,
                                     std::uint32_t ancestor) const;
};

} // namespace xmin::next

#endif
