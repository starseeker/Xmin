#include "xmin/config.h"
#include "xmin/next/unique_fd.hpp"
#include "xmin/next/xauthority.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using xmin::next::UniqueFd;

volatile sig_atomic_t signal_write_descriptor = -1;

void
wake_for_signal(int signal_number)
{
    const int saved_errno = errno;
    if (signal_write_descriptor >= 0) {
        const auto byte = static_cast<unsigned char>(signal_number);
        static_cast<void>(::write(
            static_cast<int>(signal_write_descriptor), &byte, 1));
    }
    errno = saved_errno;
}

bool
set_close_on_exec(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFD);
    return flags >= 0 &&
        ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

class SignalPipe {
public:
    bool install()
    {
        int descriptors[2];
        if (::pipe(descriptors) != 0)
            return false;
        read_.reset(descriptors[0]);
        write_.reset(descriptors[1]);
        if (!set_close_on_exec(read_.get()) ||
            !set_close_on_exec(write_.get())) {
            return false;
        }
        struct sigaction action {};
        action.sa_handler = wake_for_signal;
        ::sigemptyset(&action.sa_mask);
        if (::sigaction(SIGINT, &action, &old_int_) != 0)
            return false;
        int_installed_ = true;
        if (::sigaction(SIGTERM, &action, &old_term_) != 0)
            return false;
        term_installed_ = true;
        if (::sigaction(SIGHUP, &action, &old_hup_) != 0)
            return false;
        hup_installed_ = true;
        signal_write_descriptor = write_.get();
        return true;
    }

    SignalPipe() = default;
    SignalPipe(const SignalPipe &) = delete;
    SignalPipe &operator=(const SignalPipe &) = delete;

    ~SignalPipe()
    {
        signal_write_descriptor = -1;
        restore();
    }

    [[nodiscard]] int fd() const noexcept { return read_.get(); }

