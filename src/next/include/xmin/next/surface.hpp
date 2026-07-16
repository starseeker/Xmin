#ifndef XMIN_NEXT_SURFACE_HPP
#define XMIN_NEXT_SURFACE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace xmin::next {

constexpr std::size_t maximum_surface_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_server_surface_bytes = 256U * 1024U * 1024U;

struct Rectangle {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

class Surface {
public:
    static std::optional<Surface>
    create(std::uint16_t width, std::uint16_t height, std::uint8_t depth);

    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint8_t depth() const noexcept { return depth_; }
    [[nodiscard]] std::size_t storage_bytes() const noexcept
    {
        return pixels_.size() * sizeof(std::uint32_t);
    }

    bool resize(std::uint16_t width, std::uint16_t height);
    void fill(const Rectangle &rectangle, std::uint32_t source,
              std::uint8_t function, std::uint32_t plane_mask);
    void copy_from(const Surface &source, std::int16_t source_x,
                   std::int16_t source_y, std::int16_t destination_x,
                   std::int16_t destination_y, std::uint16_t width,
                   std::uint16_t height, std::uint8_t function,
                   std::uint32_t plane_mask);
    [[nodiscard]] std::uint32_t pixel(std::uint16_t x,
                                      std::uint16_t y) const noexcept;

private:
    Surface(std::uint16_t width, std::uint16_t height, std::uint8_t depth,
            std::vector<std::uint32_t> pixels);

    [[nodiscard]] std::uint32_t depth_mask() const noexcept;
    void store(std::size_t index, std::uint32_t source, std::uint8_t function,
               std::uint32_t plane_mask) noexcept;

    std::uint16_t width_;
    std::uint16_t height_;
    std::uint8_t depth_;
    std::vector<std::uint32_t> pixels_;
};

} // namespace xmin::next

#endif
