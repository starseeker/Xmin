#include "xmin/next/connection.hpp"

#include "xmin/next/checked.hpp"
#include "xmin/next/color.hpp"
#include "xmin/next/generated/core_protocol.hpp"

#include <array>
#include <bitset>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <poll.h>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace xmin::next {
namespace {

constexpr std::uint16_t protocol_major = 11;
constexpr std::uint16_t protocol_minor = 0;
constexpr std::size_t maximum_request_bytes = 65535U * 4U;
constexpr std::size_t maximum_buffered_input = maximum_request_bytes + 16384U;
constexpr std::size_t maximum_buffered_output = 1024U * 1024U;
constexpr std::string_view cookie_protocol = "MIT-MAGIC-COOKIE-1";
constexpr std::uint32_t all_event_masks = 0x01ffffffU;
constexpr std::uint32_t propagate_event_masks = 0x00003f4fU;
constexpr std::uint32_t exclusive_event_masks =
    (1U << 2) | (1U << 18) | (1U << 20); // ButtonPress/redirect masks
constexpr std::uint32_t input_only_attribute_masks =
    (1U << 5) | (1U << 9) | (1U << 11) | (1U << 12) | (1U << 14);

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_pixmap = 4,
    bad_atom = 5,
    bad_cursor = 6,
    bad_match = 8,
    bad_drawable = 9,
    bad_access = 10,
    bad_alloc = 11,
    bad_colormap = 12,
    bad_graphics_context = 13,
    bad_id_choice = 14,
    bad_name = 15,
    bad_length = 16,
};

constexpr std::size_t
opcode_index(CoreOpcode opcode) noexcept
{
    return static_cast<std::uint8_t>(opcode);
}

std::int16_t
signed_word(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

std::optional<std::vector<std::uint8_t>>
canonical_property_data(const std::uint8_t *data, std::size_t size,
                        std::uint8_t format, ByteOrder order)
{
    if (format == 8)
        return std::vector<std::uint8_t>(data, data + size);

    WireReader reader(data, size, order);
    std::vector<std::uint8_t> canonical;
    canonical.reserve(size);
    while (reader.remaining() != 0) {
        if (format == 16) {
            const auto value = reader.u16();
            if (!value)
                return std::nullopt;
            canonical.push_back(static_cast<std::uint8_t>(*value));
            canonical.push_back(static_cast<std::uint8_t>(*value >> 8));
        }
        else {
            const auto value = reader.u32();
            if (!value)
                return std::nullopt;
            for (unsigned shift = 0; shift < 32; shift += 8)
                canonical.push_back(static_cast<std::uint8_t>(*value >> shift));
        }
    }
    return canonical;
}

std::vector<std::uint8_t>
wire_property_data(const std::uint8_t *data, std::size_t size,
                   std::uint8_t format, ByteOrder order)
{
    if (format == 8)
        return std::vector<std::uint8_t>(data, data + size);

    WireWriter writer(order);
    const std::size_t unit = format / 8;
    for (std::size_t offset = 0; offset < size; offset += unit) {
        if (format == 16) {
            const auto value = static_cast<std::uint16_t>(data[offset]) |
                (static_cast<std::uint16_t>(data[offset + 1]) << 8);
            writer.u16(value);
        }
        else {
            std::uint32_t value = 0;
            for (unsigned index = 0; index < 4; ++index) {
                value |= static_cast<std::uint32_t>(data[offset + index])
                    << (index * 8);
            }
            writer.u32(value);
        }
    }
    return writer.data();
}

bool
cookies_equal(const std::vector<std::uint8_t> &left,
              const std::vector<std::uint8_t> &right) noexcept
{
    if (left.size() != right.size())
        return false;
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
        difference |= left[index] ^ right[index];
    return difference == 0;
}

std::uint16_t
millimetres(std::uint16_t pixels) noexcept
{
    const auto tenths = static_cast<std::uint32_t>(pixels) * 254U;
    return static_cast<std::uint16_t>((tenths + 480U) / 960U);
}

Result<void>
malformed(std::string message)
{
    return Result<void>::failure(ErrorCode::malformed, std::move(message));
}

Result<void>
io_failure(std::string_view operation)
{
    return Result<void>::failure(
        ErrorCode::io,
        std::string(operation) + ": " + std::strerror(errno));
}

} // namespace

Connection::Connection(UniqueFd socket, ServerConfig config,
                       ServerState &server)
    : socket_(std::move(socket)), config_(std::move(config)), server_(server)
{}

Connection::~Connection()
{
    server_.disconnect_client(config_.resource_base);
}

Result<void>
Connection::prepare()
{
    if (prepared_)
        return Result<void>::success();
    if (!socket_)
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "invalid client socket");
    if (!server_.valid())
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "screen surface exceeds its size limit");

    const int status_flags = ::fcntl(socket_.get(), F_GETFL);
    if (status_flags < 0)
        return io_failure("fcntl(F_GETFL)");
    if (::fcntl(socket_.get(), F_SETFL, status_flags | O_NONBLOCK) < 0)
        return io_failure("fcntl(F_SETFL)");

    const int descriptor_flags = ::fcntl(socket_.get(), F_GETFD);
    if (descriptor_flags < 0)
        return io_failure("fcntl(F_GETFD)");
    if (::fcntl(socket_.get(), F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
        return io_failure("fcntl(F_SETFD)");

    prepared_ = true;
    return Result<void>::success();
}

short
Connection::poll_events() const noexcept
{
    if (finished_)
        return 0;
    short events = 0;
    if (!close_after_output_)
        events |= POLLIN;
    if (output_offset_ < output_.size())
        events |= POLLOUT;
    if (!close_after_output_ && state_ == State::requests && order_ &&
        server_.has_pending_event(config_.resource_base)) {
        events |= POLLOUT;
    }
    return events;
}

void
Connection::consume_input(std::size_t size)
{
    input_.erase(
        input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(size));
}

void
Connection::close_after_output() noexcept
{
    close_after_output_ = true;
    if (output_offset_ == output_.size())
        finished_ = true;
}

Result<void>
Connection::queue(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.empty())
        return Result<void>::success();

    if (output_offset_ == output_.size()) {
        output_.clear();
        output_offset_ = 0;
    }
    else if (output_offset_ != 0) {
        output_.erase(
            output_.begin(),
            output_.begin() + static_cast<std::ptrdiff_t>(output_offset_));
        output_offset_ = 0;
    }

    const auto new_size = checked_add(output_.size(), bytes.size());
    if (!new_size || *new_size > maximum_buffered_output) {
        return Result<void>::failure(
            ErrorCode::io, "connection output buffer limit exceeded");
    }
    output_.insert(output_.end(), bytes.begin(), bytes.end());
    return Result<void>::success();
}

std::vector<std::uint8_t>
Connection::encode_event(const ClientEvent &event) const
{
    if (!order_)
        return {};
    WireWriter writer(*order_);
    if (const auto *clear = std::get_if<SelectionClearEvent>(&event)) {
        writer.u8(29); // SelectionClear
        writer.u8(0);
        writer.u16(sequence_);
        writer.u32(clear->time);
        writer.u32(clear->window);
        writer.u32(clear->selection);
        writer.pad(16);
        return writer.data();
    }

    const auto &message = std::get<ClientMessageEvent>(event);
    writer.u8(33 | 0x80U); // synthetic ClientMessage
    writer.u8(message.format);
    writer.u16(sequence_);
    writer.u32(message.window);
    writer.u32(message.type);
    const std::size_t count = 160U / message.format;
    for (std::size_t index = 0; index < count; ++index) {
        if (message.format == 8)
            writer.u8(static_cast<std::uint8_t>(message.data[index]));
        else if (message.format == 16)
            writer.u16(static_cast<std::uint16_t>(message.data[index]));
        else
            writer.u32(message.data[index]);
    }
    return writer.data();
}

Result<void>
Connection::drain_pending_events()
{
    while (const auto *event = server_.next_event(config_.resource_base)) {
        const auto encoded = encode_event(*event);
        if (encoded.size() != 32)
            return malformed("invalid queued client event");
        auto queued = queue(encoded);
        if (!queued)
            return queued;
        server_.pop_event(config_.resource_base);
    }
    return Result<void>::success();
}

