#include "xmin/server/display_socket.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace xmin::server {
namespace {

constexpr unsigned maximum_dynamic_display = 255;

Result<void>
io_failure(std::string operation)
{
    return Result<void>::failure(
        ErrorCode::io, std::move(operation) + ": " + std::strerror(errno));
}

Result<void>
set_descriptor_flags(int descriptor)
{
    const int status_flags = ::fcntl(descriptor, F_GETFL);
    if (status_flags < 0)
        return io_failure("fcntl(F_GETFL)");
    if (::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) < 0)
        return io_failure("fcntl(F_SETFL)");

    const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
    if (descriptor_flags < 0)
        return io_failure("fcntl(F_GETFD)");
    if (::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
        return io_failure("fcntl(F_SETFD)");
    return Result<void>::success();
}

Result<void>
write_all(int descriptor, std::string_view text)
{
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count =
            ::write(descriptor, text.data() + offset, text.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count == 0) {
            return Result<void>::failure(
                ErrorCode::io, "write made no progress");
        }
        return io_failure("write");
    }
    return Result<void>::success();
}

std::optional<pid_t>
read_lock_pid(const std::string &path)
{
    UniqueFd descriptor(::open(path.c_str(), O_RDONLY));
    if (!descriptor)
        return std::nullopt;
    std::array<char, 64> bytes{};
    ssize_t count;
    do {
        count = ::read(descriptor.get(), bytes.data(), bytes.size() - 1);
    } while (count < 0 && errno == EINTR);
    if (count <= 0)
        return std::nullopt;

    std::string_view text(bytes.data(), static_cast<std::size_t>(count));
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    long parsed = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || parsed <= 0 ||
        static_cast<unsigned long>(parsed) >
            static_cast<unsigned long>(std::numeric_limits<pid_t>::max())) {
        return std::nullopt;
    }
    return static_cast<pid_t>(parsed);
}

bool
process_is_alive(pid_t process)
{
    if (::kill(process, 0) == 0)
        return true;
    return errno == EPERM;
}

bool
stale_socket_error(int error) noexcept
{
    return error == ECONNREFUSED || error == ENOENT;
}

void
unlink_if_same(const std::string &path, dev_t device, ino_t inode) noexcept
{
    struct stat status {};
    if (::lstat(path.c_str(), &status) == 0 && status.st_dev == device &&
        status.st_ino == inode) {
        static_cast<void>(::unlink(path.c_str()));
    }
}

} // namespace

DisplaySocket::DisplaySocket(DisplaySocket &&other) noexcept
    : listener_(std::move(other.listener_)),
      display_(other.display_),
      socket_path_(std::move(other.socket_path_)),
      lock_path_(std::move(other.lock_path_)),
      socket_device_(other.socket_device_),
      socket_inode_(other.socket_inode_),
      lock_device_(other.lock_device_),
      lock_inode_(other.lock_inode_),
      owns_socket_(std::exchange(other.owns_socket_, false)),
      owns_lock_(std::exchange(other.owns_lock_, false))
{}

DisplaySocket &
DisplaySocket::operator=(DisplaySocket &&other) noexcept
{
    if (this != &other) {
        cleanup();
        listener_ = std::move(other.listener_);
        display_ = other.display_;
        socket_path_ = std::move(other.socket_path_);
        lock_path_ = std::move(other.lock_path_);
        socket_device_ = other.socket_device_;
        socket_inode_ = other.socket_inode_;
        lock_device_ = other.lock_device_;
        lock_inode_ = other.lock_inode_;
        owns_socket_ = std::exchange(other.owns_socket_, false);
        owns_lock_ = std::exchange(other.owns_lock_, false);
    }
    return *this;
}

DisplaySocket::~DisplaySocket()
{
    cleanup();
}

void
DisplaySocket::cleanup() noexcept
{
    listener_.reset();
    if (owns_socket_ && !socket_path_.empty())
        unlink_if_same(socket_path_, socket_device_, socket_inode_);
    if (owns_lock_ && !lock_path_.empty())
        unlink_if_same(lock_path_, lock_device_, lock_inode_);
    owns_socket_ = false;
    owns_lock_ = false;
}

