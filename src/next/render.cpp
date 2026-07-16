#include "xmin/next/render.hpp"

#include "xmin/next/checked.hpp"
#include "xmin/next/server_state.hpp"
#include "xmin/next/wire.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <pixman.h>
#include <utility>

namespace xmin::next {
namespace {

constexpr std::array<RenderFormat, 4> formats{{
    {render_argb32_format, 32, {16, 0xff, 8, 0xff, 0, 0xff, 24, 0xff}},
    {render_xrgb32_format, 24, {16, 0xff, 8, 0xff, 0, 0xff, 0, 0}},
    {render_a8_format, 8, {0, 0, 0, 0, 0, 0, 0, 0xff}},
    {render_a1_format, 1, {0, 0, 0, 0, 0, 0, 0, 1}},
}};

class UniqueImage {
public:
    explicit UniqueImage(pixman_image_t *image = nullptr) noexcept
        : image_(image)
    {}
    ~UniqueImage()
    {
        if (image_ != nullptr)
            pixman_image_unref(image_);
    }
    UniqueImage(const UniqueImage &) = delete;
    UniqueImage &operator=(const UniqueImage &) = delete;
    UniqueImage(UniqueImage &&other) noexcept
        : image_(std::exchange(other.image_, nullptr))
    {}
    UniqueImage &operator=(UniqueImage &&other) noexcept
    {
        if (this == &other)
            return *this;
        if (image_ != nullptr)
            pixman_image_unref(image_);
        image_ = std::exchange(other.image_, nullptr);
        return *this;
    }
    [[nodiscard]] pixman_image_t *get() const noexcept { return image_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return image_ != nullptr;
    }

private:
    pixman_image_t *image_;
};

std::optional<pixman_op_t>
render_operator(std::uint8_t operation) noexcept
{
    if (operation <= 13 ||
        (operation >= 16 && operation <= 27) ||
        (operation >= 32 && operation <= 43) ||
        (operation >= 48 && operation <= 62)) {
        return static_cast<pixman_op_t>(operation);
    }
    return std::nullopt;
}

pixman_repeat_t
pixman_repeat(RenderRepeat repeat) noexcept
{
    switch (repeat) {
    case RenderRepeat::none:
        return PIXMAN_REPEAT_NONE;
    case RenderRepeat::normal:
        return PIXMAN_REPEAT_NORMAL;
    case RenderRepeat::pad:
        return PIXMAN_REPEAT_PAD;
    case RenderRepeat::reflect:
        return PIXMAN_REPEAT_REFLECT;
    }
    return PIXMAN_REPEAT_NONE;
}

pixman_filter_t
pixman_filter(RenderFilter filter) noexcept
{
    return filter == RenderFilter::bilinear
        ? PIXMAN_FILTER_BILINEAR
        : PIXMAN_FILTER_NEAREST;
}

pixman_format_code_t
pixman_mask_format(std::uint32_t format) noexcept
{
    return format == render_a1_format ? PIXMAN_a1 : PIXMAN_a8;
}

class ImageView {
public:
    ~ImageView()
    {
        if (write_back_ && surface_ != nullptr) {
            const auto *bytes = reinterpret_cast<const std::uint8_t *>(
                staging_.data());
            for (std::uint16_t y = 0; y < surface_->height(); ++y) {
                for (std::uint16_t x = 0; x < surface_->width(); ++x) {
                    std::uint32_t value = 0;
                    if (surface_->depth() == 1) {
                        const auto *words = reinterpret_cast<
                            const std::uint32_t *>(bytes +
                            static_cast<std::size_t>(y) * staging_stride_);
                        const unsigned bit = x & 31U;
                        const std::uint32_t mask =
                            host_byte_order() == ByteOrder::little
                            ? std::uint32_t{1} << bit
                            : std::uint32_t{1} << (31U - bit);
                        value = (words[x / 32U] & mask) != 0 ? 1 : 0;
                    }
                    else {
                        value = bytes[
                            static_cast<std::size_t>(y) * staging_stride_ + x];
                    }
                    surface_->draw_pixel(x, y, value, 3, 0xffffffffU);
                }
            }
        }
    }

    ImageView(const ImageView &) = delete;
    ImageView &operator=(const ImageView &) = delete;

    [[nodiscard]] pixman_image_t *get() const noexcept { return image_.get(); }

    [[nodiscard]] static RenderStatus create(
        ServerState &server, std::uint32_t picture_id, bool writable,
        std::vector<const RenderPicture *> &ancestors,
        std::unique_ptr<ImageView> &result)
    {
        const auto *picture = server.render_picture(picture_id);
        if (picture == nullptr)
            return RenderStatus::bad_picture;
        return create(server, *picture, writable, ancestors, result);
    }