Result<void>
Connection::send_setup_failure(ByteOrder order, std::string reason)
{
    if (reason.size() > std::numeric_limits<std::uint8_t>::max())
        reason.resize(std::numeric_limits<std::uint8_t>::max());
    const auto padded_reason = padded_to_four(reason.size());
    if (!padded_reason)
        return malformed("setup failure reason overflow");

    WireWriter reply(order);
    reply.u8(0);
    reply.u8(static_cast<std::uint8_t>(reason.size()));
    reply.u16(protocol_major);
    reply.u16(protocol_minor);
    reply.u16(static_cast<std::uint16_t>(*padded_reason / 4));
    reply.bytes(reason);
    reply.pad(*padded_reason - reason.size());
    return queue(reply.data());
}

Result<void>
Connection::send_setup_success(ByteOrder order)
{
    constexpr std::string_view vendor = "Xmin-next";
    WireWriter payload(order);

    payload.u32(1); // release
    payload.u32(config_.resource_base);
    payload.u32(client_resource_mask);
    payload.u32(0);          // motion buffer
    payload.u16(static_cast<std::uint16_t>(vendor.size()));
    payload.u16(65535); // maximum request length in four-byte units
    payload.u8(1);      // roots
    payload.u8(4);      // pixmap formats
    const auto image_order = host_byte_order() == ByteOrder::little ? 0U : 1U;
    payload.u8(static_cast<std::uint8_t>(image_order));
    payload.u8(static_cast<std::uint8_t>(image_order));
    payload.u8(32);  // bitmap scanline unit
    payload.u8(32);  // bitmap scanline pad
    payload.u8(8);   // minimum keycode
    payload.u8(255); // maximum keycode
    payload.pad(4);

    payload.bytes(vendor);
    payload.pad_to_four();
    for (const auto &format : {
             std::pair<std::uint8_t, std::uint8_t>{1, 1},
             {8, 8},
             {24, 32},
             {32, 32}}) {
        payload.u8(format.first);
        payload.u8(format.second);
        payload.u8(32);
        payload.pad(5);
    }

    payload.u32(root_window_id);
    payload.u32(default_colormap_id);
    payload.u32(0x00ffffff);
    payload.u32(0x00000000);
    payload.u32(0);
    payload.u16(server_.width());
    payload.u16(server_.height());
    payload.u16(millimetres(server_.width()));
    payload.u16(millimetres(server_.height()));
    payload.u16(1);
    payload.u16(1);
    payload.u32(root_visual_id);
    payload.u8(0);
    payload.u8(0);
    payload.u8(24);
    payload.u8(1);

    payload.u8(24);
    payload.u8(0);
    payload.u16(1);
    payload.pad(4);
    payload.u32(root_visual_id);
    payload.u8(4); // TrueColor
    payload.u8(8);
    payload.u16(256);
    payload.u32(0x00ff0000);
    payload.u32(0x0000ff00);
    payload.u32(0x000000ff);
    payload.pad(4);

    if ((payload.size() & 3) != 0 || payload.size() / 4 > 65535)
        return malformed("invalid generated setup length");

    WireWriter prefix(order);
    prefix.u8(1);
    prefix.u8(0);
    prefix.u16(protocol_major);
    prefix.u16(protocol_minor);
    prefix.u16(static_cast<std::uint16_t>(payload.size() / 4));
    prefix.bytes(payload.data());
    return queue(prefix.data());
}

Result<bool>
Connection::process_setup_prefix()
{
    constexpr std::size_t prefix_size = 12;
    if (input_.size() < prefix_size)
        return Result<bool>::success(false);

    ByteOrder order;
    if (input_[0] == static_cast<std::uint8_t>('l'))
        order = ByteOrder::little;
    else if (input_[0] == static_cast<std::uint8_t>('B'))
        order = ByteOrder::big;
    else {
        return Result<bool>::failure(
            ErrorCode::malformed, "invalid setup byte order");
    }

    WireReader reader(input_.data(), prefix_size, order);
    const auto byte_order = reader.u8();
    const auto unused = reader.u8();
    const auto major = reader.u16();
    const auto minor = reader.u16();
    const auto auth_name_size = reader.u16();
    const auto auth_data_size = reader.u16();
    if (!byte_order || !unused || !major || !minor || !auth_name_size ||
        !auth_data_size || !reader.skip(2)) {
        return Result<bool>::failure(
            ErrorCode::malformed, "truncated setup prefix");
    }

    consume_input(prefix_size);
    order_ = order;
    if (*major != protocol_major || *minor != protocol_minor) {
        auto sent = send_setup_failure(order, "X11 protocol 11.0 required");
        if (!sent)
            return Result<bool>::failure(
                sent.error().code, sent.error().message);
        input_.clear();
        close_after_output();
        return Result<bool>::success(true);
    }

    const auto padded_name = padded_to_four(*auth_name_size);
    const auto padded_data = padded_to_four(*auth_data_size);
    if (!padded_name || !padded_data || *padded_name > 1024 ||
        *padded_data > 1024) {
        return Result<bool>::failure(
            ErrorCode::malformed, "invalid setup authentication lengths");
    }
    const auto payload_size = checked_add(*padded_name, *padded_data);
    if (!payload_size) {
        return Result<bool>::failure(
            ErrorCode::malformed, "setup authentication length overflow");
    }

    setup_payload_size_ = *payload_size;
    setup_name_size_ = *auth_name_size;
    setup_data_size_ = *auth_data_size;
    setup_padded_name_size_ = *padded_name;
    state_ = State::setup_authentication;
    return Result<bool>::success(true);
}

Result<bool>
Connection::process_setup_authentication()
{
    if (input_.size() < setup_payload_size_)
        return Result<bool>::success(false);
    if (!order_)
        return Result<bool>::failure(ErrorCode::malformed,
                                     "setup byte order was not recorded");

    std::string auth_name;
    auth_name.reserve(setup_name_size_);
    for (std::size_t index = 0; index < setup_name_size_; ++index)
        auth_name.push_back(static_cast<char>(input_[index]));
    std::vector<std::uint8_t> auth_data(
        input_.begin() +
            static_cast<std::ptrdiff_t>(setup_padded_name_size_),
        input_.begin() + static_cast<std::ptrdiff_t>(
                             setup_padded_name_size_ + setup_data_size_));
    consume_input(setup_payload_size_);

    const bool authenticated = config_.allow_unauthenticated
        ? auth_name.empty() && auth_data.empty()
        : !config_.cookie.empty() && auth_name == cookie_protocol &&
            cookies_equal(auth_data, config_.cookie);
    if (!authenticated) {
        auto sent = send_setup_failure(*order_, "Authentication failed");
        if (!sent)
            return Result<bool>::failure(
                sent.error().code, sent.error().message);
        input_.clear();
        close_after_output();
        return Result<bool>::success(true);
    }

    auto sent = send_setup_success(*order_);
    if (!sent)
        return Result<bool>::failure(sent.error().code, sent.error().message);
    state_ = State::requests;
    return Result<bool>::success(true);
}

Result<void>
Connection::send_error(ByteOrder order, std::uint8_t code,
                       std::uint8_t opcode, std::uint16_t sequence,
                       std::uint32_t bad_value)
{
    WireWriter reply(order);
    reply.u8(0);
    reply.u8(code);
    reply.u16(sequence);
    reply.u32(bad_value);
    reply.u16(0);
    reply.u8(opcode);
    reply.pad(21);
    return queue(reply.data());
}

