#ifndef XMIN_CLIENT_X11_PROTOCOL_HPP
#define XMIN_CLIENT_X11_PROTOCOL_HPP

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include <cstddef>
#include <cstdint>
#include <sys/uio.h>
#include <vector>

namespace xmin::client::x11 {

struct RequestPart {
    const void *data = nullptr;
    std::size_t size = 0;
};

template <typename Cookie, typename Request>
Cookie
send_request(
    xcb_connection_t *connection, xcb_extension_t *extension,
    std::uint8_t opcode, bool is_void, int flags, const Request &request,
    const std::vector<RequestPart> &additional = {})
{
    std::vector<iovec> vectors;
    vectors.reserve(2 + additional.size() * 2);
    vectors.push_back({const_cast<Request *>(&request), sizeof(request)});
    vectors.push_back({nullptr, -sizeof(request) & std::size_t{3}});
    for (const auto &part : additional) {
        vectors.push_back({const_cast<void *>(part.data), part.size});
        vectors.push_back({nullptr, -part.size & std::size_t{3}});
    }
    const xcb_protocol_request_t description{
        vectors.size(), extension, opcode,
        static_cast<std::uint8_t>(is_void ? 1 : 0)};
    Cookie cookie{};
    cookie.sequence = xcb_send_request(
        connection, flags, vectors.data(), &description);
    return cookie;
}

template <typename Reply, typename Cookie>
Reply *
wait_for_reply(
    xcb_connection_t *connection, Cookie cookie,
    xcb_generic_error_t **error)
{
    return static_cast<Reply *>(
        xcb_wait_for_reply(connection, cookie.sequence, error));
}

template <typename Iterator>
xcb_generic_iterator_t
fixed_end(Iterator iterator)
{
    xcb_generic_iterator_t result{};
    result.data = iterator.data + iterator.rem;
    result.rem = 0;
    result.index = iterator.index + static_cast<int>(
        reinterpret_cast<char *>(result.data) -
        reinterpret_cast<char *>(iterator.data));
    return result;
}

} // namespace xmin::client::x11

#endif
