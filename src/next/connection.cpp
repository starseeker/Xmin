#include "xmin/next/connection.hpp"

#include "xmin/next/checked.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>
#include <unistd.h>

namespace xmin::next {
namespace {

constexpr std::uint16_t protocol_major = 11;
constexpr std::uint16_t protocol_minor = 0;
constexpr std::uint32_t root_window = 1;
constexpr std::uint32_t default_colormap = 2;
constexpr std::uint32_t root_visual = 3;
constexpr std::size_t maximum_request_bytes = 65535U * 4U;
constexpr std::string_view cookie_protocol = "MIT-MAGIC-COOKIE-1";

enum : std::uint8_t {
    bad_request = 1,
    bad_drawable = 9,
    bad_length = 16,
    get_geometry = 14,
    get_input_focus = 43,
    query_extension = 98,
    list_extensions = 99,
    no_operation = 127,
};

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

} // namespace

Connection::Connection(UniqueFd socket, ServerConfig config)
    : socket_(std::move(socket)), config_(std::move(config))
{
    static_cast<void>(resources_.insert(root_window, ResourceKind::window, 0));
    static_cast<void>(
        resources_.insert(default_colormap, ResourceKind::colormap, 0));
}

Result<bool>
Connection::read_exact(std::uint8_t *destination, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::read(socket_.get(), destination + offset, size - offset);
        if (count == 0) {
            if (offset == 0)
                return Result<bool>::success(false);
            return Result<bool>::failure(
                ErrorCode::unexpected_eof, "connection closed within a packet");
        }
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return Result<bool>::failure(
                ErrorCode::io, std::string("read: ") + std::strerror(errno));
        }
        offset += static_cast<std::size_t>(count);
    }
    return Result<bool>::success(true);
}

Result<void>
Connection::write_all(const std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count =
            ::write(socket_.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return Result<void>::failure(
                ErrorCode::io, std::string("write: ") + std::strerror(errno));
        }
        if (count == 0)
            return Result<void>::failure(ErrorCode::io, "write made no progress");
        offset += static_cast<std::size_t>(count);
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
    return write_all(reply.data());
}

Result<void>
Connection::send_setup_success(ByteOrder order)
{
    constexpr std::string_view vendor = "Xmin-next";
    WireWriter payload(order);

    payload.u32(1);          // release
    payload.u32(0x00200000); // client resource-id base
    payload.u32(0x001fffff); // client resource-id mask
    payload.u32(0);          // motion buffer
    payload.u16(static_cast<std::uint16_t>(vendor.size()));
    payload.u16(65535);      // maximum request length in four-byte units
    payload.u8(1);           // roots
    payload.u8(4);           // pixmap formats
    const auto image_order = host_byte_order() == ByteOrder::little ? 0U : 1U;
    payload.u8(static_cast<std::uint8_t>(image_order));
    payload.u8(static_cast<std::uint8_t>(image_order));
    payload.u8(32);          // bitmap scanline unit
    payload.u8(32);          // bitmap scanline pad
    payload.u8(8);           // minimum keycode
    payload.u8(255);         // maximum keycode
    payload.pad(4);

    payload.bytes(vendor);
    payload.pad_to_four();
    for (const auto &format : {
             std::pair<std::uint8_t, std::uint8_t>{1, 1},
             {8, 8}, {24, 32}, {32, 32}}) {
        payload.u8(format.first);
        payload.u8(format.second);
        payload.u8(32);
        payload.pad(5);
    }

    payload.u32(root_window);
    payload.u32(default_colormap);
    payload.u32(0x00ffffff);
    payload.u32(0x00000000);
    payload.u32(0);
    payload.u16(config_.width);
    payload.u16(config_.height);
    payload.u16(millimetres(config_.width));
    payload.u16(millimetres(config_.height));
    payload.u16(1);
    payload.u16(1);
    payload.u32(root_visual);
    payload.u8(0);
    payload.u8(0);
    payload.u8(24);
    payload.u8(1);

    payload.u8(24);
    payload.u8(0);
    payload.u16(1);
    payload.pad(4);
    payload.u32(root_visual);
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
    return write_all(prefix.data());
}