    [[nodiscard]] static RenderStatus create(
        ServerState &server, const RenderPicture &picture, bool writable,
        std::vector<const RenderPicture *> &ancestors,
        std::unique_ptr<ImageView> &result)
    {
        if (std::find(ancestors.begin(), ancestors.end(), &picture) !=
            ancestors.end()) {
            return RenderStatus::bad_match;
        }
        if (ancestors.size() == 64)
            return RenderStatus::bad_match;

        try {
            ancestors.push_back(&picture);
            auto candidate = std::unique_ptr<ImageView>(new ImageView);
            const RenderStatus built = candidate->build(
                server, picture, writable, ancestors);
            ancestors.pop_back();
            if (built != RenderStatus::success)
                return built;
            result = std::move(candidate);
            return RenderStatus::success;
        }
        catch (const std::bad_alloc &) {
            if (!ancestors.empty() && ancestors.back() == &picture)
                ancestors.pop_back();
            return RenderStatus::bad_alloc;
        }
    }

private:
    ImageView() = default;

    [[nodiscard]] RenderStatus build(
        ServerState &server, const RenderPicture &picture, bool writable,
        std::vector<const RenderPicture *> &ancestors)
    {
        const auto *format = render_format(picture.format);
        if (format == nullptr)
            return RenderStatus::bad_format;

        if (const auto *drawable =
                std::get_if<RenderDrawableSource>(&picture.source)) {
            surface_ = drawable->surface.get();
            if (surface_ == nullptr)
                return RenderStatus::bad_drawable;
            if (surface_->depth() != format->depth)
                return RenderStatus::bad_match;
            if (!build_surface(*surface_, writable))
                return RenderStatus::bad_alloc;
        }
        else {
            if (writable)
                return RenderStatus::bad_match;
            if (const auto *solid =
                    std::get_if<RenderSolidSource>(&picture.source)) {
                const pixman_color_t color{
                    solid->color.red, solid->color.green,
                    solid->color.blue, solid->color.alpha};
                image_ = UniqueImage(pixman_image_create_solid_fill(&color));
            }
            else if (const auto *linear =
                         std::get_if<RenderLinearGradient>(&picture.source)) {
                const pixman_point_fixed_t p1{linear->p1.x, linear->p1.y};
                const pixman_point_fixed_t p2{linear->p2.x, linear->p2.y};
                std::vector<pixman_gradient_stop_t> stops;
                if (!make_stops(linear->stops, stops))
                    return RenderStatus::bad_match;
                image_ = UniqueImage(pixman_image_create_linear_gradient(
                    &p1, &p2, stops.data(), static_cast<int>(stops.size())));
            }
            else if (const auto *radial =
                         std::get_if<RenderRadialGradient>(&picture.source)) {
                const pixman_point_fixed_t inner{
                    radial->inner.x, radial->inner.y};
                const pixman_point_fixed_t outer{
                    radial->outer.x, radial->outer.y};
                std::vector<pixman_gradient_stop_t> stops;
                if (!make_stops(radial->stops, stops))
                    return RenderStatus::bad_match;
                image_ = UniqueImage(pixman_image_create_radial_gradient(
                    &inner, &outer, radial->inner_radius,
                    radial->outer_radius, stops.data(),
                    static_cast<int>(stops.size())));
            }
            else if (const auto *conical =
                         std::get_if<RenderConicalGradient>(&picture.source)) {
                const pixman_point_fixed_t center{
                    conical->center.x, conical->center.y};
                std::vector<pixman_gradient_stop_t> stops;
                if (!make_stops(conical->stops, stops))
                    return RenderStatus::bad_match;
                image_ = UniqueImage(pixman_image_create_conical_gradient(
                    &center, conical->angle, stops.data(),
                    static_cast<int>(stops.size())));
            }
            if (!image_)
                return RenderStatus::bad_alloc;
        }
        return apply_attributes(server, picture.attributes, ancestors);
    }

