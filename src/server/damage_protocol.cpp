#include "xmin/server/connection.hpp"

#include "xmin/server/extension_registry.hpp"

#include <algorithm>
#include <string_view>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_drawable = 9,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_length = 16,
};

Result<void>
malformed_damage(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

} // namespace

Result<void>
Connection::handle_damage(const RequestContext &context)
{
    constexpr std::uint8_t bad_region = xfixes_extension.first_error;
    constexpr std::uint8_t bad_damage = damage_extension.first_error;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto update = [&](DamageUpdate result) {
        if (result == DamageUpdate::invalid)
            return error(bad_damage);
        if (result == DamageUpdate::resource_exhausted ||
            result == DamageUpdate::queue_full) {
            return error(bad_alloc);
        }
        return drain_pending_events();
    };

    if (context.data != 0 && damage_major_version_ < 1)
        return error(bad_request);

    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto major = reader.u32();
        const auto minor = reader.u32();
        if (!major || !minor)
            return malformed_damage(
                "truncated DAMAGE QueryVersion request");
        const bool client_is_older =
            *major < damage_extension.major_version ||
            (*major == damage_extension.major_version &&
             *minor < damage_extension.minor_version);
        const std::uint32_t negotiated_major = client_is_older
            ? *major
            : damage_extension.major_version;
        const std::uint32_t negotiated_minor = client_is_older
            ? *minor
            : damage_extension.minor_version;
        damage_major_version_ = negotiated_major;
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
    case 1: { // Create
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto drawable = reader.u32();
        const auto level = reader.u8();
        if (!id || !drawable || !level || !reader.skip(3))
            return malformed_damage("truncated DAMAGE Create request");
        if (*level > 3)
            return error(bad_value, *level);
        if (server_.drawable_surface(*drawable) == nullptr)
            return error(bad_drawable, *drawable);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (server_.resource_limit_reached(config_.resource_base))
            return error(bad_alloc);
        return update(server_.add_damage(
            DamageRecord{*id, config_.resource_base, *drawable, *level, {}},
            config_.resource_base));
    }
    case 2: { // Destroy
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed_damage("truncated DAMAGE Destroy request");
        if (!server_.erase_damage(*id))
            return error(bad_damage, *id);
        return Result<void>::success();
    }
    case 3: { // Subtract
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto repair_id = reader.u32();
        const auto parts_id = reader.u32();
        if (!id || !repair_id || !parts_id)
            return malformed_damage("truncated DAMAGE Subtract request");
        if (server_.damage(*id) == nullptr)
            return error(bad_damage, *id);
        const Region *repair = nullptr;
        Region *parts = nullptr;
        if (*repair_id != 0) {
            repair = server_.xfixes_region(*repair_id);
            if (repair == nullptr)
                return error(bad_region, *repair_id);
        }
        if (*parts_id != 0) {
            parts = server_.xfixes_region(*parts_id);
            if (parts == nullptr)
                return error(bad_region, *parts_id);
        }
        return update(server_.subtract_damage(*id, repair, parts));
    }
    case 4: { // Add
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto drawable = reader.u32();
        const auto region_id = reader.u32();
        if (!drawable || !region_id)
            return malformed_damage("truncated DAMAGE Add request");
        if (server_.drawable_surface(*drawable) == nullptr)
            return error(bad_drawable, *drawable);
        const auto *region = server_.xfixes_region(*region_id);
        if (region == nullptr)
            return error(bad_region, *region_id);
        return update(server_.damage_drawable(*drawable, region));
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::server
