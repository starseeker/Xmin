#ifndef XMIN_SERVER_SERVER_STATE_HPP
#define XMIN_SERVER_SERVER_STATE_HPP

#include "xmin/server/atom_table.hpp"
#include "xmin/server/client_event.hpp"
#include "xmin/server/clock.hpp"
#include "xmin/server/font.hpp"
#include "xmin/server/generated/core_keymap.hpp"
#include "xmin/server/render.hpp"
#include "xmin/server/resource_registry.hpp"
#include "xmin/server/shared_memory.hpp"
#include "xmin/server/surface.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xmin::server {

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
constexpr std::size_t maximum_shape_rectangles = 32768;
constexpr std::size_t maximum_xfixes_regions = 4096;
constexpr std::size_t maximum_xfixes_barriers = 4096;
constexpr std::size_t maximum_xfixes_subscriptions = 4096;
constexpr std::size_t maximum_sync_wait_conditions =
    maximum_pending_events_per_client;
constexpr std::size_t maximum_randr_modes = 256;
constexpr std::size_t maximum_randr_monitors = 64;
constexpr std::size_t maximum_randr_output_properties = 256;
constexpr std::size_t maximum_randr_filter_parameters = 64;
constexpr std::size_t maximum_damage_objects = 4096;
constexpr std::size_t maximum_composite_redirects = 4096;
constexpr std::size_t maximum_present_operations = 4096;
constexpr std::size_t maximum_present_subscriptions = 4096;
constexpr std::size_t maximum_present_notifies = 256;
constexpr std::size_t maximum_xi2_selections = 4096;
constexpr std::size_t maximum_xi2_masks_per_selection = 4;
constexpr std::size_t maximum_xi2_properties_per_device = 256;
constexpr std::size_t maximum_shared_memory_segments = 256;
constexpr std::uint32_t randr_crtc_id = 0x00000200;
constexpr std::uint32_t randr_output_id = 0x00000201;
constexpr std::uint32_t randr_initial_mode_id = 0x00000202;
constexpr std::uint16_t any_modifier = 0x8000;
constexpr std::uint16_t all_modifiers_mask = 0x00ff;
inline constexpr auto default_repeat_delay = std::chrono::milliseconds{660};
inline constexpr auto default_repeat_interval = std::chrono::milliseconds{40};
inline constexpr auto present_refresh_interval =
    std::chrono::microseconds{16667};

enum class WindowClass : std::uint16_t {
    input_output = 1,
    input_only = 2,
};

struct PropertyValue {
    AtomId type = 0;
    std::uint8_t format = 0;
    std::vector<std::uint8_t> data;
};

struct RandrModeInfo {
    std::uint32_t id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t dot_clock = 0;
    std::uint16_t hsync_start = 0;
    std::uint16_t hsync_end = 0;
    std::uint16_t htotal = 0;
    std::uint16_t hskew = 0;
    std::uint16_t vsync_start = 0;
    std::uint16_t vsync_end = 0;
    std::uint16_t vtotal = 0;
    std::uint32_t flags = 0;
    std::string name;
    bool built_in = false;
};

struct RandrOutputProperty {
    PropertyValue value;
    PropertyValue pending_value;
    std::vector<std::int32_t> valid_values;
    bool pending = false;
    bool range = false;
    bool immutable = false;
};

struct RandrTransform {
    std::array<std::int32_t, 9> matrix{
        65536, 0, 0, 0, 65536, 0, 0, 0, 65536};
    std::string filter;
    std::vector<std::int32_t> parameters;
};

struct RandrPanning {
    std::uint16_t left = 0;
    std::uint16_t top = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t track_left = 0;
    std::uint16_t track_top = 0;
    std::uint16_t track_width = 0;
    std::uint16_t track_height = 0;
    std::int16_t border_left = 0;
    std::int16_t border_top = 0;
    std::int16_t border_right = 0;
    std::int16_t border_bottom = 0;
};

struct RandrMonitor {
    AtomId name = 0;
    bool primary = false;
    bool automatic = false;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t millimetre_width = 0;
    std::uint32_t millimetre_height = 0;
    std::vector<std::uint32_t> outputs;
};

struct RandrSubscription {
    std::uint32_t client = 0;
    std::uint32_t window = 0;
    std::uint16_t mask = 0;
};

struct RandrState {
    std::uint32_t timestamp = 1;
    std::uint32_t config_timestamp = 1;
    std::uint32_t millimetre_width = 0;
    std::uint32_t millimetre_height = 0;
    std::unordered_map<std::uint32_t, RandrModeInfo> modes;
    std::vector<std::uint32_t> output_modes;
    std::uint32_t current_mode = randr_initial_mode_id;
    std::uint32_t next_mode_id = randr_initial_mode_id + 1;
    std::uint32_t primary_output = randr_output_id;
    std::int16_t crtc_x = 0;
    std::int16_t crtc_y = 0;
    std::uint16_t rotation = 1;
    std::vector<std::uint16_t> gamma_red;
    std::vector<std::uint16_t> gamma_green;
    std::vector<std::uint16_t> gamma_blue;
    RandrTransform transform;
    RandrPanning panning;
    std::unordered_map<AtomId, RandrOutputProperty> output_properties;
    std::unordered_map<AtomId, RandrMonitor> monitors;
    std::vector<RandrSubscription> subscriptions;
};

struct DamageRecord {
    std::uint32_t id = 0;
    std::uint32_t owner = 0;
    std::uint32_t drawable = 0;
    std::uint8_t level = 0;
    Region accumulated;
};

enum class DamageUpdate {
    updated,
    invalid,
    resource_exhausted,
    queue_full,
};

struct CompositeRedirect {
    std::uint32_t owner = 0;
    std::uint32_t window = 0;
    std::uint8_t update = 0;
    bool subwindows = false;
};

enum class CompositeUpdate {
    updated,
    invalid,
    access_denied,
    resource_exhausted,
};

struct PresentSubscription {
    std::uint32_t id = 0;
    std::uint32_t owner = 0;
    std::uint32_t window = 0;
    std::uint32_t mask = 0;
};

struct PresentNotify {
    std::uint32_t window = 0;
    std::uint32_t serial = 0;
};

enum class PresentKind : std::uint8_t {
    pixmap,
    notify_msc,
};