    [[nodiscard]] bool build_surface(Surface &surface, bool writable)
    {
        pixman_format_code_t format = PIXMAN_x8r8g8b8;
        std::uint32_t *bits = surface.data();
        int stride = static_cast<int>(surface.stride_bytes());
        if (surface.depth() == 32) {
            format = PIXMAN_a8r8g8b8;
        }
        else if (surface.depth() <= 8) {
            const bool one_bit = surface.depth() == 1;
            format = one_bit ? PIXMAN_a1 : PIXMAN_a8;
            staging_stride_ = one_bit
                ? ((static_cast<std::size_t>(surface.width()) + 31U) / 32U) *
                    sizeof(std::uint32_t)
                : (static_cast<std::size_t>(surface.width()) + 3U) & ~3U;
            const std::size_t bytes = staging_stride_ * surface.height();
            staging_.assign((bytes + 3U) / 4U, 0);
            auto *staging_bytes =
                reinterpret_cast<std::uint8_t *>(staging_.data());
            for (std::uint16_t y = 0; y < surface.height(); ++y) {
                for (std::uint16_t x = 0; x < surface.width(); ++x) {
                    const std::uint32_t pixel = surface.pixel(x, y);
                    if (one_bit && pixel != 0) {
                        auto *words = reinterpret_cast<std::uint32_t *>(
                            staging_bytes + static_cast<std::size_t>(y) *
                                staging_stride_);
                        const unsigned bit = x & 31U;
                        words[x / 32U] |=
                            host_byte_order() == ByteOrder::little
                            ? std::uint32_t{1} << bit
                            : std::uint32_t{1} << (31U - bit);
                    }
                    else if (!one_bit) {
                        staging_bytes[
                            static_cast<std::size_t>(y) * staging_stride_ + x] =
                            static_cast<std::uint8_t>(pixel);
                    }
                }
            }
            bits = staging_.data();
            stride = static_cast<int>(staging_stride_);
            write_back_ = writable;
        }
        image_ = UniqueImage(pixman_image_create_bits(
            format, surface.width(), surface.height(), bits, stride));
        return static_cast<bool>(image_);
    }

    [[nodiscard]] static bool make_stops(
        const std::vector<RenderGradientStop> &source,
        std::vector<pixman_gradient_stop_t> &destination)
    {
        if (source.empty() || source.size() > maximum_render_gradient_stops)
            return false;
        destination.reserve(source.size());
        std::int32_t previous = std::numeric_limits<std::int32_t>::min();
        for (const auto &stop : source) {
            if (stop.position < previous)
                return false;
            previous = stop.position;
            destination.push_back(pixman_gradient_stop_t{
                stop.position,
                {stop.color.red, stop.color.green,
                 stop.color.blue, stop.color.alpha}});
        }
        return true;
    }

    [[nodiscard]] RenderStatus apply_attributes(
        ServerState &server, const RenderPictureAttributes &attributes,
        std::vector<const RenderPicture *> &ancestors)
    {
        pixman_image_set_repeat(image_.get(), pixman_repeat(attributes.repeat));
        pixman_image_set_component_alpha(
            image_.get(), attributes.component_alpha ? 1 : 0);

        pixman_transform_t transform{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                transform.matrix[row][column] =
                    attributes.transform[row * 3 + column];
            }
        }
        if (!pixman_image_set_transform(image_.get(), &transform) ||
            !pixman_image_set_filter(
                image_.get(), pixman_filter(attributes.filter), nullptr, 0)) {
            return RenderStatus::bad_alloc;
        }

        if (attributes.clip) {
            std::vector<pixman_box32_t> boxes;
            boxes.reserve(attributes.clip->rectangles().size());
            for (const auto &rectangle : attributes.clip->rectangles()) {
                const std::int64_t left =
                    static_cast<std::int64_t>(rectangle.x) +
                    attributes.clip_x_origin;
                const std::int64_t top =
                    static_cast<std::int64_t>(rectangle.y) +
                    attributes.clip_y_origin;
                const std::int64_t right = left + rectangle.width;
                const std::int64_t bottom = top + rectangle.height;
                if (left < std::numeric_limits<std::int32_t>::min() ||
                    top < std::numeric_limits<std::int32_t>::min() ||
                    right > std::numeric_limits<std::int32_t>::max() ||
                    bottom > std::numeric_limits<std::int32_t>::max()) {
                    return RenderStatus::bad_match;
                }
                boxes.push_back({
                    static_cast<std::int32_t>(left),
                    static_cast<std::int32_t>(top),
                    static_cast<std::int32_t>(right),
                    static_cast<std::int32_t>(bottom)});
            }
            pixman_region32_t region;
            if (!pixman_region32_init_rects(
                    &region, boxes.data(), static_cast<int>(boxes.size()))) {
                return RenderStatus::bad_alloc;
            }
            const bool clipped =
                pixman_image_set_clip_region32(image_.get(), &region) != 0;
            pixman_region32_fini(&region);
            if (!clipped)
                return RenderStatus::bad_alloc;
        }

