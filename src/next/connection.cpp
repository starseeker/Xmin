#include "xmin/next/connection.hpp"

#include "xmin/next/checked.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace xmin::next {
namespace {

constexpr std::uint16_t protocol_major = 11;
constexpr std::uint16_t protocol_minor = 0;
constexpr std::uint32_t root_window = 1;
constexpr std::uint32_t default_colormap = 2;
constexpr std::uint32_t root_visual = 3;
constexpr std::size_t maximum_request_bytes = 65535U * 4U;
constexpr std::size_t maximum_buffered_input = maximum_request_bytes + 16384U;
constexpr std::size_t maximum_buffered_output = 1024U * 1024U;
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

Result<void>
io_failure(std::string_view operation)
{
    return Result<void>::failure(
        ErrorCode::io,
        std::string(operation) + ": " + std::strerror(errno));
}

} // namespace

Connection::Connection(UniqueFd socket, ServerConfig config)
    : socket_(std::move(socket)), config_(std::move(config))
{
    static_cast<void>(resources_.insert(root_window, ResourceKind::window, 0));
    static_cast<void>(
        resources_.insert(default_colormap, ResourceKind::colormap, 0));
}

Result<void>
Connection::prepare()
{
    if (prepared_)
        return Result<void>::success();
    if (!socket_)
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "invalid client socket");

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
    payload.u32(0x001fffff); // client resource-id mask
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
        return queue(reply.data());
    }

    if (opcode == get_geometry) {
        if (request.size() != 8)
            return send_error(order, bad_length, opcode, sequence);
        WireReader reader(request.data() + 4, request.size() - 4, order);
        const auto drawable = reader.u32();
        if (!drawable)
            return malformed("truncated GetGeometry request");
        if (!resources_.is(*drawable, ResourceKind::window))
            return send_error(order, bad_drawable, opcode, sequence, *drawable);

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
        return queue(reply.data());
    }

    if (opcode == query_extension) {
        if (request.size() < 8)
            return send_error(order, bad_length, opcode, sequence);
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
        return queue(reply.data());
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
        return queue(reply.data());
    }

    return send_error(order, bad_request, opcode, sequence);
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
    auto dispatched = dispatch(*order_, *opcode, request, sequence_);
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