struct PresentOperation {
    PresentKind kind = PresentKind::pixmap;
    std::uint32_t owner = 0;
    std::uint32_t window = 0;
    std::uint32_t pixmap = 0;
    std::uint32_t serial = 0;
    std::shared_ptr<Surface> pixmap_surface;
    std::optional<Region> update;
    std::int16_t x_off = 0;
    std::int16_t y_off = 0;
    std::uint32_t wait_fence = 0;
    std::uint32_t idle_fence = 0;
    std::uint32_t options = 0;
    std::uint64_t target_msc = 0;
    std::uint64_t divisor = 0;
    std::uint64_t remainder = 0;
    std::vector<PresentNotify> notifies;
};

enum class PresentUpdate {
    updated,
    invalid,
    match,
    resource_exhausted,
    queue_full,
};

struct CursorImage {
    std::uint32_t serial = 0;
    AtomId name = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t x_hot = 0;
    std::uint16_t y_hot = 0;
    RenderColor foreground;
    RenderColor background;
    std::vector<std::uint32_t> pixels;
    // 0 is transparent, 1 is the background, and 2 is the foreground.
    // ARGB cursors leave this empty because RecolorCursor does not rewrite
    // their literal pixels.
    std::vector<std::uint8_t> pixel_roles;
    std::vector<std::pair<std::shared_ptr<CursorImage>, std::uint32_t>> frames;

    void recolor(RenderColor new_foreground,
                 RenderColor new_background) noexcept;
};

struct CursorRecord {
    std::uint32_t id = 0;
    std::shared_ptr<CursorImage> image;
};

struct WindowRecord {
    std::uint32_t id = 0;
    std::uint32_t owner = 0;
    std::uint32_t parent = 0;
    std::vector<std::uint32_t> children;
    std::unordered_map<std::uint32_t, std::uint32_t> event_masks;
    std::unordered_map<AtomId, PropertyValue> properties;
    std::array<std::optional<Region>, 3> shapes;
    std::vector<std::uint32_t> shape_event_clients;
    std::shared_ptr<Surface> surface;
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
    std::shared_ptr<CursorImage> cursor;
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

    [[nodiscard]] Rectangle default_shape(std::uint8_t kind) const noexcept
    {
        if (kind == 1)
            return Rectangle{0, 0, width, height};
        const std::int32_t border = border_width;
        return Rectangle{
            -border, -border,
            static_cast<std::uint32_t>(width) + 2U * border_width,
            static_cast<std::uint32_t>(height) + 2U * border_width};
    }
};

struct PixmapRecord {
    std::uint32_t id = 0;
    std::shared_ptr<Surface> surface;
};

struct DbeBufferRecord {
    std::uint32_t id = 0;
    std::uint32_t window = 0;
    std::uint8_t swap_action = 0;
    std::shared_ptr<Surface> surface;
};

struct FontRecord {
    std::uint32_t id = 0;
    const EmbeddedFont *font = nullptr;
};

struct GraphicsContextRecord {
    std::uint32_t id = 0;
    std::uint8_t depth = 0;
    std::uint8_t function = 3;
    std::uint32_t plane_mask = 0xffffffffU;
    std::uint32_t foreground = 0;
    std::uint32_t background = 1;
    const EmbeddedFont *font = nullptr;
    std::uint16_t line_width = 0;
    std::uint16_t dash_offset = 0;
    std::uint8_t line_style = 0;
    std::uint8_t cap_style = 1;
    std::uint8_t join_style = 0;
    std::uint8_t fill_style = 0;
    std::uint8_t fill_rule = 0;
    std::uint8_t arc_mode = 1;
    std::shared_ptr<Surface> tile;
    std::shared_ptr<Surface> stipple;
    std::int32_t tile_x_origin = 0;
    std::int32_t tile_y_origin = 0;
    std::uint8_t subwindow_mode = 0;
    bool graphics_exposures = true;
    std::array<std::uint8_t, 512> dashes = [] {
        std::array<std::uint8_t, 512> result{};
        result[0] = 4;
        result[1] = 4;
        return result;
    }();
    std::uint16_t dash_count = 2;
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

struct ScreenSaverState {
    std::int16_t timeout = 0;
    std::int16_t interval = 0;
    std::uint8_t prefer_blanking = 2;
    std::uint8_t allow_exposures = 2;
    bool active = false;
};

struct XFixesSelectionSubscription {
    std::uint32_t client = 0;
    std::uint32_t window = 0;
    AtomId selection = 0;
    std::uint32_t event_mask = 0;
};

struct XFixesCursorSubscription {
    std::uint32_t client = 0;
    std::uint32_t window = 0;
    std::uint32_t event_mask = 0;
};

struct XFixesBarrierRecord {
    std::uint32_t id = 0;
    std::uint32_t window = 0;
    std::int16_t x1 = 0;
    std::int16_t y1 = 0;
    std::int16_t x2 = 0;
    std::int16_t y2 = 0;
    std::uint32_t directions = 0;
    std::vector<std::uint16_t> devices;
};

struct SaveSetEntry {
    std::uint32_t window = 0;
    bool to_root = false;
    bool map = true;
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
    ActiveGrab() = default;
    ActiveGrab(std::uint32_t grab_owner, std::uint32_t grab_window,
               std::uint32_t confinement, std::uint32_t time,
               std::uint32_t mask, std::uint8_t pointer,
               std::uint8_t keyboard, bool owner_receives,
               bool from_passive = false, std::uint8_t detail = 0,
               bool is_automatic = false,
               std::shared_ptr<CursorImage> grab_cursor = {},
               bool xi_grab = false, std::uint32_t xi_mask = 0) noexcept
        : owner(grab_owner), window(grab_window), confine_to(confinement),
          activated_at(time), event_mask(mask), pointer_mode(pointer),
          keyboard_mode(keyboard), owner_events(owner_receives),
          passive(from_passive), passive_detail(detail),
          automatic(is_automatic), cursor(std::move(grab_cursor)),
          xi2(xi_grab), xi2_event_mask(xi_mask)
    {}

