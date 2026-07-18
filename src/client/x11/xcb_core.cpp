#include "client_connection.hpp"

#include <xcb/xproto.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>

namespace {

xmin::client::x11::Connection *
implementation(xcb_connection_t *connection)
{
    return connection == nullptr ? nullptr : connection->implementation.get();
}

template <typename Iterator>
xcb_generic_iterator_t
iterator_end(Iterator iterator)
{
    while (iterator.rem > 0) {
        ++iterator.data;
        --iterator.rem;
    }
    return {iterator.data, 0, iterator.index};
}

std::size_t
depth_size(const xcb_depth_t *depth)
{
    return sizeof(*depth) +
        static_cast<std::size_t>(depth->visuals_len) * sizeof(xcb_visualtype_t);
}

std::size_t
screen_size(const xcb_screen_t *screen)
{
    const auto *position = reinterpret_cast<const std::uint8_t *>(screen + 1);
    for (unsigned index = 0; index < screen->allowed_depths_len; ++index) {
        const auto *depth = reinterpret_cast<const xcb_depth_t *>(position);
        position += depth_size(depth);
    }
    return static_cast<std::size_t>(
        position - reinterpret_cast<const std::uint8_t *>(screen));
}

} // namespace

extern "C" {

xcb_connection_t *
xcb_connect(const char *display_name, int *screen_number)
{
    auto result = std::make_unique<xcb_connection_t>();
    result->implementation = xmin::client::x11::Connection::connect(
        display_name, screen_number);
    return result.release();
}

xcb_connection_t *
xcb_connect_to_display_with_auth_info(
    const char *display_name, xcb_auth_info_t *, int *screen_number)
{
    return xcb_connect(display_name, screen_number);
}

xcb_connection_t *
xcb_connect_to_fd(int descriptor, xcb_auth_info_t *authorization)
{
    auto result = std::make_unique<xcb_connection_t>();
    result->implementation = xmin::client::x11::Connection::connect_to_fd(
        descriptor, authorization);
    return result.release();
}

void
xcb_disconnect(xcb_connection_t *connection)
{
    delete connection;
}

int
xcb_connection_has_error(xcb_connection_t *connection)
{
    const auto *value = implementation(connection);
    return value == nullptr ? XCB_CONN_ERROR : value->error();
}

int
xcb_get_file_descriptor(xcb_connection_t *connection)
{
    const auto *value = implementation(connection);
    return value == nullptr ? -1 : value->descriptor();
}

const xcb_setup_t *
xcb_get_setup(xcb_connection_t *connection)
{
    const auto *value = implementation(connection);
    return value == nullptr ? nullptr : value->setup();
}

int
xcb_flush(xcb_connection_t *connection)
{
    return xcb_connection_has_error(connection) == 0 ? 1 : 0;
}

std::uint32_t
xcb_generate_id(xcb_connection_t *connection)
{
    auto *value = implementation(connection);
    return value == nullptr ? UINT32_MAX : value->generate_id();
}

std::uint32_t
xcb_get_maximum_request_length(xcb_connection_t *connection)
{
    auto *value = implementation(connection);
    return value == nullptr ? 0 : value->maximum_request_length();
}

void
xcb_prefetch_maximum_request_length(xcb_connection_t *)
{
}

const xcb_query_extension_reply_t *
xcb_get_extension_data(
    xcb_connection_t *connection, xcb_extension_t *extension)
{
    auto *value = implementation(connection);
    return value == nullptr ? nullptr : value->extension(extension);
}

void
xcb_prefetch_extension_data(
    xcb_connection_t *connection, xcb_extension_t *extension)
{
    (void) xcb_get_extension_data(connection, extension);
}

xcb_generic_event_t *
xcb_wait_for_event(xcb_connection_t *connection)
{
    auto *value = implementation(connection);
    return value == nullptr ? nullptr : value->wait_for_event();
}

xcb_generic_event_t *
xcb_poll_for_event(xcb_connection_t *connection)
{
    auto *value = implementation(connection);
    return value == nullptr ? nullptr : value->poll_for_event();
}

xcb_generic_event_t *
xcb_poll_for_queued_event(xcb_connection_t *connection)
{
    return xcb_poll_for_event(connection);
}

xcb_generic_error_t *
xcb_request_check(
    xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    auto *value = implementation(connection);
    return value == nullptr ? nullptr : value->request_check(cookie.sequence);
}

void
xcb_discard_reply(xcb_connection_t *connection, unsigned int sequence)
{
    auto *value = implementation(connection);
    if (value != nullptr)
        value->discard_reply(sequence);
}

void
xcb_discard_reply64(xcb_connection_t *connection, std::uint64_t sequence)
{
    auto *value = implementation(connection);
    if (value != nullptr)
        value->discard_reply(sequence);
}

unsigned int
xcb_send_request(
    xcb_connection_t *connection, int flags, struct iovec *vectors,
    const xcb_protocol_request_t *request)
{
    auto *value = implementation(connection);
    if (value == nullptr || request == nullptr)
        return 0;
    return static_cast<unsigned int>(
        value->send(flags, vectors, *request));
}

std::uint64_t
xcb_send_request64(
    xcb_connection_t *connection, int flags, struct iovec *vectors,
    const xcb_protocol_request_t *request)
{
    auto *value = implementation(connection);
    return value == nullptr || request == nullptr
        ? 0
        : value->send(flags, vectors, *request);
}

unsigned int
xcb_send_request_with_fds(
    xcb_connection_t *connection, int flags, struct iovec *vectors,
    const xcb_protocol_request_t *request, unsigned int descriptor_count,
    int *descriptors)
{
    auto *value = implementation(connection);
    if (value == nullptr || request == nullptr)
        return 0;
    return static_cast<unsigned int>(value->send(
        flags, vectors, *request, descriptors, descriptor_count));
}

std::uint64_t
xcb_send_request_with_fds64(
    xcb_connection_t *connection, int flags, struct iovec *vectors,
    const xcb_protocol_request_t *request, unsigned int descriptor_count,
    int *descriptors)
{
    auto *value = implementation(connection);
    return value == nullptr || request == nullptr
        ? 0
        : value->send(
            flags, vectors, *request, descriptors, descriptor_count);
}

void *
xcb_wait_for_reply(
    xcb_connection_t *connection, unsigned int sequence,
    xcb_generic_error_t **error)
{
    auto *value = implementation(connection);
    return value == nullptr ? nullptr : value->wait_for_reply(sequence, error);
}

void *
xcb_wait_for_reply64(
    xcb_connection_t *connection, std::uint64_t sequence,
    xcb_generic_error_t **error)
{
    auto *value = implementation(connection);
    return value == nullptr ? nullptr : value->wait_for_reply(sequence, error);
}

int
xcb_poll_for_reply(
    xcb_connection_t *connection, unsigned int sequence,
    void **reply, xcb_generic_error_t **error)
{
    auto *value = implementation(connection);
    return value != nullptr && value->poll_for_reply(sequence, reply, error);
}

int
xcb_poll_for_reply64(
    xcb_connection_t *connection, std::uint64_t sequence,
    void **reply, xcb_generic_error_t **error)
{
    auto *value = implementation(connection);
    return value != nullptr && value->poll_for_reply(sequence, reply, error);
}

int *
xcb_get_reply_fds(xcb_connection_t *, void *reply, std::size_t reply_length)
{
    return reinterpret_cast<int *>(
        static_cast<std::uint8_t *>(reply) + reply_length);
}

void
xcb_send_fd(xcb_connection_t *, int descriptor)
{
    if (descriptor >= 0)
        ::close(descriptor);
}

int
xcb_popcount(std::uint32_t mask)
{
    int result = 0;
    while (mask != 0) {
        result += static_cast<int>(mask & 1U);
        mask >>= 1;
    }
    return result;
}

xcb_format_iterator_t
xcb_setup_pixmap_formats_iterator(const xcb_setup_t *setup)
{
    auto *data = reinterpret_cast<xcb_format_t *>(
        const_cast<xcb_setup_t *>(setup) + 1);
    data = reinterpret_cast<xcb_format_t *>(
        reinterpret_cast<std::uint8_t *>(data) +
        ((setup->vendor_len + 3U) & ~3U));
    return {data, setup->pixmap_formats_len,
            static_cast<int>(reinterpret_cast<std::uint8_t *>(data) -
                             reinterpret_cast<const std::uint8_t *>(setup))};
}

void
xcb_format_next(xcb_format_iterator_t *iterator)
{
    --iterator->rem;
    ++iterator->data;
    iterator->index += sizeof(xcb_format_t);
}

xcb_generic_iterator_t
xcb_format_end(xcb_format_iterator_t iterator)
{
    return iterator_end(iterator);
}

xcb_screen_iterator_t
xcb_setup_roots_iterator(const xcb_setup_t *setup)
{
    auto formats = xcb_setup_pixmap_formats_iterator(setup);
    auto *data = reinterpret_cast<xcb_screen_t *>(
        formats.data + formats.rem);
    return {data, setup->roots_len,
            static_cast<int>(reinterpret_cast<std::uint8_t *>(data) -
                             reinterpret_cast<const std::uint8_t *>(setup))};
}

void
xcb_screen_next(xcb_screen_iterator_t *iterator)
{
    const auto size = screen_size(iterator->data);
    iterator->data = reinterpret_cast<xcb_screen_t *>(
        reinterpret_cast<std::uint8_t *>(iterator->data) + size);
    iterator->index += static_cast<int>(size);
    --iterator->rem;
}

xcb_depth_iterator_t
xcb_screen_allowed_depths_iterator(const xcb_screen_t *screen)
{
    auto *data = const_cast<xcb_depth_t *>(
        reinterpret_cast<const xcb_depth_t *>(screen + 1));
    return {data, screen->allowed_depths_len,
            static_cast<int>(sizeof(*screen))};
}

void
xcb_depth_next(xcb_depth_iterator_t *iterator)
{
    const auto size = depth_size(iterator->data);
    iterator->data = reinterpret_cast<xcb_depth_t *>(
        reinterpret_cast<std::uint8_t *>(iterator->data) + size);
    iterator->index += static_cast<int>(size);
    --iterator->rem;
}

xcb_visualtype_iterator_t
xcb_depth_visuals_iterator(const xcb_depth_t *depth)
{
    auto *data = const_cast<xcb_visualtype_t *>(
        reinterpret_cast<const xcb_visualtype_t *>(depth + 1));
    return {data, depth->visuals_len, static_cast<int>(sizeof(*depth))};
}

void
xcb_visualtype_next(xcb_visualtype_iterator_t *iterator)
{
    --iterator->rem;
    ++iterator->data;
    iterator->index += sizeof(xcb_visualtype_t);
}

int
xcb_str_sizeof(const void *buffer)
{
    const auto *value = static_cast<const xcb_str_t *>(buffer);
    return static_cast<int>(sizeof(*value) + value->name_len);
}

} // extern "C"
