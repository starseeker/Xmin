#include "xmin/next/connection.hpp"

#include "xmin/next/checked.hpp"
#include "xmin/next/extension_registry.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>

namespace xmin::next {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_pixmap = 4,
    bad_atom = 5,
    bad_cursor = 6,
    bad_match = 8,
    bad_access = 10,
    bad_alloc = 11,
    bad_gc = 13,
    bad_id_choice = 14,
    bad_length = 16,
};

std::int16_t
signed_word(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

Result<void>
malformed_xfixes(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

bool
read_rectangles(WireReader &reader, std::vector<Rectangle> &rectangles)
{
    try {
        rectangles.reserve(reader.remaining() / 8);
        while (reader.remaining() != 0) {
            const auto x = reader.u16();
            const auto y = reader.u16();
            const auto width = reader.u16();
            const auto height = reader.u16();
            if (!x || !y || !width || !height)
                return false;
            rectangles.push_back(
                {signed_word(*x), signed_word(*y), *width, *height});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

bool
copy_region(const Region &source, Region &destination)
{
    return Region::combine(
        RegionOperation::set, source, source, destination);
}

bool
bitmap_region(const PixmapRecord &pixmap, Region &region)
{
    std::vector<Rectangle> runs;
    try {
        for (std::uint16_t y = 0; y < pixmap.surface->height(); ++y) {
            std::uint16_t x = 0;
            while (x < pixmap.surface->width()) {
                while (x < pixmap.surface->width() &&
                       (pixmap.surface->pixel(x, y) & 1U) == 0) {
                    ++x;
                }
                const std::uint16_t start = x;
                while (x < pixmap.surface->width() &&
                       (pixmap.surface->pixel(x, y) & 1U) != 0) {
                    ++x;
                }
                if (x != start) {
                    if (runs.size() == maximum_shape_rectangles)
                        return false;
                    runs.push_back(
                        {start, y, static_cast<std::uint32_t>(x - start), 1});
                }
            }
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return Region::canonicalize(runs, region);
}

const CursorImage *
cursor_pixels(const std::shared_ptr<CursorImage> &cursor) noexcept
{
    if (!cursor)
        return nullptr;
    if (!cursor->frames.empty() && cursor->frames.front().first)
        return cursor->frames.front().first.get();
    return cursor.get();
}

} // namespace

Result<void>
Connection::handle_xfixes(const RequestContext &context)
{
    constexpr std::array<std::uint8_t, 7> last_request_by_major{
        0, 4, 27, 28, 30, 32, 34};
    constexpr std::uint8_t bad_region = xfixes_extension.first_error;
    constexpr std::uint8_t bad_barrier = xfixes_extension.first_error + 1;
    constexpr std::uint8_t bad_picture = render_extension.first_error + 1;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto check_new_resource = [&](std::uint32_t id)
        -> std::optional<Result<void>> {
        if (!server_.valid_client_resource(id, config_.resource_base))
            return error(bad_id_choice, id);
        if (server_.resource_limit_reached(config_.resource_base))
            return error(bad_alloc);
        return std::nullopt;
    };
    const auto update_result = [&](XFixesUpdate result) {
        if (result == XFixesUpdate::invalid)
            return error(bad_match);
        if (result == XFixesUpdate::resource_exhausted ||
            result == XFixesUpdate::queue_full) {
            return error(bad_alloc);
        }
        return drain_pending_events();
    };

    if (xfixes_major_version_ >= last_request_by_major.size() ||
        context.data > last_request_by_major[xfixes_major_version_]) {
        return error(bad_request);
    }

    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto major = reader.u32();
        const auto minor = reader.u32();
        if (!major || !minor)
            return malformed_xfixes("truncated XFIXES QueryVersion request");
        const auto negotiated_major = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(*major, xfixes_extension.major_version));
        xfixes_major_version_ = std::max(
            xfixes_major_version_, negotiated_major);
        const std::uint32_t negotiated_minor =
            negotiated_major == xfixes_extension.major_version
            ? std::min<std::uint32_t>(
                  *minor, xfixes_extension.minor_version)
            : *minor;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(negotiated_major);
        reply.u32(negotiated_minor);
        reply.pad(16);
        return queue(reply.data());
    }
    case 1: { // ChangeSaveSet
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto mode = reader.u8();
        const auto target = reader.u8();
        const auto mapping = reader.u8();
        if (!mode || !target || !mapping || !reader.skip(1))
            return malformed_xfixes("truncated XFIXES ChangeSaveSet header");
        const auto window_id = reader.u32();
        if (!window_id)
            return malformed_xfixes("truncated XFIXES ChangeSaveSet request");
        const auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        if (window->owner == config_.resource_base)
            return error(bad_match);
        if (*mode > 1)
            return error(bad_value, *mode);
        if (*target > 1)
            return error(bad_value, *target);
        if (*mapping > 1)
            return error(bad_value, *mapping);
        return update_result(server_.alter_save_set(
            config_.resource_base, *window_id, *mode == 0,
            *target == 1, *mapping == 0));
    }
    case 2: { // SelectSelectionInput
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto window_id = reader.u32();
        const auto selection = reader.u32();
        const auto mask = reader.u32();
        if (!window_id || !selection || !mask)
            return malformed_xfixes(
                "truncated XFIXES SelectSelectionInput request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        if (!server_.atoms().name(*selection))
            return error(bad_atom, *selection);
        if ((*mask & ~7U) != 0)
            return error(bad_value, *mask);
        return update_result(server_.select_xfixes_selection_input(
            config_.resource_base, *window_id, *selection, *mask));
    }
    case 3: { // SelectCursorInput
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window_id = reader.u32();
        const auto mask = reader.u32();
        if (!window_id || !mask)
            return malformed_xfixes(
                "truncated XFIXES SelectCursorInput request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        if ((*mask & ~1U) != 0)
            return error(bad_value, *mask);
        return update_result(server_.select_xfixes_cursor_input(
            config_.resource_base, *window_id, *mask));
    }
    case 4: // GetCursorImage
    case 25: { // GetCursorImageAndName
        if (context.request.size() != 4)
            return error(bad_length);
        const auto current = server_.current_cursor();
        const auto *image = cursor_pixels(current);
        if (!current || image == nullptr)
            return error(bad_cursor);
        const auto pixel_count = checked_multiply(
            static_cast<std::size_t>(image->width),
            static_cast<std::size_t>(image->height));
        if (!pixel_count || image->pixels.size() < *pixel_count)
            return error(bad_alloc);
        std::string_view name;
        if (context.data == 25 && current->name != 0) {
            const auto atom_name = server_.atoms().name(current->name);
            if (atom_name)
                name = *atom_name;
        }
        const auto padded_name = padded_to_four(name.size());
        if (!padded_name)
            return error(bad_alloc);
        const std::size_t payload = *pixel_count * 4 +
            (context.data == 25 ? *padded_name : 0);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(payload / 4));
        reply.i16(static_cast<std::int16_t>(server_.input().pointer_x));
        reply.i16(static_cast<std::int16_t>(server_.input().pointer_y));
        reply.u16(image->width);
        reply.u16(image->height);
        reply.u16(image->x_hot);
        reply.u16(image->y_hot);
        reply.u32(current->serial);
        if (context.data == 25) {
            reply.u32(current->name);
            reply.u16(static_cast<std::uint16_t>(name.size()));
            reply.pad(2);
        }
        else {
            reply.pad(8);
        }
        for (std::size_t index = 0; index < *pixel_count; ++index)
            reply.u32(image->pixels[index]);
        if (context.data == 25) {
            reply.bytes(name);
            reply.pad(*padded_name - name.size());
        }
        return queue(reply.data());
    }
    case 5: // CreateRegion
    case 11: { // SetRegion
        if (context.request.size() < 8 ||
            ((context.request.size() - 8) & 7U) != 0) {
            return error(bad_length);
        }
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed_xfixes("truncated XFIXES region request");
        if (context.data == 5) {
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
        }
        else if (server_.xfixes_region(*id) == nullptr) {
            return error(bad_region, *id);
        }
        std::vector<Rectangle> rectangles;
        if (!read_rectangles(reader, rectangles))
            return error(bad_alloc);
        Region region;
        if (!Region::canonicalize(rectangles, region))
            return error(bad_alloc);
        if (context.data == 5) {
            if (!server_.add_xfixes_region(
                    *id, std::move(region), config_.resource_base)) {
                return error(bad_alloc);
            }
        }
        else {
            *server_.xfixes_region(*id) = std::move(region);
        }
        return Result<void>::success();
    }
    case 6: { // CreateRegionFromBitmap
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto id = reader.u32();
        const auto bitmap = reader.u32();
        if (!id || !bitmap)
            return malformed_xfixes(
                "truncated XFIXES CreateRegionFromBitmap request");
        if (const auto invalid = check_new_resource(*id))
            return *invalid;
        const auto *pixmap = server_.pixmap(*bitmap);
        if (pixmap == nullptr)
            return error(bad_pixmap, *bitmap);
        if (pixmap->surface->depth() != 1)
            return error(bad_match);
        Region region;
        if (!bitmap_region(*pixmap, region) ||
            !server_.add_xfixes_region(
                *id, std::move(region), config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 7: { // CreateRegionFromWindow
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto window_id = reader.u32();
        const auto kind = reader.u8();
        if (!id || !window_id || !kind || !reader.skip(3))
            return malformed_xfixes(
                "truncated XFIXES CreateRegionFromWindow request");
        if (const auto invalid = check_new_resource(*id))
            return *invalid;
        const auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        if (*kind > 1)
            return error(bad_value, *kind);
        Region region;
        if (window->shapes[*kind]) {
            if (!copy_region(*window->shapes[*kind], region))
                return error(bad_alloc);
        }
        else {
            const std::vector<Rectangle> rectangles{
                window->default_shape(*kind)};
            if (!Region::canonicalize(rectangles, region))
                return error(bad_alloc);
        }
        if (!server_.add_xfixes_region(
                *id, std::move(region), config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 8: { // CreateRegionFromGC
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto id = reader.u32();
        const auto gc_id = reader.u32();
        if (!id || !gc_id)
            return malformed_xfixes(
                "truncated XFIXES CreateRegionFromGC request");
        if (const auto invalid = check_new_resource(*id))
            return *invalid;
        const auto *gc = server_.graphics_context(*gc_id);
        if (gc == nullptr)
            return error(bad_gc, *gc_id);
        if (!gc->clip_region)
            return error(bad_match);
        Region region;
        if (!copy_region(*gc->clip_region, region) ||
            !server_.add_xfixes_region(
                *id, std::move(region), config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 9: { // CreateRegionFromPicture
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto id = reader.u32();
        const auto picture_id = reader.u32();
        if (!id || !picture_id)
            return malformed_xfixes(
                "truncated XFIXES CreateRegionFromPicture request");
        if (const auto invalid = check_new_resource(*id))
            return *invalid;
        const auto *picture = server_.render_picture(*picture_id);
        if (picture == nullptr)
            return error(bad_picture, *picture_id);
        if (!std::holds_alternative<RenderDrawableSource>(picture->source))
            return error(bad_picture, *picture_id);
        if (!picture->attributes.clip)
            return error(bad_match);
        Region region;
        if (!copy_region(*picture->attributes.clip, region) ||
            !server_.add_xfixes_region(
                *id, std::move(region), config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 10: { // DestroyRegion
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed_xfixes("truncated XFIXES DestroyRegion request");
        if (!server_.erase_xfixes_region(*id))
            return error(bad_region, *id);
        return Result<void>::success();
    }
    case 12: { // CopyRegion
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto source_id = reader.u32();
        const auto destination_id = reader.u32();
        if (!source_id || !destination_id)
            return malformed_xfixes("truncated XFIXES CopyRegion request");
        const auto *source = server_.xfixes_region(*source_id);
        if (source == nullptr)
            return error(bad_region, *source_id);
        auto *destination = server_.xfixes_region(*destination_id);
        if (destination == nullptr)
            return error(bad_region, *destination_id);
        Region result;
        if (!copy_region(*source, result))
            return error(bad_alloc);
        *destination = std::move(result);
        return Result<void>::success();
    }
    case 13: // UnionRegion
    case 14: // IntersectRegion
    case 15: { // SubtractRegion
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto first_id = reader.u32();
        const auto second_id = reader.u32();
        const auto destination_id = reader.u32();
        if (!first_id || !second_id || !destination_id)
            return malformed_xfixes("truncated XFIXES region combine request");
        const auto *first = server_.xfixes_region(*first_id);
        if (first == nullptr)
            return error(bad_region, *first_id);
        const auto *second = server_.xfixes_region(*second_id);
        if (second == nullptr)
            return error(bad_region, *second_id);
        auto *destination = server_.xfixes_region(*destination_id);
        if (destination == nullptr)
            return error(bad_region, *destination_id);
        const RegionOperation operation = context.data == 13
            ? RegionOperation::unite
            : (context.data == 14 ? RegionOperation::intersect
                                  : RegionOperation::subtract);
        Region result;
        if (!Region::combine(operation, *first, *second, result))
            return error(bad_alloc);
        *destination = std::move(result);
        return Result<void>::success();
    }
    case 16: { // InvertRegion
        if (context.request.size() != 20)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto source_id = reader.u32();
        const auto x = reader.u16();
        const auto y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        const auto destination_id = reader.u32();
        if (!source_id || !x || !y || !width || !height || !destination_id)
            return malformed_xfixes("truncated XFIXES InvertRegion request");
        const auto *source = server_.xfixes_region(*source_id);
        if (source == nullptr)
            return error(bad_region, *source_id);
        auto *destination = server_.xfixes_region(*destination_id);
        if (destination == nullptr)
            return error(bad_region, *destination_id);
        Region bounds;
        const std::vector<Rectangle> rectangles{{
            signed_word(*x), signed_word(*y), *width, *height}};
        Region result;
        if (!Region::canonicalize(rectangles, bounds) ||
            !Region::combine(
                RegionOperation::invert, *source, bounds, result)) {
            return error(bad_alloc);
        }
        *destination = std::move(result);
        return Result<void>::success();
    }
    case 17: { // TranslateRegion
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto id = reader.u32();
        const auto dx = reader.u16();
        const auto dy = reader.u16();
        if (!id || !dx || !dy)
            return malformed_xfixes(
                "truncated XFIXES TranslateRegion request");
        auto *region = server_.xfixes_region(*id);
        if (region == nullptr)
            return error(bad_region, *id);
        if (!region->translate(signed_word(*dx), signed_word(*dy)))
            return error(bad_alloc);
        return Result<void>::success();
    }
    case 18: { // RegionExtents
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto source_id = reader.u32();
        const auto destination_id = reader.u32();
        if (!source_id || !destination_id)
            return malformed_xfixes("truncated XFIXES RegionExtents request");
        const auto *source = server_.xfixes_region(*source_id);
        if (source == nullptr)
            return error(bad_region, *source_id);
        auto *destination = server_.xfixes_region(*destination_id);
        if (destination == nullptr)
            return error(bad_region, *destination_id);
        std::vector<Rectangle> rectangles;
        if (!source->empty())
            rectangles.push_back(source->extents());
        Region result;
        if (!Region::canonicalize(rectangles, result))
            return error(bad_alloc);
        *destination = std::move(result);
        return Result<void>::success();
    }
    case 19: { // FetchRegion
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed_xfixes("truncated XFIXES FetchRegion request");
        const auto *region = server_.xfixes_region(*id);
        if (region == nullptr)
            return error(bad_region, *id);
        const auto &rectangles = region->rectangles();
        if (rectangles.size() > std::numeric_limits<std::uint32_t>::max() / 2)
            return error(bad_alloc);
        const Rectangle extents = region->extents();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(rectangles.size() * 2));
        reply.i16(static_cast<std::int16_t>(extents.x));
        reply.i16(static_cast<std::int16_t>(extents.y));
        reply.u16(static_cast<std::uint16_t>(extents.width));
        reply.u16(static_cast<std::uint16_t>(extents.height));
        reply.pad(16);
        for (const auto &rectangle : rectangles) {
            reply.i16(static_cast<std::int16_t>(rectangle.x));
            reply.i16(static_cast<std::int16_t>(rectangle.y));
            reply.u16(static_cast<std::uint16_t>(rectangle.width));
            reply.u16(static_cast<std::uint16_t>(rectangle.height));
        }
        return queue(reply.data());
    }
    case 20: { // SetGCClipRegion
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto gc_id = reader.u32();
        const auto region_id = reader.u32();
        const auto x = reader.u16();
        const auto y = reader.u16();
        if (!gc_id || !region_id || !x || !y)
            return malformed_xfixes(
                "truncated XFIXES SetGCClipRegion request");
        auto *gc = server_.graphics_context(*gc_id);
        if (gc == nullptr)
            return error(bad_gc, *gc_id);
        std::optional<Region> clip;
        if (*region_id != 0) {
            const auto *region = server_.xfixes_region(*region_id);
            if (region == nullptr)
                return error(bad_region, *region_id);
            Region copy;
            if (!copy_region(*region, copy))
                return error(bad_alloc);
            clip = std::move(copy);
        }
        gc->clip_x_origin = signed_word(*x);
        gc->clip_y_origin = signed_word(*y);
        gc->clip_region = std::move(clip);
        return Result<void>::success();
    }
    case 21: { // SetWindowShapeRegion
        if (context.request.size() != 20)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto window_id = reader.u32();
        const auto kind = reader.u8();
        if (!window_id || !kind || !reader.skip(3))
            return malformed_xfixes(
                "truncated XFIXES SetWindowShapeRegion header");
        const auto x = reader.u16();
        const auto y = reader.u16();
        const auto region_id = reader.u32();
        if (!x || !y || !region_id)
            return malformed_xfixes(
                "truncated XFIXES SetWindowShapeRegion request");
        auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        if (*kind > 2)
            return error(bad_value, *kind);
        std::optional<Region> shape;
        if (*region_id != 0) {
            const auto *region = server_.xfixes_region(*region_id);
            if (region == nullptr)
                return error(bad_region, *region_id);
            Region copy;
            if (!copy_region(*region, copy) ||
                !copy.translate(signed_word(*x), signed_word(*y))) {
                return error(bad_alloc);
            }
            shape = std::move(copy);
        }
        const auto result = server_.set_window_shape(
            *window, *kind, std::move(shape));
        if (result == ShapeUpdate::invalid)
            return error(bad_value, *kind);
        if (result != ShapeUpdate::updated)
            return error(bad_alloc);
        return drain_pending_events();
    }
    case 22: { // SetPictureClipRegion
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto picture_id = reader.u32();
        const auto region_id = reader.u32();
        const auto x = reader.u16();
        const auto y = reader.u16();
        if (!picture_id || !region_id || !x || !y)
            return malformed_xfixes(
                "truncated XFIXES SetPictureClipRegion request");
        auto *picture = server_.render_picture(*picture_id);
        if (picture == nullptr)
            return error(bad_picture, *picture_id);
        if (!std::holds_alternative<RenderDrawableSource>(picture->source))
            return error(bad_picture, *picture_id);
        std::optional<Region> clip;
        if (*region_id != 0) {
            const auto *region = server_.xfixes_region(*region_id);
            if (region == nullptr)
                return error(bad_region, *region_id);
            Region copy;
            if (!copy_region(*region, copy))
                return error(bad_alloc);
            clip = std::move(copy);
        }
        picture->attributes.clip_x_origin = signed_word(*x);
        picture->attributes.clip_y_origin = signed_word(*y);
        picture->attributes.clip = std::move(clip);
        return Result<void>::success();
    }
    case 23: // SetCursorName
    case 27: { // ChangeCursorByName
        if (context.request.size() < 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto cursor_id = reader.u32();
        const auto size = reader.u16();
        if (!cursor_id || !size || !reader.skip(2))
            return malformed_xfixes("truncated XFIXES cursor name header");
        const auto padded = padded_to_four(*size);
        if (!padded || context.request.size() != 12 + *padded)
            return error(bad_length);
        auto *cursor = server_.cursor(*cursor_id);
        if (cursor == nullptr || !cursor->image)
            return error(bad_cursor, *cursor_id);
        const std::string_view name{
            reinterpret_cast<const char *>(context.request.data() + 12),
            *size};
        if (context.data == 23) {
            const AtomId atom = server_.atoms().intern(name);
            if (atom == 0)
                return error(bad_alloc);
            cursor->image->name = atom;
            return Result<void>::success();
        }
        const AtomId atom = server_.atoms().intern(name, true);
        if (atom == 0)
            return Result<void>::success();
        return update_result(server_.replace_cursor_by_name(
            cursor->image, atom));
    }
    case 24: { // GetCursorName
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto cursor_id = reader.u32();
        if (!cursor_id)
            return malformed_xfixes("truncated XFIXES GetCursorName request");
        const auto *cursor = server_.cursor(*cursor_id);
        if (cursor == nullptr || !cursor->image)
            return error(bad_cursor, *cursor_id);
        std::string_view name;
        if (cursor->image->name != 0) {
            const auto atom_name = server_.atoms().name(cursor->image->name);
            if (atom_name)
                name = *atom_name;
        }
        const auto padded = padded_to_four(name.size());
        if (!padded)
            return error(bad_alloc);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(*padded / 4));
        reply.u32(cursor->image->name);
        reply.u16(static_cast<std::uint16_t>(name.size()));
        reply.pad(18);
        reply.bytes(name);
        reply.pad(*padded - name.size());
        return queue(reply.data());
    }
    case 26: { // ChangeCursor
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto source_id = reader.u32();
        const auto destination_id = reader.u32();
        if (!source_id || !destination_id)
            return malformed_xfixes("truncated XFIXES ChangeCursor request");
        const auto *source = server_.cursor(*source_id);
        if (source == nullptr || !source->image)
            return error(bad_cursor, *source_id);
        const auto *destination = server_.cursor(*destination_id);
        if (destination == nullptr || !destination->image)
            return error(bad_cursor, *destination_id);
        return update_result(server_.replace_cursor(
            source->image, destination->image));
    }
    case 28: { // ExpandRegion
        if (context.request.size() != 20)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto source_id = reader.u32();
        const auto destination_id = reader.u32();
        const auto left = reader.u16();
        const auto right = reader.u16();
        const auto top = reader.u16();
        const auto bottom = reader.u16();
        if (!source_id || !destination_id || !left || !right || !top ||
            !bottom) {
            return malformed_xfixes("truncated XFIXES ExpandRegion request");
        }
        const auto *source = server_.xfixes_region(*source_id);
        if (source == nullptr)
            return error(bad_region, *source_id);
        auto *destination = server_.xfixes_region(*destination_id);
        if (destination == nullptr)
            return error(bad_region, *destination_id);
        std::vector<Rectangle> rectangles;
        try {
            rectangles.reserve(source->rectangles().size());
            for (const auto &rectangle : source->rectangles()) {
                const std::int64_t x =
                    static_cast<std::int64_t>(rectangle.x) - *left;
                const std::int64_t y =
                    static_cast<std::int64_t>(rectangle.y) - *top;
                const std::uint64_t width =
                    static_cast<std::uint64_t>(rectangle.width) +
                    *left + *right;
                const std::uint64_t height =
                    static_cast<std::uint64_t>(rectangle.height) +
                    *top + *bottom;
                if (x < std::numeric_limits<std::int32_t>::min() ||
                    y < std::numeric_limits<std::int32_t>::min() ||
                    width > std::numeric_limits<std::uint32_t>::max() ||
                    height > std::numeric_limits<std::uint32_t>::max()) {
                    return error(bad_alloc);
                }
                rectangles.push_back({
                    static_cast<std::int32_t>(x),
                    static_cast<std::int32_t>(y),
                    static_cast<std::uint32_t>(width),
                    static_cast<std::uint32_t>(height)});
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        Region result;
        if (!Region::canonicalize(rectangles, result))
            return error(bad_alloc);
        *destination = std::move(result);
        return Result<void>::success();
    }
    case 29: // HideCursor
    case 30: { // ShowCursor
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window_id = reader.u32();
        if (!window_id)
            return malformed_xfixes("truncated XFIXES cursor visibility request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        const auto result = context.data == 29
            ? server_.hide_cursor(config_.resource_base)
            : server_.show_cursor(config_.resource_base);
        if (result == XFixesUpdate::invalid)
            return error(bad_match);
        return update_result(result);
    }
    case 31: { // CreatePointerBarrier
        if (context.request.size() < 28)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto id = reader.u32();
        const auto window_id = reader.u32();
        const auto x1 = reader.u16();
        const auto y1 = reader.u16();
        const auto x2 = reader.u16();
        const auto y2 = reader.u16();
        const auto directions = reader.u32();
        if (!id || !window_id || !x1 || !y1 || !x2 || !y2 || !directions ||
            !reader.skip(2)) {
            return malformed_xfixes(
                "truncated XFIXES CreatePointerBarrier header");
        }
        const auto device_count = reader.u16();
        if (!device_count)
            return malformed_xfixes(
                "truncated XFIXES CreatePointerBarrier request");
        const auto device_bytes = checked_multiply(
            static_cast<std::size_t>(*device_count), std::size_t{2});
        const auto padded = device_bytes ? padded_to_four(*device_bytes)
                                         : std::optional<std::size_t>{};
        if (!padded || context.request.size() != 28 + *padded)
            return error(bad_length);
        if (const auto invalid = check_new_resource(*id))
            return *invalid;
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        const auto sx1 = signed_word(*x1);
        const auto sy1 = signed_word(*y1);
        const auto sx2 = signed_word(*x2);
        const auto sy2 = signed_word(*y2);
        const bool vertical = sx1 == sx2;
        const bool horizontal = sy1 == sy2;
        if (vertical == horizontal || (vertical && sx1 < 0) ||
            (horizontal && sy1 < 0)) {
            return error(bad_value);
        }
        XFixesBarrierRecord barrier;
        barrier.id = *id;
        barrier.window = *window_id;
        barrier.x1 = sx1;
        barrier.y1 = sy1;
        barrier.x2 = sx2;
        barrier.y2 = sy2;
        barrier.directions = *directions & 15U;
        try {
            barrier.devices.reserve(*device_count);
            for (std::uint16_t index = 0; index < *device_count; ++index) {
                const auto device = reader.u16();
                if (!device)
                    return malformed_xfixes(
                        "truncated XFIXES pointer barrier devices");
                // Xmin deliberately exposes one core master pointer and no
                // XInput device namespace.
                if (*device != 2)
                    return error(bad_value, *device);
                barrier.devices.push_back(*device);
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        if (!server_.add_xfixes_barrier(
                std::move(barrier), config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 32: { // DeletePointerBarrier
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed_xfixes(
                "truncated XFIXES DeletePointerBarrier request");
        const auto owner = server_.resource_owner(*id);
        if (!owner || !server_.resource_exists(*id))
            return error(bad_barrier, *id);
        if (*owner != config_.resource_base)
            return error(bad_access, *id);
        if (!server_.erase_xfixes_barrier(*id, config_.resource_base))
            return error(bad_barrier, *id);
        return Result<void>::success();
    }
    case 33: { // SetClientDisconnectMode
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto mode = reader.u32();
        if (!mode)
            return malformed_xfixes(
                "truncated XFIXES SetClientDisconnectMode request");
        xfixes_disconnect_mode_ = *mode;
        return Result<void>::success();
    }
    case 34: { // GetClientDisconnectMode
        if (context.request.size() != 4)
            return error(bad_length);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(xfixes_disconnect_mode_);
        reply.pad(20);
        return queue(reply.data());
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::next