        if (attributes.alpha_map != 0) {
            if (attributes.alpha_x_origin <
                    std::numeric_limits<std::int16_t>::min() ||
                attributes.alpha_x_origin >
                    std::numeric_limits<std::int16_t>::max() ||
                attributes.alpha_y_origin <
                    std::numeric_limits<std::int16_t>::min() ||
                attributes.alpha_y_origin >
                    std::numeric_limits<std::int16_t>::max()) {
                return RenderStatus::bad_match;
            }
            if (!attributes.alpha_map_picture)
                return RenderStatus::bad_picture;
            const RenderStatus status = create(
                server, *attributes.alpha_map_picture, false,
                ancestors, alpha_map_);
            if (status != RenderStatus::success)
                return status;
            pixman_image_set_alpha_map(
                image_.get(), alpha_map_->get(),
                static_cast<std::int16_t>(attributes.alpha_x_origin),
                static_cast<std::int16_t>(attributes.alpha_y_origin));
        }
        return RenderStatus::success;
    }

    UniqueImage image_;
    Surface *surface_ = nullptr;
    std::vector<std::uint32_t> staging_;
    std::size_t staging_stride_ = 0;
    bool write_back_ = false;
    std::unique_ptr<ImageView> alpha_map_;
};

RenderStatus
make_image(ServerState &server, std::uint32_t picture, bool writable,
           std::unique_ptr<ImageView> &image)
{
    std::vector<const RenderPicture *> ancestors;
    return ImageView::create(server, picture, writable, ancestors, image);
}

} // namespace

const std::array<RenderFormat, 4> &
render_formats() noexcept
{
    return formats;
}

const RenderFormat *
render_format(std::uint32_t id) noexcept
{
    const auto found = std::find_if(
        formats.begin(), formats.end(),
        [id](const RenderFormat &format) { return format.id == id; });
    return found == formats.end() ? nullptr : &*found;
}

const RenderFormat *
render_format_for_depth(std::uint8_t depth) noexcept
{
    const auto found = std::find_if(
        formats.begin(), formats.end(),
        [depth](const RenderFormat &format) { return format.depth == depth; });
    return found == formats.end() ? nullptr : &*found;
}

bool
render_operator_valid(std::uint8_t operation) noexcept
{
    return render_operator(operation).has_value();
}

RenderStatus
RenderEngine::finish_draw(std::uint32_t picture)
{
    const auto updated = server_.damage_render_picture(picture);
    if (updated == DamageUpdate::invalid)
        return RenderStatus::bad_drawable;
    if (updated == DamageUpdate::resource_exhausted ||
        updated == DamageUpdate::queue_full) {
        return RenderStatus::bad_alloc;
    }
    return RenderStatus::success;
}

RenderStatus
RenderEngine::composite(
    std::uint8_t operation, std::uint32_t source, std::uint32_t mask,
    std::uint32_t destination, std::int32_t source_x,
    std::int32_t source_y, std::int32_t mask_x, std::int32_t mask_y,
    std::int32_t destination_x, std::int32_t destination_y,
    std::uint32_t width, std::uint32_t height)
{
    const auto selected_operator = render_operator(operation);
    if (!selected_operator)
        return RenderStatus::bad_operator;
    if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return RenderStatus::bad_match;
    }
    std::unique_ptr<ImageView> source_image;
    std::unique_ptr<ImageView> mask_image;
    std::unique_ptr<ImageView> destination_image;
    RenderStatus status = make_image(server_, source, false, source_image);
    if (status != RenderStatus::success)
        return status;
    if (mask != 0) {
        status = make_image(server_, mask, false, mask_image);
        if (status != RenderStatus::success)
            return status;
    }
    status = make_image(server_, destination, true, destination_image);
    if (status != RenderStatus::success)
        return status;
    pixman_image_composite32(
        *selected_operator, source_image->get(),
        mask_image ? mask_image->get() : nullptr, destination_image->get(),
        source_x, source_y, mask_x, mask_y, destination_x, destination_y,
        static_cast<int>(width), static_cast<int>(height));
    return finish_draw(destination);
}

RenderStatus
RenderEngine::fill_rectangles(
    std::uint8_t operation, std::uint32_t destination,
    const RenderColor &color, const std::vector<Rectangle> &rectangles)
{
    const auto selected_operator = render_operator(operation);
    if (!selected_operator)
        return RenderStatus::bad_operator;
    const pixman_color_t fill_color{
        color.red, color.green, color.blue, color.alpha};
    UniqueImage source(pixman_image_create_solid_fill(&fill_color));
    if (!source)
        return RenderStatus::bad_alloc;
    std::unique_ptr<ImageView> destination_image;
    const RenderStatus status =
        make_image(server_, destination, true, destination_image);
    if (status != RenderStatus::success)
        return status;
    for (const auto &rectangle : rectangles) {
        if (rectangle.width >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            rectangle.height >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return RenderStatus::bad_match;
        }
        pixman_image_composite32(
            *selected_operator, source.get(), nullptr,
            destination_image->get(), 0, 0, 0, 0,
            rectangle.x, rectangle.y, static_cast<int>(rectangle.width),
            static_cast<int>(rectangle.height));
    }
    return finish_draw(destination);
}

