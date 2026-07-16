#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view auth_name = "MIT-MAGIC-COOKIE-1";
constexpr std::uint32_t pointer_root = 1;
constexpr std::uint32_t root_window = 0x00000100;

class Fd {
public:
    Fd() = default;
    explicit Fd(int descriptor) : descriptor_(descriptor) {}
    ~Fd()
    {
        if (descriptor_ >= 0)
            static_cast<void>(::close(descriptor_));
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : descriptor_(std::exchange(other.descriptor_, -1))
    {}
    Fd &operator=(Fd &&other) noexcept
    {
        if (this != &other) {
            if (descriptor_ >= 0)
                static_cast<void>(::close(descriptor_));
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const { return descriptor_; }
    [[nodiscard]] explicit operator bool() const { return descriptor_ >= 0; }

private:
    int descriptor_ = -1;
};

struct Process {
    pid_t pid = -1;
    unsigned display = 0;
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
append_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void
append_field(std::vector<std::uint8_t> &bytes,
             const std::vector<std::uint8_t> &field)
{
    append_u16(bytes, static_cast<std::uint16_t>(field.size()));
    bytes.insert(bytes.end(), field.begin(), field.end());
}

std::vector<std::uint8_t>
as_bytes(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
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
write_file(const std::string &path, const std::vector<std::uint8_t> &bytes,
           mode_t mode)
{
    Fd descriptor(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode));
    return descriptor && write_all(descriptor.get(), bytes);
}

bool
write_authority(const std::string &path)
{
    std::vector<std::uint8_t> records;
    std::vector<std::uint8_t> cookie(16);
    for (std::size_t index = 0; index < cookie.size(); ++index)
        cookie[index] = static_cast<std::uint8_t>(index);
    for (unsigned display = 0; display < 4; ++display) {
        append_u16(records, 65535); // FamilyWild
        append_field(records, {});
        append_field(records, as_bytes(std::to_string(display)));
        append_field(records, as_bytes(auth_name));
        append_field(records, cookie);
    }
    return write_file(path, records, 0600);
}

bool
make_stale_socket(const std::string &path)
{
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path))
        return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||        \
    defined(__OpenBSD__)
    address.sun_len = static_cast<std::uint8_t>(address_size);
#endif
    Fd socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    return socket &&
        ::bind(socket.get(), reinterpret_cast<sockaddr *>(&address),
               address_size) == 0;
}

std::optional<unsigned>
read_display(int descriptor)
{
    std::string text;
    while (text.size() < 16) {
        pollfd poll_descriptor{descriptor, POLLIN, 0};
        int ready;
        do {
            ready = ::poll(&poll_descriptor, 1, 5000);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || (poll_descriptor.revents & POLLNVAL) != 0 ||
            ((poll_descriptor.revents & POLLIN) == 0 &&
             (poll_descriptor.revents & (POLLERR | POLLHUP)) != 0))
            return std::nullopt;
        char byte = '\0';
        const auto count = ::read(descriptor, &byte, 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return std::nullopt;
        if (byte == '\n') {
            unsigned display = 0;
            const auto result = std::from_chars(
                text.data(), text.data() + text.size(), display, 10);
            if (text.empty() || result.ec != std::errc{} ||
                result.ptr != text.data() + text.size()) {
                return std::nullopt;
            }
            return display;
        }
        text.push_back(byte);
    }
    return std::nullopt;
}

std::optional<Process>
spawn_server(const char *server, const std::string &root,
             const std::string &authority)
{
    int notification[2];
    if (::pipe(notification) != 0)
        return std::nullopt;
    Fd read_end(notification[0]);
    Fd write_end(notification[1]);
    const pid_t child = ::fork();
    if (child < 0)
        return std::nullopt;
    if (child == 0) {
        static_cast<void>(::close(read_end.get()));
        char descriptor[32];
        std::snprintf(
            descriptor, sizeof(descriptor), "%d", write_end.get());
        ::execl(server, server, "--display-fd", descriptor, "--runtime-root",
                root.c_str(), "--auth", authority.c_str(), "--screen",
                "320x240", "--max-clients", "2",
                static_cast<char *>(nullptr));
        _exit(127);
    }
    write_end = Fd();
    const auto display = read_display(read_end.get());
    if (!display) {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, nullptr, 0));
        return std::nullopt;
    }
    return Process{child, *display};
}

bool
stop_server(Process &process)
{
    if (process.pid < 0)
        return true;
    if (::kill(process.pid, SIGTERM) != 0 && errno != ESRCH)
        return false;
    const timespec pause{0, 10000000};
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        int status = 0;
        const auto result = ::waitpid(process.pid, &status, WNOHANG);
        if (result == process.pid) {
            process.pid = -1;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR)
            return false;
        static_cast<void>(::nanosleep(&pause, nullptr));
    }
    static_cast<void>(::kill(process.pid, SIGKILL));
    static_cast<void>(::waitpid(process.pid, nullptr, 0));
    process.pid = -1;
    return false;
}