Result<void>
Connection::handle_create_window(const RequestContext &context)
{
    if (context.request.size() < 32)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto parent_id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto border_width = reader.u16();
    const auto requested_class = reader.u16();
    const auto visual = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !parent_id || !x || !y || !width || !height ||
        !border_width || !requested_class || !visual || !value_mask) {
        return malformed("truncated CreateWindow request");
    }

    constexpr std::uint32_t supported_value_mask = 0x00007fffU;
    const auto value_count = std::bitset<32>(*value_mask).count();
    const auto value_bytes = checked_multiply(value_count, std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{32}, *value_bytes)
        : std::optional<std::size_t>{};
    if ((*value_mask & ~supported_value_mask) != 0) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    }
    if (!expected_size || context.request.size() != *expected_size) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    if (!server_.valid_client_resource(*id, config_.resource_base)) {
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    }
    if (server_.resource_limit_reached(config_.resource_base)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    const auto *parent = server_.window(*parent_id);
    if (parent == nullptr) {
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *parent_id);
    }
    if (*width == 0 || *height == 0) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *width == 0 ? *width : *height);
    }

    WindowClass window_class;
    if (*requested_class == 0)
        window_class = parent->window_class;
    else if (*requested_class == 1)
        window_class = WindowClass::input_output;
    else if (*requested_class == 2)
        window_class = WindowClass::input_only;
    else {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *requested_class);
    }

    WindowRecord window;
    window.id = *id;
    window.parent = *parent_id;
    window.x = signed_word(*x);
    window.y = signed_word(*y);
    window.width = *width;
    window.height = *height;
    window.border_width = *border_width;
    window.window_class = window_class;
    window.colormap = parent->colormap;

    if (window_class == WindowClass::input_only) {
        if (context.data != 0 || *visual != 0 || *border_width != 0) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        if ((*value_mask & ~input_only_attribute_masks) != 0) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        window.depth = 0;
        window.visual = 0;
        window.colormap = 0;
    }
    else {
        if (parent->window_class == WindowClass::input_only) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        window.depth = context.data == 0 ? parent->depth : context.data;
        window.visual = *visual == 0 ? parent->visual : *visual;
        if (window.depth != 24 || window.visual != root_visual_id) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        window.surface = Surface::create(window.width, window.height,
                                         window.depth);
        if (!window.surface)
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
    }

    WireReader values(context.request.data() + 32,
                      context.request.size() - 32, context.order);
    for (unsigned bit = 0; bit < 15; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = values.u32();
        if (!value)
            return malformed("truncated CreateWindow value list");
        switch (bit) {
        case 0:
            if (*value > 1) {
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            }
            break;
        case 1:
            window.background_pixel = *value;
            break;
        case 2:
            if (*value != 0) {
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            }
            break;
        case 3:
            window.border_pixel = *value;
            break;
        case 4:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.bit_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 5:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.window_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 6:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.backing_store = static_cast<std::uint8_t>(*value);
            break;
        case 7:
            window.backing_planes = *value;
            break;
        case 8:
            window.backing_pixel = *value;
            break;
        case 9:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.override_redirect = *value != 0;
            break;
        case 10:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.save_under = *value != 0;
            break;
        case 11:
            if ((*value & ~all_event_masks) != 0) {
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            }
            if (*value != 0)
                window.event_masks.emplace(config_.resource_base, *value);
            break;
        case 12:
            if ((*value & ~propagate_event_masks) != 0) {
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            }
            window.do_not_propagate_mask =
                static_cast<std::uint16_t>(*value);
            break;
        case 13:
            if (*value == 0)
                window.colormap = parent->colormap;
            else if (*value == default_colormap_id)
                window.colormap = *value;
            else
                return send_error(context.order, bad_colormap, context.opcode,
                                  context.sequence, *value);
            break;
        case 14:
            if (*value != 0) {
                return send_error(context.order, bad_cursor, context.opcode,
                                  context.sequence, *value);
            }
            break;
        }
    }

    if (window.surface) {
        window.surface->fill(
            Rectangle{0, 0, window.width, window.height},
            window.background_pixel, 3, 0xffffffffU);
    }
    if (!server_.add_window(std::move(window), config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_change_window_attributes(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !value_mask)
        return malformed("truncated ChangeWindowAttributes request");
    constexpr std::uint32_t supported_value_mask = 0x00007fffU;
    if ((*value_mask & ~supported_value_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto value_bytes = checked_multiply(
        std::bitset<32>(*value_mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{12}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    if (window->window_class == WindowClass::input_only &&
        (*value_mask & ~input_only_attribute_masks) != 0) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }

    auto bit_gravity = window->bit_gravity;
    auto window_gravity = window->window_gravity;
    auto backing_store = window->backing_store;
    auto backing_planes = window->backing_planes;
    auto backing_pixel = window->backing_pixel;
    auto override_redirect = window->override_redirect;
    auto save_under = window->save_under;
    auto do_not_propagate = window->do_not_propagate_mask;
    auto colormap = window->colormap;
    auto background_pixel = window->background_pixel;
    auto border_pixel = window->border_pixel;
    std::optional<std::uint32_t> event_mask;
    for (unsigned bit = 0; bit < 15; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = reader.u32();
        if (!value)
            return malformed("truncated ChangeWindowAttributes value list");
        switch (bit) {
        case 0:
            if (*value > 1)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            break;
        case 1:
            background_pixel = *value;
            break;
        case 2:
            if (*value != 0)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            break;
        case 3:
            border_pixel = *value;
            break;
        case 4:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            bit_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 5:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 6:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            backing_store = static_cast<std::uint8_t>(*value);
            break;
        case 7:
            backing_planes = *value;
            break;
        case 8:
            backing_pixel = *value;
            break;
        case 9:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            override_redirect = *value != 0;
            break;
        case 10:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            save_under = *value != 0;
            break;
        case 11:
            if ((*value & ~all_event_masks) != 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            for (const auto &selection : window->event_masks) {
                if (selection.first != config_.resource_base &&
                    (selection.second & *value & exclusive_event_masks) != 0) {
                    return send_error(context.order, bad_access,
                                      context.opcode, context.sequence);
                }
            }
            event_mask = *value;
            break;
        case 12:
            if ((*value & ~propagate_event_masks) != 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            do_not_propagate = static_cast<std::uint16_t>(*value);
            break;
        case 13:
            if (*value == 0) {
                const auto *parent = server_.window(window->parent);
                colormap = parent == nullptr ? default_colormap_id
                                             : parent->colormap;
            }
            else if (*value == default_colormap_id)
                colormap = *value;
            else
                return send_error(context.order, bad_colormap,
                                  context.opcode, context.sequence, *value);
            break;
        case 14:
            if (*value != 0)
                return send_error(context.order, bad_cursor, context.opcode,
                                  context.sequence, *value);
            break;
        }
    }

    if (event_mask) {
        if (*event_mask == 0)
            window->event_masks.erase(config_.resource_base);
        else {
            try {
                window->event_masks.insert_or_assign(config_.resource_base,
                                                      *event_mask);
            }
            catch (const std::bad_alloc &) {
                return send_error(context.order, bad_alloc, context.opcode,
                                  context.sequence);
            }
        }
    }
    window->bit_gravity = bit_gravity;
    window->window_gravity = window_gravity;
    window->backing_store = backing_store;
    window->backing_planes = backing_planes;
    window->backing_pixel = backing_pixel;
    window->override_redirect = override_redirect;
    window->save_under = save_under;
    window->do_not_propagate_mask = do_not_propagate;
    window->colormap = colormap;
    window->background_pixel = background_pixel;
    window->border_pixel = border_pixel;
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_get_window_attributes(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated GetWindowAttributes request");
    const auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);

    const auto own_mask = window->event_masks.find(config_.resource_base);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(window->backing_store);
    reply.u16(context.sequence);
    reply.u32(3);
    reply.u32(window->visual);
    reply.u16(static_cast<std::uint16_t>(window->window_class));
    reply.u8(window->bit_gravity);
    reply.u8(window->window_gravity);
    reply.u32(window->backing_planes);
    reply.u32(window->backing_pixel);
    reply.u8(window->save_under ? 1 : 0);
    reply.u8(window->colormap == default_colormap_id ? 1 : 0);
    reply.u8(server_.map_state(*id));
    reply.u8(window->override_redirect ? 1 : 0);
    reply.u32(window->colormap);
    reply.u32(server_.all_event_masks(*window));
    reply.u32(own_mask == window->event_masks.end() ? 0 : own_mask->second);
    reply.u16(window->do_not_propagate_mask);
    reply.pad(2);
    return queue(reply.data());
}

Result<void>
Connection::handle_destroy_window(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated DestroyWindow request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    server_.destroy_window(*id);
    return Result<void>::success();
}

Result<void>
Connection::handle_destroy_subwindows(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated DestroySubwindows request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    server_.destroy_subwindows(*id);
    return Result<void>::success();
}

Result<void>
Connection::handle_reparent_window(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto id = reader.u32();
    const auto parent_id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    if (!id || !parent_id || !x || !y)
        return malformed("truncated ReparentWindow request");
    const auto *window = server_.window(*id);
    if (window == nullptr || *id == root_window_id)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto *parent = server_.window(*parent_id);
    if (parent == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *parent_id);
    if (parent->window_class == WindowClass::input_only &&
        window->window_class == WindowClass::input_output) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    if (!server_.reparent_window(*id, *parent_id, signed_word(*x),
                                 signed_word(*y))) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_map_window(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated MapWindow request");
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    server_.set_window_mapped(*window, true);
    return Result<void>::success();
}

Result<void>
Connection::handle_map_subwindows(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated MapSubwindows request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    server_.set_subwindows_mapped(*id, true);
    return Result<void>::success();
}

Result<void>
Connection::handle_unmap_window(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated UnmapWindow request");
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    server_.set_window_mapped(*window, false);
    return Result<void>::success();
}

Result<void>
Connection::handle_unmap_subwindows(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated UnmapSubwindows request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    server_.set_subwindows_mapped(*id, false);
    return Result<void>::success();
}

Result<void>
Connection::handle_configure_window(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto mask = reader.u16();
    if (!id || !mask || !reader.skip(2))
        return malformed("truncated ConfigureWindow request");
    if ((*mask & ~std::uint16_t{0x007f}) != 0) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *mask);
    }
    const auto value_bytes = checked_multiply(
        std::bitset<16>(*mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{12}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);

    std::int16_t x = window->x;
    std::int16_t y = window->y;
    std::uint16_t width = window->width;
    std::uint16_t height = window->height;
    std::uint16_t border_width = window->border_width;
    std::optional<std::uint32_t> sibling;
    std::optional<std::uint8_t> stack_mode;
    WireReader values(context.request.data() + 12,
                      context.request.size() - 12, context.order);
    for (unsigned bit = 0; bit < 7; ++bit) {
        if ((*mask & (std::uint16_t{1} << bit)) == 0)
            continue;
        const auto value = values.u32();
        if (!value)
            return malformed("truncated ConfigureWindow value list");
        switch (bit) {
        case 0:
            x = signed_word(static_cast<std::uint16_t>(*value));
            break;
        case 1:
            y = signed_word(static_cast<std::uint16_t>(*value));
            break;
        case 2:
            width = static_cast<std::uint16_t>(*value);
            if (width == 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence);
            break;
        case 3:
            height = static_cast<std::uint16_t>(*value);
            if (height == 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence);
            break;
        case 4:
            if (window->window_class == WindowClass::input_only)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence);
            border_width = static_cast<std::uint16_t>(*value);
            break;
        case 5:
            sibling = *value;
            break;
        case 6:
            stack_mode = static_cast<std::uint8_t>(*value);
            if (*stack_mode > 4)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *stack_mode);
            break;
        }
    }
    if (sibling && !stack_mode)
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    if (sibling) {
        const auto *sibling_window = server_.window(*sibling);
        if (sibling_window == nullptr)
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *sibling);
        if (*sibling == *id || sibling_window->parent != window->parent)
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
    }
    if (window->parent == 0)
        return Result<void>::success();

    if ((window->width != width || window->height != height) &&
        !server_.resize_window_surface(*window, width, height)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    window->border_width = border_width;
    if (stack_mode) {
        auto *parent = server_.window(window->parent);
        auto &children = parent->children;
        children.erase(std::remove(children.begin(), children.end(), *id),
                       children.end());
        auto position = children.end();
        if (sibling) {
            position = std::find(children.begin(), children.end(), *sibling);
            if (*stack_mode == 0 || *stack_mode == 2 || *stack_mode == 4)
                ++position;
        }
        else if (*stack_mode == 1 || *stack_mode == 3) {
            position = children.begin();
        }
        children.insert(position, *id);
    }
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_get_geometry(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto drawable = reader.u32();
    if (!drawable)
        return malformed("truncated GetGeometry request");
    const auto *window = server_.window(*drawable);
    const auto *pixmap = server_.pixmap(*drawable);
    if (window == nullptr && pixmap == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);

    const std::uint8_t depth = window == nullptr ? pixmap->surface.depth()
                                                 : window->depth;
    const std::int16_t x = window == nullptr ? 0 : window->x;
    const std::int16_t y = window == nullptr ? 0 : window->y;
    const std::uint16_t width = window == nullptr ? pixmap->surface.width()
                                                  : window->width;
    const std::uint16_t height = window == nullptr ? pixmap->surface.height()
                                                   : window->height;
    const std::uint16_t border = window == nullptr ? 0 : window->border_width;

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(depth);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(root_window_id);
    reply.i16(x);
    reply.i16(y);
    reply.u16(width);
    reply.u16(height);
    reply.u16(border);
    reply.pad(10);
    return queue(reply.data());
}

Result<void>
Connection::handle_query_tree(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated QueryTree request");
    const auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    if (window->children.size() > std::numeric_limits<std::uint16_t>::max()) {
        return Result<void>::failure(ErrorCode::io,
                                     "window child limit exceeded");
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(window->children.size()));
    reply.u32(root_window_id);
    reply.u32(window->parent);
    reply.u16(static_cast<std::uint16_t>(window->children.size()));
    reply.pad(14);
    for (const auto child : window->children)
        reply.u32(child);
    return queue(reply.data());
}

Result<void>
Connection::handle_intern_atom(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto name_size = reader.u16();
    if (!name_size || !reader.skip(2))
        return malformed("truncated InternAtom request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 8 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1 || *name_size == 0 || *name_size > 1024) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence,
                          context.data > 1 ? context.data : *name_size);
    }
    const std::string_view name(
        reinterpret_cast<const char *>(context.request.data() + 8),
        *name_size);
    const AtomId atom = server_.atoms().intern(name, context.data != 0);
    if (atom == 0 && context.data == 0) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(atom);
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_get_atom_name(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto atom = reader.u32();
    if (!atom)
        return malformed("truncated GetAtomName request");
    const auto name = server_.atoms().name(*atom);
    if (!name)
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *atom);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>((name->size() + 3) / 4));
    reply.u16(static_cast<std::uint16_t>(name->size()));
    reply.pad(22);
    reply.bytes(*name);
    reply.pad_to_four();
    return queue(reply.data());
}

Result<void>
Connection::handle_change_property(const RequestContext &context)
{
    if (context.request.size() < 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto window_id = reader.u32();
    const auto property = reader.u32();
    const auto type = reader.u32();
    const auto format = reader.u8();
    const bool fixed_fields = reader.skip(3);
    const auto item_count = reader.u32();
    if (!window_id || !property || !type || !format || !fixed_fields ||
        !item_count) {
        return malformed("truncated ChangeProperty request");
    }
    if (context.data > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    if (*format != 8 && *format != 16 && *format != 32)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *format);

    auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (!server_.atoms().name(*property))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *property);
    if (!server_.atoms().name(*type))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *type);

    const auto data_size = checked_multiply(
        static_cast<std::size_t>(*item_count),
        static_cast<std::size_t>(*format / 8));
    const auto padded_size = data_size ? padded_to_four(*data_size)
                                       : std::optional<std::size_t>{};
    const auto expected_size = padded_size
        ? checked_add(std::size_t{24}, *padded_size)
        : std::optional<std::size_t>{};
    if (!data_size || !padded_size || !expected_size ||
        context.request.size() != *expected_size) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    const auto canonical = canonical_property_data(
        context.request.data() + 24, *data_size, *format, context.order);
    if (!canonical)
        return malformed("misaligned ChangeProperty data");

    PropertyValue updated{*type, *format, {}};
    const auto existing = window->properties.find(*property);
    if (existing != window->properties.end() && context.data != 0 &&
        (existing->second.type != *type ||
         existing->second.format != *format)) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    const std::size_t previous_size = existing == window->properties.end()
        ? 0
        : existing->second.data.size();
    const auto combined_size = context.data == 0
        ? std::optional<std::size_t>(canonical->size())
        : checked_add(previous_size, canonical->size());
    if (!combined_size || *combined_size > maximum_property_bytes)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    updated.data.reserve(*combined_size);
    if (context.data == 1)
        updated.data.insert(updated.data.end(), canonical->begin(),
                            canonical->end());
    if (context.data != 0 && existing != window->properties.end()) {
        updated.data.insert(updated.data.end(), existing->second.data.begin(),
                            existing->second.data.end());
    }
    if (context.data != 1)
        updated.data.insert(updated.data.end(), canonical->begin(),
                            canonical->end());

    if (!server_.set_property(*window, *property, std::move(updated)))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_delete_property(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto window_id = reader.u32();
    const auto property = reader.u32();
    if (!window_id || !property)
        return malformed("truncated DeleteProperty request");
    auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (!server_.atoms().name(*property))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *property);
    server_.delete_property(*window, *property);
    return Result<void>::success();
}

Result<void>
Connection::handle_get_property(const RequestContext &context)
{
    if (context.request.size() != 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 20, context.order);
    const auto window_id = reader.u32();
    const auto property = reader.u32();
    const auto requested_type = reader.u32();
    const auto long_offset = reader.u32();
    const auto long_length = reader.u32();
    if (!window_id || !property || !requested_type || !long_offset ||
        !long_length) {
        return malformed("truncated GetProperty request");
    }
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (!server_.atoms().name(*property))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *property);
    if (*requested_type != 0 && !server_.atoms().name(*requested_type))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *requested_type);

    const auto found = window->properties.find(*property);
    const PropertyValue *value = found == window->properties.end()
        ? nullptr
        : &found->second;
    std::size_t offset = 0;
    std::size_t returned_size = 0;
    std::size_t bytes_after = 0;
    bool matching_type = value != nullptr &&
        (*requested_type == 0 || value->type == *requested_type);
    if (value != nullptr) {
        const auto byte_offset = checked_multiply(
            static_cast<std::size_t>(*long_offset), std::size_t{4});
        if (!byte_offset || *byte_offset > value->data.size())
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *long_offset);
        offset = *byte_offset;
        if (matching_type) {
            const auto maximum_bytes = checked_multiply(
                static_cast<std::size_t>(*long_length), std::size_t{4});
            if (!maximum_bytes)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *long_length);
            returned_size = std::min(*maximum_bytes,
                                     value->data.size() - offset);
            bytes_after = value->data.size() - offset - returned_size;
        }
        else {
            bytes_after = value->data.size();
        }
    }

    const std::uint8_t format = value == nullptr ? 0 : value->format;
    const AtomId actual_type = value == nullptr ? 0 : value->type;
    const std::size_t unit = format == 0 ? 1 : format / 8;
    const auto encoded = matching_type && returned_size != 0
        ? wire_property_data(value->data.data() + offset, returned_size,
                             format, context.order)
        : std::vector<std::uint8_t>{};
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(format);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>((encoded.size() + 3) / 4));
    reply.u32(actual_type);
    reply.u32(static_cast<std::uint32_t>(bytes_after));
    reply.u32(static_cast<std::uint32_t>(encoded.size() / unit));
    reply.pad(12);
    reply.bytes(encoded);
    reply.pad_to_four();
    auto queued = queue(reply.data());
    if (queued && context.data != 0 && matching_type && bytes_after == 0)
        server_.delete_property(*window, *property);
    return queued;
}