Result<void>
DisplaySocket::ensure_socket_directory(const std::string &root)
{
    struct stat root_status {};
    if (::lstat(root.c_str(), &root_status) != 0)
        return io_failure("lstat " + root);
    if (!S_ISDIR(root_status.st_mode)) {
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     root + " is not a directory");
    }

    const std::string directory = root + "/.X11-unix";
    const mode_t mode = root == "/tmp" ? 01777 : 0700;
    if (::mkdir(directory.c_str(), mode) == 0) {
        if (::chmod(directory.c_str(), mode) != 0)
            return io_failure("chmod " + directory);
        return Result<void>::success();
    }
    if (errno != EEXIST)
        return io_failure("mkdir " + directory);

    struct stat status {};
    if (::lstat(directory.c_str(), &status) != 0)
        return io_failure("lstat " + directory);
    if (!S_ISDIR(status.st_mode)) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            directory + " exists but is not a directory");
    }
    return Result<void>::success();
}

Result<void>
DisplaySocket::acquire_lock(const std::string &root)
{
    lock_path_ = root + "/.X" + std::to_string(display_) + "-lock";
    for (unsigned stale_attempt = 0; stale_attempt < 4; ++stale_attempt) {
        std::string temporary_path;
        UniqueFd temporary;
        for (unsigned suffix = 0; suffix < 100; ++suffix) {
            temporary_path = root + "/.X" + std::to_string(display_) +
                "-lock-" + std::to_string(static_cast<long>(::getpid())) +
                "-" + std::to_string(suffix);
            temporary.reset(::open(
                temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0444));
            if (temporary)
                break;
            if (errno != EEXIST)
                return io_failure("create " + temporary_path);
        }
        if (!temporary) {
            return Result<void>::failure(
                ErrorCode::io, "could not create a temporary display lock");
        }

        std::array<char, 32> process_text{};
        const int text_size = std::snprintf(
            process_text.data(), process_text.size(), "%10ld\n",
            static_cast<long>(::getpid()));
        if (text_size <= 0 ||
            static_cast<std::size_t>(text_size) >= process_text.size()) {
            static_cast<void>(::unlink(temporary_path.c_str()));
            return Result<void>::failure(ErrorCode::io,
                                         "could not format display lock");
        }
        auto written = write_all(
            temporary.get(),
            std::string_view(
                process_text.data(), static_cast<std::size_t>(text_size)));
        if (!written) {
            temporary.reset();
            static_cast<void>(::unlink(temporary_path.c_str()));
            return written;
        }

        struct stat temporary_status {};
        if (::fstat(temporary.get(), &temporary_status) != 0) {
            const int status_error = errno;
            temporary.reset();
            static_cast<void>(::unlink(temporary_path.c_str()));
            errno = status_error;
            return io_failure("fstat " + temporary_path);
        }

        if (::link(temporary_path.c_str(), lock_path_.c_str()) == 0) {
            temporary.reset();
            static_cast<void>(::unlink(temporary_path.c_str()));
            lock_device_ = temporary_status.st_dev;
            lock_inode_ = temporary_status.st_ino;
            owns_lock_ = true;
            return Result<void>::success();
        }
        const int link_error = errno;
        temporary.reset();
        static_cast<void>(::unlink(temporary_path.c_str()));
        if (link_error != EEXIST) {
            errno = link_error;
            return io_failure("link " + lock_path_);
        }

        const auto process = read_lock_pid(lock_path_);
        if (process && process_is_alive(*process)) {
            return Result<void>::failure(
                ErrorCode::busy,
                "display :" + std::to_string(display_) + " is already locked");
        }
        if (::unlink(lock_path_.c_str()) != 0 && errno != ENOENT)
            return io_failure("remove stale lock " + lock_path_);
    }
    return Result<void>::failure(
        ErrorCode::busy,
        "display :" + std::to_string(display_) + " lock is unstable");
}

Result<void>
DisplaySocket::bind_listener(const std::string &root)
{
    socket_path_ =
        root + "/.X11-unix/X" + std::to_string(display_);
    sockaddr_un address{};
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        return Result<void>::failure(
            ErrorCode::invalid_argument, "X11 Unix socket path is too long");
    }
    address.sun_family = AF_UNIX;
    std::memcpy(
        address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path_.size() + 1);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||        \
    defined(__OpenBSD__)
    address.sun_len = static_cast<std::uint8_t>(address_size);
