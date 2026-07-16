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
constexpr std::uint32_t root_window = 1;

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

    request.assign(8, 0);
    request[0] = 4; // DestroyWindow
    put16(request, 2, 2, little);
    put32(request, 4, child, little);
    if (!write_all(descriptor, request))
        return false;
    request[0] = 15; // QueryTree(destroyed child)
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 0 || reply[1] != 3 ||
        get16(reply, 2, little) != 20 || get32(reply, 4, little) != child) {
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
        reply[0] != 1 || get16(reply, 2, little) != 22 ||
        get32(reply, 8, little) != root_window) {
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
        get16(reply, 2, little) != 24 || get32(reply, 8, little) != 19 ||
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
        get16(reply, 2, little) != 26 || get32(reply, 12, little) != 4 ||
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
        get16(reply, 2, little) != 27 || get16(reply, 8, little) != 1 ||
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
    return write_all(descriptor, request) &&
        read_variable_reply(descriptor, little, reply) && reply.size() == 32 &&
        reply[0] == 1 && reply[1] == 0 &&
        get16(reply, 2, little) == 29 && get32(reply, 8, little) == 0;
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
    if (!write_all(descriptor, request) || !read_reply(descriptor, reply) ||
        reply[0] != 1 || get16(reply, 2, little) != 3 || reply[1] != 0) {
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
        get32(reply, 8, little) != root_window) {
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
