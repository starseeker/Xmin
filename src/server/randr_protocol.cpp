#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/extension_registry.hpp"
#include "xmin/server/property_data.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_atom = 5,
    bad_match = 8,
    bad_access = 10,
    bad_alloc = 11,
    bad_name = 15,
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

std::int32_t
signed_dword(std::uint32_t value) noexcept
{
    const std::int64_t widened = value;
    return static_cast<std::int32_t>(
        widened <= std::numeric_limits<std::int32_t>::max()
            ? widened
            : widened - (std::int64_t{1} << 32));
}

Result<void>
malformed_randr(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

std::vector<std::uint32_t>
sorted_mode_ids(const RandrState &state)
{
    std::vector<std::uint32_t> result;
    result.reserve(state.modes.size());
    for (const auto &entry : state.modes)
        result.push_back(entry.first);
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<std::size_t>
output_property_bytes(const RandrState &state) noexcept
{
    std::size_t result = 0;
    for (const auto &entry : state.output_properties) {
        const auto current = checked_add(
            result, entry.second.value.data.size());
        const auto pending = current
            ? checked_add(*current, entry.second.pending_value.data.size())
            : std::optional<std::size_t>{};
        if (!pending)
            return std::nullopt;
        result = *pending;
    }
    return result;
}

void
write_mode(WireWriter &writer, const RandrModeInfo &mode)
{
    writer.u32(mode.id);
    writer.u16(mode.width);
    writer.u16(mode.height);
    writer.u32(mode.dot_clock);
    writer.u16(mode.hsync_start);
    writer.u16(mode.hsync_end);
    writer.u16(mode.htotal);
    writer.u16(mode.hskew);
    writer.u16(mode.vsync_start);
    writer.u16(mode.vsync_end);
    writer.u16(mode.vtotal);
    writer.u16(static_cast<std::uint16_t>(mode.name.size()));
    writer.u32(mode.flags);
}

bool
write_reply_length(WireWriter &writer, std::size_t trailing_bytes)
{
    const auto padded = padded_to_four(trailing_bytes);
    if (!padded || *padded / 4 > std::numeric_limits<std::uint32_t>::max())
        return false;
    writer.u32(static_cast<std::uint32_t>(*padded / 4));
    return true;
}

} // namespace

Result<void>
Connection::handle_randr(const RequestContext &context)
{
    constexpr std::uint8_t bad_output = randr_extension.first_error;
    constexpr std::uint8_t bad_crtc = randr_extension.first_error + 1;
    constexpr std::uint8_t bad_mode = randr_extension.first_error + 2;
    constexpr std::uint8_t bad_provider = randr_extension.first_error + 3;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto update = [&](RandrUpdate result) {
        if (result == RandrUpdate::invalid)
            return error(bad_match);
        if (result == RandrUpdate::resource_exhausted ||
            result == RandrUpdate::queue_full) {
            return error(bad_alloc);
        }
        return drain_pending_events();
    };
    const auto require_window = [&](std::uint32_t id)
        -> std::optional<Result<void>> {
        if (server_.window(id) == nullptr)
            return error(bad_window, id);
        return std::nullopt;
    };
    const auto require_output = [&](std::uint32_t id)
        -> std::optional<Result<void>> {
        if (id != randr_output_id)
            return error(bad_output, id);
        return std::nullopt;
    };
    const auto require_crtc = [&](std::uint32_t id)
        -> std::optional<Result<void>> {
        if (id != randr_crtc_id)
            return error(bad_crtc, id);
        return std::nullopt;
    };
    const auto require_atom = [&](std::uint32_t id)
        -> std::optional<Result<void>> {
        if (!server_.atoms().name(id))
            return error(bad_atom, id);
        return std::nullopt;
    };

    const auto required_minor = [](std::uint8_t opcode) {
        if (opcode == 0)
            return std::uint16_t{0};
        if (opcode == 2 || opcode == 4 || opcode == 5)
            return std::uint16_t{0};
        if (opcode >= 6 && opcode <= 24)
            return std::uint16_t{2};
        if (opcode >= 25 && opcode <= 31)
            return std::uint16_t{3};
        if (opcode >= 32 && opcode <= 41)
            return std::uint16_t{4};
        if (opcode >= 42 && opcode <= 44)
            return std::uint16_t{5};
        if (opcode >= 45 && opcode <= 46)
            return std::uint16_t{6};
        return std::numeric_limits<std::uint16_t>::max();
    };
    if (context.data != 0 &&
        (randr_major_version_ < 1 ||
         (randr_major_version_ == 1 &&
          required_minor(context.data) > randr_minor_version_))) {
        return error(bad_request);
    }

    try {
    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto major = reader.u32();
        const auto minor = reader.u32();
        if (!major || !minor)
            return malformed_randr("truncated RANDR QueryVersion request");
        const bool client_is_older =
            *major < randr_extension.major_version ||
            (*major == randr_extension.major_version &&
             *minor < randr_extension.minor_version);
        const std::uint32_t negotiated_major = client_is_older
            ? *major
            : randr_extension.major_version;
        const std::uint32_t negotiated_minor = client_is_older
            ? *minor
            : randr_extension.minor_version;
        randr_major_version_ = *major;
        randr_minor_version_ = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(
                *minor, std::numeric_limits<std::uint16_t>::max()));
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
    case 2: { // SetScreenConfig
        const bool client_knows_rates = randr_major_version_ > 1 ||
            (randr_major_version_ == 1 && randr_minor_version_ >= 1);
        const std::size_t expected_size = client_knows_rates ? 24 : 20;
        if (context.request.size() != expected_size)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window = reader.u32();
        const auto timestamp = reader.u32();
        const auto config_timestamp = reader.u32();
        const auto size = reader.u16();
        const auto rotation = reader.u16();
        if (!window || !timestamp || !config_timestamp || !size ||
            !rotation) {
            return malformed_randr(
                "truncated RANDR SetScreenConfig request");
        }
        std::uint16_t rate = 0;
        if (client_knows_rates) {
            const auto requested_rate = reader.u16();
            if (!requested_rate || !reader.skip(2))
                return malformed_randr(
                    "truncated RANDR SetScreenConfig rate");
            rate = *requested_rate;
        }
        if (auto failed = require_window(*window))
            return *failed;
        std::uint8_t status = 0;
        if (*config_timestamp != server_.randr().config_timestamp) {
            status = 1;
        }
        else if (*timestamp != 0 &&
                 static_cast<std::int32_t>(
                     *timestamp - server_.randr().timestamp) < 0) {
            status = 2;
        }
        else if (*size != 0) {
            return error(bad_value, *size);
        }
        else if (((*rotation & 0x0fU) != 1 &&
                  (*rotation & 0x0fU) != 2 &&
                  (*rotation & 0x0fU) != 4 &&
                  (*rotation & 0x0fU) != 8) ||
                 (*rotation & ~0x3fU) != 0) {
            return error(bad_value, *rotation);
        }
        else if (*rotation != 1) {
            return error(bad_match, *rotation);
        }
        else if (rate != 0 && rate != 60) {
            return error(bad_value, rate);
        }
        else {
            auto candidate = server_.randr();
            candidate.rotation = *rotation;
            const auto changed = server_.commit_randr_state(
                std::move(candidate), 1U | 2U | 4U);
            if (changed != RandrUpdate::updated)
                return update(changed);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(status);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.randr().timestamp);
        reply.u32(server_.randr().config_timestamp);
        reply.u32(root_window_id);
        reply.u16(0);
        reply.pad(10);
        return queue(reply.data());
    }
    case 4: { // SelectInput
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window = reader.u32();
        const auto mask = reader.u16();
        if (!window || !mask || !reader.skip(2))
            return malformed_randr("truncated RANDR SelectInput request");
        if (auto failed = require_window(*window))
            return *failed;
        if ((*mask & ~0xffU) != 0)
            return error(bad_value, *mask);
        return update(server_.select_randr_input(
            config_.resource_base, *window, *mask));
    }
    case 5: { // GetScreenInfo
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window = reader.u32();
        if (!window)
            return malformed_randr("truncated RANDR GetScreenInfo request");
        if (auto failed = require_window(*window))
            return *failed;
        const auto &state = server_.randr();
        const bool client_knows_rates = randr_major_version_ > 1 ||
            (randr_major_version_ == 1 && randr_minor_version_ >= 1);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(1);
        reply.u16(context.sequence);
        reply.u32(client_knows_rates ? 3 : 2);
        reply.u32(root_window_id);
        reply.u32(state.timestamp);
        reply.u32(state.config_timestamp);
        reply.u16(1);
        reply.u16(0);
        reply.u16(state.rotation);
        reply.u16(60);
        reply.u16(client_knows_rates ? 2 : 0);
        reply.pad(2);
        reply.u16(server_.width());
        reply.u16(server_.height());
        reply.u16(static_cast<std::uint16_t>(
            std::min<std::uint32_t>(state.millimetre_width, 65535U)));
        reply.u16(static_cast<std::uint16_t>(
            std::min<std::uint32_t>(state.millimetre_height, 65535U)));
        if (client_knows_rates) {
            reply.u16(1);
            reply.u16(60);
        }
        return queue(reply.data());
    }
    case 6: { // GetScreenSizeRange
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window = reader.u32();
        if (!window)
            return malformed_randr(
                "truncated RANDR GetScreenSizeRange request");
        if (auto failed = require_window(*window))
            return *failed;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(1);
        reply.u16(1);
        reply.u16(4096);
        reply.u16(4096);
        reply.pad(16);
        return queue(reply.data());
    }
    case 7: { // SetScreenSize
        if (context.request.size() != 20)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto window = reader.u32();
        const auto width = reader.u16();
        const auto height = reader.u16();
        const auto mm_width = reader.u32();
        const auto mm_height = reader.u32();
        if (!window || !width || !height || !mm_width || !mm_height)
            return malformed_randr("truncated RANDR SetScreenSize request");
        if (auto failed = require_window(*window))
            return *failed;
        if (*width == 0 || *width > 4096)
            return error(bad_value, *width);
        if (*height == 0 || *height > 4096)
            return error(bad_value, *height);
        if (*mm_width == 0 || *mm_height == 0)
            return error(bad_value, 0);
        const auto &current = server_.randr();
        const auto mode = current.modes.find(current.current_mode);
        if (mode != current.modes.end() &&
            (static_cast<std::uint32_t>(current.crtc_x) +
                     mode->second.width > *width ||
             static_cast<std::uint32_t>(current.crtc_y) +
                     mode->second.height > *height)) {
            return error(bad_match);
        }
        auto candidate = current;
        candidate.millimetre_width = *mm_width;
        candidate.millimetre_height = *mm_height;
        return update(server_.resize_randr_screen(
            std::move(candidate), *width, *height));
    }
    case 8:
    case 25: { // GetScreenResources[/Current]
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window = reader.u32();
        if (!window)
            return malformed_randr(
                "truncated RANDR GetScreenResources request");
        if (auto failed = require_window(*window))
            return *failed;
        const auto &state = server_.randr();
        std::vector<std::uint32_t> modes;
        try {
            modes = sorted_mode_ids(state);
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        std::size_t names_length = 0;
        for (const auto id : modes)
            names_length += state.modes.at(id).name.size();
        const std::size_t trailing = 8 + modes.size() * 32 + names_length;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        if (!write_reply_length(reply, trailing))
            return error(bad_alloc);
        reply.u32(state.timestamp);
        reply.u32(state.config_timestamp);
        reply.u16(1);
        reply.u16(1);
        reply.u16(static_cast<std::uint16_t>(modes.size()));
        reply.u16(static_cast<std::uint16_t>(names_length));
        reply.pad(8);
        reply.u32(randr_crtc_id);
        reply.u32(randr_output_id);
        for (const auto id : modes)
            write_mode(reply, state.modes.at(id));
        for (const auto id : modes)
            reply.bytes(state.modes.at(id).name);
        reply.pad_to_four();
        return queue(reply.data());
    }
    case 9: { // GetOutputInfo
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto output = reader.u32();
        const auto timestamp = reader.u32();
        if (!output || !timestamp)
            return malformed_randr("truncated RANDR GetOutputInfo request");
        if (auto failed = require_output(*output))
            return *failed;
        const auto &state = server_.randr();
        constexpr std::string_view name = "XMIN-0";
        const std::size_t trailing = 8 + state.output_modes.size() * 4 +
            name.size();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(*timestamp != 0 && *timestamp != state.config_timestamp
                     ? 1
                     : 0);
        reply.u16(context.sequence);
        if (!write_reply_length(reply, trailing))
            return error(bad_alloc);
        reply.u32(state.timestamp);
        reply.u32(state.current_mode == 0 ? 0 : randr_crtc_id);
        reply.u32(state.millimetre_width);
        reply.u32(state.millimetre_height);
        reply.u8(0);
        reply.u8(0);
        reply.u16(1);
        reply.u16(static_cast<std::uint16_t>(state.output_modes.size()));
        reply.u16(state.output_modes.empty() ? 0 : 1);
        reply.u16(0);
        reply.u16(static_cast<std::uint16_t>(name.size()));
        reply.u32(randr_crtc_id);
        for (const auto id : state.output_modes)
            reply.u32(id);
        reply.bytes(name);
        reply.pad_to_four();
        return queue(reply.data());
    }
    case 10: { // ListOutputProperties
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto output = reader.u32();
        if (!output)
            return malformed_randr(
                "truncated RANDR ListOutputProperties request");
        if (auto failed = require_output(*output))
            return *failed;
        std::vector<AtomId> atoms;
        try {
            atoms.reserve(server_.randr().output_properties.size());
            for (const auto &entry : server_.randr().output_properties)
                atoms.push_back(entry.first);
            std::sort(atoms.begin(), atoms.end());
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(atoms.size()));
        reply.u16(static_cast<std::uint16_t>(atoms.size()));
        reply.pad(22);
        for (const auto atom : atoms)
            reply.u32(atom);
        return queue(reply.data());
    }
    case 11: { // QueryOutputProperty
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto output = reader.u32();
        const auto property = reader.u32();
        if (!output || !property)
            return malformed_randr(
                "truncated RANDR QueryOutputProperty request");
        if (auto failed = require_output(*output))
            return *failed;
        if (auto failed = require_atom(*property))
            return *failed;
        const auto found = server_.randr().output_properties.find(*property);
        if (found == server_.randr().output_properties.end())
            return error(bad_name, *property);
        const auto &record = found->second;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(record.valid_values.size()));
        reply.u8(record.pending ? 1 : 0);
        reply.u8(record.range ? 1 : 0);
        reply.u8(record.immutable ? 1 : 0);
        reply.pad(21);
        for (const auto value : record.valid_values)
            reply.u32(static_cast<std::uint32_t>(value));
        return queue(reply.data());
    }
    case 12: { // ConfigureOutputProperty
        if (context.request.size() < 16 ||
            (context.request.size() - 16) % 4 != 0) {
            return error(bad_length);
        }
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto output = reader.u32();
        const auto property = reader.u32();
        const auto pending = reader.u8();
        const auto range = reader.u8();
        if (!output || !property || !pending || !range || !reader.skip(2))
            return malformed_randr(
                "truncated RANDR ConfigureOutputProperty request");
        if (auto failed = require_output(*output))
            return *failed;
        if (auto failed = require_atom(*property))
            return *failed;
        if (*pending > 1 || *range > 1)
            return error(bad_value, *pending > 1 ? *pending : *range);
        auto candidate = server_.randr();
        const bool creates_property =
            candidate.output_properties.count(*property) == 0;
        if (creates_property && candidate.output_properties.size() >=
                maximum_randr_output_properties) {
            return error(bad_alloc);
        }
        auto &record = candidate.output_properties[*property];
        if (!*pending)
            record.pending_value = {};
        record.pending = *pending != 0;
        record.range = *range != 0;
        record.valid_values.clear();
        try {
            record.valid_values.reserve(reader.remaining() / 4);
            while (reader.remaining() != 0) {
                const auto value = reader.u32();
                if (!value)
                    return malformed_randr(
                        "truncated RANDR output property values");
                record.valid_values.push_back(signed_dword(*value));
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        if (record.range && (record.valid_values.size() & 1U) != 0)
            return error(bad_match);
        return update(server_.commit_randr_state(
            std::move(candidate), 0));
    }
    case 13: { // ChangeOutputProperty
        if (context.request.size() < 24)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto output = reader.u32();
        const auto property = reader.u32();
        const auto type = reader.u32();
        const auto format = reader.u8();
        const auto mode = reader.u8();
        if (!output || !property || !type || !format || !mode ||
            !reader.skip(2)) {
            return malformed_randr(
                "truncated RANDR ChangeOutputProperty request");
        }
        const auto units = reader.u32();
        if (!units)
            return malformed_randr(
                "truncated RANDR ChangeOutputProperty length");
        if (auto failed = require_output(*output))
            return *failed;
        if (auto failed = require_atom(*property))
            return *failed;
        if (auto failed = require_atom(*type))
            return *failed;
        if (*format != 8 && *format != 16 && *format != 32)
            return error(bad_value, *format);
        if (*mode > 2)
            return error(bad_value, *mode);
        const auto bytes = checked_multiply(
            static_cast<std::size_t>(*units),
            static_cast<std::size_t>(*format / 8U));
        const auto padded = bytes ? padded_to_four(*bytes) : std::nullopt;
        if (!bytes || !padded || reader.remaining() != *padded)
            return error(bad_length);
        const auto data = canonical_property_data(
            context.request.data() + 24, *bytes, *format, context.order);
        if (!data)
            return malformed_randr("misaligned RANDR output property data");
        auto candidate = server_.randr();
        auto found = candidate.output_properties.find(*property);
        if (found == candidate.output_properties.end() &&
            candidate.output_properties.size() >=
                maximum_randr_output_properties) {
            return error(bad_alloc);
        }
        if (found != candidate.output_properties.end() &&
            found->second.immutable) {
            return error(bad_access, *property);
        }
        auto &record = candidate.output_properties[*property];
        auto &value = record.pending
            ? record.pending_value
            : record.value;
        if (*mode != 0 && !value.data.empty() &&
            (value.type != *type || value.format != *format)) {
            return error(bad_match);
        }
        const std::size_t previous_size = value.data.size();
        try {
            if (*mode == 0) {
                value.data = *data;
            }
            else if (*mode == 1) {
                value.data.insert(value.data.begin(), data->begin(),
                                  data->end());
            }
            else {
                value.data.insert(value.data.end(), data->begin(), data->end());
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        value.type = *type;
        value.format = *format;
        const auto total = output_property_bytes(candidate);
        if (value.data.size() > maximum_property_bytes ||
            !total || *total < previous_size ||
            *total > maximum_server_property_bytes) {
            return error(bad_alloc);
        }
        return update(server_.commit_randr_state(
            std::move(candidate), 8U, *property, 0));
    }
    case 14: { // DeleteOutputProperty
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto output = reader.u32();
        const auto property = reader.u32();
        if (!output || !property)
            return malformed_randr(
                "truncated RANDR DeleteOutputProperty request");
        if (auto failed = require_output(*output))
            return *failed;
        if (auto failed = require_atom(*property))
            return *failed;
        auto candidate = server_.randr();
        const auto found = candidate.output_properties.find(*property);
        if (found == candidate.output_properties.end())
            return error(bad_name, *property);
        if (found->second.immutable)
            return error(bad_access, *property);
        candidate.output_properties.erase(found);
        return update(server_.commit_randr_state(
            std::move(candidate), 8U, *property, 1));
    }
    case 15: { // GetOutputProperty
        if (context.request.size() != 28)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 24, context.order);
        const auto output = reader.u32();
        const auto property = reader.u32();
        const auto requested_type = reader.u32();
        const auto offset = reader.u32();
        const auto length = reader.u32();
        const auto remove = reader.u8();
        const auto pending = reader.u8();
        if (!output || !property || !requested_type || !offset || !length ||
            !remove || !pending || !reader.skip(2)) {
            return malformed_randr(
                "truncated RANDR GetOutputProperty request");
        }
        if (auto failed = require_output(*output))
            return *failed;
        if (auto failed = require_atom(*property))
            return *failed;
        if (*requested_type != 0) {
            if (auto failed = require_atom(*requested_type))
                return *failed;
        }
        if (*remove > 1 || *pending > 1)
            return error(bad_value, *remove > 1 ? *remove : *pending);
        const auto found = server_.randr().output_properties.find(*property);
        WireWriter reply(context.order);
        reply.u8(1);
        if (found == server_.randr().output_properties.end()) {
            reply.u8(0);
            reply.u16(context.sequence);
            reply.u32(0);
            reply.u32(0);
            reply.u32(0);
            reply.u32(0);
            reply.pad(12);
            return queue(reply.data());
        }
        const auto &record = found->second;
        const auto &value = *pending != 0 && record.pending
            ? record.pending_value
            : record.value;
        if (*remove != 0 && record.immutable)
            return error(bad_access, *property);
        if (*requested_type != 0 && *requested_type != value.type) {
            reply.u8(value.format);
            reply.u16(context.sequence);
            reply.u32(0);
            reply.u32(value.type);
            reply.u32(static_cast<std::uint32_t>(value.data.size()));
            reply.u32(0);
            reply.pad(12);
            return queue(reply.data());
        }
        const auto start = checked_multiply(
            static_cast<std::size_t>(*offset), std::size_t{4});
        const auto requested = checked_multiply(
            static_cast<std::size_t>(*length), std::size_t{4});
        if (!start || !requested || *start > value.data.size())
            return error(bad_value, *offset);
        const std::size_t returned = std::min(
            *requested, value.data.size() - *start);
        const std::size_t after = value.data.size() - *start - returned;
        const auto encoded = returned == 0
            ? std::vector<std::uint8_t>{}
            : wire_property_data(value.data.data() + *start, returned,
                                 value.format, context.order);
        reply.u8(value.format);
        reply.u16(context.sequence);
        const auto padded = padded_to_four(encoded.size());
        if (!padded)
            return error(bad_alloc);
        reply.u32(static_cast<std::uint32_t>(*padded / 4));
        reply.u32(value.type);
        reply.u32(static_cast<std::uint32_t>(after));
        reply.u32(static_cast<std::uint32_t>(
            encoded.size() / (value.format / 8U)));
        reply.pad(12);
        reply.bytes(encoded);
        reply.pad(*padded - encoded.size());
        if (*remove != 0 && after == 0) {
            auto candidate = server_.randr();
            candidate.output_properties.erase(*property);
            const auto changed = server_.commit_randr_state(
                std::move(candidate), 8U, *property, 1);
            if (changed != RandrUpdate::updated)
                return update(changed);
        }
        return queue(reply.data());
    }
    case 16: { // CreateMode
        if (context.request.size() < 40)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window = reader.u32();
        RandrModeInfo mode;
        const auto ignored_id = reader.u32();
        const auto width = reader.u16();
        const auto height = reader.u16();
        const auto dot_clock = reader.u32();
        const auto hsync_start = reader.u16();
        const auto hsync_end = reader.u16();
        const auto htotal = reader.u16();
        const auto hskew = reader.u16();
        const auto vsync_start = reader.u16();
        const auto vsync_end = reader.u16();
        const auto vtotal = reader.u16();
        const auto name_length = reader.u16();
        const auto flags = reader.u32();
        if (!window || !ignored_id || !width || !height || !dot_clock ||
            !hsync_start || !hsync_end || !htotal || !hskew ||
            !vsync_start || !vsync_end || !vtotal || !name_length || !flags) {
            return malformed_randr("truncated RANDR CreateMode request");
        }
        if (auto failed = require_window(*window))
            return *failed;
        const auto padded_name = padded_to_four(*name_length);
        if (!padded_name || reader.remaining() != *padded_name)
            return error(bad_length);
        if (*width == 0 || *height == 0 || *name_length == 0 ||
            *name_length > 255)
            return error(bad_value);
        try {
            mode.name.reserve(*name_length);
            for (std::size_t index = 0; index < *name_length; ++index) {
                const auto byte = reader.u8();
                if (!byte)
                    return malformed_randr("truncated RANDR mode name");
                mode.name.push_back(static_cast<char>(*byte));
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        auto candidate = server_.randr();
        if (candidate.modes.size() >= maximum_randr_modes)
            return error(bad_alloc);
        if (std::any_of(
                candidate.modes.begin(), candidate.modes.end(),
                [&mode](const auto &entry) {
                    return entry.second.name == mode.name;
                })) {
            return error(bad_name);
        }
        while (candidate.next_mode_id == 0 ||
               candidate.modes.count(candidate.next_mode_id) != 0) {
            ++candidate.next_mode_id;
        }
        mode.id = candidate.next_mode_id++;
        mode.width = *width;
        mode.height = *height;
        mode.dot_clock = *dot_clock;
        mode.hsync_start = *hsync_start;
        mode.hsync_end = *hsync_end;
        mode.htotal = *htotal;
        mode.hskew = *hskew;
        mode.vsync_start = *vsync_start;
        mode.vsync_end = *vsync_end;
        mode.vtotal = *vtotal;
        mode.flags = *flags;
        const std::uint32_t id = mode.id;
        try {
            candidate.modes.emplace(id, std::move(mode));
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        const auto changed = server_.commit_randr_state(
            std::move(candidate), 64U);
        if (changed != RandrUpdate::updated)
            return update(changed);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(id);
        reply.pad(20);
        return queue(reply.data());
    }
    case 17: { // DestroyMode
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed_randr("truncated RANDR DestroyMode request");
        auto candidate = server_.randr();
        const auto found = candidate.modes.find(*id);
        if (found == candidate.modes.end())
            return error(bad_mode, *id);
        if (found->second.built_in)
            return error(bad_match, *id);
        if (candidate.current_mode == *id ||
            std::find(candidate.output_modes.begin(),
                      candidate.output_modes.end(), *id) !=
                candidate.output_modes.end()) {
            return error(bad_access, *id);
        }
        candidate.modes.erase(found);
        return update(server_.commit_randr_state(
            std::move(candidate), 64U));
    }
    case 18:
    case 19: { // Add/DeleteOutputMode
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto output = reader.u32();
        const auto id = reader.u32();
        if (!output || !id)
            return malformed_randr(
                "truncated RANDR output mode request");
        if (auto failed = require_output(*output))
            return *failed;
        auto candidate = server_.randr();
        if (candidate.modes.count(*id) == 0)
            return error(bad_mode, *id);
        const auto found = std::find(candidate.output_modes.begin(),
                                     candidate.output_modes.end(), *id);
        if (context.data == 18) {
            if (found == candidate.output_modes.end()) {
                try {
                    candidate.output_modes.push_back(*id);
                }
                catch (const std::bad_alloc &) {
                    return error(bad_alloc);
                }
            }
        }
        else {
            if (candidate.current_mode == *id)
                return error(bad_match, *id);
            if (candidate.modes.at(*id).built_in ||
                found == candidate.output_modes.end())
                return error(bad_access, *id);
            candidate.output_modes.erase(found);
        }
        return update(server_.commit_randr_state(
            std::move(candidate), 4U | 64U));
    }
    case 20: { // GetCrtcInfo
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto crtc = reader.u32();
        const auto timestamp = reader.u32();
        if (!crtc || !timestamp)
            return malformed_randr("truncated RANDR GetCrtcInfo request");
        if (auto failed = require_crtc(*crtc))
            return *failed;
        const auto &state = server_.randr();
        const auto mode = state.modes.find(state.current_mode);
        const bool enabled = mode != state.modes.end();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(*timestamp != 0 && *timestamp != state.config_timestamp
                     ? 1
                     : 0);
        reply.u16(context.sequence);
        reply.u32(enabled ? 2 : 1);
        reply.u32(state.timestamp);
        reply.i16(enabled ? state.crtc_x : 0);
        reply.i16(enabled ? state.crtc_y : 0);
        reply.u16(enabled ? mode->second.width : 0);
        reply.u16(enabled ? mode->second.height : 0);
        reply.u32(enabled ? state.current_mode : 0);
        reply.u16(state.rotation);
        reply.u16(1);
        reply.u16(enabled ? 1 : 0);
        reply.u16(1);
        if (enabled)
            reply.u32(randr_output_id);
        reply.u32(randr_output_id);
        return queue(reply.data());
    }
    case 21: { // SetCrtcConfig
        if (context.request.size() < 28 ||
            (context.request.size() - 28) % 4 != 0) {
            return error(bad_length);
        }
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto crtc = reader.u32();
        const auto timestamp = reader.u32();
        const auto config_timestamp = reader.u32();
        const auto x = reader.u16();
        const auto y = reader.u16();
        const auto mode_id = reader.u32();
        const auto rotation = reader.u16();
        if (!crtc || !timestamp || !config_timestamp || !x || !y ||
            !mode_id || !rotation || !reader.skip(2)) {
            return malformed_randr(
                "truncated RANDR SetCrtcConfig request");
        }
        if (auto failed = require_crtc(*crtc))
            return *failed;
        const auto &state = server_.randr();
        std::uint8_t status = 0;
        if (*config_timestamp != 0 &&
            *config_timestamp != state.config_timestamp) {
            status = 1;
        }
        else if ((*rotation & 0x0fU) != 1 &&
                 (*rotation & 0x0fU) != 2 &&
                 (*rotation & 0x0fU) != 4 &&
                 (*rotation & 0x0fU) != 8) {
            return error(bad_value, *rotation);
        }
        else if ((*rotation & ~0x3fU) != 0) {
            return error(bad_value, *rotation);
        }
        else if (*rotation != 1) {
            return error(bad_match, *rotation);
        }
        else if (*mode_id == 0) {
            if (reader.remaining() != 0)
                return error(bad_match);
        }
        else {
            const auto mode = state.modes.find(*mode_id);
            const std::int32_t sx = signed_word(*x);
            const std::int32_t sy = signed_word(*y);
            if (mode == state.modes.end())
                return error(bad_mode, *mode_id);
            if (reader.remaining() != 4)
                return error(bad_match);
            const auto output = reader.u32();
            if (!output)
                return malformed_randr(
                    "truncated RANDR SetCrtcConfig output");
            if (auto failed = require_output(*output))
                return *failed;
            if (std::find(state.output_modes.begin(),
                          state.output_modes.end(), *mode_id) ==
                state.output_modes.end()) {
                return error(bad_match, *mode_id);
            }
            if (sx < 0 ||
                static_cast<std::uint32_t>(sx) + mode->second.width >
                    server_.width()) {
                return error(bad_value, *x);
            }
            if (sy < 0 ||
                static_cast<std::uint32_t>(sy) + mode->second.height >
                    server_.height()) {
                return error(bad_value, *y);
            }
        }
        if (status == 0) {
            auto candidate = state;
            candidate.current_mode = *mode_id;
            candidate.crtc_x = *mode_id == 0 ? 0 : signed_word(*x);
            candidate.crtc_y = *mode_id == 0 ? 0 : signed_word(*y);
            candidate.rotation = *rotation;
            const auto changed = server_.commit_randr_state(
                std::move(candidate), 2U | 4U);
            if (changed != RandrUpdate::updated)
                return update(changed);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(status);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.randr().timestamp);
        reply.pad(20);
        return queue(reply.data());
    }
    case 22:
    case 23: { // GetCrtcGammaSize/GetCrtcGamma
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto crtc = reader.u32();
        if (!crtc)
            return malformed_randr("truncated RANDR gamma request");
        if (auto failed = require_crtc(*crtc))
            return *failed;
        const auto &state = server_.randr();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(context.data == 23
                      ? static_cast<std::uint32_t>(
                            state.gamma_red.size() * 3U * 2U / 4U)
                      : 0);
        reply.u16(static_cast<std::uint16_t>(state.gamma_red.size()));
        reply.pad(22);
        if (context.data == 23) {
            for (const auto value : state.gamma_red)
                reply.u16(value);
            for (const auto value : state.gamma_green)
                reply.u16(value);
            for (const auto value : state.gamma_blue)
                reply.u16(value);
        }
        return queue(reply.data());
    }
    case 24: { // SetCrtcGamma
        if (context.request.size() < 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto crtc = reader.u32();
        const auto size = reader.u16();
        if (!crtc || !size || !reader.skip(2))
            return malformed_randr("truncated RANDR SetCrtcGamma request");
        if (auto failed = require_crtc(*crtc))
            return *failed;
        if (*size != server_.randr().gamma_red.size() ||
            reader.remaining() != static_cast<std::size_t>(*size) * 6U) {
            return error(bad_match, *size);
        }
        auto candidate = server_.randr();
        for (auto *channel : {&candidate.gamma_red,
                              &candidate.gamma_green,
                              &candidate.gamma_blue}) {
            for (auto &value : *channel) {
                const auto component = reader.u16();
                if (!component)
                    return malformed_randr(
                        "truncated RANDR gamma components");
                value = *component;
            }
        }
        return update(server_.commit_randr_state(
            std::move(candidate), 0));
    }
    case 26: { // SetCrtcTransform
        if (context.request.size() < 48)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto crtc = reader.u32();
        if (!crtc)
            return malformed_randr(
                "truncated RANDR SetCrtcTransform request");
        if (auto failed = require_crtc(*crtc))
            return *failed;
        RandrTransform transform;
        for (auto &element : transform.matrix) {
            const auto value = reader.u32();
            if (!value)
                return malformed_randr("truncated RANDR transform matrix");
            element = signed_dword(*value);
        }
        const auto filter_length = reader.u16();
        if (!filter_length || !reader.skip(2))
            return malformed_randr("truncated RANDR transform filter");
        const auto padded_filter = padded_to_four(*filter_length);
        if (!padded_filter || reader.remaining() < *padded_filter ||
            (reader.remaining() - *padded_filter) % 4 != 0)
            return error(bad_length);
        if ((reader.remaining() - *padded_filter) / 4 >
            maximum_randr_filter_parameters) {
            return error(bad_alloc);
        }
        try {
            transform.filter.reserve(*filter_length);
            for (std::size_t index = 0; index < *filter_length; ++index) {
                const auto byte = reader.u8();
                if (!byte)
                    return malformed_randr(
                        "truncated RANDR transform filter name");
                transform.filter.push_back(static_cast<char>(*byte));
            }
            if (!reader.skip(*padded_filter - *filter_length))
                return malformed_randr("truncated RANDR transform padding");
            transform.parameters.reserve(reader.remaining() / 4);
            while (reader.remaining() != 0) {
                const auto value = reader.u32();
                if (!value)
                    return malformed_randr(
                        "truncated RANDR transform parameters");
                transform.parameters.push_back(signed_dword(*value));
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        auto candidate = server_.randr();
        candidate.transform = std::move(transform);
        return update(server_.commit_randr_state(
            std::move(candidate), 2U));
    }
    case 27: { // GetCrtcTransform
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto crtc = reader.u32();
        if (!crtc)
            return malformed_randr(
                "truncated RANDR GetCrtcTransform request");
        if (auto failed = require_crtc(*crtc))
            return *failed;
        const auto &transform = server_.randr().transform;
        const std::size_t one = *padded_to_four(transform.filter.size()) +
            transform.parameters.size() * 4U;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>((64U + one * 2U) / 4U));
        for (const auto value : transform.matrix)
            reply.u32(static_cast<std::uint32_t>(value));
        reply.u8(1);
        reply.pad(3);
        for (const auto value : transform.matrix)
            reply.u32(static_cast<std::uint32_t>(value));
        reply.pad(4);
        reply.u16(static_cast<std::uint16_t>(transform.filter.size()));
        reply.u16(static_cast<std::uint16_t>(transform.parameters.size()));
        reply.u16(static_cast<std::uint16_t>(transform.filter.size()));
        reply.u16(static_cast<std::uint16_t>(transform.parameters.size()));
        for (unsigned copy = 0; copy < 2; ++copy) {
            reply.bytes(transform.filter);
            reply.pad_to_four();
            for (const auto value : transform.parameters)
                reply.u32(static_cast<std::uint32_t>(value));
        }
        return queue(reply.data());
    }
    case 28: { // GetPanning
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto crtc = reader.u32();
        if (!crtc)
            return malformed_randr("truncated RANDR GetPanning request");
        if (auto failed = require_crtc(*crtc))
            return *failed;
        const auto &panning = server_.randr().panning;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(1);
        reply.u32(server_.randr().timestamp);
        reply.u16(panning.left);
        reply.u16(panning.top);
        reply.u16(panning.width);
        reply.u16(panning.height);
        reply.u16(panning.track_left);
        reply.u16(panning.track_top);
        reply.u16(panning.track_width);
        reply.u16(panning.track_height);
        reply.i16(panning.border_left);
        reply.i16(panning.border_top);
        reply.i16(panning.border_right);
        reply.i16(panning.border_bottom);
        return queue(reply.data());
    }
    case 29: { // SetPanning
        if (context.request.size() != 36)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 32, context.order);
        const auto crtc = reader.u32();
        const auto timestamp = reader.u32();
        std::array<std::uint16_t, 12> values{};
        if (!crtc || !timestamp)
            return malformed_randr("truncated RANDR SetPanning request");
        for (auto &value : values) {
            const auto field = reader.u16();
            if (!field)
                return malformed_randr("truncated RANDR panning values");
            value = *field;
        }
        if (auto failed = require_crtc(*crtc))
            return *failed;
        auto candidate = server_.randr();
        candidate.panning = {
            values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7],
            signed_word(values[8]), signed_word(values[9]),
            signed_word(values[10]), signed_word(values[11])};
        const auto changed = server_.commit_randr_state(
            std::move(candidate), 2U);
        if (changed != RandrUpdate::updated)
            return update(changed);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.randr().timestamp);
        reply.pad(20);
        return queue(reply.data());
    }
    case 30: { // SetOutputPrimary
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window = reader.u32();
        const auto output = reader.u32();
        if (!window || !output)
            return malformed_randr(
                "truncated RANDR SetOutputPrimary request");
        if (auto failed = require_window(*window))
            return *failed;
        if (*output != 0) {
            if (auto failed = require_output(*output))
                return *failed;
        }
        auto candidate = server_.randr();
        candidate.primary_output = *output;
        for (auto &entry : candidate.monitors)
            entry.second.primary = *output != 0 &&
                std::find(entry.second.outputs.begin(),
                          entry.second.outputs.end(), *output) !=
                    entry.second.outputs.end();
        return update(server_.commit_randr_state(
            std::move(candidate), 64U));
    }
    case 31: { // GetOutputPrimary
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window = reader.u32();
        if (!window)
            return malformed_randr(
                "truncated RANDR GetOutputPrimary request");
        if (auto failed = require_window(*window))
            return *failed;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.randr().primary_output);
        reply.pad(20);
        return queue(reply.data());
    }
    case 32: { // GetProviders: deliberately no provider graph
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window = reader.u32();
        if (!window)
            return malformed_randr("truncated RANDR GetProviders request");
        if (auto failed = require_window(*window))
            return *failed;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.randr().timestamp);
        reply.u16(0);
        reply.pad(18);
        return queue(reply.data());
    }
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
    case 39:
    case 40:
    case 41: { // Provider operations are outside the single-output model.
        bool valid_length = false;
        switch (context.data) {
        case 33:
        case 37:
        case 40:
            valid_length = context.request.size() == 12;
            break;
        case 34:
        case 35:
            valid_length = context.request.size() == 16;
            break;
        case 36:
            valid_length = context.request.size() == 8;
            break;
        case 38:
            valid_length = context.request.size() >= 16 &&
                (context.request.size() - 16) % 4 == 0;
            break;
        case 39:
            valid_length = context.request.size() >= 24 &&
                (context.request.size() & 3U) == 0;
            break;
        case 41:
            valid_length = context.request.size() == 28;
            break;
        default:
            break;
        }
        if (!valid_length)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto provider = reader.u32();
        if (!provider)
            return malformed_randr("truncated RANDR provider request");
        return error(bad_provider, *provider);
    }
    case 42: { // GetMonitors
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window = reader.u32();
        const auto active = reader.u8();
        if (!window || !active || !reader.skip(3))
            return malformed_randr("truncated RANDR GetMonitors request");
        if (auto failed = require_window(*window))
            return *failed;
        if (*active > 1)
            return error(bad_value, *active);
        std::vector<AtomId> names;
        std::size_t outputs = 0;
        try {
            for (const auto &entry : server_.randr().monitors) {
                if (*active != 0 &&
                    (entry.second.width == 0 || entry.second.height == 0))
                    continue;
                names.push_back(entry.first);
                outputs += entry.second.outputs.size();
            }
            std::sort(names.begin(), names.end());
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(names.size() * 6U + outputs));
        reply.u32(server_.randr().timestamp);
        reply.u32(static_cast<std::uint32_t>(names.size()));
        reply.u32(static_cast<std::uint32_t>(outputs));
        reply.pad(12);
        for (const auto name : names) {
            const auto &monitor = server_.randr().monitors.at(name);
            reply.u32(monitor.name);
            reply.u8(monitor.primary ? 1 : 0);
            reply.u8(monitor.automatic ? 1 : 0);
            reply.u16(static_cast<std::uint16_t>(monitor.outputs.size()));
            reply.i16(monitor.x);
            reply.i16(monitor.y);
            reply.u16(monitor.width);
            reply.u16(monitor.height);
            reply.u32(monitor.millimetre_width);
            reply.u32(monitor.millimetre_height);
            for (const auto output : monitor.outputs)
                reply.u32(output);
        }
        return queue(reply.data());
    }
    case 43: { // SetMonitor
        if (context.request.size() < 32 ||
            (context.request.size() - 32) % 4 != 0) {
            return error(bad_length);
        }
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window = reader.u32();
        const auto name = reader.u32();
        const auto primary = reader.u8();
        const auto automatic = reader.u8();
        const auto output_count = reader.u16();
        const auto x = reader.u16();
        const auto y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        const auto mm_width = reader.u32();
        const auto mm_height = reader.u32();
        if (!window || !name || !primary || !automatic || !output_count ||
            !x || !y || !width || !height || !mm_width || !mm_height) {
            return malformed_randr("truncated RANDR SetMonitor request");
        }
        if (auto failed = require_window(*window))
            return *failed;
        if (auto failed = require_atom(*name))
            return *failed;
        const auto monitor_name = server_.atoms().name(*name);
        if (monitor_name && *monitor_name == "XMIN-0") {
            return error(bad_value, *name);
        }
        if (*primary > 1 || *automatic > 1 ||
            reader.remaining() != static_cast<std::size_t>(*output_count) * 4U)
            return error(bad_value);
        auto candidate = server_.randr();
        if (candidate.monitors.count(*name) != 0)
            return error(bad_value, *name);
        if (candidate.monitors.size() >= maximum_randr_monitors)
            return error(bad_alloc);
        RandrMonitor monitor;
        monitor.name = *name;
        monitor.primary = *primary != 0;
        monitor.x = signed_word(*x);
        monitor.y = signed_word(*y);
        monitor.width = *width;
        monitor.height = *height;
        monitor.millimetre_width = *mm_width;
        monitor.millimetre_height = *mm_height;
        try {
            monitor.outputs.reserve(*output_count);
            while (reader.remaining() != 0) {
                const auto output = reader.u32();
                if (!output)
                    return malformed_randr("truncated RANDR monitor outputs");
                if (auto failed = require_output(*output))
                    return *failed;
                if (std::find(monitor.outputs.begin(), monitor.outputs.end(),
                              *output) != monitor.outputs.end())
                    return error(bad_match, *output);
                monitor.outputs.push_back(*output);
            }
            if (monitor.primary) {
                for (auto &entry : candidate.monitors)
                    entry.second.primary = false;
                if (!monitor.outputs.empty())
                    candidate.primary_output = monitor.outputs.front();
            }
            if (!monitor.outputs.empty()) {
                for (auto iterator = candidate.monitors.begin();
                     iterator != candidate.monitors.end();) {
                    const bool covered = iterator->second.automatic &&
                        std::any_of(
                            iterator->second.outputs.begin(),
                            iterator->second.outputs.end(),
                            [&monitor](std::uint32_t output) {
                                return std::find(
                                    monitor.outputs.begin(),
                                    monitor.outputs.end(), output) !=
                                    monitor.outputs.end();
                            });
                    if (covered)
                        iterator = candidate.monitors.erase(iterator);
                    else
                        ++iterator;
                }
            }
            candidate.monitors[*name] = std::move(monitor);
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        return update(server_.commit_randr_state(
            std::move(candidate), 64U));
    }
    case 44: { // DeleteMonitor
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window = reader.u32();
        const auto name = reader.u32();
        if (!window || !name)
            return malformed_randr("truncated RANDR DeleteMonitor request");
        if (auto failed = require_window(*window))
            return *failed;
        if (auto failed = require_atom(*name))
            return *failed;
        auto candidate = server_.randr();
        const auto found = candidate.monitors.find(*name);
        if (found == candidate.monitors.end())
            return error(bad_value, *name);
        if (found->second.automatic)
            return error(bad_match, *name);
        candidate.monitors.erase(found);
        const bool output_is_covered = std::any_of(
            candidate.monitors.begin(), candidate.monitors.end(),
            [](const auto &entry) {
                return std::find(entry.second.outputs.begin(),
                                 entry.second.outputs.end(),
                                 randr_output_id) !=
                    entry.second.outputs.end();
            });
        if (!output_is_covered && candidate.current_mode != 0) {
            const AtomId automatic_name = server_.atoms().intern(
                "XMIN-0", true);
            candidate.monitors.emplace(
                automatic_name,
                RandrMonitor{automatic_name, true, true, 0, 0, 0, 0,
                             0, 0, {randr_output_id}});
        }
        return update(server_.commit_randr_state(
            std::move(candidate), 64U));
    }
    case 45: { // CreateLease: intentionally unsupported without DRM.
        if (context.request.size() < 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window = reader.u32();
        const auto lease = reader.u32();
        const auto crtcs = reader.u16();
        const auto outputs = reader.u16();
        if (!window || !lease || !crtcs || !outputs)
            return malformed_randr("truncated RANDR CreateLease request");
        if (auto failed = require_window(*window))
            return *failed;
        if (reader.remaining() !=
            static_cast<std::size_t>(*crtcs + *outputs) * 4U) {
            return error(bad_length);
        }
        return error(bad_match, *lease);
    }
    case 46: { // FreeLease: there can be no valid lease resource.
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto lease = reader.u32();
        const auto terminate = reader.u8();
        if (!lease || !terminate || !reader.skip(3))
            return malformed_randr("truncated RANDR FreeLease request");
        if (*terminate > 1)
            return error(bad_value, *terminate);
        return error(bad_value, *lease);
    }
    default:
        return error(bad_request);
    }
    }
    catch (const std::bad_alloc &) {
        return error(bad_alloc);
    }
}

} // namespace xmin::server