#endif

    listener_.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!listener_)
        return io_failure("socket");
    auto flags = set_descriptor_flags(listener_.get());
    if (!flags)
        return flags;

    if (::bind(
            listener_.get(), reinterpret_cast<const sockaddr *>(&address),
            address_size) != 0) {
        const int bind_error = errno;
        if (bind_error != EADDRINUSE)
            return io_failure("bind " + socket_path_);

        UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!probe)
            return io_failure("socket probe");
        if (::connect(
                probe.get(), reinterpret_cast<const sockaddr *>(&address),
                address_size) == 0) {
            return Result<void>::failure(
                ErrorCode::busy,
                "display :" + std::to_string(display_) +
                    " already has a listening socket");
        }
        const int connect_error = errno;
        if (!stale_socket_error(connect_error)) {
            errno = connect_error;
            return io_failure("probe " + socket_path_);
        }

        struct stat status {};
        if (::lstat(socket_path_.c_str(), &status) != 0) {
            if (errno != ENOENT)
                return io_failure("lstat " + socket_path_);
        }
        else {
            if (!S_ISSOCK(status.st_mode)) {
                return Result<void>::failure(
                    ErrorCode::busy,
                    socket_path_ + " exists and is not a socket");
            }
            if (::unlink(socket_path_.c_str()) != 0)
                return io_failure("remove stale socket " + socket_path_);
        }
        if (::bind(
                listener_.get(), reinterpret_cast<const sockaddr *>(&address),
                address_size) != 0) {
            return io_failure("bind " + socket_path_);
        }
    }
    struct stat socket_status {};
    if (::lstat(socket_path_.c_str(), &socket_status) != 0) {
        const int status_error = errno;
        static_cast<void>(::unlink(socket_path_.c_str()));
        errno = status_error;
        return io_failure("lstat " + socket_path_);
    }
    if (!S_ISSOCK(socket_status.st_mode)) {
        static_cast<void>(::unlink(socket_path_.c_str()));
        return Result<void>::failure(
            ErrorCode::io, "could not identify bound display socket");
    }
    socket_device_ = socket_status.st_dev;
    socket_inode_ = socket_status.st_ino;
    owns_socket_ = true;
    if (::chmod(socket_path_.c_str(), 0777) != 0)
        return io_failure("chmod " + socket_path_);
    if (::listen(listener_.get(), 32) != 0)
        return io_failure("listen " + socket_path_);
    return Result<void>::success();
}

Result<void>
DisplaySocket::start(unsigned display, const std::string &root)
{
    display_ = display;
    auto locked = acquire_lock(root);
    if (!locked)
        return locked;
    return bind_listener(root);
}

Result<DisplaySocket>
DisplaySocket::open(std::optional<unsigned> requested_display,
                    std::string runtime_root)
{
    if (runtime_root.empty() || runtime_root.back() == '/') {
        return Result<DisplaySocket>::failure(
            ErrorCode::invalid_argument,
            "runtime root must be a non-empty path without a trailing slash");
    }
    auto directory = ensure_socket_directory(runtime_root);
    if (!directory) {
        return Result<DisplaySocket>::failure(
            directory.error().code, directory.error().message);
    }

    if (requested_display) {
        DisplaySocket socket;
        auto started = socket.start(*requested_display, runtime_root);
        if (!started) {
            return Result<DisplaySocket>::failure(
                started.error().code, started.error().message);
        }
        return Result<DisplaySocket>::success(std::move(socket));
    }

    for (unsigned display = 0; display <= maximum_dynamic_display; ++display) {
        DisplaySocket socket;
        auto started = socket.start(display, runtime_root);
        if (started)
            return Result<DisplaySocket>::success(std::move(socket));
        if (started.error().code != ErrorCode::busy) {
            return Result<DisplaySocket>::failure(
                started.error().code, started.error().message);
        }
    }
    return Result<DisplaySocket>::failure(
        ErrorCode::busy, "no free X11 display in the range :0 through :255");
}

Result<std::optional<UniqueFd>>
DisplaySocket::accept_client()
{
    for (;;) {
        UniqueFd client(::accept(listener_.get(), nullptr, nullptr));
        if (client) {
            auto flags = set_descriptor_flags(client.get());
            if (!flags) {
                return Result<std::optional<UniqueFd>>::failure(
                    flags.error().code, flags.error().message);
            }
            return Result<std::optional<UniqueFd>>::success(
                std::optional<UniqueFd>(std::move(client)));
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Result<std::optional<UniqueFd>>::success(std::nullopt);
        }
        return Result<std::optional<UniqueFd>>::failure(
            ErrorCode::io, std::string("accept: ") + std::strerror(errno));
    }
}

Result<void>
DisplaySocket::notify_display(int descriptor) const
{
    if (descriptor < 0)
        return Result<void>::success();
    return write_all(descriptor, std::to_string(display_) + "\n");
}

} // namespace xmin::server
