#ifndef XMIN_SERVER_RENDER_HPP
#define XMIN_SERVER_RENDER_HPP

#include "xmin/server/region.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace xmin::server {

class ServerState;
class Surface;
struct RenderPicture;

constexpr std::uint32_t render_argb32_format = 1;
constexpr std::uint32_t render_xrgb32_format = 2;
constexpr std::uint32_t render_a8_format = 3;
constexpr std::uint32_t render_a1_format = 4;
constexpr std::size_t maximum_render_gradient_stops = 4096;
constexpr std::size_t maximum_render_glyphs_per_set = 65536;
constexpr std::size_t maximum_render_glyph_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_server_render_glyph_bytes =
    64U * 1024U * 1024U;

struct RenderDirectFormat {
    std::uint16_t red_shift = 0;
    std::uint16_t red_mask = 0;
    std::uint16_t green_shift = 0;
    std::uint16_t green_mask = 0;
    std::uint16_t blue_shift = 0;
    std::uint16_t blue_mask = 0;
    std::uint16_t alpha_shift = 0;
    std::uint16_t alpha_mask = 0;
};

struct RenderFormat {
    std::uint32_t id = 0;
    std::uint8_t depth = 0;
    RenderDirectFormat direct;
};

[[nodiscard]] const std::array<RenderFormat, 4> &render_formats() noexcept;
[[nodiscard]] const RenderFormat *render_format(std::uint32_t id) noexcept;
[[nodiscard]] const RenderFormat *render_format_for_depth(
    std::uint8_t depth) noexcept;
[[nodiscard]] bool render_operator_valid(std::uint8_t operation) noexcept;

struct RenderColor {
    std::uint16_t red = 0;
    std::uint16_t green = 0;
    std::uint16_t blue = 0;
    std::uint16_t alpha = 0;
};

struct RenderPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct RenderLine {
    RenderPoint p1;
    RenderPoint p2;
};

struct RenderTrapezoid {
    std::int32_t top = 0;
    std::int32_t bottom = 0;
    RenderLine left;
    RenderLine right;
};

struct RenderTriangle {
    RenderPoint p1;
    RenderPoint p2;
    RenderPoint p3;
};

struct RenderGradientStop {
    std::int32_t position = 0;
    RenderColor color;
};

enum class RenderRepeat : std::uint8_t {
    none,
    normal,
    pad,
    reflect,
};

enum class RenderFilter : std::uint8_t {
    nearest,
    bilinear,
};

struct RenderPictureAttributes {
    RenderRepeat repeat = RenderRepeat::none;
    std::uint32_t alpha_map = 0;
    std::shared_ptr<RenderPicture> alpha_map_picture;
    std::int32_t alpha_x_origin = 0;
    std::int32_t alpha_y_origin = 0;
    std::int32_t clip_x_origin = 0;
    std::int32_t clip_y_origin = 0;
    std::optional<Region> clip;
    bool graphics_exposures = false;
    std::uint8_t subwindow_mode = 0;
    std::uint8_t poly_edge = 0;
    std::uint8_t poly_mode = 0;
    std::uint32_t dither = 0;
    bool component_alpha = false;
    std::array<std::int32_t, 9> transform{
        65536, 0, 0,
        0, 65536, 0,
        0, 0, 65536};
    RenderFilter filter = RenderFilter::nearest;
};

struct RenderDrawableSource {
    RenderDrawableSource() = default;
    explicit RenderDrawableSource(std::uint32_t id) noexcept : drawable(id) {}

    std::uint32_t drawable = 0;
    std::shared_ptr<Surface> surface;
    bool pixmap = false;
};

struct RenderSolidSource {
    RenderColor color;
};

struct RenderLinearGradient {
    RenderPoint p1;
    RenderPoint p2;
    std::vector<RenderGradientStop> stops;
};

