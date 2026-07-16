#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view auth_name = "MIT-MAGIC-COOKIE-1";
constexpr std::string_view cookie_hex = "000102030405060708090a0b0c0d0e0f";
constexpr std::uint32_t pointer_root = 1;
constexpr std::uint32_t root_window = 0x00000100;

struct Child {
    pid_t process = -1;
    int socket = -1;
};

bool
host_is_little_endian()
{
    const std::uint16_t value = 1;
    std::uint8_t bytes[sizeof(value)];
    std::memcpy(bytes, &value, sizeof(value));
    return bytes[0] == 1;
}

void
put16(std::vector<std::uint8_t> &bytes, std::size_t offset,
      std::uint16_t value, bool little)
{
    if (little) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    else {
        bytes[offset] = static_cast<std::uint8_t>(value >> 8);
        bytes[offset + 1] = static_cast<std::uint8_t>(value);
    }
}

void
put32(std::vector<std::uint8_t> &bytes, std::size_t offset,
      std::uint32_t value, bool little)
{
    for (unsigned index = 0; index < 4; ++index) {
        const unsigned shift = little ? index * 8 : (3 - index) * 8;
        bytes[offset + index] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::uint16_t
get16(const std::vector<std::uint8_t> &bytes, std::size_t offset, bool little)
{
    if (little) {
        return static_cast<std::uint16_t>(bytes[offset] |
            (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
}

std::uint32_t
get32(const std::vector<std::uint8_t> &bytes, std::size_t offset, bool little)
{
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        const unsigned shift = little ? index * 8 : (3 - index) * 8;
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << shift;
    }
    return value;
}

bool
write_all(int descriptor, const std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool
write_fragmented(int descriptor, const std::vector<std::uint8_t> &bytes,
                 std::size_t split)
{
    if (split == 0 || split >= bytes.size())
        return write_all(descriptor, bytes);
    const std::vector<std::uint8_t> first(bytes.begin(), bytes.begin() +
        static_cast<std::ptrdiff_t>(split));
    const std::vector<std::uint8_t> second(
        bytes.begin() + static_cast<std::ptrdiff_t>(split), bytes.end());
    if (!write_all(descriptor, first))
        return false;
    const timespec pause{0, 10000000};
    static_cast<void>(::nanosleep(&pause, nullptr));
    return write_all(descriptor, second);
}

bool
read_all(int descriptor, std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

Child
spawn_server(const char *server)
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return {};
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return {};
    }
    if (child == 0) {
        ::close(sockets[0]);
        char descriptor[32];
        std::snprintf(descriptor, sizeof(descriptor), "%d", sockets[1]);
        ::execl(server, server, "--client-fd", descriptor, "--cookie-hex",
                cookie_hex.data(), "--screen", "320x240",
                static_cast<char *>(nullptr));
        _exit(127);
    }
    ::close(sockets[1]);
    return Child{child, sockets[0]};
}

bool
wait_for_success(pid_t child)
{
    const timespec pause{0, 10000000};
    for (int attempt = 0; attempt < 500; ++attempt) {
        int status = 0;
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR)
            return false;
        ::nanosleep(&pause, nullptr);
    }
    ::kill(child, SIGKILL);
    static_cast<void>(::waitpid(child, nullptr, 0));
    return false;
}

std::vector<std::uint8_t>
cookie_bytes(bool valid)
{
    std::vector<std::uint8_t> cookie(16);
    for (std::size_t index = 0; index < cookie.size(); ++index)
        cookie[index] = static_cast<std::uint8_t>(index);
    if (!valid)
        cookie[0] ^= 0xff;
    return cookie;
}

bool
send_setup(int descriptor, bool little, bool valid_cookie,
           std::uint16_t major = 11, bool fragmented = false)
{
    std::vector<std::uint8_t> prefix(12);
    prefix[0] = static_cast<std::uint8_t>(little ? 'l' : 'B');
    put16(prefix, 2, major, little);
    put16(prefix, 4, 0, little);
    put16(prefix, 6, static_cast<std::uint16_t>(auth_name.size()), little);
    put16(prefix, 8, 16, little);
    std::vector<std::uint8_t> authentication;
    for (const char byte : auth_name)
        authentication.push_back(static_cast<std::uint8_t>(byte));
    while ((authentication.size() & 3) != 0)
        authentication.push_back(0);
    const auto cookie = cookie_bytes(valid_cookie);
    authentication.insert(authentication.end(), cookie.begin(), cookie.end());
    if (fragmented) {
        return write_fragmented(descriptor, prefix, 1) &&
            write_fragmented(descriptor, authentication, 7);
    }
    return write_all(descriptor, prefix) && write_all(descriptor, authentication);
}

bool
read_reply(int descriptor, std::vector<std::uint8_t> &reply)
{
    reply.assign(32, 0);
    return read_all(descriptor, reply);
}

bool
check_setup_success(int descriptor, bool little, std::uint32_t &resource_base)
{
    std::vector<std::uint8_t> prefix(8);
    if (!read_all(descriptor, prefix) || prefix[0] != 1 ||
        get16(prefix, 2, little) != 11 || get16(prefix, 4, little) != 0) {
        return false;
    }
    std::vector<std::uint8_t> setup(
        static_cast<std::size_t>(get16(prefix, 6, little)) * 4);
    if (!read_all(descriptor, setup) || setup.size() < 148)
        return false;
    const auto vendor_size = get16(setup, 16, little);
    const std::size_t root_offset = 32 + ((vendor_size + 3) & ~std::size_t{3}) +
        static_cast<std::size_t>(setup[21]) * 8;
    resource_base = get32(setup, 4, little);
    return vendor_size == 9 && setup[20] == 1 && setup[21] == 4 &&
        root_offset + 40 <= setup.size() &&
        get32(setup, root_offset, little) == root_window &&
        get16(setup, root_offset + 20, little) == 320 &&
        get16(setup, root_offset + 22, little) == 240 &&
        setup[root_offset + 38] == 24 && setup[root_offset + 39] == 1;
}

bool
read_variable_reply(int descriptor, bool little,
                    std::vector<std::uint8_t> &reply)
{
    reply.assign(32, 0);
    if (!read_all(descriptor, reply))
        return false;
    const std::size_t extra =
        static_cast<std::size_t>(get32(reply, 4, little)) * 4;
    const std::size_t original_size = reply.size();
    reply.resize(original_size + extra);
    if (extra == 0)
        return true;
    std::vector<std::uint8_t> tail(extra);
    if (!read_all(descriptor, tail))
        return false;
    std::memcpy(reply.data() + original_size, tail.data(), tail.size());
    return true;
}

std::vector<std::uint8_t>
extension_list_payload()
{
    constexpr std::array<std::string_view, 4> extensions{{
        "BIG-REQUESTS",
        "XC-MISC",
        "Generic Event Extension",
        "XTEST",
    }};
    std::vector<std::uint8_t> payload;
    for (const auto name : extensions) {
        payload.push_back(static_cast<std::uint8_t>(name.size()));
        payload.insert(payload.end(), name.begin(), name.end());
    }
    while ((payload.size() & 3U) != 0)
        payload.push_back(0);
    return payload;
}

bool
check_extension_list(const std::vector<std::uint8_t> &reply, bool little,
                     std::uint16_t sequence)
{
    const auto expected = extension_list_payload();
    return reply.size() == 32 + expected.size() && reply[0] == 1 &&
        reply[1] == 4 && get16(reply, 2, little) == sequence &&
        get32(reply, 4, little) == expected.size() / 4 &&
        std::equal(expected.begin(), expected.end(), reply.begin() + 32);
}

bool
query_extension(int descriptor, bool little, std::string_view name,
                std::uint16_t sequence, std::uint8_t expected_opcode,
                std::vector<std::uint8_t> &reply)
{
    std::vector<std::uint8_t> request(
        8 + ((name.size() + 3) & ~std::size_t{3}), 0);
    request[0] = 98; // QueryExtension
    put16(request, 2, static_cast<std::uint16_t>(request.size() / 4), little);
    put16(request, 4, static_cast<std::uint16_t>(name.size()), little);
    std::memcpy(request.data() + 8, name.data(), name.size());
    return write_all(descriptor, request) && read_reply(descriptor, reply) &&
        reply[0] == 1 && get16(reply, 2, little) == sequence &&
        reply[8] == 1 && reply[9] == expected_opcode &&
        reply[10] == 0 && reply[11] == 0;
}

bool
check_foundation_extensions(int descriptor, bool little)
{
    constexpr std::uint8_t big_requests_opcode = 128;
    constexpr std::uint8_t xc_misc_opcode = 129;
    constexpr std::uint8_t generic_event_opcode = 130;
    std::vector<std::uint8_t> request(4, 0);
    std::vector<std::uint8_t> reply;

    request[0] = 127; // extended framing is invalid before Enable
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 16 ||
        get16(reply, 2, little) != 1 || reply[10] != 127) {
        return false;
    }
    if (!query_extension(descriptor, little, "BIG-REQUESTS", 2,
                         big_requests_opcode, reply)) {
        return false;
    }

    request.assign(4, 0);
    request[0] = big_requests_opcode; // Enable
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 3 ||
        get32(reply, 8, little) != 262144U) {
        return false;
    }
    if (!query_extension(descriptor, little, "XC-MISC", 4,
                         xc_misc_opcode, reply)) {
        return false;
    }

    request.assign(8, 0);
    request[0] = xc_misc_opcode; // GetVersion
    put16(request, 2, 2, little);
    put16(request, 4, 1, little);
    put16(request, 6, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 5 ||
        get16(reply, 8, little) != 1 || get16(reply, 10, little) != 1) {
        return false;
    }

    request.assign(4, 0);
    request[0] = xc_misc_opcode; // GetXIDRange: setup range is exhaustive
    request[1] = 1;
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 6 ||
        get32(reply, 8, little) != 0 || get32(reply, 12, little) != 0) {
        return false;
    }

    request.assign(8, 0);
    request[0] = xc_misc_opcode; // GetXIDList: no supplemental pool
    request[1] = 2;
    put16(request, 2, 2, little);
    put32(request, 4, 4, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 32 || reply[0] != 1 ||
        get16(reply, 2, little) != 7 || get32(reply, 8, little) != 0) {
        return false;
    }
    if (!query_extension(descriptor, little, "Generic Event Extension", 8,
                         generic_event_opcode, reply)) {
        return false;
    }

    request.assign(8, 0);
    request[0] = generic_event_opcode; // QueryVersion
    put16(request, 2, 2, little);
    put16(request, 4, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 9 ||
        get16(reply, 8, little) != 1 || get16(reply, 10, little) != 0) {
        return false;
    }

    request.assign(4, 0);
    request[0] = 99; // ListExtensions is the exact implemented manifest
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        !check_extension_list(reply, little, 10)) {
        return false;
    }

    constexpr std::uint32_t oversized_units = 65536;
    request.assign(static_cast<std::size_t>(oversized_units) * 4, 0);
    request[0] = 127; // extended-length NoOperation
    put32(request, 4, oversized_units, little);
    if (!write_fragmented(descriptor, request, 6))
        return false;

    request.assign(4, 0);
    request[0] = 43; // synchronize after the no-reply oversized request
    put16(request, 2, 1, little);
    return write_all(descriptor, request) && read_reply(descriptor, reply) &&
        reply[0] == 1 && get16(reply, 2, little) == 12 &&
        get32(reply, 8, little) == pointer_root;
}

