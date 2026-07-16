#include "xmin/next/render.hpp"

#include "xmin/next/server_state.hpp"

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
                    std::uint32_t value = bytes[
                        static_cast<std::size_t>(y) * staging_stride_ + x];
                    if (surface_->depth() == 1)
                        value = value >= 0x80 ? 1 : 0;
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
        std::vector<std::uint32_t> &ancestors,
        std::unique_ptr<ImageView> &result)
    {
        const auto *picture = server.render_picture(picture_id);
        if (picture == nullptr)
            return RenderStatus::bad_picture;
        if (std::find(ancestors.begin(), ancestors.end(), picture_id) !=
            ancestors.end()) {
            return RenderStatus::bad_match;
        }
        if (ancestors.size() == 64)
            return RenderStatus::bad_match;

        try {
            ancestors.push_back(picture_id);
            auto candidate = std::unique_ptr<ImageView>(new ImageView);
            const RenderStatus built = candidate->build(
                server, *picture, writable, ancestors);
            ancestors.pop_back();
            if (built != RenderStatus::success)
                return built;
            result = std::move(candidate);
            return RenderStatus::success;
        }
        catch (const std::bad_alloc &) {
            if (!ancestors.empty() && ancestors.back() == picture_id)
                ancestors.pop_back();
            return RenderStatus::bad_alloc;
        }
    }

private:
    ImageView() = default;

    [[nodiscard]] RenderStatus build(
        ServerState &server, const RenderPicture &picture, bool writable,
        std::vector<std::uint32_t> &ancestors)
    {
        const auto *format = render_format(picture.format);
        if (format == nullptr)
            return RenderStatus::bad_format;

        if (const auto *drawable =
                std::get_if<RenderDrawableSource>(&picture.source)) {
            surface_ = server.drawable_surface(drawable->drawable);
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
            format = PIXMAN_a8;
            staging_stride_ =
                (static_cast<std::size_t>(surface.width()) + 3U) & ~3U;
            const std::size_t bytes = staging_stride_ * surface.height();
            staging_.assign((bytes + 3U) / 4U, 0);
            auto *staging_bytes =
                reinterpret_cast<std::uint8_t *>(staging_.data());
            for (std::uint16_t y = 0; y < surface.height(); ++y) {
                for (std::uint16_t x = 0; x < surface.width(); ++x) {
                    const std::uint32_t pixel = surface.pixel(x, y);
                    staging_bytes[
                        static_cast<std::size_t>(y) * staging_stride_ + x] =
                        surface.depth() == 1
                        ? static_cast<std::uint8_t>(pixel == 0 ? 0 : 0xff)
                        : static_cast<std::uint8_t>(pixel);
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
        std::vector<std::uint32_t> &ancestors)
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
            const RenderStatus status = create(
                server, attributes.alpha_map, false, ancestors, alpha_map_);
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
    std::vector<std::uint32_t> ancestors;
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
    server_.invalidate_scene();
    return RenderStatus::success;
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
    server_.invalidate_scene();
    return RenderStatus::success;
}

RenderStatus
RenderEngine::composite_trapezoids(
    std::uint8_t operation, std::uint32_t source,
    std::uint32_t destination, std::uint32_t mask_format,
    std::int32_t source_x, std::int32_t source_y,
    const std::vector<RenderTrapezoid> &trapezoids)
{
    const auto selected_operator = render_operator(operation);
    const auto *format = render_format(mask_format);
    if (!selected_operator)
        return RenderStatus::bad_operator;
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
    pixman_composite_trapezoids(
        *selected_operator, source_image->get(), destination_image->get(),
        pixman_mask_format(mask_format), source_x, source_y, 0, 0,
        static_cast<int>(converted.size()), converted.data());
    server_.invalidate_scene();
    return RenderStatus::success;
}

RenderStatus
RenderEngine::composite_triangles(
    std::uint8_t operation, std::uint32_t source,
    std::uint32_t destination, std::uint32_t mask_format,
    std::int32_t source_x, std::int32_t source_y,
    const std::vector<RenderTriangle> &triangles)
{
    const auto selected_operator = render_operator(operation);
    const auto *format = render_format(mask_format);
    if (!selected_operator)
        return RenderStatus::bad_operator;
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
    pixman_composite_triangles(
        *selected_operator, source_image->get(), destination_image->get(),
        pixman_mask_format(mask_format), source_x, source_y, 0, 0,
        static_cast<int>(converted.size()), converted.data());
    server_.invalidate_scene();
    return RenderStatus::success;
}

} // namespace xmin::next
