#ifndef XMIN_SERVER_SURFACE_HPP
#define XMIN_SERVER_SURFACE_HPP

#include "xmin/server/region.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace xmin::server {

class SharedMemory;

constexpr std::size_t maximum_surface_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_server_surface_bytes = 256U * 1024U * 1024U;

class Surface {
public:
    static std::optional<Surface>
    create(std::uint16_t width, std::uint16_t height, std::uint8_t depth);
    static std::optional<Surface> create_window_backing(
        std::uint16_t width, std::uint16_t height, std::uint8_t depth,
        std::uint16_t viewport_width, std::uint16_t viewport_height);
    static std::optional<Surface> create_shared(
        std::uint16_t width, std::uint16_t height, std::uint8_t depth,
        std::shared_ptr<SharedMemory> memory, std::size_t offset);

    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint8_t depth() const noexcept { return depth_; }
    [[nodiscard]] std::size_t storage_bytes() const noexcept;
    [[nodiscard]] bool has_contiguous_storage() const noexcept;

    bool resize(std::uint16_t width, std::uint16_t height);
    void fill(const Rectangle &rectangle, std::uint32_t source,
              std::uint8_t function, std::uint32_t plane_mask,
              ClipView clip = {});
    void draw_pixel(std::int32_t x, std::int32_t y, std::uint32_t source,
                    std::uint8_t function,
                    std::uint32_t plane_mask, ClipView clip = {}) noexcept;
    void draw_line(std::int32_t start_x, std::int32_t start_y,
                   std::int32_t end_x, std::int32_t end_y,
                   std::uint32_t source, std::uint8_t function,
                   std::uint32_t plane_mask, ClipView clip = {}) noexcept;
    void copy_from(const Surface &source, std::int32_t source_x,
                   std::int32_t source_y, std::int32_t destination_x,
                   std::int32_t destination_y, std::uint32_t width,
                   std::uint32_t height, std::uint8_t function,
                   std::uint32_t plane_mask, ClipView clip = {});
    void copy_plane_from(const Surface &source, std::int32_t source_x,
                         std::int32_t source_y,
                         std::int32_t destination_x,
                         std::int32_t destination_y, std::uint32_t width,
                         std::uint32_t height, std::uint32_t bit_plane,
                         std::uint32_t foreground, std::uint32_t background,
                         std::uint8_t function, std::uint32_t plane_mask,
                         ClipView clip = {});
    [[nodiscard]] std::uint32_t pixel(std::uint16_t x,
                                      std::uint16_t y) const noexcept;
    [[nodiscard]] std::uint32_t *data() noexcept
    {
        if (sparse_)
            return nullptr;
        return shared_pixels_ != nullptr ? shared_pixels_ : pixels_.data();
    }
    [[nodiscard]] const std::uint32_t *data() const noexcept
    {
        if (sparse_)
            return nullptr;
        return shared_pixels_ != nullptr ? shared_pixels_ : pixels_.data();
    }
    [[nodiscard]] std::size_t stride_bytes() const noexcept
    {
        return static_cast<std::size_t>(width_) * sizeof(std::uint32_t);
    }

private:
    Surface(std::uint16_t width, std::uint16_t height, std::uint8_t depth,
            std::vector<std::uint32_t> pixels);
    Surface(std::uint16_t width, std::uint16_t height, std::uint8_t depth,
            std::shared_ptr<SharedMemory> memory,
            std::uint32_t *shared_pixels) noexcept;
    Surface(std::uint16_t width, std::uint16_t height, std::uint8_t depth,
            std::uint16_t tile_columns,
            std::vector<std::uint32_t> tile_indices,
            std::vector<std::uint32_t> slot_tiles,
            std::vector<std::uint32_t> pixels) noexcept;

    [[nodiscard]] std::uint32_t depth_mask() const noexcept;
    [[nodiscard]] std::uint32_t allocate_sparse_tile(
        std::uint16_t x, std::uint16_t y) noexcept;
    void reset_sparse(std::uint32_t value) noexcept;
    void store(std::uint16_t x, std::uint16_t y,
               std::uint32_t source, std::uint8_t function,
               std::uint32_t plane_mask) noexcept;

    std::uint16_t width_;
    std::uint16_t height_;
    std::uint8_t depth_;
    std::vector<std::uint32_t> pixels_;
    std::shared_ptr<SharedMemory> shared_memory_;
    std::uint32_t *shared_pixels_ = nullptr;
    std::vector<std::uint32_t> tile_indices_;
    std::vector<std::uint32_t> slot_tiles_;
    std::uint32_t default_pixel_ = 0;
    std::uint32_t allocated_tiles_ = 0;
    std::uint32_t next_tile_slot_ = 0;
    std::uint16_t tile_columns_ = 0;
    bool sparse_ = false;
};

} // namespace xmin::server

#endif