bool
check_core_objects(int descriptor, bool little, std::uint32_t resource_base)
{
    constexpr std::string_view atom_name = "XMIN_NEXT_TEST";
    constexpr std::uint32_t root_visual = 3;
    constexpr std::uint32_t event_mask = 1U << 17;
    const std::uint32_t child = resource_base;
    std::vector<std::uint8_t> request(
        8 + ((atom_name.size() + 3) & ~std::size_t{3}), 0);
    request[0] = 16; // InternAtom
    put16(request, 2, static_cast<std::uint16_t>(request.size() / 4), little);
    put16(request, 4, static_cast<std::uint16_t>(atom_name.size()), little);
    std::memcpy(request.data() + 8, atom_name.data(), atom_name.size());
    std::vector<std::uint8_t> reply;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 9 ||
        get32(reply, 8, little) <= 68) {
        return false;
    }
    const auto atom = get32(reply, 8, little);

    request.assign(8, 0);
    request[0] = 17; // GetAtomName
    put16(request, 2, 2, little);
    put32(request, 4, atom, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply[0] != 1 ||
        get16(reply, 2, little) != 10 ||
        get16(reply, 8, little) != atom_name.size() ||
        reply.size() < 32 + atom_name.size() ||
        std::memcmp(reply.data() + 32, atom_name.data(), atom_name.size()) != 0) {
        return false;
    }

    request.assign(36, 0);
    request[0] = 1;  // CreateWindow
    request[1] = 24; // depth
    put16(request, 2, 9, little);
    put32(request, 4, child, little);
    put32(request, 8, root_window, little);
    put16(request, 12, static_cast<std::uint16_t>(-3), little);
    put16(request, 14, 5, little);
    put16(request, 16, 40, little);
    put16(request, 18, 30, little);
    put16(request, 20, 2, little);
    put16(request, 22, 1, little); // InputOutput
    put32(request, 24, root_visual, little);
    put32(request, 28, 1U << 11, little); // CWEventMask
    put32(request, 32, event_mask, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 8; // MapWindow
    put16(request, 2, 2, little);
    put32(request, 4, child, little);
    if (!write_all(descriptor, request))
        return false;

    request[0] = 15; // QueryTree(root)
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply[0] != 1 ||
        get16(reply, 2, little) != 13 || get32(reply, 8, little) != root_window ||
        get16(reply, 16, little) != 1 || reply.size() != 36 ||
        get32(reply, 32, little) != child) {
        return false;
    }

    request[0] = 3; // GetWindowAttributes
    put32(request, 4, child, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 44 ||
        reply[0] != 1 || get16(reply, 2, little) != 14 ||
        get32(reply, 8, little) != root_visual || reply[26] != 2 ||
        get32(reply, 32, little) != event_mask ||
        get32(reply, 36, little) != event_mask) {
        return false;
    }

    request[0] = 14; // GetGeometry
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 24 ||
        get16(reply, 2, little) != 15 ||
        get16(reply, 12, little) != static_cast<std::uint16_t>(-3) ||
        get16(reply, 14, little) != 5 || get16(reply, 16, little) != 40 ||
        get16(reply, 18, little) != 30 || get16(reply, 20, little) != 2) {
        return false;
    }

    request.assign(28, 0);
    request[0] = 12; // ConfigureWindow
    put16(request, 2, 7, little);
    put32(request, 4, child, little);
    put16(request, 8, 0x000f, little); // x, y, width, height
    put32(request, 12, 5, little);
    put32(request, 16, 6, little);
    put32(request, 20, 12, little);
    put32(request, 24, 9, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 14; // GetGeometry after configure
    put16(request, 2, 2, little);
    put32(request, 4, child, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 17 ||
        get16(reply, 12, little) != 5 || get16(reply, 14, little) != 6 ||
        get16(reply, 16, little) != 12 || get16(reply, 18, little) != 9) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 40; // TranslateCoordinates(child -> root)
    put16(request, 2, 4, little);
    put32(request, 4, child, little);
    put32(request, 8, root_window, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 1 ||
        get16(reply, 2, little) != 18 || get32(reply, 8, little) != child ||
        get16(reply, 12, little) != 7 || get16(reply, 14, little) != 8) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 22; // SetSelectionOwner
    put16(request, 2, 4, little);
    put32(request, 4, child, little);
    put32(request, 8, atom, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 23; // GetSelectionOwner
    put16(request, 2, 2, little);
    put32(request, 4, atom, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 20 ||
        get32(reply, 8, little) != child) {
        return false;
    }

    constexpr std::string_view color_name = "red";
    request.assign(16, 0);
    request[0] = 85; // AllocNamedColor
    put16(request, 2, 4, little);
    put32(request, 4, 2, little); // default colormap
    put16(request, 8, static_cast<std::uint16_t>(color_name.size()), little);
    std::memcpy(request.data() + 12, color_name.data(), color_name.size());
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 21 ||
        get32(reply, 8, little) != 0x00ff0000U ||
        get16(reply, 12, little) != 0xffff ||
        get16(reply, 14, little) != 0 || get16(reply, 16, little) != 0) {
        return false;
    }
    const auto red_pixel = get32(reply, 8, little);

    request.assign(12, 0);
    request[0] = 91; // QueryColors
    put16(request, 2, 3, little);
    put32(request, 4, 2, little);
    put32(request, 8, red_pixel, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 40 ||
        reply[0] != 1 || get16(reply, 2, little) != 22 ||
        get16(reply, 8, little) != 1 || get16(reply, 32, little) != 0xffff ||
        get16(reply, 34, little) != 0 || get16(reply, 36, little) != 0) {
        return false;
    }

    request.assign(44, 0);
    request[0] = 25; // SendEvent
    put16(request, 2, 11, little);
    put32(request, 4, child, little);
    request[12] = 33; // ClientMessage
    request[13] = 32;
    put32(request, 16, child, little);
    put32(request, 20, atom, little);
    put32(request, 24, 0x584d494eU, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != (33 | 0x80U) || reply[1] != 32 ||
        get16(reply, 2, little) != 23 || get32(reply, 4, little) != child ||
        get32(reply, 8, little) != atom ||
        get32(reply, 12, little) != 0x584d494eU) {
        return false;
    }

    request.assign(8, 0);
    request[0] = 4; // DestroyWindow
    put16(request, 2, 2, little);
    put32(request, 4, child, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 15; // QueryTree(destroyed child)
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 3 ||
        get16(reply, 2, little) != 25 || get32(reply, 4, little) != child) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 127; // variable-length NoOperation
    put16(request, 2, 3, little);
    if (!write_all(descriptor, request))
        return false;
    request.assign(4, 0);
    request[0] = 43; // GetInputFocus proves recovery
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 27 ||
        get32(reply, 8, little) != pointer_root) {
        return false;
    }

    request.assign(32, 0);
    request[0] = 18; // ChangeProperty, replace two CARD32 values
    request[1] = 0;
    put16(request, 2, 8, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    put32(request, 12, 19, little); // INTEGER
    request[16] = 32;
    put32(request, 20, 2, little);
    put32(request, 24, 0x11223344U, little);
    put32(request, 28, 0xaabbccddU, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(24, 0);
    request[0] = 20; // GetProperty
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    put32(request, 12, 19, little);
    put32(request, 20, 2, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 40 ||
        reply[0] != 1 || reply[1] != 32 ||
        get16(reply, 2, little) != 29 || get32(reply, 8, little) != 19 ||
        get32(reply, 12, little) != 0 || get32(reply, 16, little) != 2 ||
        get32(reply, 32, little) != 0x11223344U ||
        get32(reply, 36, little) != 0xaabbccddU) {
        return false;
    }

    request.assign(28, 0);
    request[0] = 18; // ChangeProperty, prepend one value
    request[1] = 1;
    put16(request, 2, 7, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    put32(request, 12, 19, little);
    request[16] = 32;
    put32(request, 20, 1, little);
    put32(request, 24, 0x01020304U, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(24, 0);
    request[0] = 20; // partial GetProperty: skip prepended value
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    put32(request, 12, 19, little);
    put32(request, 16, 1, little);
    put32(request, 20, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 36 ||
        get16(reply, 2, little) != 31 || get32(reply, 12, little) != 4 ||
        get32(reply, 16, little) != 1 ||
        get32(reply, 32, little) != 0x11223344U) {
        return false;
    }

    request.assign(8, 0);
    request[0] = 21; // ListProperties
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 36 ||
        get16(reply, 2, little) != 32 || get16(reply, 8, little) != 1 ||
        get32(reply, 32, little) != atom) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 19; // DeleteProperty
    put16(request, 2, 3, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(24, 0);
    request[0] = 20; // deleted property is absent
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    put32(request, 12, 19, little);
    put32(request, 20, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 32 ||
        reply[0] != 1 || reply[1] != 0 ||
        get16(reply, 2, little) != 34 || get32(reply, 8, little) != 0) {
        return false;
    }

    const std::uint32_t pixmap = resource_base + 1;
    const std::uint32_t graphics = resource_base + 2;
    const std::uint32_t copied_graphics = resource_base + 3;
    request.assign(16, 0);
    request[0] = 53; // CreatePixmap
    request[1] = 24;
    put16(request, 2, 4, little);
    put32(request, 4, pixmap, little);
    put32(request, 8, root_window, little);
    put16(request, 12, 4, little);
    put16(request, 14, 4, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 55; // CreateGC
    put16(request, 2, 5, little);
    put32(request, 4, graphics, little);
    put32(request, 8, pixmap, little);
    put32(request, 12, 1U << 2, little); // GCForeground
    put32(request, 16, 0x0000ff00U, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 70; // PolyFillRectangle
    put16(request, 2, 5, little);
    put32(request, 4, pixmap, little);
    put32(request, 8, graphics, little);
    put16(request, 16, 4, little);
    put16(request, 18, 4, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(28, 0);
    request[0] = 72; // PutImage
    request[1] = 2;  // ZPixmap
    put16(request, 2, 7, little);
    put32(request, 4, pixmap, little);
    put32(request, 8, graphics, little);
    put16(request, 12, 1, little);
    put16(request, 14, 1, little);
    put16(request, 16, 1, little);
    put16(request, 18, 1, little);
    request[21] = 24;
    const bool image_little = host_is_little_endian();
    put32(request, 24, 0x000000ffU, image_little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 73; // verify PutImage before copying
    request[1] = 2;
    put16(request, 2, 5, little);
    put32(request, 4, pixmap, little);
    put16(request, 8, 1, little);
    put16(request, 10, 1, little);
    put16(request, 12, 1, little);
    put16(request, 14, 1, little);
    put32(request, 16, 0xffffffffU, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 36 ||
        reply[0] != 1 || reply[1] != 24 ||
        get16(reply, 2, little) != 39 ||
        get32(reply, 32, image_little) != 0x000000ffU) {
        std::cerr << "direct PutImage readback failed\n";
        return false;
    }

    request.assign(28, 0);
    request[0] = 62; // CopyArea
    put16(request, 2, 7, little);
    put32(request, 4, pixmap, little);
    put32(request, 8, root_window, little);
    put32(request, 12, graphics, little);
    put16(request, 24, 4, little);
    put16(request, 26, 4, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 73; // GetImage
    request[1] = 2;  // ZPixmap
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 1, little);
    put16(request, 10, 1, little);
    put16(request, 12, 1, little);
    put16(request, 14, 1, little);
    put32(request, 16, 0xffffffffU, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 36 ||
        reply[0] != 1 || reply[1] != 24 ||
        get16(reply, 2, little) != 41 ||
        get32(reply, 32, image_little) != 0x000000ffU) {
        std::cerr << "copied PutImage readback failed\n";
        return false;
    }

    request.assign(16, 0);
    request[0] = 2; // ChangeWindowAttributes: root background pixel
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    put32(request, 8, 1U << 1, little);
    put32(request, 12, 0x00123456U, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 61; // ClearArea
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    put16(request, 12, 1, little);
    put16(request, 14, 1, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 56; // ChangeGC foreground
    put16(request, 2, 4, little);
    put32(request, 4, graphics, little);
    put32(request, 8, 1U << 2, little);
    put32(request, 12, 0x00abcdefU, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 55; // destination GC for CopyGC
    put16(request, 2, 4, little);
    put32(request, 4, copied_graphics, little);
    put32(request, 8, root_window, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 57; // CopyGC foreground
    put16(request, 2, 4, little);
    put32(request, 4, graphics, little);
    put32(request, 8, copied_graphics, little);
    put32(request, 12, 1U << 2, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 70; // draw with the copied GC
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put32(request, 8, copied_graphics, little);
    put16(request, 12, 1, little);
    put16(request, 16, 1, little);
    put16(request, 18, 1, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 73; // verify ClearArea and changed/copied GC state
    request[1] = 2;
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put16(request, 12, 2, little);
    put16(request, 14, 1, little);
    put32(request, 16, 0xffffffffU, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 40 ||
        reply[0] != 1 || reply[1] != 24 ||
        get16(reply, 2, little) != 48 ||
        get32(reply, 32, image_little) != 0x00123456U ||
        get32(reply, 36, image_little) != 0x00abcdefU) {
        std::cerr << "mutable GC/window state readback failed\n";
        return false;
    }

    request.assign(8, 0);
    request[0] = 60; // Free copied GC
    put16(request, 2, 2, little);
    put32(request, 4, copied_graphics, little);
    if (!write_all(descriptor, request))
        return false;
    put32(request, 4, graphics, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 54; // FreePixmap
    put32(request, 4, pixmap, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 53; // 1-bit pixmap exercises bitmap order and row padding
    request[1] = 1;
    put16(request, 2, 4, little);
    put32(request, 4, pixmap, little);
    put32(request, 8, root_window, little);
    put16(request, 12, 9, little);
    put16(request, 14, 1, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 55;
    put16(request, 2, 4, little);
    put32(request, 4, graphics, little);
    put32(request, 8, pixmap, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(28, 0);
    request[0] = 72;
    request[1] = 2;
    put16(request, 2, 7, little);
    put32(request, 4, pixmap, little);
    put32(request, 8, graphics, little);
    put16(request, 12, 9, little);
    put16(request, 14, 1, little);
    request[21] = 1;
    const std::uint8_t bitmap_edge = image_little ? 0x01U : 0x80U;
    request[24] = bitmap_edge;
    request[25] = bitmap_edge;
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 73;
    request[1] = 2;
    put16(request, 2, 5, little);
    put32(request, 4, pixmap, little);
    put16(request, 12, 9, little);
    put16(request, 14, 1, little);
    put32(request, 16, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 36 ||
        reply[0] != 1 || reply[1] != 1 ||
        get16(reply, 2, little) != 55 || reply[32] != bitmap_edge ||
        reply[33] != bitmap_edge || reply[34] != 0 || reply[35] != 0) {
        return false;
    }

    request.assign(24, 0);
    request[0] = 55; // 24-bit GC maps CopyPlane bits to colors
    put16(request, 2, 6, little);
    put32(request, 4, copied_graphics, little);
    put32(request, 8, root_window, little);
    put32(request, 12, (1U << 2) | (1U << 3), little);
    put32(request, 16, 0x00ffff00U, little);
    put32(request, 20, 0x0000ffffU, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(32, 0);
    request[0] = 63; // CopyPlane from depth 1 to depth 24
    put16(request, 2, 8, little);
    put32(request, 4, pixmap, little);
    put32(request, 8, root_window, little);
    put32(request, 12, copied_graphics, little);
    put16(request, 22, 12, little);
    put16(request, 24, 9, little);
    put16(request, 26, 1, little);
    put32(request, 28, 1, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 73;
    request[1] = 2;
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put16(request, 10, 12, little);
    put16(request, 12, 9, little);
    put16(request, 14, 1, little);
    put32(request, 16, 0xffffffffU, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 68 ||
        reply[0] != 1 || reply[1] != 24 ||
        get16(reply, 2, little) != 58 ||
        get32(reply, 32, image_little) != 0x00ffff00U ||
        get32(reply, 36, image_little) != 0x0000ffffU ||
        get32(reply, 64, image_little) != 0x00ffff00U) {
        return false;
    }

    request.assign(8, 0);
    request[0] = 60;
    put16(request, 2, 2, little);
    put32(request, 4, copied_graphics, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 60;
    put16(request, 2, 2, little);
    put32(request, 4, graphics, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 54;
    put32(request, 4, pixmap, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(32, 0);
    request[0] = 1;  // reusable child for tree-wide operations
    request[1] = 24;
    put16(request, 2, 8, little);
    put32(request, 4, child, little);
    put32(request, 8, root_window, little);
    put16(request, 16, 4, little);
    put16(request, 18, 4, little);
    put16(request, 22, 1, little);
    put32(request, 24, root_visual, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 7; // ReparentWindow (same parent also restacks)
    put16(request, 2, 4, little);
    put32(request, 4, child, little);
    put32(request, 8, root_window, little);
    put16(request, 12, 3, little);
    put16(request, 14, 4, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 9; // MapSubwindows
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 3; // mapped child is viewable
    put32(request, 4, child, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 44 ||
        reply[0] != 1 || get16(reply, 2, little) != 65 || reply[26] != 2) {
        return false;
    }

    request[0] = 11; // UnmapSubwindows
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 3; // child is now unmapped
    put32(request, 4, child, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 44 ||
        reply[0] != 1 || get16(reply, 2, little) != 67 || reply[26] != 0) {
        return false;
    }

    request[0] = 5; // DestroySubwindows(root)
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 15; // destroyed child is no longer a window
    put32(request, 4, child, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 3 ||
        get16(reply, 2, little) != 69 || get32(reply, 4, little) != child) {
        return false;
    }

    request.assign(20, 0);
    request[0] = 55; // GC for thin solid primitive coverage
    put16(request, 2, 5, little);
    put32(request, 4, graphics, little);
    put32(request, 8, root_window, little);
    put32(request, 12, 1U << 2, little);
    put32(request, 16, 0x00fedcbaU, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 64; // PolyPoint
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    put32(request, 8, graphics, little);
    put16(request, 14, 5, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 65; // PolyLine with relative second coordinate
    request[1] = 1;
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put32(request, 8, graphics, little);
    put16(request, 12, 1, little);
    put16(request, 14, 5, little);
    put16(request, 16, 2, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 66; // PolySegment
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put32(request, 8, graphics, little);
    put16(request, 12, 4, little);
    put16(request, 14, 5, little);
    put16(request, 16, 4, little);
    put16(request, 18, 7, little);
    if (!write_all(descriptor, request))
        return false;

    request[0] = 67; // PolyRectangle
    put16(request, 12, 6, little);
    put16(request, 16, 2, little);
    put16(request, 18, 2, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 73;
    request[1] = 2;
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put16(request, 10, 5, little);
    put16(request, 12, 9, little);
    put16(request, 14, 3, little);
    put32(request, 16, 0xffffffffU, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 140 || reply[0] != 1 || reply[1] != 24 ||
        get16(reply, 2, little) != 75) {
        return false;
    }
    {
        constexpr std::uint32_t stroke = 0x00fedcbaU;
        const auto pixel_at = [&reply, image_little](std::size_t x,
                                                     std::size_t y) {
            return get32(reply, 32 + (y * 9 + x) * 4, image_little);
        };
        if (pixel_at(0, 0) != stroke || pixel_at(1, 0) != stroke ||
            pixel_at(2, 0) != stroke || pixel_at(3, 0) != stroke ||
            pixel_at(4, 0) != stroke || pixel_at(4, 1) != stroke ||
            pixel_at(4, 2) != stroke || pixel_at(6, 0) != stroke ||
            pixel_at(7, 0) != stroke || pixel_at(8, 0) != stroke ||
            pixel_at(6, 1) != stroke || pixel_at(8, 1) != stroke ||
            pixel_at(6, 2) != stroke || pixel_at(7, 2) != stroke ||
            pixel_at(8, 2) != stroke || pixel_at(7, 1) != 0) {
            return false;
        }
    }

    request.assign(8, 0);
    request[0] = 60;
    put16(request, 2, 2, little);
    put32(request, 4, graphics, little);
    if (!write_all(descriptor, request))
        return false;

    const std::uint32_t colormap = resource_base + 4;
    const std::uint32_t copied_colormap = resource_base + 5;
    request.assign(16, 0);
    request[0] = 78; // CreateColormap
    put16(request, 2, 4, little);
    put32(request, 4, colormap, little);
    put32(request, 8, root_window, little);
    put32(request, 12, root_visual, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 84; // AllocColor with 8-bit TrueColor quantization
    put16(request, 2, 4, little);
    put32(request, 4, colormap, little);
    put16(request, 8, 0x1234U, little);
    put16(request, 10, 0x5678U, little);
    put16(request, 12, 0x9abcU, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 78 ||
        get16(reply, 8, little) != 0x1212U ||
        get16(reply, 10, little) != 0x5656U ||
        get16(reply, 12, little) != 0x9a9aU ||
        get32(reply, 16, little) != 0x0012569aU) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 92; // LookupColor
    put16(request, 2, 4, little);
    put32(request, 4, colormap, little);
    put16(request, 8, 3, little);
    std::memcpy(request.data() + 12, "red", 3);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 79 ||
        get16(reply, 8, little) != 0xffffU ||
        get16(reply, 14, little) != 0xffffU) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 80; // CopyColormapAndFree
    put16(request, 2, 3, little);
    put32(request, 4, copied_colormap, little);
    put32(request, 8, colormap, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 81; // InstallColormap
    put16(request, 2, 2, little);
    put32(request, 4, copied_colormap, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 83; // ListInstalledColormaps
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 36 ||
        reply[0] != 1 || get16(reply, 2, little) != 82 ||
        get16(reply, 8, little) != 1 ||
        get32(reply, 32, little) != copied_colormap) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 91; // QueryColors on the copied colormap
    put16(request, 2, 3, little);
    put32(request, 4, copied_colormap, little);
    put32(request, 8, 0x0012569aU, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 40 ||
        reply[0] != 1 || get16(reply, 2, little) != 83 ||
        get16(reply, 8, little) != 1 ||
        get16(reply, 32, little) != 0x1212U ||
        get16(reply, 34, little) != 0x5656U ||
        get16(reply, 36, little) != 0x9a9aU) {
        return false;
    }

    request.assign(20, 0);
    request[0] = 89; // TrueColor entries are read-only
    put16(request, 2, 5, little);
    put32(request, 4, copied_colormap, little);
    put32(request, 8, 0x0012569aU, little);
    request[18] = 7;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 10 ||
        get16(reply, 2, little) != 84) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 88; // FreeColors is a no-op for fixed TrueColor
    put16(request, 2, 4, little);
    put32(request, 4, copied_colormap, little);
    put32(request, 12, 0x0012569aU, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 82; // UninstallColormap
    put16(request, 2, 2, little);
    put32(request, 4, copied_colormap, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 79;
    if (!write_all(descriptor, request))
        return false;
    put32(request, 4, colormap, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(28, 0);
    request[0] = 18; // first property for RotateProperties
    put16(request, 2, 7, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    put32(request, 12, 19, little); // INTEGER
    request[16] = 32;
    put32(request, 20, 1, little);
    put32(request, 24, 0x11111111U, little);
    if (!write_all(descriptor, request))
        return false;

    put32(request, 8, 20, little); // predefined atom used as second property
    put32(request, 24, 0x22222222U, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 114; // RotateProperties
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 2, little);
    put16(request, 10, 1, little);
    put32(request, 12, atom, little);
    put32(request, 16, 20, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(24, 0);
    request[0] = 20; // first property now has the second value
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put32(request, 8, atom, little);
    put32(request, 12, 19, little);
    put32(request, 20, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 36 ||
        reply[0] != 1 || reply[1] != 32 ||
        get16(reply, 2, little) != 92 || get32(reply, 16, little) != 1 ||
        get32(reply, 32, little) != 0x22222222U) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 97; // QueryBestSize rounds a short tile width
    request[1] = 1;
    put16(request, 2, 3, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 13, little);
    put16(request, 10, 7, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 93 ||
        get16(reply, 8, little) != 16 || get16(reply, 10, little) != 7) {
        return false;
    }

    const std::uint32_t clip_graphics = resource_base + 6;
    request.assign(20, 0);
    request[0] = 55; // CreateGC for clipped drawing
    put16(request, 2, 5, little);
    put32(request, 4, clip_graphics, little);
    put32(request, 8, root_window, little);
    put32(request, 12, 1U << 2, little); // GCForeground
    put32(request, 16, 0x00abcdefU, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(28, 0);
    request[0] = 59; // SetClipRectangles with an overlapping union
    put16(request, 2, 7, little);
    put32(request, 4, clip_graphics, little);
    put16(request, 8, 1, little); // clip x origin
    put16(request, 12, 0, little);
    put16(request, 14, 20, little);
    put16(request, 16, 3, little);
    put16(request, 18, 1, little);
    put16(request, 20, 2, little);
    put16(request, 22, 20, little);
    put16(request, 24, 3, little);
    put16(request, 26, 1, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 70; // PolyFillRectangle through the clip union
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put32(request, 8, clip_graphics, little);
    put16(request, 14, 20, little);
    put16(request, 16, 8, little);
    put16(request, 18, 1, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(20, 0);
    request[0] = 73; // GetImage verifies the canonical clip
    request[1] = 2;
    put16(request, 2, 5, little);
    put32(request, 4, root_window, little);
    put16(request, 10, 20, little);
    put16(request, 12, 8, little);
    put16(request, 14, 1, little);
    put32(request, 16, 0xffffffffU, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) || reply.size() != 64 ||
        reply[0] != 1 || get16(reply, 2, little) != 97) {
        return false;
    }
    for (std::size_t x = 0; x < 8; ++x) {
        const std::uint32_t expected = x >= 1 && x < 6
            ? 0x00abcdefU
            : 0;
        if (get32(reply, 32 + x * 4, image_little) != expected)
            return false;
    }

    request.assign(8, 0);
    request[0] = 60; // FreeGC
    put16(request, 2, 2, little);
    put32(request, 4, clip_graphics, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 38; // QueryPointer starts at screen center
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 1 ||
        get16(reply, 2, little) != 99 ||
        get32(reply, 8, little) != root_window ||
        get32(reply, 12, little) != 0 ||
        get16(reply, 16, little) != 160 ||
        get16(reply, 18, little) != 120 ||
        get16(reply, 20, little) != 160 ||
        get16(reply, 22, little) != 120 ||
        get16(reply, 24, little) != 0) {
        return false;
    }

    request.assign(24, 0);
    request[0] = 41; // WarpPointer to an absolute root position
    put16(request, 2, 6, little);
    put32(request, 8, root_window, little);
    put16(request, 20, 7, little);
    put16(request, 22, 9, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 38; // QueryPointer observes the warp
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 101 ||
        get16(reply, 16, little) != 7 || get16(reply, 18, little) != 9 ||
        get16(reply, 20, little) != 7 || get16(reply, 22, little) != 9) {
        return false;
    }

    request.assign(24, 0);
    request[0] = 41; // source rectangle excludes the current pointer
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put32(request, 8, root_window, little);
    put16(request, 12, 100, little);
    put16(request, 14, 100, little);
    put16(request, 16, 1, little);
    put16(request, 18, 1, little);
    put16(request, 20, 20, little);
    put16(request, 22, 20, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 38; // excluded warp was a no-op
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 103 ||
        get16(reply, 16, little) != 7 || get16(reply, 18, little) != 9) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 39; // no retained motion history
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 32 || reply[0] != 1 ||
        get16(reply, 2, little) != 104 || get32(reply, 8, little) != 0) {
        return false;
    }

    request.assign(4, 0);
    request[0] = 44; // QueryKeymap starts clear
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 40 || reply[0] != 1 ||
        get16(reply, 2, little) != 105 || get32(reply, 4, little) != 2) {
        return false;
    }
    for (std::size_t index = 8; index < reply.size(); ++index) {
        if (reply[index] != 0)
            return false;
    }

    request.assign(24, 0);
    request[0] = 26; // GrabPointer
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 1U << 2, little); // ButtonPressMask
    request[10] = 1; // GrabModeAsync
    request[11] = 1;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 0 ||
        get16(reply, 2, little) != 106) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 30; // ChangeActivePointerGrab
    put16(request, 2, 4, little);
    put16(request, 12, 1U << 3, little); // ButtonReleaseMask
    if (!write_all(descriptor, request))
        return false;

    request.assign(8, 0);
    request[0] = 35; // AllowEvents(AsyncPointer)
    put16(request, 2, 2, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 27; // UngrabPointer
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 31; // GrabKeyboard
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    request[12] = 1;
    request[13] = 1;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 0 ||
        get16(reply, 2, little) != 110) {
        return false;
    }

    request.assign(8, 0);
    request[0] = 32; // UngrabKeyboard
    put16(request, 2, 2, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 33; // GrabKey
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 1, little); // ShiftMask
    request[10] = 38;
    request[11] = 1;
    request[12] = 1;
    if (!write_all(descriptor, request))
        return false;

    request.assign(12, 0);
    request[0] = 34; // UngrabKey
    request[1] = 38;
    put16(request, 2, 3, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 1, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(24, 0);
    request[0] = 28; // GrabButton
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 1U << 2, little); // ButtonPressMask
    request[10] = 1;
    request[11] = 1;
    request[20] = 1;
    put16(request, 22, 4, little); // ControlMask
    if (!write_all(descriptor, request))
        return false;

    request.assign(12, 0);
    request[0] = 29; // UngrabButton
    request[1] = 1;
    put16(request, 2, 3, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 4, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(16, 0);
    request[0] = 33; // invalid modifier mask is endian-aware
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    put16(request, 8, 0x0100, little);
    request[10] = 38;
    request[11] = 1;
    request[12] = 1;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 2 ||
        get16(reply, 2, little) != 116 ||
        get32(reply, 4, little) != 0x0100) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 42; // focus the actual root, not PointerRoot
    request[1] = 2;
    put16(request, 2, 3, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(4, 0);
    request[0] = 43;
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 2 ||
        get16(reply, 2, little) != 118 ||
        get32(reply, 8, little) != root_window) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 42; // restore PointerRoot focus
    put16(request, 2, 3, little);
    put32(request, 4, pointer_root, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(4, 0);
    request[0] = 43; // synchronize teardown requests
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 0 ||
        get16(reply, 2, little) != 120 ||
        get32(reply, 8, little) != pointer_root) {
        return false;
    }

    request.assign(8, 0);
    request[0] = 101; // GetKeyboardMapping(F12)
    put16(request, 2, 2, little);
    request[4] = 96;
    request[5] = 1;
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 60 || reply[0] != 1 || reply[1] != 7 ||
        get16(reply, 2, little) != 121 || get32(reply, 4, little) != 7 ||
        get32(reply, 32, little) != 0x0000ffc9U) {
        return false;
    }

    request.assign(4, 0);
    request[0] = 103; // GetKeyboardControl
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 52 || reply[0] != 1 || reply[1] != 1 ||
        get16(reply, 2, little) != 122 || get32(reply, 8, little) != 0 ||
        reply[12] != 0 || reply[13] != 50 ||
        get16(reply, 14, little) != 400 ||
        get16(reply, 16, little) != 100) {
        return false;
    }

    request[0] = 106; // GetPointerControl
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 123 ||
        get16(reply, 8, little) != 2 ||
        get16(reply, 10, little) != 1 ||
        get16(reply, 12, little) != 4) {
        return false;
    }

    request[0] = 117; // GetPointerMapping
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 44 || reply[0] != 1 || reply[1] != 10 ||
        get16(reply, 2, little) != 124 || reply[32] != 1 ||
        reply[41] != 10) {
        return false;
    }

    request[0] = 119; // GetModifierMapping
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 64 || reply[0] != 1 || reply[1] != 4 ||
        get16(reply, 2, little) != 125 || reply[32] != 50 ||
        reply[33] != 62 || reply[63] != 0) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 105; // ChangePointerControl
    put16(request, 2, 3, little);
    put16(request, 4, 3, little);
    put16(request, 6, 2, little);
    put16(request, 8, 7, little);
    request[10] = 1;
    request[11] = 1;
    if (!write_all(descriptor, request))
        return false;

    request.assign(4, 0);
    request[0] = 106; // GetPointerControl observes mutation
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 127 ||
        get16(reply, 8, little) != 3 ||
        get16(reply, 10, little) != 2 ||
        get16(reply, 12, little) != 7) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 102; // ChangeKeyboardControl(BellPercent)
    put16(request, 2, 3, little);
    put32(request, 4, 1U << 1, little);
    put32(request, 8, 17, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(4, 0);
    request[0] = 103; // GetKeyboardControl observes mutation
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 52 || get16(reply, 2, little) != 129 ||
        reply[13] != 17) {
        return false;
    }

    request[0] = 104; // Bell rejects an out-of-range signed percentage
    request[1] = 101;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 2 ||
        get16(reply, 2, little) != 130 ||
        get32(reply, 4, little) != 101) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 105; // restore default pointer controls
    put16(request, 2, 3, little);
    put16(request, 4, 0xffff, little);
    put16(request, 6, 0xffff, little);
    put16(request, 8, 0xffff, little);
    request[10] = 1;
    request[11] = 1;
    if (!write_all(descriptor, request))
        return false;

    request.assign(12, 0);
    request[0] = 102; // restore default bell percentage
    put16(request, 2, 3, little);
    put32(request, 4, 1U << 1, little);
    put32(request, 8, 0xffffffffU, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(4, 0);
    request[0] = 103; // synchronize restored input state
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 52 || get16(reply, 2, little) != 133 ||
        reply[13] != 50) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 100; // ChangeKeyboardMapping
    request[1] = 1;
    put16(request, 2, 4, little);
    request[4] = 96;
    request[5] = 2;
    put32(request, 8, 0x00000078U, little);
    put32(request, 12, 0x00000058U, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 34 || get16(reply, 2, little) != 134 ||
        reply[4] != 1 || reply[5] != 96 || reply[6] != 1) {
        return false;
    }

    request.assign(8, 0);
    request[0] = 101; // GetKeyboardMapping observes replicated core row
    put16(request, 2, 2, little);
    request[4] = 96;
    request[5] = 1;
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 60 || reply[1] != 7 ||
        get16(reply, 2, little) != 135 ||
        get32(reply, 32, little) != 0x00000078U ||
        get32(reply, 36, little) != 0x00000058U ||
        get32(reply, 40, little) != 0x00000078U ||
        get32(reply, 44, little) != 0x00000058U) {
        return false;
    }

    request.assign(36, 0);
    request[0] = 100; // restore generated F12 mapping
    request[1] = 1;
    put16(request, 2, 9, little);
    request[4] = 96;
    request[5] = 7;
    put32(request, 8, 0x0000ffc9U, little);
    put32(request, 16, 0x0000ffc9U, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 34 || get16(reply, 2, little) != 136 ||
        reply[4] != 1 || reply[5] != 96 || reply[6] != 1) {
        return false;
    }

    request.assign(8, 0);
    request[0] = 101; // synchronize restored keymap
    put16(request, 2, 2, little);
    request[4] = 96;
    request[5] = 1;
    return write_all(descriptor, request) &&
        read_variable_reply(descriptor, little, reply) &&
        reply.size() == 60 && reply[1] == 7 &&
        get16(reply, 2, little) == 137 &&
        get32(reply, 32, little) == 0x0000ffc9U &&
        get32(reply, 40, little) == 0x0000ffc9U;
}

bool
check_request_sequence(int descriptor, bool little, bool fragmented)
{
    std::vector<std::uint8_t> request(8);
    request[0] = 14; // GetGeometry
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!(fragmented ? write_fragmented(descriptor, request, 4)
                     : write_all(descriptor, request)))
        return false;
    std::vector<std::uint8_t> reply;
    if (!read_reply(descriptor, reply) || reply[0] != 1 || reply[1] != 24 ||
        get16(reply, 2, little) != 1 || get32(reply, 8, little) != root_window ||
        get16(reply, 16, little) != 320 || get16(reply, 18, little) != 240) {
        return false;
    }

    constexpr std::string_view extension = "RENDER";
    request.assign(8 + ((extension.size() + 3) & ~std::size_t{3}), 0);
    request[0] = 98;
    put16(request, 2, static_cast<std::uint16_t>(request.size() / 4), little);
    put16(request, 4, static_cast<std::uint16_t>(extension.size()), little);
    std::memcpy(request.data() + 8, extension.data(), extension.size());
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 2 || reply[8] != 0) {
        return false;
    }

    request.assign(4, 0);
    request[0] = 99; // ListExtensions
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        !check_extension_list(reply, little, 3)) {
        return false;
    }

    request[0] = 14; // deliberately too short GetGeometry
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 16 || get16(reply, 2, little) != 4 ||
        reply[10] != 14) {
        return false;
    }

    request[0] = 43; // GetInputFocus proves recovery after BadLength
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 5 ||
        get32(reply, 8, little) != pointer_root) {
        return false;
    }

    request[0] = 120; // reserved core slot
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 1 || get16(reply, 2, little) != 6 ||
        reply[10] != 120) {
        return false;
    }

    request[0] = 255; // unadvertised extension major opcode
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 1 || get16(reply, 2, little) != 7 ||
        reply[10] != 255) {
        return false;
    }

    request[0] = 127; // NoOperation
    return write_all(descriptor, request);
}

bool
check_xtest(int descriptor, bool little, std::uint32_t resource_base)
{
    constexpr std::string_view extension = "XTEST";
    constexpr std::uint8_t xtest_opcode = 131;
    constexpr std::uint32_t crossing_motion_mask =
        (1U << 4) | (1U << 5) | (1U << 6);
    const std::uint32_t child = resource_base;
    std::vector<std::uint8_t> request(
        8 + ((extension.size() + 3) & ~std::size_t{3}), 0);
    request[0] = 98; // QueryExtension
    put16(request, 2, static_cast<std::uint16_t>(request.size() / 4), little);
    put16(request, 4, static_cast<std::uint16_t>(extension.size()), little);
    std::memcpy(request.data() + 8, extension.data(), extension.size());
    std::vector<std::uint8_t> reply;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 1 ||
        reply[8] != 1 || reply[9] != xtest_opcode) {
        return false;
    }

    request.assign(8, 0);
    request[0] = xtest_opcode; // XTEST GetVersion
    put16(request, 2, 2, little);
    request[4] = 2;
    put16(request, 6, 2, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 2 ||
        get16(reply, 2, little) != 2 || get16(reply, 8, little) != 2) {
        return false;
    }

    request.assign(16, 0);
    request[0] = 2; // ChangeWindowAttributes
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    put32(request, 8, 1U << 11, little); // CWEventMask
    put32(request, 12, crossing_motion_mask, little);
    if (!write_all(descriptor, request))
        return false;

    request.assign(36, 0);
    request[0] = 1;  // CreateWindow
    request[1] = 24; // depth
    put16(request, 2, 9, little);
    put32(request, 4, child, little);
    put32(request, 8, root_window, little);
    put16(request, 12, 10, little);
    put16(request, 14, 10, little);
    put16(request, 16, 30, little);
    put16(request, 18, 30, little);
    put16(request, 22, 1, little); // InputOutput
    put32(request, 24, 3, little); // root visual
    put32(request, 28, 1U << 11, little); // CWEventMask
    put32(request, 32, crossing_motion_mask, little);
    if (!write_all(descriptor, request))
        return false;
    request.assign(8, 0);
    request[0] = 8; // MapWindow
    put16(request, 2, 2, little);
    put32(request, 4, child, little);
    if (!write_all(descriptor, request))
        return false;

    const auto fake_input = [little](std::uint8_t type, std::uint8_t detail,
                                     std::int16_t x = 0,
                                     std::int16_t y = 0) {
        std::vector<std::uint8_t> fake(36, 0);
        fake[0] = xtest_opcode;
        fake[1] = 2;
        put16(fake, 2, 9, little);
        fake[4] = type;
        fake[5] = detail;
        put16(fake, 24, static_cast<std::uint16_t>(x), little);
        put16(fake, 26, static_cast<std::uint16_t>(y), little);
        return fake;
    };

    request = fake_input(2, 96); // KeyPress
    if (!write_all(descriptor, request))
        return false;
    request.assign(4, 0);
    request[0] = 44; // QueryKeymap
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 40 || get16(reply, 2, little) != 7 ||
        reply[20] != 1) {
        return false;
    }

    request = fake_input(99, 0); // invalid event type
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 2 ||
        get16(reply, 2, little) != 8 || get16(reply, 8, little) != 2 ||
        reply[10] != xtest_opcode) {
        return false;
    }

    request = fake_input(3, 96); // KeyRelease
    if (!write_all(descriptor, request))
        return false;
    request.assign(4, 0);
    request[0] = 44;
    put16(request, 2, 1, little);
    if (!write_all(descriptor, request) ||
        !read_variable_reply(descriptor, little, reply) ||
        reply.size() != 40 || get16(reply, 2, little) != 10 ||
        reply[20] != 0) {
        return false;
    }

    request = fake_input(6, 0, 17, 19); // absolute MotionNotify
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 8 || reply[1] != 2 ||
        get16(reply, 2, little) != 11 ||
        get32(reply, 8, little) != root_window ||
        get32(reply, 12, little) != root_window ||
        get32(reply, 16, little) != 0 ||
        get16(reply, 20, little) != 17 ||
        get16(reply, 22, little) != 19 ||
        get16(reply, 24, little) != 17 ||
        get16(reply, 26, little) != 19 ||
        get16(reply, 28, little) != 0 || reply[30] != 0 ||
        reply[31] != 3 ||
        !read_reply(descriptor, reply) || reply[0] != 7 || reply[1] != 0 ||
        get16(reply, 2, little) != 11 ||
        get32(reply, 12, little) != child ||
        get16(reply, 24, little) != 7 ||
        get16(reply, 26, little) != 9 || reply[31] != 3 ||
        !read_reply(descriptor, reply) || reply[0] != 6 || reply[1] != 0 ||
        get16(reply, 2, little) != 11 ||
        get32(reply, 12, little) != child ||
        get16(reply, 20, little) != 17 ||
        get16(reply, 22, little) != 19 ||
        get16(reply, 24, little) != 7 ||
        get16(reply, 26, little) != 9 ||
        get16(reply, 28, little) != 0 || reply[30] != 1) {
        return false;
    }
    request.assign(8, 0);
    request[0] = 38; // QueryPointer
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        get16(reply, 2, little) != 12 || get16(reply, 16, little) != 17 ||
        get16(reply, 18, little) != 19) {
        return false;
    }

    request = fake_input(4, 1); // ButtonPress
    if (!write_all(descriptor, request))
        return false;
    request.assign(8, 0);
    request[0] = 38;
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        get16(reply, 2, little) != 14 ||
        get16(reply, 24, little) != 0x0100) {
        return false;
    }

    request = fake_input(5, 1); // ButtonRelease
    if (!write_all(descriptor, request))
        return false;
    request = fake_input(6, 0, 160, 120); // restore pointer center
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 8 || reply[1] != 0 ||
        get16(reply, 2, little) != 16 ||
        get32(reply, 12, little) != child ||
        get16(reply, 24, little) != 150 ||
        get16(reply, 26, little) != 110 || reply[31] != 3 ||
        !read_reply(descriptor, reply) || reply[0] != 7 ||
        reply[1] != 2 || get16(reply, 2, little) != 16 ||
        get32(reply, 12, little) != root_window ||
        get16(reply, 24, little) != 160 ||
        get16(reply, 26, little) != 120 || reply[31] != 3 ||
        !read_reply(descriptor, reply) || reply[0] != 6 ||
        get16(reply, 2, little) != 16 ||
        get32(reply, 12, little) != root_window ||
        get16(reply, 20, little) != 160 ||
        get16(reply, 22, little) != 120) {
        return false;
    }
    request.assign(8, 0);
    request[0] = 38;
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        get16(reply, 2, little) != 17 ||
        get16(reply, 16, little) != 160 ||
        get16(reply, 18, little) != 120 ||
        get16(reply, 24, little) != 0) {
        return false;
    }

    request.assign(8, 0);
    request[0] = xtest_opcode; // XTEST GrabControl rejects non-boolean values
    request[1] = 3;
    put16(request, 2, 2, little);
    request[4] = 2;
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 2 ||
        get16(reply, 2, little) != 18 || get16(reply, 8, little) != 3) {
        return false;
    }
    request[4] = 1;
    if (!write_all(descriptor, request))
        return false;

    request.assign(12, 0);
    request[0] = xtest_opcode; // XTEST CompareCursor(Current)
    request[1] = 1;
    put16(request, 2, 3, little);
    put32(request, 4, root_window, little);
    put32(request, 8, 1, little);
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || reply[1] != 1 ||
        get16(reply, 2, little) != 20) {
        return false;
    }

    request.assign(12, 0);
    request[0] = 42; // SetInputFocus(root), before focus event selection
    put16(request, 2, 3, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
        return false;
    const std::uint32_t focus_event_mask =
        crossing_motion_mask | (1U << 21);
    request.assign(16, 0);
    request[0] = 2; // ChangeWindowAttributes(root)
    put16(request, 2, 4, little);
    put32(request, 4, root_window, little);
    put32(request, 8, 1U << 11, little);
    put32(request, 12, focus_event_mask, little);
    if (!write_all(descriptor, request))
        return false;
    put32(request, 4, child, little); // ChangeWindowAttributes(child)
    if (!write_all(descriptor, request))
        return false;
    request.assign(12, 0);
    request[0] = 42; // SetInputFocus(child)
    put16(request, 2, 3, little);
    put32(request, 4, child, little);
    return write_all(descriptor, request) && read_reply(descriptor, reply) &&
        reply[0] == 10 && reply[1] == 2 &&
        get16(reply, 2, little) == 24 &&
        get32(reply, 4, little) == root_window && reply[8] == 0 &&
        read_reply(descriptor, reply) && reply[0] == 9 && reply[1] == 0 &&
        get16(reply, 2, little) == 24 &&
        get32(reply, 4, little) == child && reply[8] == 0;
}

bool
run_foundation_case(const char *server, bool little)
{
    Child child = spawn_server(server);
    if (child.process < 0 || child.socket < 0)
        return false;
    std::uint32_t resource_base = 0;
    const bool passed = send_setup(child.socket, little, true, 11) &&
        check_setup_success(child.socket, little, resource_base) &&
        check_foundation_extensions(child.socket, little);
    static_cast<void>(::shutdown(child.socket, SHUT_WR));
    ::close(child.socket);
    const bool exited = wait_for_success(child.process);
    return passed && exited;
}

bool
run_xtest_case(const char *server, bool little)
{
    Child child = spawn_server(server);
    if (child.process < 0 || child.socket < 0)
        return false;
    std::uint32_t resource_base = 0;
    const bool passed = send_setup(child.socket, little, true, 11) &&
        check_setup_success(child.socket, little, resource_base) &&
        check_xtest(child.socket, little, resource_base);
    static_cast<void>(::shutdown(child.socket, SHUT_WR));
    ::close(child.socket);
    const bool exited = wait_for_success(child.process);
    return passed && exited;
}

bool
run_success_case(const char *server, bool little, bool fragmented)
{
    Child child = spawn_server(server);
    if (child.process < 0 || child.socket < 0)
        return false;
    std::uint32_t resource_base = 0;
    const bool passed = send_setup(child.socket, little, true, 11, fragmented) &&
        check_setup_success(child.socket, little, resource_base) &&
        check_request_sequence(child.socket, little, fragmented) &&
        check_core_objects(child.socket, little, resource_base);
    static_cast<void>(::shutdown(child.socket, SHUT_WR));
    ::close(child.socket);
    const bool exited = wait_for_success(child.process);
    return passed && exited;
}

bool
run_abandoned_setup_case(const char *server, bool little)
{
    Child child = spawn_server(server);
    if (child.process < 0 || child.socket < 0)
        return false;
    const bool sent = send_setup(child.socket, little, true);
    static_cast<void>(::shutdown(child.socket, SHUT_RDWR));
    ::close(child.socket);
    return sent && wait_for_success(child.process);
}

bool
run_rejected_case(const char *server, bool wrong_version)
{
    Child child = spawn_server(server);
    if (child.process < 0 || child.socket < 0)
        return false;
    const bool little = host_is_little_endian();
    bool passed = send_setup(
        child.socket, little, !wrong_version ? false : true,
        wrong_version ? 10 : 11);
    std::vector<std::uint8_t> prefix(8);
    passed = passed && read_all(child.socket, prefix) && prefix[0] == 0 &&
        prefix[1] != 0 && get16(prefix, 2, little) == 11;
    if (passed) {
        std::vector<std::uint8_t> reason(
            static_cast<std::size_t>(get16(prefix, 6, little)) * 4);
        passed = read_all(child.socket, reason);
    }
    ::close(child.socket);
    const bool exited = wait_for_success(child.process);
    return passed && exited;
}

} // namespace

int
main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: next_handshake /path/to/Xmin-next\n";
        return 2;
    }
    const bool native = host_is_little_endian();
    if (!run_success_case(argv[1], native, true)) {
        std::cerr << "native-order setup/request case failed\n";
        return 1;
    }
    if (!run_success_case(argv[1], !native, false)) {
        std::cerr << "opposite-order setup/request case failed\n";
        return 1;
    }
    if (!run_foundation_case(argv[1], native) ||
        !run_foundation_case(argv[1], !native)) {
        std::cerr << "foundation extension case failed\n";
        return 1;
    }
    if (!run_xtest_case(argv[1], native) ||
        !run_xtest_case(argv[1], !native)) {
        std::cerr << "XTEST state injection case failed\n";
        return 1;
    }
    if (!run_abandoned_setup_case(argv[1], native)) {
        std::cerr << "abandoned setup-output case failed\n";
        return 1;
    }
    if (!run_rejected_case(argv[1], false)) {
        std::cerr << "cookie rejection case failed\n";
        return 1;
    }
    if (!run_rejected_case(argv[1], true)) {
        std::cerr << "protocol-version rejection case failed\n";
        return 1;
    }
    return 0;
}