Result<std::optional<ByteOrder>>
Connection::perform_setup()
{
    std::vector<std::uint8_t> prefix(12);
    auto prefix_read = read_exact(prefix.data(), prefix.size());
    if (!prefix_read)
        return Result<std::optional<ByteOrder>>::failure(
            prefix_read.error().code, prefix_read.error().message);
    if (!prefix_read.value())
        return Result<std::optional<ByteOrder>>::failure(
            ErrorCode::unexpected_eof, "connection closed before setup");

    ByteOrder order;
    if (prefix[0] == static_cast<std::uint8_t>('l'))
        order = ByteOrder::little;
    else if (prefix[0] == static_cast<std::uint8_t>('B'))
        order = ByteOrder::big;
    else
        return Result<std::optional<ByteOrder>>::failure(
            ErrorCode::malformed, "invalid setup byte order");

    WireReader reader(prefix, order);
    const auto byte_order = reader.u8();
    const auto unused = reader.u8();
    const auto major = reader.u16();
    const auto minor = reader.u16();
    const auto auth_name_size = reader.u16();
    const auto auth_data_size = reader.u16();
    if (!byte_order || !unused || !major || !minor || !auth_name_size ||
        !auth_data_size || !reader.skip(2)) {
        return Result<std::optional<ByteOrder>>::failure(
            ErrorCode::malformed, "truncated setup prefix");
    }
    if (*major != protocol_major || *minor != protocol_minor) {
        auto sent = send_setup_failure(order, "X11 protocol 11.0 required");
        if (!sent)
            return Result<std::optional<ByteOrder>>::failure(
                sent.error().code, sent.error().message);
        return Result<std::optional<ByteOrder>>::success(std::nullopt);
    }

    const auto padded_name = padded_to_four(*auth_name_size);
    const auto padded_data = padded_to_four(*auth_data_size);
    if (!padded_name || !padded_data || *padded_name > 1024 ||
        *padded_data > 1024) {
        return Result<std::optional<ByteOrder>>::failure(
            ErrorCode::malformed, "invalid setup authentication lengths");
    }
    const auto payload_size = checked_add(*padded_name, *padded_data);
    if (!payload_size) {
        return Result<std::optional<ByteOrder>>::failure(
            ErrorCode::malformed, "setup authentication length overflow");
    }
    std::vector<std::uint8_t> payload(*payload_size);
    auto payload_read = read_exact(payload.data(), payload.size());
    if (!payload_read)
        return Result<std::optional<ByteOrder>>::failure(
            payload_read.error().code, payload_read.error().message);
    if (!payload_read.value())
        return Result<std::optional<ByteOrder>>::failure(
            ErrorCode::unexpected_eof, "connection closed before authentication");

    std::string auth_name;
    auth_name.reserve(*auth_name_size);
    for (std::size_t index = 0; index < *auth_name_size; ++index)
        auth_name.push_back(static_cast<char>(payload[index]));
    std::vector<std::uint8_t> auth_data(
        payload.begin() + static_cast<std::ptrdiff_t>(*padded_name),
        payload.begin() + static_cast<std::ptrdiff_t>(*padded_name + *auth_data_size));

    const bool authenticated = config_.allow_unauthenticated
        ? auth_name.empty() && auth_data.empty()
        : auth_name == cookie_protocol && cookies_equal(auth_data, config_.cookie);
    if (!authenticated) {
        auto sent = send_setup_failure(order, "Authentication failed");
        if (!sent)
            return Result<std::optional<ByteOrder>>::failure(
                sent.error().code, sent.error().message);
        return Result<std::optional<ByteOrder>>::success(std::nullopt);
    }

    auto sent = send_setup_success(order);
    if (!sent)
        return Result<std::optional<ByteOrder>>::failure(
            sent.error().code, sent.error().message);
    return Result<std::optional<ByteOrder>>::success(order);
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
    return write_all(reply.data());
}

