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

using ClientEvent = std::variant<
    SelectionClearEvent, ClientMessageEvent, MappingNotifyEvent>;

} // namespace xmin::next

#endif
