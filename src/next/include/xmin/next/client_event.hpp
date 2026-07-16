#ifndef XMIN_NEXT_CLIENT_EVENT_HPP
#define XMIN_NEXT_CLIENT_EVENT_HPP

#include <array>
#include <cstdint>
#include <variant>

namespace xmin::next {

struct SelectionClearEvent {
    std::uint32_t time = 0;
    std::uint32_t window = 0;
    std::uint32_t selection = 0;
    std::uint16_t sequence = 0;
};

struct ClientMessageEvent {
    std::uint8_t format = 0;
    std::uint32_t window = 0;
    std::uint32_t type = 0;
    std::array<std::uint32_t, 20> data{};
    std::uint16_t sequence = 0;
};

struct MappingNotifyEvent {
    std::uint8_t request = 0;
    std::uint8_t first_keycode = 0;
    std::uint8_t count = 0;
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
    SelectionClearEvent, ClientMessageEvent, MappingNotifyEvent,
    ShapeNotifyEvent, SyncCounterNotifyEvent, SyncAlarmNotifyEvent,
    XFixesSelectionNotifyEvent, XFixesCursorNotifyEvent, CoreInputEvent,
    CrossingEvent, FocusEvent>;

} // namespace xmin::next

#endif