struct RenderRadialGradient {
    RenderPoint inner;
    RenderPoint outer;
    std::int32_t inner_radius = 0;
    std::int32_t outer_radius = 0;
    std::vector<RenderGradientStop> stops;
};

struct RenderConicalGradient {
    RenderPoint center;
    std::int32_t angle = 0;
    std::vector<RenderGradientStop> stops;
};

using RenderPictureSource = std::variant<
    RenderDrawableSource, RenderSolidSource, RenderLinearGradient,
    RenderRadialGradient, RenderConicalGradient>;

struct RenderPicture {
    std::uint32_t id = 0;
    std::uint32_t format = render_argb32_format;
    RenderPictureSource source = RenderSolidSource{};
    RenderPictureAttributes attributes;
};

struct RenderGlyphInfo {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t x_offset = 0;
    std::int16_t y_offset = 0;
};

struct RenderGlyph {
    RenderGlyphInfo info;
    std::vector<std::uint32_t> pixels;
};

struct RenderGlyphStorage {
    std::uint32_t format = 0;
    std::unordered_map<std::uint32_t, RenderGlyph> glyphs;
    std::size_t bytes = 0;
};

struct RenderGlyphSet {
    std::uint32_t id = 0;
    std::shared_ptr<RenderGlyphStorage> storage;
};

struct RenderGlyphRun {
    std::int16_t x_offset = 0;
    std::int16_t y_offset = 0;
    std::uint32_t glyph_set = 0;
    std::vector<std::uint32_t> glyphs;
};

struct RenderSpan {
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::int32_t y = 0;
};

struct RenderTrap {
    RenderSpan top;
    RenderSpan bottom;
};

enum class RenderStatus {
    success,
    bad_picture,
    bad_format,
    bad_operator,
    bad_glyph_set,
    bad_glyph,
    bad_drawable,
    bad_match,
    bad_alloc,
};

class RenderEngine {
public:
    explicit RenderEngine(ServerState &server) noexcept : server_(server) {}

    [[nodiscard]] RenderStatus composite(
        std::uint8_t operation, std::uint32_t source,
        std::uint32_t mask, std::uint32_t destination,
        std::int32_t source_x, std::int32_t source_y,
        std::int32_t mask_x, std::int32_t mask_y,
        std::int32_t destination_x, std::int32_t destination_y,
        std::uint32_t width, std::uint32_t height);
    [[nodiscard]] RenderStatus fill_rectangles(
        std::uint8_t operation, std::uint32_t destination,
        const RenderColor &color, const std::vector<Rectangle> &rectangles);
    [[nodiscard]] RenderStatus composite_trapezoids(
        std::uint8_t operation, std::uint32_t source,
        std::uint32_t destination, std::uint32_t mask_format,
        std::int32_t source_x, std::int32_t source_y,
        const std::vector<RenderTrapezoid> &trapezoids);
    [[nodiscard]] RenderStatus composite_triangles(
        std::uint8_t operation, std::uint32_t source,
        std::uint32_t destination, std::uint32_t mask_format,
        std::int32_t source_x, std::int32_t source_y,
        const std::vector<RenderTriangle> &triangles);
    [[nodiscard]] RenderStatus composite_glyphs(
        std::uint8_t operation, std::uint32_t source,
        std::uint32_t destination, std::uint32_t mask_format,
        std::int32_t source_x, std::int32_t source_y,
        const std::vector<RenderGlyphRun> &runs);
    [[nodiscard]] RenderStatus add_traps(
        std::uint32_t picture, std::int16_t x_offset,
        std::int16_t y_offset, const std::vector<RenderTrap> &traps);
    [[nodiscard]] RenderStatus snapshot(
        std::uint32_t picture, std::uint16_t &width,
        std::uint16_t &height, std::vector<std::uint32_t> &pixels);

private:
    [[nodiscard]] RenderStatus finish_draw(std::uint32_t picture);
    ServerState &server_;
};

} // namespace xmin::server

#endif
