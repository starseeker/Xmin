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
           std::uint16_t major = 11)
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
    return write_all(descriptor, prefix) && write_all(descriptor, authentication);
}

bool
read_reply(int descriptor, std::vector<std::uint8_t> &reply)
{
    reply.assign(32, 0);
    return read_all(descriptor, reply);
}

bool
check_setup_success(int descriptor, bool little)
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
    return vendor_size == 9 && setup[20] == 1 && setup[21] == 4 &&
        root_offset + 40 <= setup.size() &&
        get32(setup, root_offset, little) == root_window &&
        get16(setup, root_offset + 20, little) == 320 &&
        get16(setup, root_offset + 22, little) == 240 &&
        setup[root_offset + 38] == 24 && setup[root_offset + 39] == 1;
}

bool
check_request_sequence(int descriptor, bool little)
{
    std::vector<std::uint8_t> request(8);
    request[0] = 14; // GetGeometry
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
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

    request[0] = 127; // NoOperation
    return write_all(descriptor, request);
}

bool
run_success_case(const char *server, bool little)
{
    Child child = spawn_server(server);
    if (child.process < 0 || child.socket < 0)
        return false;
    const bool passed = send_setup(child.socket, little, true) &&
        check_setup_success(child.socket, little) &&
        check_request_sequence(child.socket, little);
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
    if (!run_success_case(argv[1], native)) {
        std::cerr << "native-order setup/request case failed\n";
        return 1;
    }
    if (!run_success_case(argv[1], !native)) {
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