    std::uint32_t owner = 0;
    std::uint32_t window = 0;
    std::uint32_t confine_to = 0;
    std::uint32_t activated_at = 0;
    std::uint32_t event_mask = 0;
    std::uint8_t pointer_mode = 1;
    std::uint8_t keyboard_mode = 1;
    bool owner_events = false;
    bool passive = false;
    std::uint8_t passive_detail = 0;
    bool automatic = false;
    std::shared_ptr<CursorImage> cursor;
    bool xi2 = false;
    std::uint32_t xi2_event_mask = 0;
};

struct FrozenPointerEvent {
    struct PendingInput {
        std::uint8_t type = 0;
        std::uint8_t detail = 0;
        std::int32_t root_x = 0;
        std::int32_t root_y = 0;
    };

    CoreInputEvent event;
    std::uint32_t mask = 0;
    std::uint32_t source = 0;
    std::uint32_t propagation_stop = 0;
    std::uint32_t pointer_window = 0;
    std::deque<PendingInput> pending;
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
    std::shared_ptr<CursorImage> cursor;
    bool xi2 = false;
    std::uint32_t xi2_event_mask = 0;
};

[[nodiscard]] PassiveGrabDomain passive_grab_details(
    PassiveGrabKind kind, std::uint8_t detail) noexcept;
[[nodiscard]] PassiveGrabDomain passive_grab_modifiers(
    std::uint16_t modifiers) noexcept;

constexpr std::uint8_t xkb_keyboard_device_id = 3;
constexpr std::uint32_t xkb_supported_boolean_controls =
    (1U << 0) | (1U << 9);
constexpr std::uint32_t xkb_supported_client_flags = 0x1f;

struct XkbControls {
    std::uint8_t mouse_keys_default_button = 1;
    std::uint8_t groups_wrap = 1;
    std::uint8_t internal_mods = 0;
    std::uint8_t ignore_lock_mods = 0;
    std::uint16_t internal_virtual_mods = 0;
    std::uint16_t ignore_lock_virtual_mods = 0;
    std::uint16_t repeat_delay = static_cast<std::uint16_t>(
        default_repeat_delay.count());
    std::uint16_t repeat_interval = static_cast<std::uint16_t>(
        default_repeat_interval.count());
    std::uint16_t slow_keys_delay = 300;
    std::uint16_t debounce_delay = 300;
    std::uint16_t mouse_keys_delay = 160;
    std::uint16_t mouse_keys_interval = 40;
    std::uint16_t mouse_keys_time_to_max = 30;
    std::uint16_t mouse_keys_max_speed = 500;
    std::int16_t mouse_keys_curve = 0;
    std::uint16_t access_x_options = 0;
    std::uint16_t access_x_timeout = 0;
    std::uint16_t access_x_timeout_options_mask = 0;
    std::uint16_t access_x_timeout_options_values = 0;
    std::uint32_t access_x_timeout_mask = 0;
    std::uint32_t access_x_timeout_values = 0;
    std::uint32_t enabled = xkb_supported_boolean_controls;
    std::array<std::uint8_t, 32> per_key_repeat = default_auto_repeats;
};

struct XkbKeyboardState {
    std::uint8_t latched_mods = 0;
    std::uint8_t locked_mods = 0;
    std::int16_t base_group = 0;
    std::int16_t latched_group = 0;
    std::uint8_t locked_group = 0;
    XkbControls controls;
};

struct XkbStateSnapshot {
    std::uint8_t mods = 0;
    std::uint8_t base_mods = 0;
    std::uint8_t latched_mods = 0;
    std::uint8_t locked_mods = 0;
    std::uint8_t group = 0;
    std::int16_t base_group = 0;
    std::int16_t latched_group = 0;
    std::uint8_t locked_group = 0;
    std::uint16_t pointer_buttons = 0;
};

struct XkbEventSelection {
    std::uint32_t owner = 0;
    std::uint16_t events = 0;
    std::uint16_t map = 0;
    std::uint16_t new_keyboard = 0;
    std::uint16_t state = 0;
    std::uint32_t controls = 0;
    std::uint32_t indicator_state = 0;
    std::uint32_t indicator_map = 0;
    std::uint16_t names = 0;
    std::uint8_t compatibility = 0;
    std::uint8_t bell = 0;
    std::uint8_t action_message = 0;
    std::uint16_t access_x = 0;
    std::uint16_t extension_device = 0;
};

constexpr std::uint16_t xi2_all_devices = 0;
constexpr std::uint16_t xi2_all_master_devices = 1;
constexpr std::uint16_t xi2_pointer_device_id = 2;
constexpr std::uint16_t xi2_keyboard_device_id = 3;

struct Xi2EventMask {
    std::uint16_t device = 0;
    std::vector<std::uint32_t> words;
};

struct Xi2EventSelection {
    std::uint32_t owner = 0;
    std::uint32_t window = 0;
    std::vector<Xi2EventMask> masks;
};

enum class Xi2Update {
    updated,
    resource_exhausted,
    queue_full,
};

struct InputState {
    std::uint8_t keymap_width =
        static_cast<std::uint8_t>(keysyms_per_keycode);
    std::array<std::uint8_t, 256> keymap_row_widths = [] {
        std::array<std::uint8_t, 256> widths{};
        for (std::size_t keycode = minimum_keycode;
             keycode <= maximum_keycode; ++keycode) {
            widths[keycode] = static_cast<std::uint8_t>(
                keysyms_per_keycode);
        }
        return widths;
    }();
    std::vector<std::uint32_t> keymap = [] {
        std::vector<std::uint32_t> symbols;
        symbols.reserve(core_keymap.size() * keysyms_per_keycode);
        for (const auto &row : core_keymap)
            symbols.insert(symbols.end(), row.begin(), row.end());
        return symbols;
    }();
    std::vector<std::uint8_t> modifier_map{
        core_modifier_map.begin(), core_modifier_map.end()};
    std::array<std::uint8_t, 32> auto_repeats = default_auto_repeats;
    std::array<std::uint8_t, 10> pointer_map = default_pointer_map;
    std::int32_t pointer_x = 0;
    std::int32_t pointer_y = 0;
    std::uint16_t modifier_button_mask = 0;
    std::bitset<256> pressed_buttons;
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
    std::uint8_t modifier_keys_per_group =
        static_cast<std::uint8_t>(keys_per_modifier);
    std::array<std::uint8_t, 32> pressed_keys{};
    FocusState focus;
    std::optional<ActiveGrab> pointer_grab;
    std::optional<ActiveGrab> keyboard_grab;
    std::optional<FrozenPointerEvent> frozen_pointer_event;
    std::uint32_t pointer_grab_time = 1;
    std::uint32_t keyboard_grab_time = 1;
    bool global_auto_repeat = default_global_auto_repeat;
    XkbKeyboardState xkb;
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

enum class RedirectDelivery {
    not_redirected,
    redirected,
    queue_full,
};

enum class XkbUpdate {
    updated,
    invalid,
    resource_exhausted,
    queue_full,
};

enum class FocusUpdate {
    updated,
    ignored,
    queue_full,
};

enum class ReparentUpdate {
    updated,
    invalid,
    queue_full,
};

enum class PassiveGrabUpdate {
    updated,
    access_denied,
    resource_exhausted,
};

enum class ShapeUpdate {
    updated,
    invalid,
    resource_exhausted,
    queue_full,
};

enum class XFixesUpdate {
    updated,
    invalid,
    resource_exhausted,
    queue_full,
};

enum class SyncTestType : std::uint8_t {
    positive_transition = 0,
    negative_transition = 1,
    positive_comparison = 2,
    negative_comparison = 3,
};

struct SyncTrigger {
    std::uint32_t counter = 0;
    std::int64_t wait_value = 0;
    std::int64_t test_value = 0;
    std::uint8_t value_type = 0;
    SyncTestType test_type = SyncTestType::positive_comparison;
};

struct SyncCounterRecord {
    std::uint32_t id = 0;
    std::int64_t value = 0;
};

struct SyncAlarmRecord {
    std::uint32_t id = 0;
    std::uint32_t owner = 0;
    SyncTrigger trigger;
    std::int64_t delta = 1;
    std::vector<std::uint32_t> event_clients;
    std::uint8_t state = 1;
};

struct SyncFenceRecord {
    std::uint32_t id = 0;
    bool triggered = false;
};

struct SyncWaitCondition {
    SyncTrigger trigger;
    std::int64_t event_threshold = 0;
};

enum class SyncUpdate {
    updated,
    invalid,
    resource_exhausted,
    queue_full,
};

enum class RandrUpdate {
    updated,
    invalid,
    resource_exhausted,
    queue_full,
};

class ServerState {
public:
    ServerState(std::uint16_t width, std::uint16_t height,
                Clock &clock = default_clock());

    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] AtomTable &atoms() noexcept { return atoms_; }
    [[nodiscard]] const AtomTable &atoms() const noexcept { return atoms_; }
    [[nodiscard]] const RandrState &randr() const noexcept { return randr_; }
    [[nodiscard]] RandrUpdate select_randr_input(
        std::uint32_t client, std::uint32_t window, std::uint16_t mask);
    [[nodiscard]] RandrUpdate commit_randr_state(
        RandrState candidate, std::uint16_t notify_mask,
        AtomId property = 0, std::uint8_t property_status = 0);
    [[nodiscard]] RandrUpdate resize_randr_screen(RandrState candidate,
                                                  std::uint16_t width,
                                                  std::uint16_t height);