Fd
connect_client(const std::string &root, unsigned display)
{
    const std::string path =
        root + "/.X11-unix/X" + std::to_string(display);
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path))
        return {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||        \
    defined(__OpenBSD__)
    address.sun_len = static_cast<std::uint8_t>(address_size);
#endif
    Fd socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socket)
        return {};
    const timeval timeout{3, 0};
    static_cast<void>(::setsockopt(
        socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    static_cast<void>(::setsockopt(
        socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    if (::connect(socket.get(), reinterpret_cast<sockaddr *>(&address),
                  address_size) != 0) {
        return {};
    }
    return socket;
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
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
    }
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) << 8 | bytes[offset + 1]);
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
read_exact(int descriptor, std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count =
            ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool
send_setup(int descriptor, bool little)
{
    std::vector<std::uint8_t> prefix(12);
    prefix[0] = static_cast<std::uint8_t>(little ? 'l' : 'B');
    put16(prefix, 2, 11, little);
    put16(prefix, 6, static_cast<std::uint16_t>(auth_name.size()), little);
    put16(prefix, 8, 16, little);
    std::vector<std::uint8_t> authentication = as_bytes(auth_name);
    while ((authentication.size() & 3) != 0)
        authentication.push_back(0);
    for (unsigned byte = 0; byte < 16; ++byte)
        authentication.push_back(static_cast<std::uint8_t>(byte));
    return write_all(descriptor, prefix) &&
        write_all(descriptor, authentication);
}

std::optional<std::uint32_t>
read_setup(int descriptor, bool little)
{
    std::vector<std::uint8_t> prefix(8);
    if (!read_exact(descriptor, prefix) || prefix[0] != 1 ||
        get16(prefix, 2, little) != 11) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> setup(
        static_cast<std::size_t>(get16(prefix, 6, little)) * 4);
    if (!read_exact(descriptor, setup) || setup.size() < 148)
        return std::nullopt;
    const auto vendor_size = get16(setup, 16, little);
    const std::size_t root_offset =
        32 + ((vendor_size + 3) & ~std::size_t{3}) +
        static_cast<std::size_t>(setup[21]) * 8;
    if (root_offset + 40 > setup.size() ||
        get32(setup, root_offset, little) != root_window ||
        get16(setup, root_offset + 20, little) != 320 ||
        get16(setup, root_offset + 22, little) != 240) {
        return std::nullopt;
    }
    return get32(setup, 4, little);
}

bool
geometry_round_trip(int descriptor, bool little)
{
    std::vector<std::uint8_t> request(8);
    request[0] = 14;
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
        return false;
    std::vector<std::uint8_t> reply(32);
    return read_exact(descriptor, reply) && reply[0] == 1 && reply[1] == 24 &&
        get16(reply, 2, little) == 1 &&
        get32(reply, 8, little) == root_window;
}

bool
send_simple_request(int descriptor, bool little, std::uint8_t opcode)
{
    std::vector<std::uint8_t> request(4);
    request[0] = opcode;
    put16(request, 2, 1, little);
    return write_all(descriptor, request);
}

bool
synchronize(int descriptor, bool little, std::uint16_t sequence)
{
    if (!send_simple_request(descriptor, little, 43)) // GetInputFocus
        return false;
    std::vector<std::uint8_t> reply(32);
    return read_exact(descriptor, reply) && reply[0] == 1 &&
        get16(reply, 2, little) == sequence &&
        get32(reply, 8, little) == pointer_root;
}

bool
send_geometry_query(int descriptor, bool little)
{
    std::vector<std::uint8_t> request(8);
    request[0] = 14;
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    return write_all(descriptor, request);
}

bool
read_geometry_reply(int descriptor, bool little, std::uint16_t sequence)
{
    std::vector<std::uint8_t> reply(32);
    return read_exact(descriptor, reply) && reply[0] == 1 && reply[1] == 24 &&
        get16(reply, 2, little) == sequence &&
        get32(reply, 8, little) == root_window;
}

bool
remains_blocked(int descriptor)
{
    pollfd candidate{descriptor, POLLIN, 0};
    int ready;
    do {
        ready = ::poll(&candidate, 1, 100);
    } while (ready < 0 && errno == EINTR);
    return ready == 0;
}

bool
create_child(int descriptor, bool little, std::uint32_t id)
{
    std::vector<std::uint8_t> request(32);
    request[0] = 1;  // CreateWindow
    request[1] = 24; // depth
    put16(request, 2, 8, little);
    put32(request, 4, id, little);
    put32(request, 8, root_window, little);
    put16(request, 16, 20, little);
    put16(request, 18, 10, little);
    put16(request, 22, 1, little); // InputOutput
    put32(request, 24, 3, little); // root visual
    return write_all(descriptor, request);
}

bool
set_selection_owner(int descriptor, bool little, std::uint32_t window)
{
    std::vector<std::uint8_t> request(16);
    request[0] = 22;
    put16(request, 2, 4, little);
    put32(request, 4, window, little);
    put32(request, 8, 1, little); // PRIMARY
    return write_all(descriptor, request);
}

bool
check_selection_owner(int descriptor, bool little, std::uint16_t sequence,
                      std::uint32_t expected)
{
    std::vector<std::uint8_t> request(8);
    request[0] = 23;
    put16(request, 2, 2, little);
    put32(request, 4, 1, little); // PRIMARY
    if (!write_all(descriptor, request))
        return false;
    std::vector<std::uint8_t> reply(32);
    return read_exact(descriptor, reply) && reply[0] == 1 &&
        get16(reply, 2, little) == sequence &&
        get32(reply, 8, little) == expected;
}

bool
send_client_message(int descriptor, bool little, std::uint32_t window)
{
    std::vector<std::uint8_t> request(44);
    request[0] = 25;
    put16(request, 2, 11, little);
    put32(request, 4, window, little);
    request[12] = 33; // ClientMessage
    request[13] = 32;
    put32(request, 16, window, little);
    put32(request, 20, 1, little); // PRIMARY
    put32(request, 24, 0x584d494eU, little);
    return write_all(descriptor, request);
}

bool
read_client_message(int descriptor, bool little, std::uint16_t sequence,
                    std::uint32_t window)
{
    std::vector<std::uint8_t> event(32);
    return read_exact(descriptor, event) && event[0] == (33 | 0x80U) &&
        event[1] == 32 && get16(event, 2, little) == sequence &&
        get32(event, 4, little) == window &&
        get32(event, 8, little) == 1 &&
        get32(event, 12, little) == 0x584d494eU;
}

bool
change_root_property(int descriptor, bool little)
{
    std::vector<std::uint8_t> request(28);
    request[0] = 18; // ChangeProperty
    put16(request, 2, 7, little);
    put32(request, 4, root_window, little);
    put32(request, 8, 9, little);   // CUT_BUFFER0
    put32(request, 12, 19, little); // INTEGER
    request[16] = 32;
    put32(request, 20, 1, little);
    put32(request, 24, 0x11223344U, little);
    return write_all(descriptor, request);
}

bool
check_root_property(int descriptor, bool little, std::uint16_t sequence)
{
    std::vector<std::uint8_t> request(24);
    request[0] = 20; // GetProperty
    put16(request, 2, 6, little);
    put32(request, 4, root_window, little);
    put32(request, 8, 9, little);   // CUT_BUFFER0
    put32(request, 12, 19, little); // INTEGER
    put32(request, 20, 1, little);
    if (!write_all(descriptor, request))
        return false;
    std::vector<std::uint8_t> reply(36);
    return read_exact(descriptor, reply) && reply[0] == 1 && reply[1] == 32 &&
        get16(reply, 2, little) == sequence &&
        get32(reply, 8, little) == 19 && get32(reply, 12, little) == 0 &&
        get32(reply, 16, little) == 1 &&
        get32(reply, 32, little) == 0x11223344U;
}

std::optional<std::vector<std::uint32_t>>
query_root(int descriptor, bool little, std::uint16_t sequence)
{
    std::vector<std::uint8_t> request(8);
    request[0] = 15; // QueryTree
    put16(request, 2, 2, little);
    put32(request, 4, root_window, little);
    if (!write_all(descriptor, request))
        return std::nullopt;
    std::vector<std::uint8_t> reply(32);
    if (!read_exact(descriptor, reply) || reply[0] != 1 ||
        get16(reply, 2, little) != sequence ||
        get32(reply, 8, little) != root_window) {
        return std::nullopt;
    }
    const auto count = get16(reply, 16, little);
    if (get32(reply, 4, little) != count)
        return std::nullopt;
    std::vector<std::uint8_t> wire_children(
        static_cast<std::size_t>(count) * 4);
    if (!read_exact(descriptor, wire_children))
        return std::nullopt;
    std::vector<std::uint32_t> children;
    children.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        children.push_back(get32(wire_children, index * 4, little));
    return children;
}

bool
path_exists(const std::string &path)
{
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0;
}

} // namespace

int
main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: next_standalone /path/to/Xmin-next\n";
        return 2;
    }

    std::string pattern = "/tmp/xmin-next-standalone.XXXXXX";
    std::vector<char> root_buffer(pattern.begin(), pattern.end());
    root_buffer.push_back('\0');
    const char *created = ::mkdtemp(root_buffer.data());
    if (created == nullptr)
        return 1;
    const std::string root(created);
    const std::string socket_directory = root + "/.X11-unix";
    const std::string authority = root + "/authority";
    if (::mkdir(socket_directory.c_str(), 0700) != 0 ||
        !write_authority(authority) ||
        !write_file(root + "/.X0-lock", as_bytes("stale\n"), 0444) ||
        !make_stale_socket(socket_directory + "/X0")) {
        return 1;
    }

    Process first;
    Process second;
    bool passed = false;
    auto first_spawned = spawn_server(argv[1], root, authority);
    if (first_spawned) {
        first = *first_spawned;
        auto second_spawned = spawn_server(argv[1], root, authority);
        if (second_spawned) {
            second = *second_spawned;
            const bool native = host_is_little_endian();
            Fd first_client = connect_client(root, first.display);
            Fd second_client = connect_client(root, first.display);
            Fd other_server_client = connect_client(root, second.display);
            if (first_client && second_client && other_server_client &&
                send_setup(first_client.get(), native) &&
                send_setup(second_client.get(), !native) &&
                send_setup(other_server_client.get(), native)) {
                const auto second_base = read_setup(second_client.get(), !native);
                const auto first_base = read_setup(first_client.get(), native);
                const auto other_base =
                    read_setup(other_server_client.get(), native);
                passed = first.display == 0 && second.display == 1 &&
                    first_base && second_base && other_base &&
                    *first_base != *second_base &&
                    geometry_round_trip(second_client.get(), !native) &&
                    geometry_round_trip(first_client.get(), native) &&
                    geometry_round_trip(other_server_client.get(), native);
                if (passed) {
                    passed = send_simple_request(
                        first_client.get(), native, 36) &&
                        synchronize(first_client.get(), native, 3) &&
                        send_geometry_query(second_client.get(), !native) &&
                        remains_blocked(second_client.get()) &&
                        send_simple_request(first_client.get(), native, 37) &&
                        synchronize(first_client.get(), native, 5) &&
                        read_geometry_reply(second_client.get(), !native, 2);
                }
                if (passed) {
                    std::uint16_t second_sequence = 3;
                    passed = create_child(
                        first_client.get(), native, *first_base);
                    auto shared_tree = query_root(
                        second_client.get(), !native, second_sequence);
                    passed = passed && shared_tree &&
                        std::find(shared_tree->begin(), shared_tree->end(),
                                  *first_base) != shared_tree->end();
                    passed = passed && set_selection_owner(
                        first_client.get(), native, *first_base);
                    passed = passed && check_selection_owner(
                        second_client.get(), !native, ++second_sequence,
                        *first_base);
                    passed = passed && send_client_message(
                        second_client.get(), !native, *first_base);
                    ++second_sequence;
                    passed = passed && read_client_message(
                        first_client.get(), native, 7, *first_base);
                    passed = passed &&
                        change_root_property(first_client.get(), native);
                    first_client = Fd();
                    passed = passed &&
                        check_root_property(second_client.get(), !native,
                                            ++second_sequence);
                    auto after_disconnect = query_root(
                        second_client.get(), !native, ++second_sequence);
                    if (after_disconnect &&
                        std::find(after_disconnect->begin(),
                                  after_disconnect->end(), *first_base) !=
                            after_disconnect->end()) {
                        after_disconnect = query_root(
                            second_client.get(), !native,
                            ++second_sequence);
                    }
                    passed = passed && after_disconnect &&
                        std::find(after_disconnect->begin(),
                                  after_disconnect->end(), *first_base) ==
                            after_disconnect->end();
                    passed = passed && check_selection_owner(
                        second_client.get(), !native, ++second_sequence, 0);
                }
            }
        }
    }

    const bool first_stopped = stop_server(first);
    const bool second_stopped = stop_server(second);
    passed = passed && first_stopped && second_stopped &&
        !path_exists(root + "/.X0-lock") &&
        !path_exists(root + "/.X1-lock") &&
        !path_exists(socket_directory + "/X0") &&
        !path_exists(socket_directory + "/X1");

    static_cast<void>(::unlink(authority.c_str()));
    static_cast<void>(::rmdir(socket_directory.c_str()));
    static_cast<void>(::rmdir(root.c_str()));
    if (!passed) {
        std::cerr << "standalone display/auth/concurrency lifecycle failed\n";
        return 1;
    }
    return 0;
}
