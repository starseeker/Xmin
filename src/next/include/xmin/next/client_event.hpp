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
};

struct ClientMessageEvent {
    std::uint8_t format = 0;
    std::uint32_t window = 0;
    std::uint32_t type = 0;
    std::array<std::uint32_t, 20> data{};
};

using ClientEvent = std::variant<SelectionClearEvent, ClientMessageEvent>;

} // namespace xmin::next

#endif