    std::vector<int> drain()
    {
        std::vector<int> signals;
        std::array<unsigned char, 32> bytes{};
        for (;;) {
            const auto count = ::read(read_.get(), bytes.data(), bytes.size());
            if (count > 0) {
                for (ssize_t index = 0; index < count; ++index)
                    signals.push_back(bytes[static_cast<std::size_t>(index)]);
            }
            else if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        return signals;
    }

    void restore()
    {
        if (hup_installed_) {
            static_cast<void>(::sigaction(SIGHUP, &old_hup_, nullptr));
            hup_installed_ = false;
        }
        if (term_installed_) {
            static_cast<void>(::sigaction(SIGTERM, &old_term_, nullptr));
            term_installed_ = false;
        }
        if (int_installed_) {
            static_cast<void>(::sigaction(SIGINT, &old_int_, nullptr));
            int_installed_ = false;
        }
    }

private:
    UniqueFd read_;
    UniqueFd write_;
    struct sigaction old_int_ {};
    struct sigaction old_term_ {};
    struct sigaction old_hup_ {};
    bool int_installed_ = false;
    bool term_installed_ = false;
    bool hup_installed_ = false;
};

void
print_usage(std::ostream &stream, std::string_view program)
{
    stream << "usage: " << program
           << " [--server PATH] [--screen WxHxD] [--dpi DPI] -- "
              "COMMAND [ARG ...]\n";
}

std::string
resolved_program(std::string_view program)
{
    const auto resolve = [](const std::string &candidate) {
        std::vector<char> path(
            static_cast<std::size_t>(PATH_MAX) + candidate.size() + 2);
        if (::realpath(candidate.c_str(), path.data()) != nullptr)
            return std::string(path.data());
        return candidate;
    };
    if (program.find('/') != std::string_view::npos)
        return resolve(std::string(program));
    const char *environment = std::getenv("PATH");
    if (environment == nullptr)
        return std::string(program);
    std::string_view paths(environment);
    while (true) {
        const auto separator = paths.find(':');
        std::string directory(paths.substr(0, separator));
        if (directory.empty())
            directory = ".";
        const std::string candidate = directory + "/" + std::string(program);
        if (::access(candidate.c_str(), X_OK) == 0)
            return resolve(candidate);
        if (separator == std::string_view::npos)
            break;
        paths.remove_prefix(separator + 1);
    }
    return std::string(program);
}

std::string
default_server_path(const std::string &launcher)
{
    const auto slash = launcher.rfind('/');
    if (slash == std::string::npos)
        return "Xmin";
    return launcher.substr(0, slash + 1) + "Xmin";
}

bool
configure_bundled_gl(const std::string &launcher)
{
#if XMIN_BUILD_CLIENT_GL
#if defined(__APPLE__)
    constexpr std::string_view variable = "DYLD_LIBRARY_PATH";
#else
    constexpr std::string_view variable = "LD_LIBRARY_PATH";
#endif
    const auto slash = launcher.rfind('/');
    if (slash == std::string::npos)
        return true;
    const std::string directory = launcher.substr(0, slash) + "/../" +
        XMIN_INSTALL_LIBDIR + "/xmin";
    struct stat status {};
    if (::stat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
        return true;
    const char *existing = std::getenv(variable.data());
    const std::string value = existing != nullptr && *existing != '\0'
        ? directory + ":" + existing
        : directory;
    return ::setenv(variable.data(), value.c_str(), 1) == 0;
#else
    static_cast<void>(launcher);
    return true;
#endif
}

std::optional<std::string>
make_private_directory()
{
    const char *base = std::getenv("TMPDIR");
    if (base == nullptr || *base == '\0')
        base = "/tmp";
    std::string pattern = std::string(base) + "/xmin-run-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char *created = ::mkdtemp(writable.data());
    if (created == nullptr || ::chmod(created, 0700) != 0)
        return std::nullopt;
    return std::string(created);
}

std::vector<char *>
argument_pointers(std::vector<std::string> &arguments)
{
    std::vector<char *> pointers;
    pointers.reserve(arguments.size() + 1);
    for (auto &argument : arguments)
        pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return pointers;
}

std::optional<std::string>
read_display(int descriptor, int signal_descriptor)
{
    std::string display;
    for (;;) {
        std::array<pollfd, 2> ready{{
            pollfd{descriptor, POLLIN | POLLHUP, 0},
            pollfd{signal_descriptor, POLLIN, 0},
        }};
        int result;
        do {
            result = ::poll(ready.data(), ready.size(), 15000);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || (ready[1].revents & POLLIN) != 0)
            return std::nullopt;
        char byte = 0;
        ssize_t count;
        do {
            count = ::read(descriptor, &byte, 1);
        } while (count < 0 && errno == EINTR);
        if (count != 1)
            return std::nullopt;
        if (byte == '\n')
            return display.empty() ? std::nullopt
                                   : std::optional<std::string>(display);
        if (byte < '0' || byte > '9' || display.size() == 5)
            return std::nullopt;
        display.push_back(byte);
    }
}

int
exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 125;
}

bool
terminate_and_wait(pid_t process, int &status)
{
    if (process <= 0)
        return true;
    if (::kill(process, SIGTERM) != 0 && errno != ESRCH)
        return false;
    for (unsigned attempt = 0; attempt < 250; ++attempt) {
        const auto result = ::waitpid(process, &status, WNOHANG);
        if (result == process || (result < 0 && errno == ECHILD))
            return true;
        if (result < 0 && errno != EINTR)
            return false;
        const timespec delay{0, 20 * 1000 * 1000};
        static_cast<void>(::nanosleep(&delay, nullptr));
    }
    static_cast<void>(::kill(process, SIGKILL));
    while (::waitpid(process, &status, 0) < 0) {
        if (errno != EINTR)
            return errno == ECHILD;
    }
    return false;
}

int
run(int argc, char **argv)
{
    std::string server_override;
    std::string screen;
    std::string dpi;
    int command_index = -1;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--") {
            command_index = index + 1;
            break;
        }
        if (argument == "--help") {
            print_usage(std::cout, argv[0]);
            return 0;
        }
        if (argument == "--version") {
            std::cout << "xmin-run " << XMIN_VERSION << '\n';
            return 0;
        }
        if ((argument == "--server" || argument == "--screen" ||
             argument == "--dpi") &&
            index + 1 < argc) {
            std::string &destination = argument == "--server"
                ? server_override
                : argument == "--screen" ? screen : dpi;
            destination = argv[++index];
            continue;
        }
        std::cerr << "xmin-run: unknown or incomplete option: " << argument
                  << '\n';
        print_usage(std::cerr, argv[0]);
        return 2;
    }
    if (command_index < 0 || command_index >= argc) {
        print_usage(std::cerr, argv[0]);
        return 2;
    }

    const std::string launcher = resolved_program(argv[0]);
    const std::string server_path = server_override.empty()
        ? default_server_path(launcher)
        : server_override;
    const auto temporary = make_private_directory();
    if (!temporary) {
        std::cerr << "xmin-run: cannot create private temporary directory: "
                  << std::strerror(errno) << '\n';
        return 125;
    }
    const std::string authority = *temporary + "/Xauthority";
    auto cookie = xmin::next::secure_random_bytes(16);
    if (!cookie) {
        std::cerr << "xmin-run: " << cookie.error().message << '\n';
        static_cast<void>(::rmdir(temporary->c_str()));
        return 125;
    }
    auto authority_result =
        xmin::next::write_xauthority_cookie(authority, cookie.value());
    std::fill(cookie.value().begin(), cookie.value().end(), 0);
    if (!authority_result) {
        std::cerr << "xmin-run: " << authority_result.error().message << '\n';
        static_cast<void>(::rmdir(temporary->c_str()));
        return 125;
    }

    SignalPipe signals;
    int descriptors[2] = {-1, -1};
    if (!signals.install() || ::pipe(descriptors) != 0) {
        std::cerr << "xmin-run: process setup: " << std::strerror(errno)
                  << '\n';
        static_cast<void>(::unlink(authority.c_str()));
        static_cast<void>(::rmdir(temporary->c_str()));
        return 125;
    }
    UniqueFd ready_read(descriptors[0]);
    UniqueFd ready_write(descriptors[1]);
    std::vector<std::string> server_arguments{
        server_path,
        "-displayfd",
        std::to_string(ready_write.get()),
        "-auth",
        authority,
        "-noreset",
    };
    if (!screen.empty()) {
        server_arguments.emplace_back("-screen");
        server_arguments.emplace_back("0");
        server_arguments.push_back(screen);
    }
    if (!dpi.empty()) {
        server_arguments.emplace_back("-dpi");
        server_arguments.push_back(dpi);
    }
    auto server_pointers = argument_pointers(server_arguments);

    pid_t server = ::fork();
    if (server == 0) {
        ready_read.reset();
        signals.restore();
        ::execvp(server_path.c_str(), server_pointers.data());
        std::cerr << "xmin-run: cannot execute " << server_path << ": "
                  << std::strerror(errno) << '\n';
        _exit(127);
    }
    ready_write.reset();
    pid_t command = -1;
    int command_status = 125 << 8;
    int server_status = 0;
    int result = 125;
    bool server_exited_early = false;
    if (server < 0) {
        std::cerr << "xmin-run: cannot fork Xmin: " << std::strerror(errno)
                  << '\n';
    }
    else {
        const auto display = read_display(ready_read.get(), signals.fd());
        ready_read.reset();
        if (!display) {
            std::cerr << "xmin-run: Xmin did not report a ready display\n";
        }
        else {
            const std::string display_environment = ":" + *display;
            if (::setenv("DISPLAY", display_environment.c_str(), 1) != 0 ||
                ::setenv("XAUTHORITY", authority.c_str(), 1) != 0 ||
                !configure_bundled_gl(launcher)) {
                std::cerr << "xmin-run: cannot set child environment: "
                          << std::strerror(errno) << '\n';
            }
            else {
                command = ::fork();
                if (command == 0) {
                    signals.restore();
                    ::execvp(argv[command_index], argv + command_index);
                    std::cerr << "xmin-run: cannot execute "
                              << argv[command_index] << ": "
                              << std::strerror(errno) << '\n';
                    _exit(127);
                }
                if (command < 0) {
                    std::cerr << "xmin-run: cannot fork command: "
                              << std::strerror(errno) << '\n';
                }
                else {
                    for (;;) {
                        const auto command_wait =
                            ::waitpid(command, &command_status, WNOHANG);
                        if (command_wait == command) {
                            command = -1;
                            result = exit_code(command_status);
                            break;
                        }
                        if (command_wait < 0 && errno != EINTR) {
                            std::cerr << "xmin-run: cannot wait for command: "
                                      << std::strerror(errno) << '\n';
                            break;
                        }
                        const auto server_wait =
                            ::waitpid(server, &server_status, WNOHANG);
                        if (server_wait == server) {
                            server = -1;
                            server_exited_early = true;
                            std::cerr
                                << "xmin-run: Xmin exited before the command\n";
                            break;
                        }
                        if (server_wait < 0 && errno != EINTR) {
                            std::cerr << "xmin-run: cannot wait for Xmin: "
                                      << std::strerror(errno) << '\n';
                            break;
                        }
                        pollfd signal_ready{signals.fd(), POLLIN, 0};
                        int polled;
                        do {
                            polled = ::poll(&signal_ready, 1, 20);
                        } while (polled < 0 && errno == EINTR);
                        if (polled > 0 &&
                            (signal_ready.revents & POLLIN) != 0) {
                            for (const int forwarded : signals.drain()) {
                                static_cast<void>(::kill(command, forwarded));
                                static_cast<void>(::kill(server, forwarded));
                            }
                        }
                    }
                }
            }
        }
    }

    if (command > 0 && !terminate_and_wait(command, command_status))
        result = 125;
    if (server > 0 && !terminate_and_wait(server, server_status))
        result = 125;
    if (server_exited_early && result == 0)
        result = 125;
    if (::unlink(authority.c_str()) != 0 && errno != ENOENT)
        result = 125;
    if (::rmdir(temporary->c_str()) != 0 && errno != ENOENT)
        result = 125;
    return result;
}

} // namespace

int
main(int argc, char **argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    try {
        return run(argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << "xmin-run: fatal error: " << error.what() << '\n';
        return 125;
    }
}
