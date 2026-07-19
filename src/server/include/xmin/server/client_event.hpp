#ifndef XMIN_SERVER_CLIENT_EVENT_HPP
#define XMIN_SERVER_CLIENT_EVENT_HPP

#include <array>
#include <cstdint>
#include <variant>

namespace xmin::server {

struct SelectionClearEvent {
    std::uint32_t time = 0;
    std::uint32_t window = 0;
    std::uint32_t selection = 0;
    std::uint16_t sequence = 0;
};

struct SelectionRequestEvent {
    std::uint32_t time = 0;
    std::uint32_t owner = 0;
    std::uint32_t requestor = 0;
    std::uint32_t selection = 0;
    std::uint32_t target = 0;
    std::uint32_t property = 0;
    std::uint16_t sequence = 0;
};

struct SelectionNotifyEvent {
    std::uint32_t time = 0;
    std::uint32_t requestor = 0;
    std::uint32_t selection = 0;
    std::uint32_t target = 0;
    std::uint32_t property = 0;
    std::uint16_t sequence = 0;
};

struct ClientMessageEvent {
    std::uint8_t format = 0;
    std::uint32_t window = 0;
    std::uint32_t type = 0;
    std::array<std::uint32_t, 20> data{};
    std::uint16_t sequence = 0;
};

struct PropertyNotifyEvent {
    std::uint8_t state = 0;
    std::uint32_t window = 0;
    std::uint32_t atom = 0;
    std::uint32_t time = 0;
    std::uint16_t sequence = 0;
};

struct MappingNotifyEvent {
    std::uint8_t request = 0;
    std::uint8_t first_keycode = 0;
    std::uint8_t count = 0;
    std::uint16_t sequence = 0;
};

struct CreateNotifyEvent {
    std::uint32_t parent = 0;
    std::uint32_t window = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t border_width = 0;
    bool override_redirect = false;
    std::uint16_t sequence = 0;
};

struct MapNotifyEvent {
    std::uint32_t event = 0;
    std::uint32_t window = 0;
    bool override_redirect = false;
    std::uint16_t sequence = 0;
};

struct MapRequestEvent {
    std::uint32_t parent = 0;
    std::uint32_t window = 0;
    std::uint16_t sequence = 0;
};

struct VisibilityNotifyEvent {
    std::uint32_t window = 0;
    std::uint8_t state = 0;
    std::uint16_t sequence = 0;
};

struct ExposeEvent {
    std::uint32_t window = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t count = 0;
    std::uint16_t sequence = 0;
};

struct ConfigureNotifyEvent {
    std::uint32_t event = 0;
    std::uint32_t window = 0;
    std::uint32_t above_sibling = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t border_width = 0;
    bool override_redirect = false;
    std::uint16_t sequence = 0;
};

struct ConfigureRequestEvent {
    std::uint8_t stack_mode = 0;
    std::uint32_t parent = 0;
    std::uint32_t window = 0;
    std::uint32_t sibling = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t border_width = 0;
    std::uint16_t value_mask = 0;
    std::uint16_t sequence = 0;
};

struct ResizeRequestEvent {
    std::uint32_t window = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t sequence = 0;
};

struct CirculateNotifyEvent {
    std::uint32_t event = 0;
    std::uint32_t window = 0;
    std::uint8_t place = 0;
    std::uint16_t sequence = 0;
};

struct CirculateRequestEvent {
    std::uint32_t parent = 0;
    std::uint32_t window = 0;
    std::uint8_t place = 0;
    std::uint16_t sequence = 0;
};

struct UnmapNotifyEvent {
    std::uint32_t event = 0;
    std::uint32_t window = 0;
    bool from_configure = false;
    std::uint16_t sequence = 0;
};

struct ShapeNotifyEvent {
    std::uint8_t kind = 0;
    std::uint32_t window = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t time = 0;
    bool shaped = false;
    std::uint16_t sequence = 0;
};

struct SyncCounterNotifyEvent {
    std::uint32_t counter = 0;
    std::int64_t wait_value = 0;
    std::int64_t counter_value = 0;
    std::uint32_t time = 0;
    std::uint16_t count = 0;
    bool destroyed = false;
    std::uint16_t sequence = 0;
};

struct SyncAlarmNotifyEvent {
    std::uint32_t alarm = 0;
    std::int64_t counter_value = 0;
    std::int64_t alarm_value = 0;
    std::uint32_t time = 0;
    std::uint8_t state = 0;
    std::uint16_t sequence = 0;
};

struct XFixesSelectionNotifyEvent {
    std::uint8_t subtype = 0;
    std::uint32_t window = 0;
    std::uint32_t owner = 0;
    std::uint32_t selection = 0;
    std::uint32_t time = 0;
    std::uint32_t selection_time = 0;
    std::uint16_t sequence = 0;
};

struct XFixesCursorNotifyEvent {
    std::uint8_t subtype = 0;
    std::uint32_t window = 0;
    std::uint32_t cursor_serial = 0;
    std::uint32_t time = 0;
    std::uint32_t name = 0;
    std::uint16_t sequence = 0;
};

struct RandrScreenChangeNotifyEvent {
    std::uint32_t timestamp = 0;
    std::uint32_t config_timestamp = 0;
    std::uint32_t request_window = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t millimetre_width = 0;
    std::uint16_t millimetre_height = 0;
    std::uint16_t rotation = 1;
    std::uint16_t sequence = 0;
};

struct RandrNotifyEvent {
    std::uint8_t subtype = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t config_timestamp = 0;
    std::uint32_t window = 0;
    std::uint32_t crtc = 0;
    std::uint32_t output = 0;
    std::uint32_t mode = 0;
    std::uint32_t atom = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t rotation = 1;
    std::uint8_t connection = 0;
    std::uint8_t subpixel_order = 0;
    std::uint8_t property_status = 0;
    std::uint16_t sequence = 0;
};

struct DamageNotifyEvent {
    std::uint8_t level = 0;
    std::uint32_t drawable = 0;
    std::uint32_t damage = 0;
    std::uint32_t timestamp = 0;
    std::int16_t area_x = 0;
    std::int16_t area_y = 0;
    std::uint16_t area_width = 0;
    std::uint16_t area_height = 0;
    std::int16_t geometry_x = 0;
    std::int16_t geometry_y = 0;
    std::uint16_t geometry_width = 0;
    std::uint16_t geometry_height = 0;
    std::uint16_t sequence = 0;
};

struct PresentConfigureNotifyEvent {
    std::uint32_t event = 0;
    std::uint32_t window = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t off_x = 0;
    std::int16_t off_y = 0;
    std::uint16_t pixmap_width = 0;
    std::uint16_t pixmap_height = 0;
    std::uint32_t pixmap_flags = 0;
    std::uint16_t sequence = 0;
};

struct PresentCompleteNotifyEvent {
    std::uint8_t kind = 0;
    std::uint8_t mode = 0;
    std::uint32_t event = 0;
    std::uint32_t window = 0;
    std::uint32_t serial = 0;
    std::uint64_t ust = 0;
    std::uint64_t msc = 0;
    std::uint16_t sequence = 0;
};

struct PresentIdleNotifyEvent {
    std::uint32_t event = 0;
    std::uint32_t window = 0;
    std::uint32_t serial = 0;
    std::uint32_t pixmap = 0;
    std::uint32_t idle_fence = 0;
    std::uint16_t sequence = 0;
};

struct XkbStateNotifyEvent {
    std::uint32_t time = 0;
    std::uint8_t device = 3;
    std::uint8_t mods = 0;
    std::uint8_t base_mods = 0;
    std::uint8_t latched_mods = 0;
    std::uint8_t locked_mods = 0;
    std::uint8_t group = 0;
    std::int16_t base_group = 0;
    std::int16_t latched_group = 0;
    std::uint8_t locked_group = 0;
    std::uint16_t pointer_buttons = 0;
    std::uint16_t changed = 0;
    std::uint8_t keycode = 0;
    std::uint8_t event_type = 0;
    std::uint8_t request_major = 0;
    std::uint8_t request_minor = 0;
    std::uint16_t sequence = 0;
};

struct XkbMapNotifyEvent {
    std::uint32_t time = 0;
    std::uint8_t device = 3;
    std::uint16_t changed = 0;
    std::uint8_t min_keycode = 8;
    std::uint8_t max_keycode = 255;
    std::uint8_t first_type = 0;
    std::uint8_t type_count = 0;
    std::uint8_t first_keysym = 0;
    std::uint8_t keysym_count = 0;
    std::uint8_t first_modmap = 0;
    std::uint8_t modmap_count = 0;
    std::uint16_t sequence = 0;
};

struct XkbControlsNotifyEvent {
    std::uint32_t time = 0;
    std::uint8_t device = 3;
    std::uint8_t groups = 1;
    std::uint32_t changed = 0;
    std::uint32_t enabled = 0;
    std::uint32_t enabled_changes = 0;
    std::uint8_t keycode = 0;
    std::uint8_t event_type = 0;
    std::uint8_t request_major = 0;
    std::uint8_t request_minor = 0;
    std::uint16_t sequence = 0;
};

struct Xi2DeviceEvent {
    std::uint16_t event_type = 0;
    std::uint16_t device = 0;
    std::uint16_t source = 0;
    std::uint32_t time = 0;
    std::uint32_t detail = 0;
    std::uint32_t root = 0;
    std::uint32_t event = 0;
    std::uint32_t child = 0;
    std::int32_t root_x = 0;
    std::int32_t root_y = 0;
    std::int32_t event_x = 0;
    std::int32_t event_y = 0;
    std::uint32_t buttons = 0;
    std::uint8_t base_mods = 0;
    std::uint8_t latched_mods = 0;
    std::uint8_t locked_mods = 0;
    std::uint8_t effective_mods = 0;
    std::uint8_t base_group = 0;
    std::uint8_t latched_group = 0;
    std::uint8_t locked_group = 0;
    std::uint8_t effective_group = 0;
    std::uint32_t flags = 0;
    std::uint16_t sequence = 0;
};

struct Xi2RawEvent {
    std::uint16_t event_type = 0;
    std::uint16_t device = 0;
    std::uint16_t source = 0;
    std::uint32_t time = 0;
    std::uint32_t detail = 0;
    std::int32_t root_x = 0;
    std::int32_t root_y = 0;
    std::uint32_t flags = 0;
    std::uint16_t sequence = 0;
};

struct Xi2PropertyEvent {
    std::uint16_t device = 0;
    std::uint32_t time = 0;
    std::uint32_t property = 0;
    std::uint8_t what = 0;
    std::uint16_t sequence = 0;
};

struct Xi2CrossingEvent {
    std::uint16_t event_type = 0;
    std::uint16_t device = 0;
    std::uint16_t source = 0;
    std::uint32_t time = 0;
    std::uint8_t mode = 0;
    std::uint8_t detail = 0;
    std::uint32_t root = 0;
    std::uint32_t event = 0;
    std::uint32_t child = 0;
    std::int32_t root_x = 0;
    std::int32_t root_y = 0;
    std::int32_t event_x = 0;
    std::int32_t event_y = 0;
    std::uint32_t buttons = 0;
    std::uint8_t base_mods = 0;
    std::uint8_t latched_mods = 0;
    std::uint8_t locked_mods = 0;
    std::uint8_t effective_mods = 0;
    std::uint8_t base_group = 0;
    std::uint8_t latched_group = 0;
    std::uint8_t locked_group = 0;
    std::uint8_t effective_group = 0;
    bool same_screen = true;
    bool focus = false;
    std::uint16_t sequence = 0;
};

struct CoreInputEvent {
    std::uint8_t type = 0;
    std::uint8_t detail = 0;
    std::uint32_t time = 0;
    std::uint32_t root = 0;
    std::uint32_t event = 0;
    std::uint32_t child = 0;
    std::int16_t root_x = 0;
    std::int16_t root_y = 0;
    std::int16_t event_x = 0;
    std::int16_t event_y = 0;
    std::uint16_t state = 0;
    bool same_screen = true;
    std::uint16_t sequence = 0;
};

struct CrossingEvent {
    std::uint8_t type = 0;
    std::uint8_t detail = 0;
    std::uint32_t time = 0;
    std::uint32_t root = 0;
    std::uint32_t event = 0;
    std::uint32_t child = 0;
    std::int16_t root_x = 0;
    std::int16_t root_y = 0;
    std::int16_t event_x = 0;
    std::int16_t event_y = 0;
    std::uint16_t state = 0;
    std::uint8_t mode = 0;
    bool same_screen = true;
    bool focus = false;
    std::uint16_t sequence = 0;
};

struct FocusEvent {
    std::uint8_t type = 0;
    std::uint8_t detail = 0;
    std::uint32_t event = 0;
    std::uint8_t mode = 0;
    std::uint16_t sequence = 0;
};

using ClientEvent = std::variant<
    SelectionClearEvent, SelectionRequestEvent, SelectionNotifyEvent,
    ClientMessageEvent, PropertyNotifyEvent, MappingNotifyEvent,
    CreateNotifyEvent, MapNotifyEvent, MapRequestEvent, UnmapNotifyEvent,
    VisibilityNotifyEvent, ExposeEvent,
    ConfigureNotifyEvent, ConfigureRequestEvent, ResizeRequestEvent,
    CirculateNotifyEvent, CirculateRequestEvent, ShapeNotifyEvent,
    SyncCounterNotifyEvent, SyncAlarmNotifyEvent,
    XFixesSelectionNotifyEvent, XFixesCursorNotifyEvent,
    RandrScreenChangeNotifyEvent, RandrNotifyEvent, DamageNotifyEvent,
    PresentConfigureNotifyEvent, PresentCompleteNotifyEvent,
    PresentIdleNotifyEvent, XkbMapNotifyEvent, XkbStateNotifyEvent,
    XkbControlsNotifyEvent, Xi2DeviceEvent, Xi2RawEvent, Xi2PropertyEvent,
    Xi2CrossingEvent,
    CoreInputEvent,
    CrossingEvent, FocusEvent>;

} // namespace xmin::server

#endif