    [[nodiscard]] WindowRecord *window(std::uint32_t id);
    [[nodiscard]] const WindowRecord *window(std::uint32_t id) const;
    [[nodiscard]] bool resource_exists(std::uint32_t id) const;
    [[nodiscard]] bool reserve_resource(std::uint32_t id, ResourceKind kind,
                                        std::uint32_t owner);
    [[nodiscard]] bool release_resource(std::uint32_t id, ResourceKind kind);
    [[nodiscard]] std::optional<std::uint32_t>
    resource_owner(std::uint32_t id) const;
    [[nodiscard]] bool valid_client_resource(std::uint32_t id,
                                             std::uint32_t base) const;
    [[nodiscard]] bool resource_limit_reached(std::uint32_t owner) const;
    [[nodiscard]] bool add_window(WindowRecord window, std::uint32_t owner);
    [[nodiscard]] std::shared_ptr<Surface> adopt_surface(Surface surface);
    [[nodiscard]] bool resize_window_surface(WindowRecord &window,
                                             std::uint16_t width,
                                             std::uint16_t height);
    [[nodiscard]] EventDelivery configure_window(
        WindowRecord &window, std::int16_t x, std::int16_t y,
        std::uint16_t width, std::uint16_t height,
        std::uint16_t border_width,
        std::optional<std::uint32_t> sibling,
        std::optional<std::uint8_t> stack_mode,
        std::uint32_t requester = 0,
        std::uint16_t value_mask = 0);
    [[nodiscard]] RedirectDelivery redirect_map_request(
        std::uint32_t requester, const WindowRecord &window);
    [[nodiscard]] RedirectDelivery redirect_configure_request(
        std::uint32_t requester, const WindowRecord &window,
        std::int16_t x, std::int16_t y,
        std::uint16_t width, std::uint16_t height,
        std::uint16_t border_width, std::uint16_t value_mask,
        std::optional<std::uint32_t> sibling,
        std::optional<std::uint8_t> stack_mode);
    [[nodiscard]] RedirectDelivery redirect_circulate_request(
        std::uint32_t requester, const WindowRecord &parent,
        bool raise_lowest);
    [[nodiscard]] EventDelivery circulate_window(std::uint32_t parent,
                                                 bool raise_lowest);
    [[nodiscard]] PixmapRecord *pixmap(std::uint32_t id);
    [[nodiscard]] const PixmapRecord *pixmap(std::uint32_t id) const;
    [[nodiscard]] bool add_pixmap(PixmapRecord pixmap, std::uint32_t owner);
    [[nodiscard]] bool erase_pixmap(std::uint32_t id);
    [[nodiscard]] DbeBufferRecord *dbe_buffer(std::uint32_t id);
    [[nodiscard]] const DbeBufferRecord *dbe_buffer(std::uint32_t id) const;
    [[nodiscard]] bool add_dbe_buffer(DbeBufferRecord buffer,
                                      std::uint32_t owner);
    [[nodiscard]] bool erase_dbe_buffer(std::uint32_t id);
    [[nodiscard]] bool swap_dbe_buffer(std::uint32_t window,
                                       std::uint8_t action);
    [[nodiscard]] DamageRecord *damage(std::uint32_t id);
    [[nodiscard]] const DamageRecord *damage(std::uint32_t id) const;
    [[nodiscard]] DamageUpdate add_damage(DamageRecord damage,
                                          std::uint32_t owner);
    [[nodiscard]] bool erase_damage(std::uint32_t id);
    [[nodiscard]] DamageUpdate damage_drawable(
        std::uint32_t drawable, const Region *region = nullptr);
    [[nodiscard]] DamageUpdate subtract_damage(
        std::uint32_t id, const Region *repair, Region *parts);
    [[nodiscard]] DamageUpdate damage_render_picture(std::uint32_t picture);
    [[nodiscard]] CompositeUpdate redirect_window(
        std::uint32_t owner, std::uint32_t window, bool subwindows,
        std::uint8_t update);
    [[nodiscard]] CompositeUpdate unredirect_window(
        std::uint32_t owner, std::uint32_t window, bool subwindows,
        std::uint8_t update);
    [[nodiscard]] bool composite_window_redirected(
        std::uint32_t window) const noexcept;
    [[nodiscard]] bool composite_window_manually_redirected(
        std::uint32_t window) const noexcept;
    [[nodiscard]] CompositeUpdate name_window_pixmap(
        std::uint32_t window, std::uint32_t pixmap, std::uint32_t owner);
    [[nodiscard]] PresentUpdate select_present_input(
        std::uint32_t owner, std::uint32_t id, std::uint32_t window,
        std::uint32_t mask);
    [[nodiscard]] PresentUpdate submit_present(PresentOperation operation);
    [[nodiscard]] EventDelivery present_window_configured(
        std::uint32_t window);
    [[nodiscard]] std::uint64_t present_msc() const noexcept;
    [[nodiscard]] std::uint64_t present_ust() const noexcept;
    [[nodiscard]] GraphicsContextRecord *graphics_context(std::uint32_t id);
    [[nodiscard]] const GraphicsContextRecord *
    graphics_context(std::uint32_t id) const;
    [[nodiscard]] bool add_graphics_context(GraphicsContextRecord context,
                                            std::uint32_t owner);
    [[nodiscard]] bool erase_graphics_context(std::uint32_t id);
    [[nodiscard]] FontRecord *font(std::uint32_t id);
    [[nodiscard]] const FontRecord *font(std::uint32_t id) const;
    [[nodiscard]] bool add_font(FontRecord font, std::uint32_t owner);
    [[nodiscard]] bool erase_font(std::uint32_t id);
    [[nodiscard]] SharedMemory *shared_memory(std::uint32_t id);
    [[nodiscard]] const SharedMemory *shared_memory(std::uint32_t id) const;
    [[nodiscard]] std::shared_ptr<SharedMemory>
    shared_memory_storage(std::uint32_t id) const;
    [[nodiscard]] bool add_shared_memory(
        std::uint32_t id, SharedMemory memory, std::uint32_t owner);
    [[nodiscard]] bool erase_shared_memory(std::uint32_t id);
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
    [[nodiscard]] Surface *readable_surface(std::uint32_t id,
                                            const Rectangle &area);
    [[nodiscard]] std::uint8_t drawable_depth(std::uint32_t id) const;
    [[nodiscard]] std::uint32_t pointer_window() const noexcept;
    [[nodiscard]] std::shared_ptr<CursorImage>
    effective_cursor(std::uint32_t window) const noexcept;
    [[nodiscard]] std::shared_ptr<CursorImage> current_cursor() const noexcept;
    void invalidate_scene() noexcept { scene_dirty_ = true; }
    [[nodiscard]] EventDelivery set_window_mapped(
        WindowRecord &window, bool mapped);
    void advance_time() noexcept;
    [[nodiscard]] std::uint32_t current_time() const noexcept
    {
        return current_time_;
    }
    [[nodiscard]] std::uint32_t selection_owner(AtomId selection) const;
    [[nodiscard]] SelectionUpdate set_selection_owner(
        AtomId selection, std::uint32_t window, std::uint32_t client,
        std::uint32_t time);
    [[nodiscard]] EventDelivery convert_selection(
        std::uint32_t client, std::uint32_t requestor, AtomId selection,
        AtomId target, AtomId property, std::uint32_t time);
    [[nodiscard]] EventDelivery deliver_client_message(
        std::uint32_t destination, std::uint32_t event_mask, bool propagate,
        const ClientMessageEvent &event);
    [[nodiscard]] bool register_client(std::uint32_t client);
    [[nodiscard]] bool request_client_termination(std::uint32_t resource);
    [[nodiscard]] bool client_termination_requested(
        std::uint32_t client) const noexcept;
    void note_client_sequence(std::uint32_t client,
                              std::uint16_t sequence) noexcept;
    [[nodiscard]] bool broadcast_mapping_notify(
        std::uint8_t request, std::uint8_t first_keycode,
        std::uint8_t count);
    [[nodiscard]] ShapeUpdate set_window_shape(
        WindowRecord &window, std::uint8_t kind,
        std::optional<Region> shape);
    [[nodiscard]] bool select_shape_events(
        WindowRecord &window, std::uint32_t client, bool enabled);
    [[nodiscard]] bool shape_events_selected(
        const WindowRecord &window, std::uint32_t client) const noexcept;
    [[nodiscard]] RenderPicture *render_picture(std::uint32_t id);
    [[nodiscard]] const RenderPicture *render_picture(std::uint32_t id) const;
    [[nodiscard]] std::shared_ptr<RenderPicture>
    render_picture_handle(std::uint32_t id) const;
    [[nodiscard]] bool add_render_picture(
        RenderPicture picture, std::uint32_t owner);
    [[nodiscard]] bool erase_render_picture(std::uint32_t id);
    [[nodiscard]] RenderGlyphSet *render_glyph_set(std::uint32_t id);
    [[nodiscard]] const RenderGlyphSet *render_glyph_set(
        std::uint32_t id) const;
    [[nodiscard]] bool add_render_glyph_set(
        RenderGlyphSet glyph_set, std::uint32_t owner);
    [[nodiscard]] bool erase_render_glyph_set(std::uint32_t id);
    [[nodiscard]] bool render_glyph_storage_fits(
        const RenderGlyphStorage &changed,
        std::size_t changed_bytes) const noexcept;
    [[nodiscard]] CursorRecord *cursor(std::uint32_t id);
    [[nodiscard]] const CursorRecord *cursor(std::uint32_t id) const;
    [[nodiscard]] bool add_cursor(CursorRecord cursor, std::uint32_t owner);
    [[nodiscard]] bool erase_cursor(std::uint32_t id);
    [[nodiscard]] EventDelivery cursor_maybe_changed();
    [[nodiscard]] XFixesUpdate set_window_cursor(
        WindowRecord &window, std::shared_ptr<CursorImage> cursor);
    [[nodiscard]] XFixesUpdate set_pointer_grab_cursor(
        std::uint32_t event_mask, std::shared_ptr<CursorImage> cursor);
    [[nodiscard]] XFixesUpdate replace_cursor(
        const std::shared_ptr<CursorImage> &source,
        const std::shared_ptr<CursorImage> &destination);
    [[nodiscard]] XFixesUpdate replace_cursor_by_name(
        const std::shared_ptr<CursorImage> &source, AtomId name);
    [[nodiscard]] Region *xfixes_region(std::uint32_t id);
    [[nodiscard]] const Region *xfixes_region(std::uint32_t id) const;
    [[nodiscard]] bool add_xfixes_region(
        std::uint32_t id, Region region, std::uint32_t owner);
    [[nodiscard]] bool erase_xfixes_region(std::uint32_t id);
    [[nodiscard]] XFixesUpdate select_xfixes_selection_input(
        std::uint32_t client, std::uint32_t window, AtomId selection,
        std::uint32_t event_mask);
    [[nodiscard]] XFixesUpdate select_xfixes_cursor_input(
        std::uint32_t client, std::uint32_t window,
        std::uint32_t event_mask);
    [[nodiscard]] XFixesUpdate hide_cursor(std::uint32_t client);
    [[nodiscard]] XFixesUpdate show_cursor(std::uint32_t client);
    [[nodiscard]] bool cursor_hidden() const noexcept
    {
        return !cursor_hide_counts_.empty();
    }
    [[nodiscard]] bool add_xfixes_barrier(
        XFixesBarrierRecord barrier, std::uint32_t owner);
    [[nodiscard]] const XFixesBarrierRecord *xfixes_barrier(
        std::uint32_t id) const noexcept;
    [[nodiscard]] bool erase_xfixes_barrier(std::uint32_t id,
                                            std::uint32_t owner);
    [[nodiscard]] XFixesUpdate alter_save_set(
        std::uint32_t client, std::uint32_t window, bool insert,
        bool to_root, bool map);
    [[nodiscard]] SyncCounterRecord *sync_counter(std::uint32_t id);
    [[nodiscard]] const SyncCounterRecord *sync_counter(
        std::uint32_t id) const;
    [[nodiscard]] bool add_sync_counter(
        SyncCounterRecord counter, std::uint32_t owner);
    [[nodiscard]] SyncUpdate set_sync_counter(
        SyncCounterRecord &counter, std::int64_t value);
    [[nodiscard]] SyncUpdate erase_sync_counter(std::uint32_t id);
    [[nodiscard]] SyncAlarmRecord *sync_alarm(std::uint32_t id);
    [[nodiscard]] const SyncAlarmRecord *sync_alarm(std::uint32_t id) const;
    [[nodiscard]] SyncUpdate add_sync_alarm(
        SyncAlarmRecord alarm, std::uint32_t owner);
    [[nodiscard]] SyncUpdate change_sync_alarm(SyncAlarmRecord alarm);
    [[nodiscard]] SyncUpdate erase_sync_alarm(std::uint32_t id);
    [[nodiscard]] SyncFenceRecord *sync_fence(std::uint32_t id);
    [[nodiscard]] const SyncFenceRecord *sync_fence(std::uint32_t id) const;
    [[nodiscard]] bool add_sync_fence(
        SyncFenceRecord fence, std::uint32_t owner);
    [[nodiscard]] SyncUpdate trigger_sync_fence(std::uint32_t id);
    [[nodiscard]] bool reset_sync_fence(std::uint32_t id) noexcept;
    [[nodiscard]] SyncUpdate erase_sync_fence(std::uint32_t id);
    [[nodiscard]] SyncUpdate begin_sync_counter_await(
        std::uint32_t client, std::vector<SyncWaitCondition> conditions);
    [[nodiscard]] SyncUpdate begin_sync_fence_await(
        std::uint32_t client, std::vector<std::uint32_t> fences);
    [[nodiscard]] bool sync_waiting(std::uint32_t client) const noexcept;
    void set_sync_priority(std::uint32_t client, std::int32_t priority);
    [[nodiscard]] std::int32_t sync_priority(
        std::uint32_t client) const noexcept;
    [[nodiscard]] EventDelivery inject_input(
        std::uint8_t type, std::uint8_t detail,
        std::int32_t root_x, std::int32_t root_y);
    [[nodiscard]] XkbStateSnapshot xkb_state() const noexcept;
    [[nodiscard]] std::uint32_t xkb_indicator_state() const noexcept;
    [[nodiscard]] const XkbEventSelection *xkb_selection(
        std::uint32_t owner) const noexcept;
    [[nodiscard]] XkbUpdate select_xkb_events(XkbEventSelection selection);
    [[nodiscard]] XkbUpdate latch_lock_xkb(
        std::uint8_t affect_locks, std::uint8_t locks,
        bool lock_group, std::uint8_t group_lock,
        std::uint8_t affect_latches, bool latch_group,
        std::int16_t group_latch,
        std::uint8_t request_major, std::uint8_t request_minor);
    [[nodiscard]] XkbUpdate set_xkb_controls(
        XkbControls controls, std::uint32_t changed,
        std::uint32_t enabled_changes,
        std::uint8_t request_major, std::uint8_t request_minor);
    [[nodiscard]] std::uint32_t xkb_client_flags(
        std::uint32_t owner) const noexcept;
    [[nodiscard]] XkbUpdate set_xkb_client_flags(
        std::uint32_t owner, std::uint32_t value);
    [[nodiscard]] const Xi2EventSelection *xi2_selection(
        std::uint32_t owner, std::uint32_t window) const noexcept;
    [[nodiscard]] Xi2Update select_xi2_events(Xi2EventSelection selection);
    [[nodiscard]] const std::unordered_map<AtomId, PropertyValue> &
    xi2_properties(std::uint16_t device) const noexcept;
    [[nodiscard]] Xi2Update set_xi2_property(
        std::uint16_t device, AtomId property, PropertyValue value);
    [[nodiscard]] Xi2Update delete_xi2_property(
        std::uint16_t device, AtomId property);
    [[nodiscard]] EventDelivery process_timers();
    [[nodiscard]] int timer_timeout_milliseconds() const noexcept;
    void update_repeat_controls() noexcept;
    [[nodiscard]] bool has_pending_event(std::uint32_t client) const;
    [[nodiscard]] const ClientEvent *next_event(std::uint32_t client) const;
    void pop_event(std::uint32_t client);
    [[nodiscard]] EventDelivery set_property(
        WindowRecord &window, AtomId property, PropertyValue value);
    [[nodiscard]] EventDelivery delete_property(
        WindowRecord &window, AtomId property);
    [[nodiscard]] EventDelivery rotate_properties(
        WindowRecord &window, const std::vector<AtomId> &properties,
        std::size_t delta);
    [[nodiscard]] EventDelivery destroy_window(std::uint32_t id);
    [[nodiscard]] EventDelivery destroy_subwindows(std::uint32_t id);
    [[nodiscard]] ReparentUpdate reparent_window(
        std::uint32_t id, std::uint32_t new_parent,
        std::int16_t x, std::int16_t y);
    [[nodiscard]] EventDelivery set_subwindows_mapped(
        std::uint32_t id, bool mapped);
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
    [[nodiscard]] const ScreenSaverState &screen_saver() const noexcept
    {
        return screen_saver_;
    }
    void set_screen_saver(ScreenSaverState state) noexcept
    {
        screen_saver_ = state;
    }
    [[nodiscard]] bool access_control_enabled() const noexcept
    {
        return access_control_enabled_;
    }
    void set_access_control(bool enabled) noexcept
    {
        access_control_enabled_ = enabled;
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
        std::uint32_t time);
    [[nodiscard]] EventDelivery activate_pointer_grab(ActiveGrab grab);
    [[nodiscard]] EventDelivery deactivate_pointer_grab();
    [[nodiscard]] EventDelivery allow_events(
        std::uint32_t owner, std::uint8_t mode, std::uint32_t time);
    [[nodiscard]] EventDelivery activate_keyboard_grab(ActiveGrab grab);
    [[nodiscard]] EventDelivery deactivate_keyboard_grab();
    void disconnect_client(std::uint32_t owner);
    [[nodiscard]] std::uint8_t map_state(std::uint32_t id) const;
    [[nodiscard]] std::uint32_t all_event_masks(const WindowRecord &window) const;
    [[nodiscard]] std::uint32_t child_window_at(
        std::uint32_t parent, std::int32_t x, std::int32_t y) const;
    [[nodiscard]] std::pair<std::int32_t, std::int32_t>
    absolute_position(std::uint32_t id) const;

private:
    struct SurfaceBudget;
    struct ManagedSurface;