RenderStatus
RenderEngine::composite_trapezoids(
    std::uint8_t operation, std::uint32_t source,
    std::uint32_t destination, std::uint32_t mask_format,
    std::int32_t source_x, std::int32_t source_y,
    const std::vector<RenderTrapezoid> &trapezoids)
{
    const auto selected_operator = render_operator(operation);
    if (!selected_operator)
        return RenderStatus::bad_operator;
    const auto *destination_picture = server_.render_picture(destination);
    if (destination_picture == nullptr)
        return RenderStatus::bad_picture;
    const bool separate_masks = mask_format == 0;
    const std::uint32_t effective_mask_format = separate_masks
        ? (destination_picture->attributes.poly_edge == 0
              ? render_a1_format
              : render_a8_format)
        : mask_format;
    const auto *format = render_format(effective_mask_format);
    if (format == nullptr)
        return RenderStatus::bad_format;
    if (format->depth != 1 && format->depth != 8)
        return RenderStatus::bad_match;
    if (trapezoids.empty())
        return RenderStatus::success;
    if (trapezoids.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return RenderStatus::bad_alloc;
    }
    std::unique_ptr<ImageView> source_image;
    std::unique_ptr<ImageView> destination_image;
    RenderStatus status = make_image(server_, source, false, source_image);
    if (status != RenderStatus::success)
        return status;
    status = make_image(server_, destination, true, destination_image);
    if (status != RenderStatus::success)
        return status;
    std::vector<pixman_trapezoid_t> converted;
    try {
        converted.reserve(trapezoids.size());
        for (const auto &trap : trapezoids) {
            converted.push_back({
                trap.top, trap.bottom,
                {{trap.left.p1.x, trap.left.p1.y},
                 {trap.left.p2.x, trap.left.p2.y}},
                {{trap.right.p1.x, trap.right.p1.y},
                 {trap.right.p2.x, trap.right.p2.y}}});
        }
    }
    catch (const std::bad_alloc &) {
        return RenderStatus::bad_alloc;
    }
    source_x -= trapezoids.front().left.p1.x >> 16;
    source_y -= trapezoids.front().left.p1.y >> 16;
    if (separate_masks) {
        for (const auto &trapezoid : converted) {
            pixman_composite_trapezoids(
                *selected_operator, source_image->get(),
                destination_image->get(),
                pixman_mask_format(effective_mask_format),
                source_x, source_y, 0, 0, 1, &trapezoid);
        }
    }
    else {
        pixman_composite_trapezoids(
            *selected_operator, source_image->get(), destination_image->get(),
            pixman_mask_format(effective_mask_format), source_x, source_y,
            0, 0, static_cast<int>(converted.size()), converted.data());
    }
    return finish_draw(destination);
}

RenderStatus
RenderEngine::composite_triangles(
    std::uint8_t operation, std::uint32_t source,
    std::uint32_t destination, std::uint32_t mask_format,
    std::int32_t source_x, std::int32_t source_y,
    const std::vector<RenderTriangle> &triangles)
{
    const auto selected_operator = render_operator(operation);
    if (!selected_operator)
        return RenderStatus::bad_operator;
    const auto *destination_picture = server_.render_picture(destination);
    if (destination_picture == nullptr)
        return RenderStatus::bad_picture;
    const bool separate_masks = mask_format == 0;
    const std::uint32_t effective_mask_format = separate_masks
        ? (destination_picture->attributes.poly_edge == 0
              ? render_a1_format
              : render_a8_format)
        : mask_format;
    const auto *format = render_format(effective_mask_format);
    if (format == nullptr)
        return RenderStatus::bad_format;
    if (format->depth != 1 && format->depth != 8)
        return RenderStatus::bad_match;
    if (triangles.empty())
        return RenderStatus::success;
    if (triangles.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return RenderStatus::bad_alloc;
    }
    std::unique_ptr<ImageView> source_image;
    std::unique_ptr<ImageView> destination_image;
    RenderStatus status = make_image(server_, source, false, source_image);
    if (status != RenderStatus::success)
        return status;
    status = make_image(server_, destination, true, destination_image);
    if (status != RenderStatus::success)
        return status;
    std::vector<pixman_triangle_t> converted;
    try {
        converted.reserve(triangles.size());
        for (const auto &triangle : triangles) {
            converted.push_back({
                {triangle.p1.x, triangle.p1.y},
                {triangle.p2.x, triangle.p2.y},
                {triangle.p3.x, triangle.p3.y}});
        }
    }
    catch (const std::bad_alloc &) {
        return RenderStatus::bad_alloc;
    }
    source_x -= triangles.front().p1.x >> 16;
    source_y -= triangles.front().p1.y >> 16;
    if (separate_masks) {
        for (const auto &triangle : converted) {
            pixman_composite_triangles(
                *selected_operator, source_image->get(),
                destination_image->get(),
                pixman_mask_format(effective_mask_format),
                source_x, source_y, 0, 0, 1, &triangle);
        }
    }
    else {
        pixman_composite_triangles(
            *selected_operator, source_image->get(), destination_image->get(),
            pixman_mask_format(effective_mask_format), source_x, source_y,
            0, 0, static_cast<int>(converted.size()), converted.data());
    }
    return finish_draw(destination);
}

