#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/extension_registry.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_match = 8,
    bad_drawable = 9,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_length = 16,
};

enum : std::uint32_t {
    glx_rgba_type = 0x8014,
    glx_width = 0x801d,
    glx_height = 0x801e,
    glx_event_mask = 0x801f,
    glx_fbcconfig_id = 0x8013,
};

constexpr std::uint32_t glx_config_count = 8;

bool
valid_fbconfig(std::uint32_t id) noexcept
{
    return id >= 1 && id <= glx_config_count;
}

std::uint32_t
config_depth(std::uint32_t id) noexcept
{
    constexpr std::array<std::uint32_t, 4> values{0, 16, 24, 24};
    return values[(id - 1U) % values.size()];
}

std::uint32_t
config_stencil(std::uint32_t id) noexcept
{
    return (id - 1U) % 4U == 3U ? 8U : 0U;
}

bool
config_double_buffer(std::uint32_t id) noexcept
{
    return id > 4U;
}

std::size_t
padded_string_size(std::size_t size) noexcept
{
    return (size + 3U) & ~std::size_t{3};
}

} // namespace

Result<void>
Connection::handle_glx(const RequestContext &context)
{
    constexpr std::uint8_t bad_context = glx_extension.first_error;
    constexpr std::uint8_t bad_context_state = bad_context + 1;
    constexpr std::uint8_t glx_bad_drawable = bad_context + 2;
    constexpr std::uint8_t bad_pixmap = bad_context + 3;
    constexpr std::uint8_t bad_context_tag = bad_context + 4;
    constexpr std::uint8_t bad_render_request = bad_context + 6;
    constexpr std::uint8_t bad_large_request = bad_context + 7;
    constexpr std::uint8_t bad_fbconfig = bad_context + 9;
    constexpr std::uint8_t bad_pbuffer = bad_context + 10;
    constexpr std::uint8_t bad_current_drawable = bad_context + 11;
    constexpr std::uint8_t bad_window = bad_context + 12;
    constexpr std::uint8_t bad_profile = bad_context + 13;

    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto exact = [&](std::size_t size) {
        return context.request.size() == size;
    };
    const auto context_exists = [&](std::uint32_t id) {
        return glx_contexts_.find(id) != glx_contexts_.end();
    };
    const auto drawable_exists = [&](std::uint32_t id) {
        return glx_drawables_.find(id) != glx_drawables_.end() ||
            server_.readable_surface(id) != nullptr;
    };
    const auto drawable_is_current = [&](std::uint32_t id) {
        for (const auto &[tag, binding] : glx_bindings_) {
            static_cast<void>(tag);
            if (binding.draw == id || binding.read == id)
                return true;
        }
        return false;
    };
    const auto next_context_tag = [&]() {
        std::uint32_t tag = 0;
        do {
            tag = next_glx_context_tag_++;
            if (next_glx_context_tag_ == 0)
                next_glx_context_tag_ = 1;
        } while (tag == 0 ||
                 glx_bindings_.find(tag) != glx_bindings_.end());
        return tag;
    };
    const auto parse_attributes = [&](WireReader &reader, std::uint32_t count,
                                      auto &&visitor) -> bool {
        if (count > 256 || reader.remaining() != count * 8U)
            return false;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto name = reader.u32();
            const auto value = reader.u32();
            if (!name || !value)
                return false;
            visitor(*name, *value);
        }
        return true;
    };
    const auto make_current_reply = [&](std::uint32_t tag) {
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(tag);
        reply.pad(20);
        return queue(reply.data());
    };

    switch (context.data) {
    case 1: // Render (indirect command stream)
        return error(bad_render_request);
    case 2: // RenderLarge (indirect command stream)
        return error(bad_large_request);
    case 3: { // CreateContext
        if (!exact(24))
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 20, context.order);
        const auto id = reader.u32();
        const auto visual = reader.u32();
        const auto screen = reader.u32();
        const auto share = reader.u32();
        const auto direct = reader.u8();
        if (!id || !visual || !screen || !share || !direct ||
            !reader.skip(3))
            return error(bad_length);
        if (*direct != 1)
            return error(bad_value, *direct);
        if (*screen != 0 || *visual != root_visual_id)
            return error(bad_match, *visual);
        if (*share != 0 && !context_exists(*share))
            return error(bad_context, *share);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (!server_.reserve_resource(*id, ResourceKind::glx_context,
                                      config_.resource_base))
            return error(server_.resource_exists(*id) ? bad_id_choice
                                                       : bad_alloc, *id);
        try {
            glx_contexts_.emplace(*id, GlxContextRecord{
                *id, 1, *screen, glx_rgba_type, *share});
        }
        catch (const std::bad_alloc &) {
            static_cast<void>(server_.release_resource(
                *id, ResourceKind::glx_context));
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 4: { // DestroyContext
        if (!exact(8))
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return error(bad_length);
        if (!context_exists(*id))
            return error(bad_context, *id);
        glx_contexts_.erase(*id);
        static_cast<void>(server_.release_resource(
            *id, ResourceKind::glx_context));
        return Result<void>::success();
    }
    case 5: { // MakeCurrent
        if (!exact(16))
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto drawable = reader.u32();
        const auto id = reader.u32();
        const auto old_tag = reader.u32();
        if (!drawable || !id || !old_tag)
            return error(bad_length);
        if (*old_tag != 0 &&
            glx_bindings_.find(*old_tag) == glx_bindings_.end())
            return error(bad_context_tag, *old_tag);
        if (*id == 0) {
            if (*drawable != 0)
                return error(bad_match, *drawable);
            if (*old_tag != 0)
                glx_bindings_.erase(*old_tag);
            return make_current_reply(0);
        }
        if (!context_exists(*id))
            return error(bad_context, *id);
        if (!drawable_exists(*drawable))
            return error(glx_bad_drawable, *drawable);
        const std::uint32_t tag = next_context_tag();
        try {
            glx_bindings_.emplace(tag, GlxBinding{*id, *drawable, *drawable});
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        if (*old_tag != 0)
            glx_bindings_.erase(*old_tag);
        return make_current_reply(tag);
    }
    case 6: { // IsDirect
        if (!exact(8))
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return error(bad_length);
        if (!context_exists(*id))
            return error(bad_context, *id);
        WireWriter reply(context.order);
        reply.u8(1); reply.u8(0); reply.u16(context.sequence); reply.u32(0);
        reply.u8(1); reply.pad(23);
        return queue(reply.data());
    }
    case 7: { // QueryVersion
        if (!exact(12))
            return error(bad_length);
        WireWriter reply(context.order);
        reply.u8(1); reply.u8(0); reply.u16(context.sequence); reply.u32(0);
        reply.u32(1); reply.u32(4); reply.pad(16);
        return queue(reply.data());
    }
    case 8: // WaitGL
    case 9: { // WaitX
        if (!exact(8))
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto tag = reader.u32();
        if (!tag)
            return error(bad_length);
        if (*tag != 0 && glx_bindings_.find(*tag) == glx_bindings_.end())
            return error(bad_context_tag, *tag);
        return Result<void>::success();
    }
    case 10: { // CopyContext
        if (!exact(20))
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto source = reader.u32();
        const auto destination = reader.u32();
        const auto mask = reader.u32();
        const auto tag = reader.u32();
        if (!source || !destination || !mask || !tag)
            return error(bad_length);
        if (!context_exists(*source)) return error(bad_context, *source);
        if (!context_exists(*destination)) return error(bad_context, *destination);
        if (*tag != 0 && glx_bindings_.find(*tag) == glx_bindings_.end())
            return error(bad_context_tag, *tag);
        return Result<void>::success();
    }
    case 11: { // SwapBuffers: direct clients present themselves
        if (!exact(12))
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto tag = reader.u32();
        const auto drawable = reader.u32();
        if (!tag || !drawable)
            return error(bad_length);
        if (*tag != 0 && glx_bindings_.find(*tag) == glx_bindings_.end())
            return error(bad_context_tag, *tag);
        if (!drawable_exists(*drawable))
            return error(glx_bad_drawable, *drawable);
        return Result<void>::success();
    }
    case 12: { // UseXFont belongs to indirect display-list execution
        if (!exact(24)) return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto tag = reader.u32();
        if (!tag) return error(bad_length);
        if (glx_bindings_.find(*tag) == glx_bindings_.end())
            return error(bad_context_tag, *tag);
        return error(bad_context_state, *tag);
    }
    case 13: // CreateGLXPixmap (GLX 1.2)
    case 22: // CreatePixmap
    case 31: { // CreateWindow
        const std::size_t base = context.data == 13 ? 20U : 24U;
        if ((context.data == 13 && !exact(base)) ||
            (context.data != 13 && context.request.size() < base))
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto screen = reader.u32();
        const auto config = reader.u32();
        const auto source = reader.u32();
        const auto id = reader.u32();
        if (!screen || !config || !source || !id)
            return error(bad_length);
        std::uint32_t count = 0;
        if (context.data != 13) {
            const auto parsed_count = reader.u32();
            if (!parsed_count) return error(bad_length);
            count = *parsed_count;
        }
        if (*screen != 0) return error(bad_value, *screen);
        if (context.data == 13) {
            if (*config != root_visual_id) return error(bad_match, *config);
        }
        else if (!valid_fbconfig(*config)) {
            return error(bad_fbconfig, *config);
        }
        const Surface *surface = nullptr;
        if (context.data == 31) {
            const auto *window = server_.window(*source);
            if (window == nullptr || window->window_class != WindowClass::input_output ||
                !window->surface)
                return error(bad_window, *source);
            surface = window->surface.get();
        }
        else {
            const auto *pixmap = server_.pixmap(*source);
            if (pixmap == nullptr || !pixmap->surface)
                return error(bad_pixmap, *source);
            surface = pixmap->surface.get();
        }
        if (context.data != 13 &&
            !parse_attributes(reader, count, [](auto, auto) {}))
            return error(bad_length);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (!server_.reserve_resource(*id, ResourceKind::glx_drawable,
                                      config_.resource_base))
            return error(server_.resource_exists(*id) ? bad_id_choice
                                                       : bad_alloc, *id);
        const auto kind = context.data == 31
            ? GlxDrawableRecord::Kind::window
            : GlxDrawableRecord::Kind::pixmap;
        try {
            glx_drawables_.emplace(*id, GlxDrawableRecord{
                *id, *source, context.data == 13 ? 1U : *config,
                surface->width(), surface->height(), 0, kind});
        }
        catch (const std::bad_alloc &) {
            static_cast<void>(server_.release_resource(
                *id, ResourceKind::glx_drawable));
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 14: { // GetVisualConfigs
        if (!exact(8)) return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto screen = reader.u32();
        if (!screen) return error(bad_length);
        if (*screen != 0) return error(bad_value, *screen);
        constexpr std::array<std::uint32_t, 18> properties{
            root_visual_id, 4, 1, 8, 8, 8, 8, 0, 0, 0, 0, 0,
            1, 0, 32, 24, 8, 0};
        WireWriter reply(context.order);
        reply.u8(1); reply.u8(0); reply.u16(context.sequence);
        reply.u32(properties.size()); reply.u32(1);
        reply.u32(properties.size()); reply.pad(16);
        for (auto value : properties) reply.u32(value);
        return queue(reply.data());
    }
    case 15: // DestroyGLXPixmap
    case 23: // DestroyPixmap
    case 28: // DestroyPbuffer
    case 32: { // DestroyWindow
        if (!exact(8)) return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id) return error(bad_length);
        const auto found = glx_drawables_.find(*id);
        if (found == glx_drawables_.end()) {
            const auto code = context.data == 28 ? bad_pbuffer
                : (context.data == 32 ? bad_window : bad_pixmap);
            return error(code, *id);
        }
        const auto expected = context.data == 28
            ? GlxDrawableRecord::Kind::pbuffer
            : (context.data == 32 ? GlxDrawableRecord::Kind::window
                                  : GlxDrawableRecord::Kind::pixmap);
        if (found->second.kind != expected) {
            const auto code = context.data == 28 ? bad_pbuffer
                : (context.data == 32 ? bad_window : bad_pixmap);
            return error(code, *id);
        }
        if (drawable_is_current(*id))
            return error(bad_current_drawable, *id);
        glx_drawables_.erase(found);
        static_cast<void>(server_.release_resource(
            *id, ResourceKind::glx_drawable));
        return Result<void>::success();
    }
    case 16: // VendorPrivate
    case 17: // VendorPrivateWithReply
        return error(bad_request);
    case 18: // QueryExtensionsString
    case 19: { // QueryServerString
        if (!exact(context.data == 18 ? 8U : 12U)) return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto screen = reader.u32();
        if (!screen) return error(bad_length);
        if (*screen != 0) return error(bad_value, *screen);
        std::string_view value;
        if (context.data == 18) {
            value = "GLX_ARB_create_context GLX_EXT_swap_control";
        }
        else {
            const auto name = reader.u32();
            if (!name) return error(bad_length);
            if (*name == 1) value = "Xmin";
            else if (*name == 2) value = "1.4 Xmin direct-only";
            else if (*name == 3)
                value = "GLX_ARB_create_context GLX_EXT_swap_control";
            else return error(bad_value, *name);
        }
        const std::size_t padded = padded_string_size(value.size());
        WireWriter reply(context.order);
        reply.u8(1); reply.u8(0); reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(padded / 4));
        reply.u32(0); reply.u32(static_cast<std::uint32_t>(value.size()));
        reply.pad(16);
        reply.bytes(value);
        reply.pad(padded - value.size());
        return queue(reply.data());
    }
    case 20: // ClientInfo
    case 33: // SetClientInfoARB
    case 35: { // SetClientInfo2ARB
        const std::size_t base = context.data == 20 ? 16U : 24U;
        if (context.request.size() < base || (context.request.size() & 3U) != 0)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto major = reader.u32();
        const auto minor = reader.u32();
        if (!major || !minor)
            return error(bad_length);
        if (context.data == 20) {
            const auto string_size = reader.u32();
            if (!string_size)
                return error(bad_length);
            const auto padded = padded_to_four(
                static_cast<std::size_t>(*string_size));
            if (!padded || reader.remaining() != *padded)
                return error(bad_length);
            return Result<void>::success();
        }
        const auto version_count = reader.u32();
        const auto gl_string_size = reader.u32();
        const auto glx_string_size = reader.u32();
        if (!version_count || !gl_string_size || !glx_string_size)
            return error(bad_length);
        const auto version_words = checked_multiply(
            static_cast<std::size_t>(*version_count),
            context.data == 33 ? std::size_t{2} : std::size_t{3});
        const auto version_bytes = version_words
            ? checked_multiply(*version_words, std::size_t{4})
            : std::nullopt;
        const auto padded_gl = padded_to_four(
            static_cast<std::size_t>(*gl_string_size));
        const auto padded_glx = padded_to_four(
            static_cast<std::size_t>(*glx_string_size));
        if (!version_bytes || !padded_gl || !padded_glx)
            return error(bad_length);
        const auto strings_size = checked_add(*padded_gl, *padded_glx);
        const auto body_size = strings_size
            ? checked_add(*version_bytes, *strings_size)
            : std::nullopt;
        if (!body_size || reader.remaining() != *body_size)
            return error(bad_length);
        return Result<void>::success();
    }
    case 21: { // GetFBConfigs
        if (!exact(8)) return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto screen = reader.u32();
        if (!screen) return error(bad_length);
        if (*screen != 0) return error(bad_value, *screen);
        constexpr std::size_t count = 18;
        WireWriter reply(context.order);
        reply.u8(1); reply.u8(0); reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(glx_config_count * count * 2));
        reply.u32(glx_config_count); reply.u32(count); reply.pad(16);
        for (std::uint32_t id = 1; id <= glx_config_count; ++id) {
            const std::array<std::pair<std::uint32_t, std::uint32_t>, count> p{{
                {0x8013, id}, {2, 32}, {3, 0}, {5, config_double_buffer(id)},
                {6, 0}, {7, 0}, {8, 8}, {9, 8}, {10, 8}, {11, 8},
                {12, config_depth(id)}, {13, config_stencil(id)},
                {0x8011, 1}, {0x8010, 7}, {0x8012, 1},
                {0x800b, root_visual_id}, {0x22, 0x8002}, {0x20, 0x8000}}};
            for (const auto &[name, value] : p) {
                reply.u32(name); reply.u32(value);
            }
        }
        return queue(reply.data());
    }
    case 24: // CreateNewContext
    case 34: { // CreateContextAttribsARB
        constexpr std::size_t base = 28U;
        if ((context.data == 24 && !exact(base)) ||
            (context.data == 34 && context.request.size() < base))
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto id = reader.u32();
        const auto config = reader.u32();
        const auto screen = reader.u32();
        std::uint32_t render_type = glx_rgba_type;
        std::optional<std::uint32_t> share;
        std::optional<std::uint8_t> direct;
        if (context.data == 24) {
            const auto parsed_render = reader.u32();
            share = reader.u32(); direct = reader.u8();
            if (!parsed_render || !share || !direct || !reader.skip(3))
                return error(bad_length);
            render_type = *parsed_render;
        }
        else {
            share = reader.u32(); direct = reader.u8();
            if (!share || !direct || !reader.skip(3)) return error(bad_length);
            const auto count = reader.u32();
            if (!count) return error(bad_length);
            bool profile_error = false;
            bool version_error = false;
            std::uint32_t major = 1, minor = 0;
            if (!parse_attributes(reader, *count,
                    [&](std::uint32_t name, std::uint32_t value) {
                        if (name == 0x2091) major = value;
                        else if (name == 0x2092) minor = value;
                        else if (name == 0x9126 && value != 0x2)
                            profile_error = true;
                        else if (name == 0x2094 && value != 0)
                            version_error = true;
                    })) return error(bad_length);
            if (profile_error) return error(bad_profile);
            if (version_error || major > 2 || (major == 2 && minor > 1))
                return error(bad_value);
        }
        if (!id || !config || !screen || !share || !direct)
            return error(bad_length);
        if (*direct != 1) return error(bad_value, *direct);
        if (*screen != 0) return error(bad_value, *screen);
        if (!valid_fbconfig(*config)) return error(bad_fbconfig, *config);
        if (*share != 0 && !context_exists(*share))
            return error(bad_context, *share);
        if (render_type != glx_rgba_type)
            return error(bad_value, render_type);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (!server_.reserve_resource(*id, ResourceKind::glx_context,
                                      config_.resource_base))
            return error(server_.resource_exists(*id) ? bad_id_choice
                                                       : bad_alloc, *id);
        try {
            glx_contexts_.emplace(*id, GlxContextRecord{
                *id, *config, *screen, render_type, *share});
        }
        catch (const std::bad_alloc &) {
            static_cast<void>(server_.release_resource(
                *id, ResourceKind::glx_context));
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 25: { // QueryContext
        if (!exact(8)) return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id) return error(bad_length);
        const auto found = glx_contexts_.find(*id);
        if (found == glx_contexts_.end()) return error(bad_context, *id);
        const auto &record = found->second;
        constexpr std::uint32_t pairs = 5;
        WireWriter reply(context.order);
        reply.u8(1); reply.u8(0); reply.u16(context.sequence);
        reply.u32(pairs * 2); reply.u32(pairs); reply.pad(20);
        reply.u32(0x800a); reply.u32(record.share);
        reply.u32(0x800b); reply.u32(root_visual_id);
        reply.u32(0x800c); reply.u32(record.screen);
        reply.u32(0x8013); reply.u32(record.fbconfig);
        reply.u32(0x8011); reply.u32(record.render_type == glx_rgba_type ? 1 : 0);
        return queue(reply.data());
    }
    case 26: { // MakeContextCurrent
        if (!exact(20)) return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto old_tag = reader.u32();
        const auto draw = reader.u32();
        const auto read = reader.u32();
        const auto id = reader.u32();
        if (!old_tag || !draw || !read || !id) return error(bad_length);
        if (*old_tag != 0 &&
            glx_bindings_.find(*old_tag) == glx_bindings_.end())
            return error(bad_context_tag, *old_tag);
        if (*id == 0) {
            if (*draw != 0 || *read != 0) return error(bad_match);
            if (*old_tag != 0)
                glx_bindings_.erase(*old_tag);
            return make_current_reply(0);
        }
        if (!context_exists(*id)) return error(bad_context, *id);
        if (!drawable_exists(*draw)) return error(glx_bad_drawable, *draw);
        if (!drawable_exists(*read)) return error(glx_bad_drawable, *read);
        const std::uint32_t tag = next_context_tag();
        try { glx_bindings_.emplace(tag, GlxBinding{*id, *draw, *read}); }
        catch (const std::bad_alloc &) { return error(bad_alloc); }
        if (*old_tag != 0)
            glx_bindings_.erase(*old_tag);
        return make_current_reply(tag);
    }
    case 27: { // CreatePbuffer
        if (context.request.size() < 20) return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto screen = reader.u32(); const auto config = reader.u32();
        const auto id = reader.u32(); const auto count = reader.u32();
        if (!screen || !config || !id || !count) return error(bad_length);
        std::uint32_t width = 0, height = 0;
        if (!parse_attributes(reader, *count,
                [&](std::uint32_t name, std::uint32_t value) {
                    if (name == glx_width) width = value;
                    else if (name == glx_height) height = value;
                })) return error(bad_length);
        if (*screen != 0) return error(bad_value, *screen);
        if (!valid_fbconfig(*config)) return error(bad_fbconfig, *config);
        if (width == 0 || height == 0 || width > 4096 || height > 4096)
            return error(bad_value);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (!server_.reserve_resource(*id, ResourceKind::glx_drawable,
                                      config_.resource_base))
            return error(server_.resource_exists(*id) ? bad_id_choice
                                                       : bad_alloc, *id);
        try {
            glx_drawables_.emplace(*id, GlxDrawableRecord{
                *id, 0, *config, width, height, 0,
                GlxDrawableRecord::Kind::pbuffer});
        }
        catch (const std::bad_alloc &) {
            static_cast<void>(server_.release_resource(
                *id, ResourceKind::glx_drawable));
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 29: { // GetDrawableAttributes
        if (!exact(8)) return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id) return error(bad_length);
        const auto found = glx_drawables_.find(*id);
        if (found == glx_drawables_.end()) return error(glx_bad_drawable, *id);
        const auto &record = found->second;
        constexpr std::uint32_t pairs = 4;
        WireWriter reply(context.order);
        reply.u8(1); reply.u8(0); reply.u16(context.sequence);
        reply.u32(pairs * 2); reply.u32(pairs); reply.pad(20);
        reply.u32(glx_width); reply.u32(record.width);
        reply.u32(glx_height); reply.u32(record.height);
        reply.u32(glx_event_mask); reply.u32(record.event_mask);
        reply.u32(glx_fbcconfig_id); reply.u32(record.fbconfig);
        return queue(reply.data());
    }
    case 30: { // ChangeDrawableAttributes
        if (context.request.size() < 12) return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto id = reader.u32(); const auto count = reader.u32();
        if (!id || !count) return error(bad_length);
        const auto found = glx_drawables_.find(*id);
        if (found == glx_drawables_.end()) return error(glx_bad_drawable, *id);
        std::uint32_t mask = found->second.event_mask;
        if (!parse_attributes(reader, *count,
                [&](std::uint32_t name, std::uint32_t value) {
                    if (name == glx_event_mask) mask = value;
                })) return error(bad_length);
        found->second.event_mask = mask;
        return Result<void>::success();
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::server