Result<void>
Connection::dispatch(ByteOrder order, std::uint8_t opcode,
                     const std::vector<std::uint8_t> &request,
                     std::uint16_t sequence)
{
    if (opcode == no_operation) {
        if (request.size() != 4)
            return send_error(order, bad_length, opcode, sequence);
        return Result<void>::success();
    }

    if (opcode == get_input_focus) {
        if (request.size() != 4)
            return send_error(order, bad_length, opcode, sequence);
        WireWriter reply(order);
        reply.u8(1);
        reply.u8(0); // RevertToNone
        reply.u16(sequence);
        reply.u32(0);
        reply.u32(root_window);
        reply.pad(20);
        return write_all(reply.data());
    }

    if (opcode == get_geometry) {
        if (request.size() != 8)
            return send_error(order, bad_length, opcode, sequence);
        WireReader reader(request.data() + 4, request.size() - 4, order);
        const auto drawable = reader.u32();
        if (!drawable)
            return malformed("truncated GetGeometry request");
        if (!resources_.is(*drawable, ResourceKind::window))
            return send_error(
                order, bad_drawable, opcode, sequence, *drawable);

        WireWriter reply(order);
        reply.u8(1);
        reply.u8(24);
        reply.u16(sequence);
        reply.u32(0);
        reply.u32(root_window);
        reply.i16(0);
        reply.i16(0);
        reply.u16(config_.width);
        reply.u16(config_.height);
        reply.u16(0);
        reply.pad(10);
        return write_all(reply.data());
    }

    if (opcode == query_extension) {
        if (request.size() < 8) {
            return send_error(order, bad_length, opcode, sequence);
        }
        WireReader reader(request.data() + 4, request.size() - 4, order);
        const auto name_size = reader.u16();
        if (!name_size || !reader.skip(2))
            return malformed("truncated QueryExtension request");
        const auto padded_name = padded_to_four(*name_size);
        if (!padded_name || request.size() != 8 + *padded_name)
            return send_error(order, bad_length, opcode, sequence);

        WireWriter reply(order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(sequence);
        reply.u32(0);
        reply.u8(0); // not yet advertised by Xmin-next
        reply.u8(0);
        reply.u8(0);
        reply.u8(0);
        reply.pad(20);
        return write_all(reply.data());
    }

    if (opcode == list_extensions) {
        if (request.size() != 4)
            return send_error(order, bad_length, opcode, sequence);
        WireWriter reply(order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(sequence);
        reply.u32(0);
        reply.pad(24);
        return write_all(reply.data());
    }

    return send_error(order, bad_request, opcode, sequence);
}

Result<void>
Connection::serve_requests(ByteOrder order)
{
    std::uint16_t sequence = 0;
    for (;;) {
        std::vector<std::uint8_t> header(4);
        auto header_read = read_exact(header.data(), header.size());
        if (!header_read)
            return Result<void>::failure(
                header_read.error().code, header_read.error().message);
        if (!header_read.value())
            return Result<void>::success();

        ++sequence;
        WireReader header_reader(header, order);
        const auto opcode = header_reader.u8();
        const auto data = header_reader.u8();
        const auto length_units = header_reader.u16();
        if (!opcode || !data || !length_units)
            return malformed("truncated request header");
        if (*length_units == 0) {
            auto sent = send_error(order, bad_length, *opcode, sequence);
            if (!sent)
                return sent;
            continue;
        }
        const auto request_size = checked_multiply(
            static_cast<std::size_t>(*length_units), std::size_t{4});
        if (!request_size || *request_size < header.size() ||
            *request_size > maximum_request_bytes) {
            return malformed("request length exceeds the negotiated maximum");
        }

        std::vector<std::uint8_t> request(*request_size);
        request[0] = header[0];
        request[1] = header[1];
        request[2] = header[2];
        request[3] = header[3];
        if (request.size() > header.size()) {
            auto body_read = read_exact(
                request.data() + header.size(), request.size() - header.size());
            if (!body_read)
                return Result<void>::failure(
                    body_read.error().code, body_read.error().message);
            if (!body_read.value()) {
                return Result<void>::failure(
                    ErrorCode::unexpected_eof,
                    "connection closed before request body");
            }
        }

        auto dispatched = dispatch(order, *opcode, request, sequence);
        if (!dispatched)
            return dispatched;
    }
}

Result<void>
Connection::serve()
{
    auto setup = perform_setup();
    if (!setup)
        return Result<void>::failure(setup.error().code, setup.error().message);
    if (!setup.value())
        return Result<void>::success();
    return serve_requests(*setup.value());
}

} // namespace xmin::next