RenderStatus
RenderEngine::composite_glyphs(
    std::uint8_t operation, std::uint32_t source,
    std::uint32_t destination, std::uint32_t mask_format,
    std::int32_t source_x, std::int32_t source_y,
    const std::vector<RenderGlyphRun> &runs)
{
    struct Placement {
        const RenderGlyph *glyph;
        std::uint32_t format;
        std::int32_t x;
        std::int32_t y;
    };
    struct GlyphImage {
        UniqueImage image;
        std::vector<std::uint32_t> storage;
    };
    const auto selected_operator = render_operator(operation);
    if (!selected_operator)
        return RenderStatus::bad_operator;
    const auto *selected_mask = mask_format == 0
        ? nullptr
        : render_format(mask_format);
    if (mask_format != 0 && selected_mask == nullptr)
        return RenderStatus::bad_format;
    if (selected_mask != nullptr &&
        selected_mask->direct.alpha_mask == 0) {
        return RenderStatus::bad_match;
    }

    std::vector<Placement> placements;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int32_t first_x = 0;
    std::int32_t first_y = 0;
    bool first_run = true;
    try {
        for (const auto &run : runs) {
            const auto *glyph_set = server_.render_glyph_set(run.glyph_set);
            if (glyph_set == nullptr || !glyph_set->storage)
                return RenderStatus::bad_glyph_set;
            x += run.x_offset;
            y += run.y_offset;
            if (x < std::numeric_limits<std::int32_t>::min() ||
                x > std::numeric_limits<std::int32_t>::max() ||
                y < std::numeric_limits<std::int32_t>::min() ||
                y > std::numeric_limits<std::int32_t>::max()) {
                return RenderStatus::bad_match;
            }
            if (first_run) {
                first_x = static_cast<std::int32_t>(x);
                first_y = static_cast<std::int32_t>(y);
                first_run = false;
            }
            for (const auto glyph_id : run.glyphs) {
                const auto found = glyph_set->storage->glyphs.find(glyph_id);
                if (found == glyph_set->storage->glyphs.end())
                    continue;
                placements.push_back({
                    &found->second, glyph_set->storage->format,
                    static_cast<std::int32_t>(x),
                    static_cast<std::int32_t>(y)});
                x += found->second.info.x_offset;
                y += found->second.info.y_offset;
                if (x < std::numeric_limits<std::int32_t>::min() ||
                    x > std::numeric_limits<std::int32_t>::max() ||
                    y < std::numeric_limits<std::int32_t>::min() ||
                    y > std::numeric_limits<std::int32_t>::max()) {
                    return RenderStatus::bad_match;
                }
            }
        }
    }
    catch (const std::bad_alloc &) {
        return RenderStatus::bad_alloc;
    }
    if (placements.empty())
        return RenderStatus::success;

    const auto make_glyph_image = [](const Placement &placement,
                                     GlyphImage &result) {
        const auto *format = render_format(placement.format);
        if (format == nullptr)
            return RenderStatus::bad_format;
        const auto &glyph = *placement.glyph;
        const auto count = static_cast<std::size_t>(glyph.info.width) *
            glyph.info.height;
        if (glyph.pixels.size() != count)
            return RenderStatus::bad_glyph;
        pixman_format_code_t pixman_format = PIXMAN_a8;
        std::size_t stride =
            (static_cast<std::size_t>(glyph.info.width) + 3U) & ~3U;
        try {
            if (format->depth == 1 || format->depth == 8) {
                const std::size_t bytes = stride * glyph.info.height;
                result.storage.assign((bytes + 3U) / 4U, 0);
                auto *pixels = reinterpret_cast<std::uint8_t *>(
                    result.storage.data());
                for (std::uint16_t row = 0; row < glyph.info.height; ++row) {
                    for (std::uint16_t column = 0;
                         column < glyph.info.width; ++column) {
                        const std::uint32_t value = glyph.pixels[
                            static_cast<std::size_t>(row) *
                                glyph.info.width + column];
                        pixels[static_cast<std::size_t>(row) * stride +
                               column] = format->depth == 1
                            ? static_cast<std::uint8_t>(value == 0 ? 0 : 0xff)
                            : static_cast<std::uint8_t>(value);
                    }
                }
            }
            else {
                pixman_format = format->depth == 32
                    ? PIXMAN_a8r8g8b8
                    : PIXMAN_x8r8g8b8;
                stride = static_cast<std::size_t>(glyph.info.width) * 4U;
                result.storage = glyph.pixels;
            }
        }
        catch (const std::bad_alloc &) {
            return RenderStatus::bad_alloc;
        }
        result.image = UniqueImage(pixman_image_create_bits(
            pixman_format, glyph.info.width, glyph.info.height,
            result.storage.data(), static_cast<int>(stride)));
        if (!result.image)
            return RenderStatus::bad_alloc;
        if (format->direct.alpha_mask != 0 &&
            (format->direct.red_mask != 0 ||
             format->direct.green_mask != 0 ||
             format->direct.blue_mask != 0)) {
            pixman_image_set_component_alpha(result.image.get(), 1);
        }
        return RenderStatus::success;
    };

    std::unique_ptr<ImageView> source_image;
    std::unique_ptr<ImageView> destination_image;
    RenderStatus status = make_image(server_, source, false, source_image);
    if (status != RenderStatus::success)
        return status;
    status = make_image(server_, destination, true, destination_image);
    if (status != RenderStatus::success)
        return status;

    if (selected_mask == nullptr) {
        for (const auto &placement : placements) {
            GlyphImage glyph_image;
            status = make_glyph_image(placement, glyph_image);
            if (status != RenderStatus::success)
                return status;
            const std::int32_t left = placement.x - placement.glyph->info.x;
            const std::int32_t top = placement.y - placement.glyph->info.y;
            pixman_image_composite32(
                *selected_operator, source_image->get(),
                glyph_image.image.get(), destination_image->get(),
                source_x + left - first_x,
                source_y + top - first_y, 0, 0, left, top,
                placement.glyph->info.width,
                placement.glyph->info.height);
        }
        return finish_draw(destination);
    }

    std::int64_t left = std::numeric_limits<std::int32_t>::max();
    std::int64_t top = std::numeric_limits<std::int32_t>::max();
    std::int64_t right = std::numeric_limits<std::int32_t>::min();
    std::int64_t bottom = std::numeric_limits<std::int32_t>::min();
    for (const auto &placement : placements) {
        const std::int64_t glyph_left =
            static_cast<std::int64_t>(placement.x) - placement.glyph->info.x;
        const std::int64_t glyph_top =
            static_cast<std::int64_t>(placement.y) - placement.glyph->info.y;
        left = std::min(left, glyph_left);
        top = std::min(top, glyph_top);
        right = std::max(
            right, glyph_left + placement.glyph->info.width);
        bottom = std::max(
            bottom, glyph_top + placement.glyph->info.height);
    }
    const std::int64_t width = right - left;
    const std::int64_t height = bottom - top;
    if (width <= 0 || height <= 0)
        return RenderStatus::success;
    const auto pixel_count = checked_multiply(
        static_cast<std::size_t>(width), static_cast<std::size_t>(height));
    if (left < std::numeric_limits<std::int32_t>::min() ||
        top < std::numeric_limits<std::int32_t>::min() ||
        right > std::numeric_limits<std::int32_t>::max() ||
        bottom > std::numeric_limits<std::int32_t>::max() ||
        width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max() || !pixel_count ||
        *pixel_count > maximum_render_glyph_bytes / 4U) {
        return RenderStatus::bad_alloc;
    }

    const bool one_bit = selected_mask->depth == 1;
    const bool alpha_only = selected_mask->depth <= 8;
    const std::size_t mask_stride = one_bit
        ? ((static_cast<std::size_t>(width) + 31U) / 32U) * 4U
        : (alpha_only
              ? (static_cast<std::size_t>(width) + 3U) & ~3U
              : static_cast<std::size_t>(width) * 4U);
    const auto mask_bytes = checked_multiply(
        mask_stride, static_cast<std::size_t>(height));
    if (!mask_bytes || *mask_bytes > maximum_render_glyph_bytes)
        return RenderStatus::bad_alloc;
    std::vector<std::uint32_t> mask_storage;
    try {
        mask_storage.assign((*mask_bytes + 3U) / 4U, 0);
    }
    catch (const std::bad_alloc &) {
        return RenderStatus::bad_alloc;
    }
    UniqueImage mask_image(pixman_image_create_bits(
        one_bit ? PIXMAN_a1
                : (alpha_only ? PIXMAN_a8 : PIXMAN_a8r8g8b8),
        static_cast<int>(width), static_cast<int>(height),
        mask_storage.data(), static_cast<int>(mask_stride)));
    if (!mask_image)
        return RenderStatus::bad_alloc;
    if (!alpha_only)
        pixman_image_set_component_alpha(mask_image.get(), 1);
    for (const auto &placement : placements) {
        GlyphImage glyph_image;
        status = make_glyph_image(placement, glyph_image);
        if (status != RenderStatus::success)
            return status;
        const std::int32_t glyph_left =
            placement.x - placement.glyph->info.x;
        const std::int32_t glyph_top =
            placement.y - placement.glyph->info.y;
        pixman_image_composite32(
            PIXMAN_OP_ADD, glyph_image.image.get(), nullptr, mask_image.get(),
            0, 0, 0, 0, glyph_left - static_cast<std::int32_t>(left),
            glyph_top - static_cast<std::int32_t>(top),
            placement.glyph->info.width, placement.glyph->info.height);
    }
    pixman_image_composite32(
        *selected_operator, source_image->get(), mask_image.get(),
        destination_image->get(),
        source_x + static_cast<std::int32_t>(left) - first_x,
        source_y + static_cast<std::int32_t>(top) - first_y,
        0, 0, static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top), static_cast<int>(width),
        static_cast<int>(height));
    return finish_draw(destination);
}

