#include "xmin/server/display_socket.hpp"
#include "xmin/server/unique_fd.hpp"
#include "xmin/server/xauthority.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using xmin::server::UniqueFd;

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

void
append_authority(std::vector<std::uint8_t> &bytes, std::uint16_t family,
                 std::string_view address, std::string_view display,
                 const std::vector<std::uint8_t> &cookie)
{
    append_u16(bytes, family);
    append_field(bytes, as_bytes(address));
    append_field(bytes, as_bytes(display));
    append_field(bytes, as_bytes("MIT-MAGIC-COOKIE-1"));
    append_field(bytes, cookie);
}

bool
write_bytes(const std::string &path, const std::vector<std::uint8_t> &bytes,
            mode_t mode = 0600)
{
    UniqueFd descriptor(
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode));
    if (!descriptor)
        return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(
            descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool
exists(const std::string &path)
{
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0;
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
    UniqueFd descriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
    return descriptor &&
        ::bind(descriptor.get(), reinterpret_cast<sockaddr *>(&address),
               address_size) == 0;
}

bool
test_xauthority(const std::string &root)
{
    const std::string valid_path = root + "/authority";
    const std::string wildcard_path = root + "/wildcard-authority";
    const std::string local_empty_path = root + "/local-empty-authority";
    const std::string invalid_path = root + "/invalid-authority";
    const std::string written_path = root + "/written-authority";
    const std::vector<std::uint8_t> wildcard_cookie{1, 2, 3, 4};
    const std::vector<std::uint8_t> local_cookie{5, 6, 7, 8};
    std::vector<std::uint8_t> records;
    append_authority(records, 65535, {}, "7", wildcard_cookie);
    append_authority(records, 256, {}, "7", local_cookie);
    append_authority(records, 65535, {}, "8", wildcard_cookie);
    std::vector<std::uint8_t> wildcard_record;
    append_authority(wildcard_record, 65535, {}, {}, wildcard_cookie);
    std::vector<std::uint8_t> local_empty_record;
    append_authority(local_empty_record, 256, {}, {}, local_cookie);
    if (!write_bytes(valid_path, records) ||
        !write_bytes(wildcard_path, wildcard_record) ||
        !write_bytes(local_empty_path, local_empty_record) ||
        !write_bytes(invalid_path, std::vector<std::uint8_t>{0xff})) {
        return false;
    }

    auto selected = xmin::server::load_xauthority_cookie(valid_path, 7);
    auto missing = xmin::server::load_xauthority_cookie(valid_path, 9);
    auto wildcard = xmin::server::load_xauthority_cookie(wildcard_path, 9);
    auto local_empty = xmin::server::load_xauthority_cookie(local_empty_path, 9);
    auto malformed = xmin::server::load_xauthority_cookie(invalid_path, 7);
    auto random = xmin::server::secure_random_bytes(16);
    auto written = random
        ? xmin::server::write_xauthority_cookie(written_path, random.value())
        : xmin::server::Result<void>::failure(
              xmin::server::ErrorCode::io, "random generation failed");
    auto round_trip = written
        ? xmin::server::load_xauthority_cookie(written_path, 42)
        : xmin::server::Result<std::vector<std::uint8_t>>::failure(
              xmin::server::ErrorCode::io, "authority writing failed");
    const bool passed = selected && selected.value() == local_cookie &&
        !missing && wildcard && wildcard.value() == wildcard_cookie &&
        !local_empty && !malformed && random && written && round_trip &&
        round_trip.value() == random.value();
    static_cast<void>(::unlink(valid_path.c_str()));
    static_cast<void>(::unlink(wildcard_path.c_str()));
    static_cast<void>(::unlink(local_empty_path.c_str()));
    static_cast<void>(::unlink(invalid_path.c_str()));
    static_cast<void>(::unlink(written_path.c_str()));
    return passed;
}

bool
test_display_reservation(const std::string &root)
{
    const std::string socket_directory = root + "/.X11-unix";
    if (::mkdir(socket_directory.c_str(), 0700) != 0)
        return false;
    const std::string first_socket = socket_directory + "/X0";
    const std::string first_lock = root + "/.X0-lock";
    if (!make_stale_socket(first_socket) ||
        !write_bytes(first_lock, as_bytes("not-a-pid\n"), 0444)) {
        return false;
    }

    std::string second_socket;
    std::string second_lock;
    {
        auto first = xmin::server::DisplaySocket::open(std::nullopt, root);
        if (!first || first.value().display() != 0 ||
            !exists(first.value().socket_path()) ||
            !exists(first.value().lock_path())) {
            return false;
        }
        auto duplicate = xmin::server::DisplaySocket::open(0, root);
        if (duplicate)
            return false;
        auto second = xmin::server::DisplaySocket::open(std::nullopt, root);
        if (!second || second.value().display() != 1)
            return false;
        second_socket = second.value().socket_path();
        second_lock = second.value().lock_path();
        if (!exists(second_socket) || !exists(second_lock))
            return false;
    }

    const bool cleaned = !exists(first_socket) && !exists(first_lock) &&
        !exists(second_socket) && !exists(second_lock);
    static_cast<void>(::rmdir(socket_directory.c_str()));
    return cleaned;
}

bool
test_replacement_paths_are_preserved(const std::string &root)
{
    std::string socket_path;
    std::string lock_path;
    {
        auto display = xmin::server::DisplaySocket::open(12, root);
        if (!display)
            return false;
        socket_path = display.value().socket_path();
        lock_path = display.value().lock_path();
        if (::unlink(socket_path.c_str()) != 0 ||
            ::unlink(lock_path.c_str()) != 0 ||
            !write_bytes(socket_path, as_bytes("replacement")) ||
            !write_bytes(lock_path, as_bytes("replacement"))) {
            return false;
        }
    }

    const bool preserved = exists(socket_path) && exists(lock_path);
    static_cast<void>(::unlink(socket_path.c_str()));
    static_cast<void>(::unlink(lock_path.c_str()));
    static_cast<void>(::rmdir((root + "/.X11-unix").c_str()));
    return preserved;
}

} // namespace

int
main()
{
    std::string pattern = "/tmp/xmin-next-platform.XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    const char *created = ::mkdtemp(path.data());
    if (created == nullptr) {
        std::cerr << "mkdtemp failed: " << std::strerror(errno) << '\n';
        return 1;
    }
    const std::string root(created);
    const bool passed = test_xauthority(root) && test_display_reservation(root) &&
        test_replacement_paths_are_preserved(root);
    static_cast<void>(::rmdir(root.c_str()));
    if (!passed) {
        std::cerr << "next platform ownership/authentication test failed\n";
        return 1;
    }
    return 0;
}
