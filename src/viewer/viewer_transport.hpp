#ifndef XMIN_VIEWER_TRANSPORT_HPP
#define XMIN_VIEWER_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace xmin::viewer {

struct FrameView {
    const std::uint8_t *pixels = nullptr;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t stride = 0;
    // More disjoint rectangles from the same guest update follow.  The host
    // uploads them all before presenting the completed texture.
    bool more = false;
};

class GuestTransport {
public:
    virtual ~GuestTransport() = default;

    [[nodiscard]] virtual std::uint16_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t height() const noexcept = 0;
    [[nodiscard]] virtual bool using_shared_memory() const noexcept = 0;
    [[nodiscard]] virtual const std::string &error() const noexcept = 0;

    // Returns true when the guest framebuffer may have changed.  Transports
    // without change notifications may conservatively return true.
    virtual bool frame_pending() = 0;
    // Captures one pending dirty rectangle. FrameView::more tells the caller
    // to drain the remaining rectangles before presenting.
    virtual bool capture(FrameView &frame) = 0;
    virtual void key(std::uint8_t keycode, bool pressed) = 0;
    virtual void button(std::uint8_t button, bool pressed) = 0;
    virtual void pointer(std::int16_t x, std::int16_t y) = 0;
    virtual void flush() = 0;
};

[[nodiscard]] std::unique_ptr<GuestTransport>
create_guest_transport(const std::string &display,
                       const std::string &authority,
                       bool allow_shared_memory, std::string &error);

} // namespace xmin::viewer

#endif