RenderStatus
RenderEngine::add_traps(
    std::uint32_t picture, std::int16_t x_offset,
    std::int16_t y_offset, const std::vector<RenderTrap> &traps)
{
    if (traps.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return RenderStatus::bad_alloc;
    }
    std::unique_ptr<ImageView> destination_image;
    const RenderStatus status =
        make_image(server_, picture, true, destination_image);
    if (status != RenderStatus::success)
        return status;
    std::vector<pixman_trap_t> converted;
    try {
        converted.reserve(traps.size());
        for (const auto &trap : traps) {
            converted.push_back({
                {trap.top.left, trap.top.right, trap.top.y},
                {trap.bottom.left, trap.bottom.right, trap.bottom.y}});
        }
    }
    catch (const std::bad_alloc &) {
        return RenderStatus::bad_alloc;
    }
    pixman_add_traps(
        destination_image->get(), x_offset, y_offset,
        static_cast<int>(converted.size()), converted.data());
    return finish_draw(picture);
}

RenderStatus
RenderEngine::snapshot(
    std::uint32_t picture, std::uint16_t &width,
    std::uint16_t &height, std::vector<std::uint32_t> &pixels)
{
    const auto *source = server_.render_picture(picture);
    if (source == nullptr)
        return RenderStatus::bad_picture;
    const auto *drawable = std::get_if<RenderDrawableSource>(&source->source);
    const auto *surface = drawable == nullptr
        ? nullptr
        : drawable->surface.get();
    if (surface == nullptr)
        return RenderStatus::bad_drawable;
    const auto count = checked_multiply(
        static_cast<std::size_t>(surface->width()),
        static_cast<std::size_t>(surface->height()));
    if (!count || *count > maximum_surface_bytes / sizeof(std::uint32_t))
        return RenderStatus::bad_alloc;
    std::vector<std::uint32_t> snapshot;
    try {
        snapshot.assign(*count, 0);
    }
    catch (const std::bad_alloc &) {
        return RenderStatus::bad_alloc;
    }
    std::unique_ptr<ImageView> source_image;
    RenderStatus status = make_image(server_, picture, false, source_image);
    if (status != RenderStatus::success)
        return status;
    UniqueImage destination_image(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, surface->width(), surface->height(),
        snapshot.data(), static_cast<int>(surface->width()) * 4));
    if (!destination_image)
        return RenderStatus::bad_alloc;
    pixman_image_composite32(
        PIXMAN_OP_SRC, source_image->get(), nullptr, destination_image.get(),
        0, 0, 0, 0, 0, 0, surface->width(), surface->height());
    width = surface->width();
    height = surface->height();
    pixels = std::move(snapshot);
    return RenderStatus::success;
}

} // namespace xmin::next