    using PlannedEvent = std::pair<std::uint32_t, ClientEvent>;

    struct KeyRepeat {
        std::uint8_t key = 0;
        Clock::time_point deadline;
    };

    AtomTable atoms_;
    ResourceRegistry resources_;
    std::unordered_map<std::uint32_t, WindowRecord> windows_;
    std::unordered_map<std::uint32_t, PixmapRecord> pixmaps_;
    std::unordered_map<std::uint32_t, DamageRecord> damages_;
    std::vector<CompositeRedirect> composite_redirects_;
    std::vector<PresentSubscription> present_subscriptions_;
    std::deque<PresentOperation> present_operations_;
    std::vector<XkbEventSelection> xkb_selections_;
    std::unordered_map<std::uint32_t, std::uint32_t> xkb_client_flags_;
    std::unordered_map<std::uint32_t, GraphicsContextRecord>
        graphics_contexts_;
    std::unordered_map<std::uint32_t, FontRecord> fonts_;
    std::unordered_map<std::uint32_t, DbeBufferRecord> dbe_buffers_;
    std::unordered_map<std::uint32_t, std::shared_ptr<SharedMemory>>
        shared_memory_;
    std::unordered_map<std::uint32_t, SyncCounterRecord> sync_counters_;
    std::unordered_map<std::uint32_t, SyncAlarmRecord> sync_alarms_;
    std::unordered_map<std::uint32_t, SyncFenceRecord> sync_fences_;
    std::unordered_map<std::uint32_t, std::shared_ptr<RenderPicture>>
        render_pictures_;
    std::unordered_map<std::uint32_t, RenderGlyphSet> render_glyph_sets_;
    std::unordered_map<std::uint32_t, CursorRecord> cursors_;
    std::unordered_map<std::uint32_t, Region> xfixes_regions_;
    std::unordered_map<std::uint32_t, XFixesBarrierRecord> xfixes_barriers_;
    std::vector<XFixesSelectionSubscription> xfixes_selection_inputs_;
    std::vector<XFixesCursorSubscription> xfixes_cursor_inputs_;
    std::unordered_map<std::uint32_t, std::uint32_t> cursor_hide_counts_;
    std::unordered_map<std::uint32_t, std::vector<SaveSetEntry>> save_sets_;
    std::unordered_map<std::uint32_t, std::vector<SyncWaitCondition>>
        sync_counter_waits_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
        sync_fence_waits_;
    std::unordered_map<std::uint32_t, std::int32_t> sync_priorities_;
    RandrState randr_;
    std::unordered_map<AtomId, SelectionRecord> selections_;
    std::unordered_map<std::uint32_t, std::deque<ClientEvent>> event_queues_;
    std::unordered_set<std::uint32_t> clients_to_terminate_;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> clients_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::shared_ptr<Surface> composited_root_;
    std::shared_ptr<SurfaceBudget> surface_budget_;
    std::size_t property_bytes_ = 0;
    std::size_t pending_events_ = 0;
    std::uint32_t current_time_ = 1;
    std::uint32_t installed_colormap_ = default_colormap_id;
    std::uint32_t server_grab_owner_ = 0;
    ScreenSaverState screen_saver_;
    bool access_control_enabled_ = true;
    Clock &clock_;
    Clock::time_point present_epoch_;
    InputState input_;
    std::optional<KeyRepeat> key_repeat_;
    std::vector<PassiveGrab> passive_grabs_;
    std::vector<Xi2EventSelection> xi2_selections_;
    std::array<std::unordered_map<AtomId, PropertyValue>, 2>
        xi2_properties_;
    std::shared_ptr<CursorImage> displayed_cursor_;
    std::uint32_t next_cursor_serial_ = 1;
    bool scene_dirty_ = true;

