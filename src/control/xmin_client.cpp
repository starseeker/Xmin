#include "xmin_client.h"

#include "xmin/next/generated/core_protocol.hpp"
#include "xmin/next/unique_fd.hpp"
#include "xmin/next/xauthority.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using xmin::next::CoreOpcode;
using xmin::next::UniqueFd;

constexpr std::string_view auth_protocol = "MIT-MAGIC-COOKIE-1";

std::uint8_t
opcode(CoreOpcode value)
{
    return static_cast<std::uint8_t>(value);
}

bool
host_is_little_endian()
{
    const std::uint16_t value = 1;
    std::uint8_t first = 0;
    std::memcpy(&first, &value, 1);
    return first == 1;
}

void
put16(std::vector<std::uint8_t> &bytes, std::size_t offset,
      std::uint16_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void
put32(std::vector<std::uint8_t> &bytes, std::size_t offset,
      std::uint32_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::uint16_t
get16(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint32_t
get32(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::vector<std::uint8_t>
request(std::uint8_t major, std::uint8_t minor, std::size_t size)
{
    std::vector<std::uint8_t> bytes(size);
    bytes[0] = major;
    bytes[1] = minor;
    put16(bytes, 2, static_cast<std::uint16_t>(size / 4));
    return bytes;
}

unsigned
bit_count(std::uint16_t value)
{
    unsigned count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1;
    }
    return count;
}

bool
write_all(int descriptor, const std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool
read_all(int descriptor, std::uint8_t *bytes, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::read(descriptor, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

template <typename Reply>
Reply *
copy_packet(const std::vector<std::uint8_t> &packet)
{
    const std::size_t size = std::max(sizeof(Reply), packet.size());
    auto *reply = static_cast<Reply *>(std::calloc(1, size));
    if (reply != nullptr)
        std::memcpy(reply, packet.data(), packet.size());
    return reply;
}

class ClientConnection {
public:
    static std::unique_ptr<ClientConnection>
    connect(const char *display_text, std::string &error)
    {
        int display = 0;
        int screen_number = 0;
        if (!parse_display(display_text, display, screen_number)) {
            error = "DISPLAY must name a local Xmin display such as :20";
            return {};
        }
        if (screen_number != 0) {
            error = "Xmin provides only screen 0";
            return {};
        }
        auto client = std::unique_ptr<ClientConnection>(new ClientConnection);
        client->display_number_ = display;
        if (!client->open_socket(display, error) ||
            !client->setup_connection(error)) {
            return {};
        }
        return client;
    }

    unsigned send(std::vector<std::uint8_t> bytes)
    {
        if (failed_ || !write_all(socket_.get(), bytes)) {
            failed_ = true;
            return 0;
        }
        ++sequence_;
        return sequence_;
    }

    std::optional<std::vector<std::uint8_t>>
    wait_reply(unsigned sequence, xcb_generic_error_t **reported_error)
    {
        const auto key = static_cast<std::uint16_t>(sequence);
        for (;;) {
            auto reply = replies_.find(key);
            if (reply != replies_.end()) {
                auto packet = std::move(reply->second);
                replies_.erase(reply);
                return packet;
            }
            auto error = errors_.find(key);
            if (error != errors_.end()) {
                if (reported_error != nullptr)
                    *reported_error = copy_packet<xcb_generic_error_t>(
                        error->second);
                errors_.erase(error);
                return std::nullopt;
            }
            if (!read_one())
                return std::nullopt;
        }
    }

    xcb_generic_error_t *check(unsigned target)
    {
        auto sync = request(opcode(CoreOpcode::GetInputFocus), 0, 4);
        const unsigned sequence = send(std::move(sync));
        if (sequence == 0 || !wait_reply(sequence, nullptr))
            return nullptr;
        const auto found = errors_.find(static_cast<std::uint16_t>(target));
        if (found == errors_.end())
            return nullptr;
        auto *error = copy_packet<xcb_generic_error_t>(found->second);
        errors_.erase(found);
        return error;
    }

    const xcb_query_extension_reply_t *extension(std::string_view name)
    {
        auto cached = extensions_.find(std::string(name));
        if (cached != extensions_.end())
            return &cached->second;
        auto bytes = request(
            opcode(CoreOpcode::QueryExtension), 0,
            8 + ((name.size() + 3) & ~std::size_t{3}));
        put16(bytes, 4, static_cast<std::uint16_t>(name.size()));
        std::memcpy(bytes.data() + 8, name.data(), name.size());
        const unsigned sequence = send(std::move(bytes));
        xcb_query_extension_reply_t decoded{};
        if (auto packet = wait_reply(sequence, nullptr);
            packet && packet->size() >= 32) {
            std::memcpy(&decoded, packet->data(), 32);
        }
        const auto inserted = extensions_.emplace(std::string(name), decoded);
        return &inserted.first->second;
    }

    xcb_generic_event_t *poll_event()
    {
        if (!events_.empty()) {
            auto packet = std::move(events_.front());
            events_.pop_front();
            return copy_packet<xcb_generic_event_t>(packet);
        }
        pollfd ready{socket_.get(), POLLIN, 0};
        int result;
        do {
            result = ::poll(&ready, 1, 0);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || (ready.revents & POLLIN) == 0)
            return nullptr;
        if (!read_one() || events_.empty())
            return nullptr;
        auto packet = std::move(events_.front());
        events_.pop_front();
        return copy_packet<xcb_generic_event_t>(packet);
    }

    [[nodiscard]] xcb_setup_t *setup() noexcept { return &setup_; }
    [[nodiscard]] xcb_screen_t *screen() noexcept { return &screen_; }
    [[nodiscard]] int fd() const noexcept { return socket_.get(); }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] int display_number() const noexcept
    {
        return display_number_;
    }

    std::uint32_t generate_id()
    {
        const std::uint32_t id = setup_.resource_id_base |
            (next_resource_ & setup_.resource_id_mask);
        ++next_resource_;
        return id;
    }

private:
    static bool
    parse_display(const char *text, int &display, int &screen)
    {
        if (text == nullptr)
            text = std::getenv("DISPLAY");
        if (text == nullptr || *text != ':')
            return false;
        char *end = nullptr;
        errno = 0;
        const long parsed = std::strtol(text + 1, &end, 10);
        if (errno != 0 || end == text + 1 || parsed < 0 || parsed > 65535)
            return false;
        long parsed_screen = 0;
        if (*end == '.') {
            char *screen_end = nullptr;
            parsed_screen = std::strtol(end + 1, &screen_end, 10);
            if (screen_end == end + 1 || parsed_screen < 0 ||
                parsed_screen > 65535) {
                return false;
            }
            end = screen_end;
        }
        if (*end != '\0')
            return false;
        display = static_cast<int>(parsed);
        screen = static_cast<int>(parsed_screen);
        return true;
    }

    bool open_socket(int display, std::string &error)
    {
        const std::string path = "/tmp/.X11-unix/X" + std::to_string(display);
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path)) {
            error = "X11 socket path is too long";
            return false;
        }
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        const auto address_size = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path.size() + 1);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||        \
    defined(__OpenBSD__)
        address.sun_len = static_cast<std::uint8_t>(address_size);
#endif
        socket_.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!socket_) {
            error = std::string("cannot create X11 socket: ") +
                std::strerror(errno);
            return false;
        }
        if (::connect(
                socket_.get(), reinterpret_cast<const sockaddr *>(&address),
                address_size) != 0) {
            error = "cannot connect to :" + std::to_string(display) + ": " +
                std::strerror(errno);
            return false;
        }
        return true;
    }

    bool setup_connection(std::string &error)
    {
        std::vector<std::uint8_t> cookie;
        const char *authority = std::getenv("XAUTHORITY");
        if (authority != nullptr && *authority != '\0') {
            auto loaded = xmin::next::load_xauthority_cookie(
                authority, static_cast<unsigned>(display_number_));
            if (!loaded) {
                error = loaded.error().message;
                return false;
            }
            cookie = std::move(loaded.value());
        }
        const std::string_view name = cookie.empty()
            ? std::string_view{}
            : auth_protocol;
        std::vector<std::uint8_t> prefix(12);
        prefix[0] = host_is_little_endian() ? 'l' : 'B';
        put16(prefix, 2, 11);
        put16(prefix, 6, static_cast<std::uint16_t>(name.size()));
        put16(prefix, 8, static_cast<std::uint16_t>(cookie.size()));
        std::vector<std::uint8_t> authentication;
        authentication.insert(authentication.end(), name.begin(), name.end());
        while ((authentication.size() & 3U) != 0)
            authentication.push_back(0);
        authentication.insert(
            authentication.end(), cookie.begin(), cookie.end());
        while ((authentication.size() & 3U) != 0)
            authentication.push_back(0);
        if (!write_all(socket_.get(), prefix) ||
            !write_all(socket_.get(), authentication)) {
            error = "failed to write X11 setup";
            return false;
        }
        std::vector<std::uint8_t> setup_prefix(8);
        if (!read_all(socket_.get(), setup_prefix.data(), setup_prefix.size())) {
            error = "X11 server closed during setup";
            return false;
        }
        const std::size_t body_size =
            static_cast<std::size_t>(get16(setup_prefix, 6)) * 4;
        std::vector<std::uint8_t> body(body_size);
        if (!read_all(socket_.get(), body.data(), body.size())) {
            error = "truncated X11 setup reply";
            return false;
        }
        if (setup_prefix[0] != 1 || body.size() < 40) {
            error = setup_prefix[0] == 0
                ? "X11 authentication was rejected"
                : "invalid X11 setup reply";
            return false;
        }
        setup_.status = 1;
        setup_.protocol_major_version = get16(setup_prefix, 2);
        setup_.protocol_minor_version = get16(setup_prefix, 4);
        setup_.length = get16(setup_prefix, 6);
        setup_.release_number = get32(body, 0);
        setup_.resource_id_base = get32(body, 4);
        setup_.resource_id_mask = get32(body, 8);
        setup_.motion_buffer_size = get32(body, 12);
        setup_.vendor_len = get16(body, 16);
        setup_.maximum_request_length = get16(body, 18);
        setup_.roots_len = body[20];
        setup_.pixmap_formats_len = body[21];
        setup_.image_byte_order = body[22];
        setup_.bitmap_format_bit_order = body[23];
        setup_.bitmap_format_scanline_unit = body[24];
        setup_.bitmap_format_scanline_pad = body[25];
        setup_.min_keycode = body[26];
        setup_.max_keycode = body[27];
        std::size_t offset =
            32 + ((static_cast<std::size_t>(setup_.vendor_len) + 3) & ~3U);
        const unsigned format_count =
            std::min<unsigned>(setup_.pixmap_formats_len, 8);
        if (offset + static_cast<std::size_t>(setup_.pixmap_formats_len) * 8 +
                40 >
            body.size()) {
            error = "invalid X11 setup layout";
            return false;
        }
        for (unsigned index = 0; index < format_count; ++index) {
            const std::size_t at = offset + index * 8;
            setup_.xmin_formats[index].depth = body[at];
            setup_.xmin_formats[index].bits_per_pixel = body[at + 1];
            setup_.xmin_formats[index].scanline_pad = body[at + 2];
        }
        offset += static_cast<std::size_t>(setup_.pixmap_formats_len) * 8;
        screen_.root = get32(body, offset);
        screen_.default_colormap = get32(body, offset + 4);
        screen_.white_pixel = get32(body, offset + 8);
        screen_.black_pixel = get32(body, offset + 12);
        screen_.current_input_masks = get32(body, offset + 16);
        screen_.width_in_pixels = get16(body, offset + 20);
        screen_.height_in_pixels = get16(body, offset + 22);
        screen_.width_in_millimeters = get16(body, offset + 24);
        screen_.height_in_millimeters = get16(body, offset + 26);
        screen_.min_installed_maps = get16(body, offset + 28);
        screen_.max_installed_maps = get16(body, offset + 30);
        screen_.root_visual = get32(body, offset + 32);
        screen_.backing_stores = body[offset + 36];
        screen_.save_unders = body[offset + 37];
        screen_.root_depth = body[offset + 38];
        screen_.allowed_depths_len = 1;
        std::size_t depth_offset = offset + 40;
        bool visual_found = false;
        for (unsigned depth_index = 0;
             depth_index < body[offset + 39] && depth_offset + 8 <= body.size();
             ++depth_index) {
            const std::uint8_t depth = body[depth_offset];
            const std::uint16_t visual_count = get16(body, depth_offset + 2);
            const std::size_t visuals_offset = depth_offset + 8;
            if (visuals_offset + static_cast<std::size_t>(visual_count) * 24 >
                body.size()) {
                break;
            }
            for (unsigned visual_index = 0; visual_index < visual_count;
                 ++visual_index) {
                const std::size_t at = visuals_offset + visual_index * 24;
                if (get32(body, at) != screen_.root_visual)
                    continue;
                auto &visual = screen_.xmin_depth.xmin_visual;
                screen_.xmin_depth.depth = depth;
                screen_.xmin_depth.visuals_len = 1;
                visual.visual_id = get32(body, at);
                visual.class_ = body[at + 4];
                visual.bits_per_rgb_value = body[at + 5];
                visual.colormap_entries = get16(body, at + 6);
                visual.red_mask = get32(body, at + 8);
                visual.green_mask = get32(body, at + 12);
                visual.blue_mask = get32(body, at + 16);
                visual_found = true;
            }
            depth_offset = visuals_offset +
                static_cast<std::size_t>(visual_count) * 24;
        }
        if (!visual_found) {
            error = "X11 setup omitted the root visual";
            return false;
        }
        return true;
    }

    bool read_one()
    {
        std::vector<std::uint8_t> packet(32);
        if (!read_all(socket_.get(), packet.data(), packet.size())) {
            failed_ = true;
            return false;
        }
        const std::uint8_t type = packet[0] & 0x7fU;
        if (type == 1 || type == 35) {
            const std::size_t extra =
                static_cast<std::size_t>(get32(packet, 4)) * 4;
            if (extra > 16U * 1024U * 1024U) {
                failed_ = true;
                return false;
            }
            packet.resize(32 + extra);
            if (extra != 0 &&
                !read_all(socket_.get(), packet.data() + 32, extra)) {
                failed_ = true;
                return false;
            }
        }
        const std::uint16_t sequence = get16(packet, 2);
        if (type == 0)
            errors_[sequence] = std::move(packet);
        else if (type == 1)
            replies_[sequence] = std::move(packet);
        else
            events_.push_back(std::move(packet));
        return true;
    }

    UniqueFd socket_;
    xcb_setup_t setup_{};
    xcb_screen_t screen_{};
    unsigned sequence_ = 0;
    std::uint32_t next_resource_ = 1;
    int display_number_ = 0;
    bool failed_ = false;
    std::map<std::uint16_t, std::vector<std::uint8_t>> replies_;
    std::map<std::uint16_t, std::vector<std::uint8_t>> errors_;
    std::deque<std::vector<std::uint8_t>> events_;
    std::map<std::string, xcb_query_extension_reply_t> extensions_;
};

ClientConnection *
client(xcb_connection_t *connection);

template <typename Reply>
Reply *
reply_for(xcb_connection_t *connection, xcb_cookie_t cookie,
          xcb_generic_error_t **error)
{
    auto packet = client(connection)->wait_reply(cookie.sequence, error);
    return packet ? copy_packet<Reply>(*packet) : nullptr;
}

xcb_cookie_t
send_request(xcb_connection_t *connection, std::vector<std::uint8_t> bytes)
{
    return xcb_cookie_t{client(connection)->send(std::move(bytes))};
}

void
set_error(char *destination, std::size_t size, const std::string &message)
{
    if (destination != nullptr && size != 0)
        std::snprintf(destination, size, "%s", message.c_str());
}

} // namespace