Result<void>
Connection::handle_list_properties(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto window_id = reader.u32();
    if (!window_id)
        return malformed("truncated ListProperties request");
    const auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (window->properties.size() > std::numeric_limits<std::uint16_t>::max())
        return Result<void>::failure(ErrorCode::io,
                                     "window property limit exceeded");

    std::vector<AtomId> properties;
    properties.reserve(window->properties.size());
    for (const auto &property : window->properties)
        properties.push_back(property.first);
    std::sort(properties.begin(), properties.end());

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(properties.size()));
    reply.u16(static_cast<std::uint16_t>(properties.size()));
    reply.pad(22);
    for (const auto property : properties)
        reply.u32(property);
    return queue(reply.data());
}

Result<void>
Connection::handle_set_selection_owner(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto owner = reader.u32();
    const auto selection = reader.u32();
    const auto time = reader.u32();
    if (!owner || !selection || !time)
        return malformed("truncated SetSelectionOwner request");
    if (*owner != 0 && server_.window(*owner) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *owner);
    if (!server_.atoms().name(*selection))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *selection);

    const auto updated = server_.set_selection_owner(
        *selection, *owner, config_.resource_base, *time);
    if (updated == SelectionUpdate::event_queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_get_selection_owner(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto selection = reader.u32();
    if (!selection)
        return malformed("truncated GetSelectionOwner request");
    if (!server_.atoms().name(*selection))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *selection);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(server_.selection_owner(*selection));
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_send_event(const RequestContext &context)
{
    if (context.request.size() != 44)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto requested_destination = reader.u32();
    const auto event_mask = reader.u32();
    if (!requested_destination || !event_mask)
        return malformed("truncated SendEvent request");
    if ((*event_mask & ~all_event_masks) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *event_mask);

    const std::uint32_t destination = *requested_destination <= 1
        ? root_window_id
        : *requested_destination;
    if (server_.window(destination) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *requested_destination);
    const std::uint8_t event_type = context.request[12] & 0x7fU;
    const std::uint8_t format = context.request[13];
    if (event_type != 33)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, event_type);
    if (format != 8 && format != 16 && format != 32)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, format);

    WireReader event_reader(context.request.data() + 16, 28, context.order);
    const auto event_window = event_reader.u32();
    const auto event_type_atom = event_reader.u32();
    if (!event_window || !event_type_atom)
        return malformed("truncated ClientMessage event");
    ClientMessageEvent event;
    event.format = format;
    event.window = *event_window;
    event.type = *event_type_atom;
    const std::size_t count = 160U / format;
    for (std::size_t index = 0; index < count; ++index) {
        if (format == 8) {
            const auto value = event_reader.u8();
            if (!value)
                return malformed("truncated 8-bit ClientMessage data");
            event.data[index] = *value;
        }
        else if (format == 16) {
            const auto value = event_reader.u16();
            if (!value)
                return malformed("truncated 16-bit ClientMessage data");
            event.data[index] = *value;
        }
        else {
            const auto value = event_reader.u32();
            if (!value)
                return malformed("truncated 32-bit ClientMessage data");
            event.data[index] = *value;
        }
    }

    const auto delivered = server_.deliver_client_message(
        destination, *event_mask, context.data != 0, event);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_translate_coordinates(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto source_x = reader.u16();
    const auto source_y = reader.u16();
    if (!source_id || !destination_id || !source_x || !source_y)
        return malformed("truncated TranslateCoordinates request");
    if (server_.window(*source_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *source_id);
    const auto *destination = server_.window(*destination_id);
    if (destination == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *destination_id);

    const auto source_origin = server_.absolute_position(*source_id);
    const auto destination_origin = server_.absolute_position(*destination_id);
    const auto signed_source_x = signed_word(*source_x);
    const auto signed_source_y = signed_word(*source_y);
    const std::int32_t x = source_origin.first + signed_source_x -
        destination_origin.first;
    const std::int32_t y = source_origin.second + signed_source_y -
        destination_origin.second;
    std::uint32_t child = 0;
    for (auto iterator = destination->children.rbegin();
         iterator != destination->children.rend(); ++iterator) {
        const auto *candidate = server_.window(*iterator);
        if (candidate != nullptr && candidate->mapped &&
            x >= candidate->x && y >= candidate->y &&
            x < candidate->x + candidate->width &&
            y < candidate->y + candidate->height) {
            child = candidate->id;
            break;
        }
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(1); // same screen
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(child);
    reply.i16(static_cast<std::int16_t>(std::clamp<std::int32_t>(
        x, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max())));
    reply.i16(static_cast<std::int16_t>(std::clamp<std::int32_t>(
        y, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max())));
    reply.pad(16);
    return queue(reply.data());
}

Result<void>
Connection::handle_create_pixmap(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto id = reader.u32();
    const auto drawable = reader.u32();
    const auto width = reader.u16();
    const auto height = reader.u16();
    if (!id || !drawable || !width || !height)
        return malformed("truncated CreatePixmap request");
    if (server_.drawable_surface(*drawable) == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    if (*width == 0 || *height == 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence);
    if (context.data != 1 && context.data != 8 && context.data != 24 &&
        context.data != 32) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    }
    auto surface = Surface::create(*width, *height, context.data);
    if (!surface)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    if (!server_.add_pixmap(PixmapRecord{*id, std::move(*surface)},
                            config_.resource_base)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_free_pixmap(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated FreePixmap request");
    if (!server_.erase_pixmap(*id))
        return send_error(context.order, bad_pixmap, context.opcode,
                          context.sequence, *id);
    return Result<void>::success();
}

Result<void>
Connection::handle_create_graphics_context(const RequestContext &context)
{
    if (context.request.size() < 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto drawable = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !drawable || !value_mask)
        return malformed("truncated CreateGC request");
    constexpr std::uint32_t supported_mask = 0x0000000fU;
    if ((*value_mask & ~supported_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto value_bytes = checked_multiply(
        std::bitset<32>(*value_mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{16}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::uint8_t depth = server_.drawable_depth(*drawable);
    if (depth == 0)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);

    GraphicsContextRecord graphics{*id, depth};
    WireReader values(context.request.data() + 16,
                      context.request.size() - 16, context.order);
    for (unsigned bit = 0; bit < 4; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = values.u32();
        if (!value)
            return malformed("truncated CreateGC value list");
        switch (bit) {
        case 0:
            if (*value > 15)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.function = static_cast<std::uint8_t>(*value);
            break;
        case 1:
            graphics.plane_mask = *value;
            break;
        case 2:
            graphics.foreground = *value;
            break;
        case 3:
            graphics.background = *value;
            break;
        }
    }
    if (!server_.add_graphics_context(std::move(graphics),
                                      config_.resource_base)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_change_graphics_context(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !value_mask)
        return malformed("truncated ChangeGC request");
    constexpr std::uint32_t supported_mask = 0x0000000fU;
    if ((*value_mask & ~supported_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto value_bytes = checked_multiply(
        std::bitset<32>(*value_mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{12}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    auto *graphics = server_.graphics_context(*id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *id);

    GraphicsContextRecord updated = *graphics;
    for (unsigned bit = 0; bit < 4; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = reader.u32();
        if (!value)
            return malformed("truncated ChangeGC value list");
        switch (bit) {
        case 0:
            if (*value > 15)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated.function = static_cast<std::uint8_t>(*value);
            break;
        case 1:
            updated.plane_mask = *value;
            break;
        case 2:
            updated.foreground = *value;
            break;
        case 3:
            updated.background = *value;
            break;
        }
    }
    *graphics = updated;
    return Result<void>::success();
}

Result<void>
Connection::handle_copy_graphics_context(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto value_mask = reader.u32();
    if (!source_id || !destination_id || !value_mask)
        return malformed("truncated CopyGC request");
    constexpr std::uint32_t supported_mask = 0x0000000fU;
    if ((*value_mask & ~supported_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto *source = server_.graphics_context(*source_id);
    if (source == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *source_id);
    auto *destination = server_.graphics_context(*destination_id);
    if (destination == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *destination_id);
    if ((*value_mask & (1U << 0)) != 0)
        destination->function = source->function;
    if ((*value_mask & (1U << 1)) != 0)
        destination->plane_mask = source->plane_mask;
    if ((*value_mask & (1U << 2)) != 0)
        destination->foreground = source->foreground;
    if ((*value_mask & (1U << 3)) != 0)
        destination->background = source->background;
    return Result<void>::success();
}

Result<void>
Connection::handle_free_graphics_context(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated FreeGC request");
    if (!server_.erase_graphics_context(*id))
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, *id);
    return Result<void>::success();
}

Result<void>
Connection::handle_clear_area(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    if (!id || !x || !y || !width || !height)
        return malformed("truncated ClearArea request");
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    if (!window->surface)
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    const std::int32_t area_x = signed_word(*x);
    const std::int32_t area_y = signed_word(*y);
    const auto remaining_width = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(window->width) - area_x);
    const auto remaining_height = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(window->height) - area_y);
    window->surface->fill(
        Rectangle{
            area_x, area_y,
            *width == 0 ? static_cast<std::uint32_t>(remaining_width) : *width,
            *height == 0 ? static_cast<std::uint32_t>(remaining_height)
                         : *height},
        window->background_pixel, 3, 0xffffffffU);
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_copy_area(const RequestContext &context)
{
    if (context.request.size() != 28)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 24, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto graphics_id = reader.u32();
    const auto source_x = reader.u16();
    const auto source_y = reader.u16();
    const auto destination_x = reader.u16();
    const auto destination_y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    if (!source_id || !destination_id || !graphics_id || !source_x ||
        !source_y || !destination_x || !destination_y || !width || !height) {
        return malformed("truncated CopyArea request");
    }
    const auto *source = server_.drawable_surface(*source_id);
    if (source == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *source_id);
    auto *destination = server_.drawable_surface(*destination_id);
    if (destination == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *destination_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, *graphics_id);
    if (source->depth() != destination->depth() ||
        graphics->depth != destination->depth()) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    destination->copy_from(*source, signed_word(*source_x),
                           signed_word(*source_y),
                           signed_word(*destination_x),
                           signed_word(*destination_y), *width, *height,
                           graphics->function, graphics->plane_mask);
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_poly_points(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyPoint request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    std::int32_t current_x = 0;
    std::int32_t current_y = 0;
    bool first = true;
    while (reader.remaining() != 0) {
        const auto x = reader.u16();
        const auto y = reader.u16();
        if (!x || !y)
            return malformed("truncated PolyPoint list");
        const std::int32_t decoded_x = signed_word(*x);
        const std::int32_t decoded_y = signed_word(*y);
        if (context.data == 1 && !first) {
            current_x += decoded_x;
            current_y += decoded_y;
        }
        else {
            current_x = decoded_x;
            current_y = decoded_y;
        }
        first = false;
        surface->draw_pixel(current_x, current_y, graphics->foreground,
                            graphics->function, graphics->plane_mask);
    }
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_poly_lines(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyLine request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    std::int32_t previous_x = 0;
    std::int32_t previous_y = 0;
    bool first = true;
    while (reader.remaining() != 0) {
        const auto x = reader.u16();
        const auto y = reader.u16();
        if (!x || !y)
            return malformed("truncated PolyLine list");
        std::int32_t current_x = signed_word(*x);
        std::int32_t current_y = signed_word(*y);
        if (context.data == 1 && !first) {
            current_x += previous_x;
            current_y += previous_y;
        }
        if (!first) {
            surface->draw_line(previous_x, previous_y, current_x, current_y,
                               graphics->foreground, graphics->function,
                               graphics->plane_mask);
        }
        previous_x = current_x;
        previous_y = current_y;
        first = false;
    }
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_poly_segments(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 7U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolySegment request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    while (reader.remaining() != 0) {
        const auto start_x = reader.u16();
        const auto start_y = reader.u16();
        const auto end_x = reader.u16();
        const auto end_y = reader.u16();
        if (!start_x || !start_y || !end_x || !end_y)
            return malformed("truncated PolySegment list");
        surface->draw_line(signed_word(*start_x), signed_word(*start_y),
                           signed_word(*end_x), signed_word(*end_y),
                           graphics->foreground, graphics->function,
                           graphics->plane_mask);
    }
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_poly_rectangles(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 7U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyRectangle request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    while (reader.remaining() != 0) {
        const auto encoded_x = reader.u16();
        const auto encoded_y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        if (!encoded_x || !encoded_y || !width || !height)
            return malformed("truncated PolyRectangle list");
        const std::int32_t x = signed_word(*encoded_x);
        const std::int32_t y = signed_word(*encoded_y);
        const std::int32_t right = x + *width;
        const std::int32_t bottom = y + *height;
        surface->draw_line(x, y, right, y, graphics->foreground,
                           graphics->function, graphics->plane_mask);
        if (*height == 0)
            continue;
        surface->draw_line(x, bottom, right, bottom, graphics->foreground,
                           graphics->function, graphics->plane_mask);
        if (*height > 1) {
            surface->draw_line(x, y + 1, x, bottom - 1,
                               graphics->foreground, graphics->function,
                               graphics->plane_mask);
            if (*width != 0) {
                surface->draw_line(right, y + 1, right, bottom - 1,
                                   graphics->foreground, graphics->function,
                                   graphics->plane_mask);
            }
        }
    }
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_fill_rectangles(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 7U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyFillRectangle request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    while (reader.remaining() != 0) {
        const auto x = reader.u16();
        const auto y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        if (!x || !y || !width || !height)
            return malformed("truncated PolyFillRectangle list");
        surface->fill(Rectangle{signed_word(*x), signed_word(*y), *width,
                                *height},
                      graphics->foreground, graphics->function,
                      graphics->plane_mask);
    }
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_put_image(const RequestContext &context)
{
    if (context.request.size() < 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto destination_x = reader.u16();
    const auto destination_y = reader.u16();
    const auto left_pad = reader.u8();
    const auto depth = reader.u8();
    if (!drawable_id || !graphics_id || !width || !height ||
        !destination_x || !destination_y || !left_pad || !depth ||
        !reader.skip(2)) {
        return malformed("truncated PutImage request");
    }
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, *graphics_id);
    if (context.data != 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    if (*left_pad != 0)
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    if (*width == 0 || *height == 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence);
    if (*depth != surface->depth() || graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    const std::size_t bits_per_pixel = *depth == 1
        ? 1
        : (*depth == 8 ? 8 : 32);
    const auto row_bits = checked_multiply(
        static_cast<std::size_t>(*width), bits_per_pixel);
    const auto rounded_bits = row_bits
        ? checked_add(*row_bits, std::size_t{7})
        : std::optional<std::size_t>{};
    const auto stride = rounded_bits
        ? padded_to_four(*rounded_bits / 8)
        : std::optional<std::size_t>{};
    const auto image_size = stride
        ? checked_multiply(*stride, static_cast<std::size_t>(*height))
        : std::optional<std::size_t>{};
    const auto expected_size = image_size
        ? checked_add(std::size_t{24}, *image_size)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    const auto *image = context.request.data() + 24;
    const bool least_significant_bit_first =
        host_byte_order() == ByteOrder::little;
    const std::int32_t target_x = signed_word(*destination_x);
    const std::int32_t target_y = signed_word(*destination_y);
    for (std::uint32_t row = 0; row < *height; ++row) {
        const auto *row_data = image + static_cast<std::size_t>(row) * *stride;
        WireReader pixels(row_data, *stride, host_byte_order());
        for (std::uint32_t column = 0; column < *width; ++column) {
            std::uint32_t pixel = 0;
            if (*depth == 1) {
                const auto byte = row_data[column / 8];
                const unsigned bit = least_significant_bit_first
                    ? column & 7U
                    : 7U - (column & 7U);
                pixel = (byte >> bit) & 1U;
            }
            else if (*depth == 8) {
                pixel = row_data[column];
            }
            else {
                const auto value = pixels.u32();
                if (!value)
                    return malformed("truncated ZPixmap scanline");
                pixel = *value;
            }
            surface->draw_pixel(target_x + static_cast<std::int32_t>(column),
                                target_y + static_cast<std::int32_t>(row),
                                pixel, graphics->function,
                                graphics->plane_mask);
        }
    }
    server_.invalidate_scene();
    return Result<void>::success();
}

Result<void>
Connection::handle_get_image(const RequestContext &context)
{
    if (context.request.size() != 20)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 16, context.order);
    const auto drawable_id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto plane_mask = reader.u32();
    if (!drawable_id || !x || !y || !width || !height || !plane_mask)
        return malformed("truncated GetImage request");
    const auto *surface = server_.readable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    if (context.data != 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    const std::int32_t image_x = signed_word(*x);
    const std::int32_t image_y = signed_word(*y);
    if (*width == 0 || *height == 0 || image_x < 0 || image_y < 0 ||
        image_x + *width > surface->width() ||
        image_y + *height > surface->height()) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }

    const std::size_t bits_per_pixel = surface->depth() == 1
        ? 1
        : (surface->depth() == 8 ? 8 : 32);
    const auto row_bits = checked_multiply(
        static_cast<std::size_t>(*width), bits_per_pixel);
    const auto rounded_bits = row_bits
        ? checked_add(*row_bits, std::size_t{7})
        : std::optional<std::size_t>{};
    const auto stride = rounded_bits
        ? padded_to_four(*rounded_bits / 8)
        : std::optional<std::size_t>{};
    const auto image_size = stride
        ? checked_multiply(*stride, static_cast<std::size_t>(*height))
        : std::optional<std::size_t>{};
    if (!image_size || *image_size > maximum_buffered_output - 32)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    std::vector<std::uint8_t> image;
    try {
        image.assign(*image_size, 0);
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    const bool least_significant_byte_first =
        host_byte_order() == ByteOrder::little;
    for (std::uint32_t row = 0; row < *height; ++row) {
        for (std::uint32_t column = 0; column < *width; ++column) {
            const std::uint32_t pixel = surface->pixel(
                static_cast<std::uint16_t>(image_x + column),
                static_cast<std::uint16_t>(image_y + row)) & *plane_mask;
            const std::size_t row_offset =
                static_cast<std::size_t>(row) * *stride;
            if (surface->depth() == 1) {
                const unsigned bit = least_significant_byte_first
                    ? column & 7U
                    : 7U - (column & 7U);
                image[row_offset + column / 8] |=
                    static_cast<std::uint8_t>((pixel & 1U) << bit);
            }
            else if (surface->depth() == 8) {
                image[row_offset + column] =
                    static_cast<std::uint8_t>(pixel);
            }
            else {
                const std::size_t pixel_offset = row_offset + column * 4;
                for (unsigned index = 0; index < 4; ++index) {
                    const unsigned shift = least_significant_byte_first
                        ? index * 8
                        : (3 - index) * 8;
                    image[pixel_offset + index] =
                        static_cast<std::uint8_t>(pixel >> shift);
                }
            }
        }
    }
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(surface->depth());
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(image.size() / 4));
    reply.u32(server_.window(*drawable_id) == nullptr ? 0 : root_visual_id);
    reply.pad(20);
    reply.bytes(image);
    return queue(reply.data());
}

Result<void>
Connection::handle_alloc_named_color(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    const auto name_size = reader.u16();
    if (!colormap || !name_size || !reader.skip(2))
        return malformed("truncated AllocNamedColor request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 12 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (*colormap != default_colormap_id)
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);

    const auto *name_data = reinterpret_cast<const char *>(
        context.request.data() + 12);
    const auto color = parse_color(std::string_view(name_data, *name_size));
    if (!color)
        return send_error(context.order, bad_name, context.opcode,
                          context.sequence);
    const std::uint32_t pixel = true_color_pixel(*color);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(pixel);
    reply.u16(color->red);
    reply.u16(color->green);
    reply.u16(color->blue);
    reply.u16(color->red);
    reply.u16(color->green);
    reply.u16(color->blue);
    reply.pad(8);
    return queue(reply.data());
}

Result<void>
Connection::handle_query_colors(const RequestContext &context)
{
    if (context.request.size() < 8 ||
        ((context.request.size() - 8) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    if (!colormap)
        return malformed("truncated QueryColors request");
    if (*colormap != default_colormap_id)
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);

    std::vector<RgbColor> colors;
    colors.reserve(reader.remaining() / 4);
    while (reader.remaining() != 0) {
        const auto pixel = reader.u32();
        if (!pixel)
            return malformed("truncated QueryColors pixel list");
        if ((*pixel & 0xff000000U) != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *pixel);
        colors.push_back(true_color_rgb(*pixel));
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(colors.size() * 2));
    reply.u16(static_cast<std::uint16_t>(colors.size()));
    reply.pad(22);
    for (const auto color : colors) {
        reply.u16(color.red);
        reply.u16(color.green);
        reply.u16(color.blue);
        reply.pad(2);
    }
    return queue(reply.data());
}

Result<void>
Connection::handle_get_input_focus(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0); // RevertToNone
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(root_window_id);
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_query_extension(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto name_size = reader.u16();
    if (!name_size || !reader.skip(2))
        return malformed("truncated QueryExtension request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 8 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u8(0); // not yet advertised by Xmin-next
    reply.u8(0);
    reply.u8(0);
    reply.u8(0);
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_list_extensions(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.pad(24);
    return queue(reply.data());
}

Result<void>
Connection::handle_no_operation(const RequestContext &)
{
    return Result<void>::success();
}

Result<void>
Connection::dispatch(const RequestContext &context)
{
    static const std::array<RequestHandler, 128> handlers = [] {
        std::array<RequestHandler, 128> table{};
        table[opcode_index(CoreOpcode::CreateWindow)] =
            &Connection::handle_create_window;
        table[opcode_index(CoreOpcode::ChangeWindowAttributes)] =
            &Connection::handle_change_window_attributes;
        table[opcode_index(CoreOpcode::GetWindowAttributes)] =
            &Connection::handle_get_window_attributes;
        table[opcode_index(CoreOpcode::DestroyWindow)] =
            &Connection::handle_destroy_window;
        table[opcode_index(CoreOpcode::DestroySubwindows)] =
            &Connection::handle_destroy_subwindows;
        table[opcode_index(CoreOpcode::ReparentWindow)] =
            &Connection::handle_reparent_window;
        table[opcode_index(CoreOpcode::MapWindow)] =
            &Connection::handle_map_window;
        table[opcode_index(CoreOpcode::MapSubwindows)] =
            &Connection::handle_map_subwindows;
        table[opcode_index(CoreOpcode::UnmapWindow)] =
            &Connection::handle_unmap_window;
        table[opcode_index(CoreOpcode::UnmapSubwindows)] =
            &Connection::handle_unmap_subwindows;
        table[opcode_index(CoreOpcode::ConfigureWindow)] =
            &Connection::handle_configure_window;
        table[opcode_index(CoreOpcode::GetGeometry)] =
            &Connection::handle_get_geometry;
        table[opcode_index(CoreOpcode::QueryTree)] =
            &Connection::handle_query_tree;
        table[opcode_index(CoreOpcode::InternAtom)] =
            &Connection::handle_intern_atom;
        table[opcode_index(CoreOpcode::GetAtomName)] =
            &Connection::handle_get_atom_name;
        table[opcode_index(CoreOpcode::ChangeProperty)] =
            &Connection::handle_change_property;
        table[opcode_index(CoreOpcode::DeleteProperty)] =
            &Connection::handle_delete_property;
        table[opcode_index(CoreOpcode::GetProperty)] =
            &Connection::handle_get_property;
        table[opcode_index(CoreOpcode::ListProperties)] =
            &Connection::handle_list_properties;
        table[opcode_index(CoreOpcode::SetSelectionOwner)] =
            &Connection::handle_set_selection_owner;
        table[opcode_index(CoreOpcode::GetSelectionOwner)] =
            &Connection::handle_get_selection_owner;
        table[opcode_index(CoreOpcode::SendEvent)] =
            &Connection::handle_send_event;
        table[opcode_index(CoreOpcode::TranslateCoordinates)] =
            &Connection::handle_translate_coordinates;
        table[opcode_index(CoreOpcode::CreatePixmap)] =
            &Connection::handle_create_pixmap;
        table[opcode_index(CoreOpcode::FreePixmap)] =
            &Connection::handle_free_pixmap;
        table[opcode_index(CoreOpcode::CreateGC)] =
            &Connection::handle_create_graphics_context;
        table[opcode_index(CoreOpcode::ChangeGC)] =
            &Connection::handle_change_graphics_context;
        table[opcode_index(CoreOpcode::CopyGC)] =
            &Connection::handle_copy_graphics_context;
        table[opcode_index(CoreOpcode::FreeGC)] =
            &Connection::handle_free_graphics_context;
        table[opcode_index(CoreOpcode::ClearArea)] =
            &Connection::handle_clear_area;
        table[opcode_index(CoreOpcode::CopyArea)] =
            &Connection::handle_copy_area;
        table[opcode_index(CoreOpcode::PolyPoint)] =
            &Connection::handle_poly_points;
        table[opcode_index(CoreOpcode::PolyLine)] =
            &Connection::handle_poly_lines;
        table[opcode_index(CoreOpcode::PolySegment)] =
            &Connection::handle_poly_segments;
        table[opcode_index(CoreOpcode::PolyRectangle)] =
            &Connection::handle_poly_rectangles;
        table[opcode_index(CoreOpcode::PolyFillRectangle)] =
            &Connection::handle_fill_rectangles;
        table[opcode_index(CoreOpcode::PutImage)] =
            &Connection::handle_put_image;
        table[opcode_index(CoreOpcode::GetImage)] =
            &Connection::handle_get_image;
        table[opcode_index(CoreOpcode::AllocNamedColor)] =
            &Connection::handle_alloc_named_color;
        table[opcode_index(CoreOpcode::QueryColors)] =
            &Connection::handle_query_colors;
        table[opcode_index(CoreOpcode::GetInputFocus)] =
            &Connection::handle_get_input_focus;
        table[opcode_index(CoreOpcode::QueryExtension)] =
            &Connection::handle_query_extension;
        table[opcode_index(CoreOpcode::ListExtensions)] =
            &Connection::handle_list_extensions;
        table[opcode_index(CoreOpcode::NoOperation)] =
            &Connection::handle_no_operation;
        return table;
    }();
    if (context.opcode >= handlers.size()) {
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence);
    }
    const auto handler = handlers[context.opcode];
    if (handler == nullptr) {
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence);
    }
    return (this->*handler)(context);
}

Result<bool>
Connection::process_request()
{
    constexpr std::size_t header_size = 4;
    if (input_.size() < header_size)
        return Result<bool>::success(false);
    if (!order_)
        return Result<bool>::failure(ErrorCode::malformed,
                                     "request byte order was not recorded");

    WireReader header(input_.data(), header_size, *order_);
    const auto opcode = header.u8();
    const auto data = header.u8();
    const auto length_units = header.u16();
    if (!opcode || !data || !length_units)
        return Result<bool>::failure(ErrorCode::malformed,
                                     "truncated request header");

    ++sequence_;
    if (*length_units == 0) {
        consume_input(header_size);
        auto sent = send_error(*order_, bad_length, *opcode, sequence_);
        if (!sent)
            return Result<bool>::failure(
                sent.error().code, sent.error().message);
        return Result<bool>::success(true);
    }

    const auto request_size = checked_multiply(
        static_cast<std::size_t>(*length_units), std::size_t{4});
    if (!request_size || *request_size < header_size ||
        *request_size > maximum_request_bytes) {
        auto sent = send_error(*order_, bad_length, *opcode, sequence_);
        if (!sent)
            return Result<bool>::failure(
                sent.error().code, sent.error().message);
        input_.clear();
        close_after_output();
        return Result<bool>::success(true);
    }
    if (input_.size() < *request_size) {
        --sequence_;
        return Result<bool>::success(false);
    }

    std::vector<std::uint8_t> request(
        input_.begin(),
        input_.begin() + static_cast<std::ptrdiff_t>(*request_size));
    consume_input(*request_size);
    const RequestContext context{
        *order_, *opcode, *data, sequence_, request};
    server_.advance_time();
    auto dispatched = dispatch(context);
    if (!dispatched) {
        return Result<bool>::failure(
            dispatched.error().code, dispatched.error().message);
    }
    return Result<bool>::success(true);
}

Result<void>
Connection::process_input()
{
    while (!finished_ && !close_after_output_) {
        Result<bool> processed = [&]() {
            switch (state_) {
            case State::setup_prefix:
                return process_setup_prefix();
            case State::setup_authentication:
                return process_setup_authentication();
            case State::requests:
                return process_request();
            }
            return Result<bool>::failure(ErrorCode::malformed,
                                         "invalid connection state");
        }();
        if (!processed) {
            return Result<void>::failure(
                processed.error().code, processed.error().message);
        }
        if (!processed.value())
            break;
    }
    return Result<void>::success();
}

Result<void>
Connection::on_readable()
{
    if (!prepared_)
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "connection was not prepared");
    if (finished_ || close_after_output_)
        return Result<void>::success();

    std::array<std::uint8_t, 16384> bytes{};
    for (;;) {
        const auto count = ::read(socket_.get(), bytes.data(), bytes.size());
        if (count > 0) {
            const auto new_size = checked_add(
                input_.size(), static_cast<std::size_t>(count));
            if (!new_size || *new_size > maximum_buffered_input) {
                return Result<void>::failure(
                    ErrorCode::malformed,
                    "connection input buffer limit exceeded");
            }
            input_.insert(
                input_.end(), bytes.begin(),
                bytes.begin() + static_cast<std::ptrdiff_t>(count));
            auto processed = process_input();
            if (!processed)
                return processed;
            if (close_after_output_)
                return Result<void>::success();
            continue;
        }
        if (count == 0) {
            if (!input_.empty() || state_ != State::requests) {
                return Result<void>::failure(
                    ErrorCode::unexpected_eof,
                    "connection closed within a protocol packet");
            }
            close_after_output();
            return Result<void>::success();
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return Result<void>::success();
        return io_failure("read");
    }
}

Result<void>
Connection::on_writable()
{
    if (!prepared_)
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "connection was not prepared");
    auto drained = drain_pending_events();
    if (!drained)
        return drained;
    while (output_offset_ < output_.size()) {
        const auto count = ::write(
            socket_.get(), output_.data() + output_offset_,
            output_.size() - output_offset_);
        if (count > 0) {
            output_offset_ += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0)
            return Result<void>::failure(ErrorCode::io,
                                         "write made no progress");
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return Result<void>::success();
        return io_failure("write");
    }

    output_.clear();
    output_offset_ = 0;
    if (close_after_output_)
        finished_ = true;
    return Result<void>::success();
}

Result<void>
Connection::serve()
{
    auto prepared = prepare();
    if (!prepared)
        return prepared;

    while (!finished_) {
        pollfd descriptor{socket_.get(), poll_events(), 0};
        int result;
        do {
            result = ::poll(&descriptor, 1, -1);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return io_failure("poll");
        if ((descriptor.revents & POLLNVAL) != 0) {
            return Result<void>::failure(ErrorCode::io,
                                         "client descriptor became invalid");
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            auto read = on_readable();
            if (!read)
                return read;
        }
        if (!finished_ && (descriptor.revents & POLLOUT) != 0) {
            auto written = on_writable();
            if (!written)
                return written;
        }
        if (!finished_ && (descriptor.revents & POLLERR) != 0 &&
            (descriptor.revents & (POLLIN | POLLOUT | POLLHUP)) == 0) {
            return Result<void>::failure(ErrorCode::io,
                                         "client socket reported an error");
        }
    }
    return Result<void>::success();
}

} // namespace xmin::next