    [[nodiscard]] bool can_queue_event(std::uint32_t client) const;
    [[nodiscard]] bool queue_event(std::uint32_t client, ClientEvent event);
    [[nodiscard]] bool queue_events_atomically(
        const std::vector<PlannedEvent> &events);
    [[nodiscard]] EventDelivery queue_property_notifications(
        const WindowRecord &window, const AtomId *properties,
        std::size_t count, std::uint8_t state);
    [[nodiscard]] bool append_damage_notifications(
        const DamageRecord &damage, const Region &reported,
        bool full_area, std::vector<PlannedEvent> &events) const;
    [[nodiscard]] bool apply_damage(
        DamageRecord &damage, const Region &added,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] std::optional<std::uint64_t> present_target_msc(
        std::uint64_t requested, std::uint64_t divisor,
        std::uint64_t remainder, std::uint32_t options) const noexcept;
    [[nodiscard]] bool present_wait_ready(
        const PresentOperation &operation) const noexcept;
    [[nodiscard]] PresentUpdate execute_present(
        PresentOperation &operation, std::uint64_t msc,
        std::uint64_t ust);
    [[nodiscard]] bool append_present_complete_events(
        std::uint32_t window, std::uint32_t serial,
        std::uint8_t kind, std::uint64_t msc, std::uint64_t ust,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] bool append_present_configure_events(
        std::uint32_t window, std::int16_t x, std::int16_t y,
        std::uint16_t width, std::uint16_t height,
        std::vector<PlannedEvent> &events) const;
    void replace_window_surface(
        WindowRecord &window, std::shared_ptr<Surface> replacement) noexcept;
    [[nodiscard]] std::shared_ptr<Surface> adopt_replacement_surface(
        Surface surface, std::size_t released_bytes);
    [[nodiscard]] bool append_randr_events(
        const RandrState &candidate, std::uint16_t width,
        std::uint16_t height, std::uint16_t notify_mask,
        AtomId property, std::uint8_t property_status,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] SyncUpdate update_sync_counter(
        SyncCounterRecord &counter, std::int64_t value,
        bool destroying);
    [[nodiscard]] SyncUpdate commit_sync_alarm(
        SyncAlarmRecord alarm, bool creating);
    [[nodiscard]] bool append_sync_alarm_event(
        const SyncAlarmRecord &alarm, std::int64_t counter_value,
        std::int64_t alarm_value, std::uint8_t state,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] std::uint16_t client_sequence(
        std::uint32_t client) const noexcept;
    [[nodiscard]] std::shared_ptr<CursorImage> current_cursor_for(
        std::uint32_t pointer_window,
        const ActiveGrab *pointer_grab) const noexcept;
    [[nodiscard]] EventDelivery append_cursor_change(
        const std::shared_ptr<CursorImage> &cursor,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] std::uint32_t deepest_window_at(
        std::uint32_t parent, std::int32_t x, std::int32_t y) const;
    [[nodiscard]] EventDelivery route_input_event(
        CoreInputEvent event, std::uint32_t mask,
        std::uint32_t source, std::uint32_t propagation_stop,
        std::uint32_t pointer_window, const ActiveGrab *grab,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] EventDelivery repeat_key(std::uint8_t detail);
    [[nodiscard]] std::uint8_t xkb_base_modifiers(
        const std::array<std::uint8_t, 32> &keys) const noexcept;
    [[nodiscard]] XkbStateSnapshot xkb_state_for(
        const std::array<std::uint8_t, 32> &keys,
        const XkbKeyboardState &state) const noexcept;
    [[nodiscard]] bool append_xkb_state_events(
        const XkbStateSnapshot &before,
        const XkbStateSnapshot &after,
        std::uint8_t keycode, std::uint8_t event_type,
        std::uint8_t request_major, std::uint8_t request_minor,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] bool append_xi2_input_events(
        std::uint8_t type, std::uint8_t detail, std::uint8_t raw_detail,
        std::uint32_t source_window, std::int32_t root_x,
        std::int32_t root_y, std::uint16_t state,
        const XkbStateSnapshot &xkb,
        std::uint32_t flags, const ActiveGrab *grab,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] bool xi2_mask_selected(
        const Xi2EventSelection &selection, std::uint16_t device,
        std::uint16_t event_type) const noexcept;
    [[nodiscard]] EventDelivery append_crossing_events(
        std::uint32_t from, std::uint32_t to,
        std::int32_t root_x, std::int32_t root_y,
        std::uint16_t state, std::uint8_t mode,
        const ActiveGrab *grab, const FocusState &focus,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] EventDelivery append_focus_events(
        const FocusState &from, const FocusState &to,
        std::uint8_t mode, std::uint32_t pointer_window,
        std::vector<PlannedEvent> &events) const;
    [[nodiscard]] EventDelivery update_window_mappings(
        const std::uint32_t *windows, std::size_t count, bool mapped);
    [[nodiscard]] FocusState reverted_focus_state(
        std::uint32_t unavailable = 0) const noexcept;
    void erase_window_tree(std::uint32_t id) noexcept;
    void refresh_modifier_button_mask() noexcept;
    void clear_selections_for_window(std::uint32_t window);
    void apply_save_set(std::uint32_t owner);
    void constrain_pointer_by_barriers(std::int32_t old_x,
                                       std::int32_t old_y,
                                       std::int32_t &new_x,
                                       std::int32_t &new_y) const noexcept;
    void revert_focus_from(std::uint32_t window) noexcept;
    void composite_scene();
    void composite_scene(const Rectangle &area);
    void composite_window(std::uint32_t id, std::int64_t parent_x,
                          std::int64_t parent_y, std::int64_t clip_left,
                          std::int64_t clip_top, std::int64_t clip_right,
                          std::int64_t clip_bottom);
    [[nodiscard]] bool is_descendant(std::uint32_t candidate,
                                     std::uint32_t ancestor) const;
};

} // namespace xmin::server

#endif