struct xcb_connection_t {
    std::unique_ptr<ClientConnection> implementation;
};

namespace {
ClientConnection *
client(xcb_connection_t *connection)
{
    return connection->implementation.get();
}
} // namespace

extern "C" {

xcb_extension_t xcb_test_id{"XTEST", 0};
xcb_extension_t xcb_composite_id{"Composite", 0};
xcb_extension_t xcb_damage_id{"DAMAGE", 0};

int
xmin_xcb_connect(xmin_xcb_session *session, const char *display,
                 char *error, size_t error_size)
{
    std::memset(session, 0, sizeof(*session));
    std::string message;
    auto implementation = ClientConnection::connect(display, message);
    if (!implementation) {
        set_error(error, error_size, message);
        return -1;
    }
    auto connection = std::make_unique<xcb_connection_t>();
    session->screen = implementation->screen();
    session->display_number = implementation->display_number();
    session->screen_number = 0;
    connection->implementation = std::move(implementation);
    session->connection = connection.release();
    return 0;
}

void
xmin_xcb_disconnect(xmin_xcb_session *session)
{
    delete session->connection;
    std::memset(session, 0, sizeof(*session));
}

const xcb_setup_t *xcb_get_setup(xcb_connection_t *connection)
{
    return client(connection)->setup();
}

xcb_depth_iterator_t xcb_screen_allowed_depths_iterator(xcb_screen_t *screen)
{
    return {&screen->xmin_depth, 1, 0};
}
void xcb_depth_next(xcb_depth_iterator_t *iterator)
{
    iterator->data = nullptr;
    iterator->rem = 0;
    ++iterator->index;
}
xcb_visualtype_iterator_t xcb_depth_visuals_iterator(xcb_depth_t *depth)
{
    return {&depth->xmin_visual, depth->visuals_len != 0 ? 1 : 0, 0};
}
void xcb_visualtype_next(xcb_visualtype_iterator_t *iterator)
{
    iterator->data = nullptr;
    iterator->rem = 0;
    ++iterator->index;
}
xcb_format_iterator_t xcb_setup_pixmap_formats_iterator(const xcb_setup_t *setup)
{
    return {const_cast<xcb_format_t *>(setup->xmin_formats),
            std::min<int>(setup->pixmap_formats_len, 8), 0};
}
void xcb_format_next(xcb_format_iterator_t *iterator)
{
    if (iterator->rem > 0) {
        --iterator->rem;
        ++iterator->index;
        ++iterator->data;
    }
}

xcb_intern_atom_cookie_t
xcb_intern_atom(xcb_connection_t *connection, uint8_t only_if_exists,
                uint16_t name_len, const char *name)
{
    const std::size_t size = 8 + ((name_len + 3U) & ~3U);
    auto bytes = request(opcode(CoreOpcode::InternAtom), only_if_exists, size);
    put16(bytes, 4, name_len);
    std::memcpy(bytes.data() + 8, name, name_len);
    return send_request(connection, std::move(bytes));
}
xcb_intern_atom_reply_t *xcb_intern_atom_reply(
    xcb_connection_t *connection, xcb_intern_atom_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_intern_atom_reply_t>(connection, cookie, error);
}

xcb_get_property_cookie_t
xcb_get_property(xcb_connection_t *connection, uint8_t remove,
                 xcb_window_t window, xcb_atom_t property, xcb_atom_t type,
                 uint32_t offset, uint32_t length)
{
    auto bytes = request(opcode(CoreOpcode::GetProperty), remove, 24);
    put32(bytes, 4, window);
    put32(bytes, 8, property);
    put32(bytes, 12, type);
    put32(bytes, 16, offset);
    put32(bytes, 20, length);
    return send_request(connection, std::move(bytes));
}
xcb_get_property_reply_t *xcb_get_property_reply(
    xcb_connection_t *connection, xcb_get_property_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_get_property_reply_t>(connection, cookie, error);
}
int xcb_get_property_value_length(const xcb_get_property_reply_t *reply)
{
    return static_cast<int>(reply->value_len * (reply->format / 8U));
}
void *xcb_get_property_value(const xcb_get_property_reply_t *reply)
{
    return const_cast<xcb_get_property_reply_t *>(reply) + 1;
}

xcb_get_window_attributes_cookie_t
xcb_get_window_attributes(xcb_connection_t *connection, xcb_window_t window)
{
    auto bytes = request(opcode(CoreOpcode::GetWindowAttributes), 0, 8);
    put32(bytes, 4, window);
    return send_request(connection, std::move(bytes));
}
xcb_get_window_attributes_reply_t *xcb_get_window_attributes_reply(
    xcb_connection_t *connection, xcb_get_window_attributes_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_get_window_attributes_reply_t>(
        connection, cookie, error);
}

xcb_query_tree_cookie_t xcb_query_tree(
    xcb_connection_t *connection, xcb_window_t window)
{
    auto bytes = request(opcode(CoreOpcode::QueryTree), 0, 8);
    put32(bytes, 4, window);
    return send_request(connection, std::move(bytes));
}
xcb_query_tree_reply_t *xcb_query_tree_reply(
    xcb_connection_t *connection, xcb_query_tree_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_query_tree_reply_t>(connection, cookie, error);
}
int xcb_query_tree_children_length(const xcb_query_tree_reply_t *reply)
{
    return reply->children_len;
}
xcb_window_t *xcb_query_tree_children(const xcb_query_tree_reply_t *reply)
{
    return reinterpret_cast<xcb_window_t *>(
        const_cast<xcb_query_tree_reply_t *>(reply) + 1);
}

xcb_get_geometry_cookie_t xcb_get_geometry(
    xcb_connection_t *connection, xcb_drawable_t drawable)
{
    auto bytes = request(opcode(CoreOpcode::GetGeometry), 0, 8);
    put32(bytes, 4, drawable);
    return send_request(connection, std::move(bytes));
}
xcb_get_geometry_reply_t *xcb_get_geometry_reply(
    xcb_connection_t *connection, xcb_get_geometry_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_get_geometry_reply_t>(connection, cookie, error);
}

xcb_translate_coordinates_cookie_t xcb_translate_coordinates(
    xcb_connection_t *connection, xcb_window_t source,
    xcb_window_t destination, int16_t x, int16_t y)
{
    auto bytes = request(opcode(CoreOpcode::TranslateCoordinates), 0, 16);
    put32(bytes, 4, source);
    put32(bytes, 8, destination);
    put16(bytes, 12, static_cast<std::uint16_t>(x));
    put16(bytes, 14, static_cast<std::uint16_t>(y));
    return send_request(connection, std::move(bytes));
}
xcb_translate_coordinates_reply_t *xcb_translate_coordinates_reply(
    xcb_connection_t *connection, xcb_translate_coordinates_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_translate_coordinates_reply_t>(
        connection, cookie, error);
}

xcb_get_input_focus_cookie_t xcb_get_input_focus(xcb_connection_t *connection)
{
    return send_request(
        connection, request(opcode(CoreOpcode::GetInputFocus), 0, 4));
}
xcb_get_input_focus_reply_t *xcb_get_input_focus_reply(
    xcb_connection_t *connection, xcb_get_input_focus_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_get_input_focus_reply_t>(connection, cookie, error);
}

xcb_get_keyboard_mapping_cookie_t xcb_get_keyboard_mapping(
    xcb_connection_t *connection, xcb_keycode_t first, uint8_t count)
{
    auto bytes = request(opcode(CoreOpcode::GetKeyboardMapping), 0, 8);
    bytes[4] = first;
    bytes[5] = count;
    return send_request(connection, std::move(bytes));
}
xcb_get_keyboard_mapping_reply_t *xcb_get_keyboard_mapping_reply(
    xcb_connection_t *connection, xcb_get_keyboard_mapping_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_get_keyboard_mapping_reply_t>(
        connection, cookie, error);
}
xcb_keysym_t *xcb_get_keyboard_mapping_keysyms(
    const xcb_get_keyboard_mapping_reply_t *reply)
{
    return reinterpret_cast<xcb_keysym_t *>(
        const_cast<xcb_get_keyboard_mapping_reply_t *>(reply) + 1);
}

xcb_query_pointer_cookie_t xcb_query_pointer(
    xcb_connection_t *connection, xcb_window_t window)
{
    auto bytes = request(opcode(CoreOpcode::QueryPointer), 0, 8);
    put32(bytes, 4, window);
    return send_request(connection, std::move(bytes));
}
xcb_query_pointer_reply_t *xcb_query_pointer_reply(
    xcb_connection_t *connection, xcb_query_pointer_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_query_pointer_reply_t>(connection, cookie, error);
}

xcb_get_image_cookie_t xcb_get_image(
    xcb_connection_t *connection, uint8_t format, xcb_drawable_t drawable,
    int16_t x, int16_t y, uint16_t width, uint16_t height, uint32_t plane_mask)
{
    auto bytes = request(opcode(CoreOpcode::GetImage), format, 20);
    put32(bytes, 4, drawable);
    put16(bytes, 8, static_cast<std::uint16_t>(x));
    put16(bytes, 10, static_cast<std::uint16_t>(y));
    put16(bytes, 12, width);
    put16(bytes, 14, height);
    put32(bytes, 16, plane_mask);
    return send_request(connection, std::move(bytes));
}
xcb_get_image_reply_t *xcb_get_image_reply(
    xcb_connection_t *connection, xcb_get_image_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_get_image_reply_t>(connection, cookie, error);
}
int xcb_get_image_data_length(const xcb_get_image_reply_t *reply)
{
    return static_cast<int>(reply->length * 4U);
}
uint8_t *xcb_get_image_data(const xcb_get_image_reply_t *reply)
{
    return reinterpret_cast<std::uint8_t *>(
        const_cast<xcb_get_image_reply_t *>(reply) + 1);
}

const xcb_query_extension_reply_t *xcb_get_extension_data(
    xcb_connection_t *connection, xcb_extension_t *extension)
{
    return client(connection)->extension(extension->name);
}
uint32_t xcb_generate_id(xcb_connection_t *connection)
{
    return client(connection)->generate_id();
}
xcb_generic_error_t *xcb_request_check(
    xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    return client(connection)->check(cookie.sequence);
}
int xcb_flush(xcb_connection_t *) { return 1; }
int xcb_connection_has_error(xcb_connection_t *connection)
{
    return client(connection)->failed() ? 1 : 0;
}
int xcb_get_file_descriptor(xcb_connection_t *connection)
{
    return client(connection)->fd();
}
xcb_generic_event_t *xcb_poll_for_event(xcb_connection_t *connection)
{
    return client(connection)->poll_event();
}

xcb_void_cookie_t xcb_configure_window_checked(
    xcb_connection_t *connection, xcb_window_t window, uint16_t mask,
    const uint32_t *values)
{
    const unsigned count = bit_count(mask);
    auto bytes = request(opcode(CoreOpcode::ConfigureWindow), 0, 12 + count * 4);
    put32(bytes, 4, window);
    put16(bytes, 8, mask);
    for (unsigned index = 0; index < count; ++index)
        put32(bytes, 12 + index * 4, values[index]);
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_map_window_checked(
    xcb_connection_t *connection, xcb_window_t window)
{
    auto bytes = request(opcode(CoreOpcode::MapWindow), 0, 8);
    put32(bytes, 4, window);
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_unmap_window_checked(
    xcb_connection_t *connection, xcb_window_t window)
{
    auto bytes = request(opcode(CoreOpcode::UnmapWindow), 0, 8);
    put32(bytes, 4, window);
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_set_input_focus_checked(
    xcb_connection_t *connection, uint8_t revert_to, xcb_window_t focus,
    uint32_t time)
{
    auto bytes = request(opcode(CoreOpcode::SetInputFocus), revert_to, 12);
    put32(bytes, 4, focus);
    put32(bytes, 8, time);
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_send_event_checked(
    xcb_connection_t *connection, uint8_t propagate, xcb_window_t destination,
    uint32_t event_mask, const char *event)
{
    auto bytes = request(opcode(CoreOpcode::SendEvent), propagate, 44);
    put32(bytes, 4, destination);
    put32(bytes, 8, event_mask);
    std::memcpy(bytes.data() + 12, event, 32);
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_free_pixmap(
    xcb_connection_t *connection, xcb_pixmap_t pixmap)
{
    auto bytes = request(opcode(CoreOpcode::FreePixmap), 0, 8);
    put32(bytes, 4, pixmap);
    return send_request(connection, std::move(bytes));
}

xcb_void_cookie_t xcb_test_fake_input_checked(
    xcb_connection_t *connection, uint8_t type, uint8_t detail, uint32_t time,
    xcb_window_t root, int16_t root_x, int16_t root_y, uint8_t device)
{
    const auto *extension = client(connection)->extension("XTEST");
    auto bytes = request(extension->major_opcode, 2, 36);
    bytes[4] = type;
    bytes[5] = detail;
    put32(bytes, 8, time);
    put32(bytes, 12, root);
    put16(bytes, 24, static_cast<std::uint16_t>(root_x));
    put16(bytes, 26, static_cast<std::uint16_t>(root_y));
    bytes[35] = device;
    return send_request(connection, std::move(bytes));
}

xcb_composite_query_version_cookie_t xcb_composite_query_version(
    xcb_connection_t *connection, uint32_t major, uint32_t minor)
{
    const auto *extension = client(connection)->extension("Composite");
    auto bytes = request(extension->major_opcode, 0, 12);
    put32(bytes, 4, major);
    put32(bytes, 8, minor);
    return send_request(connection, std::move(bytes));
}
xcb_composite_query_version_reply_t *xcb_composite_query_version_reply(
    xcb_connection_t *connection, xcb_composite_query_version_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_composite_query_version_reply_t>(
        connection, cookie, error);
}
xcb_void_cookie_t xcb_composite_redirect_window_checked(
    xcb_connection_t *connection, xcb_window_t window, uint8_t update)
{
    const auto *extension = client(connection)->extension("Composite");
    auto bytes = request(extension->major_opcode, 1, 12);
    put32(bytes, 4, window);
    bytes[8] = update;
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_composite_name_window_pixmap_checked(
    xcb_connection_t *connection, xcb_window_t window, xcb_pixmap_t pixmap)
{
    const auto *extension = client(connection)->extension("Composite");
    auto bytes = request(extension->major_opcode, 6, 12);
    put32(bytes, 4, window);
    put32(bytes, 8, pixmap);
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_composite_unredirect_window(
    xcb_connection_t *connection, xcb_window_t window, uint8_t update)
{
    const auto *extension = client(connection)->extension("Composite");
    auto bytes = request(extension->major_opcode, 3, 12);
    put32(bytes, 4, window);
    bytes[8] = update;
    return send_request(connection, std::move(bytes));
}

xcb_damage_query_version_cookie_t xcb_damage_query_version(
    xcb_connection_t *connection, uint32_t major, uint32_t minor)
{
    const auto *extension = client(connection)->extension("DAMAGE");
    auto bytes = request(extension->major_opcode, 0, 12);
    put32(bytes, 4, major);
    put32(bytes, 8, minor);
    return send_request(connection, std::move(bytes));
}
xcb_damage_query_version_reply_t *xcb_damage_query_version_reply(
    xcb_connection_t *connection, xcb_damage_query_version_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return reply_for<xcb_damage_query_version_reply_t>(
        connection, cookie, error);
}
xcb_void_cookie_t xcb_damage_create_checked(
    xcb_connection_t *connection, xcb_damage_damage_t damage,
    xcb_drawable_t drawable, uint8_t level)
{
    const auto *extension = client(connection)->extension("DAMAGE");
    auto bytes = request(extension->major_opcode, 1, 16);
    put32(bytes, 4, damage);
    put32(bytes, 8, drawable);
    bytes[12] = level;
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_damage_subtract(
    xcb_connection_t *connection, xcb_damage_damage_t damage,
    uint32_t repair, uint32_t parts)
{
    const auto *extension = client(connection)->extension("DAMAGE");
    auto bytes = request(extension->major_opcode, 3, 16);
    put32(bytes, 4, damage);
    put32(bytes, 8, repair);
    put32(bytes, 12, parts);
    return send_request(connection, std::move(bytes));
}
xcb_void_cookie_t xcb_damage_destroy(
    xcb_connection_t *connection, xcb_damage_damage_t damage)
{
    const auto *extension = client(connection)->extension("DAMAGE");
    auto bytes = request(extension->major_opcode, 2, 8);
    put32(bytes, 4, damage);
    return send_request(connection, std::move(bytes));
}

} // extern "C"
