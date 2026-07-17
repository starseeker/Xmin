#include "xmin/config.h"
#include "xmin/server/connection.hpp"
#include "xmin/server/display_socket.hpp"
#include "xmin/server/server.hpp"
#include "xmin/server/server_state.hpp"
#include "xmin/server/unique_fd.hpp"
#include "xmin/server/xauthority.hpp"

#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using xmin::server::DisplaySocket;
using xmin::server::ServerConfig;
using xmin::server::UniqueFd;

void
print_help()
{
    std::cout
        << "usage: Xmin [DISPLAY] [--display-fd FD] "
           "(--auth FILE | --cookie-hex HEX | --no-auth)\n"
        << "                 [--screen WIDTHxHEIGHT] [--max-clients N]\n"
        << "       Xmin --client-fd FD "
           "(--cookie-hex HEX | --no-auth) [--screen WIDTHxHEIGHT]\n"
        << "       Xmin --help | --version\n";
}

template <typename T>
bool
parse_unsigned(std::string_view text, T &value)
{
    static_assert(std::is_unsigned_v<T>);
    T parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 10);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool
parse_screen(std::string_view text, ServerConfig &config)
{
    const auto separator = text.find('x');
    if (separator == std::string_view::npos)
        return false;
    const auto depth_separator = text.find('x', separator + 1);
    unsigned width = 0;
    unsigned height = 0;
    if (!parse_unsigned(text.substr(0, separator), width) ||
        !parse_unsigned(
            text.substr(
                separator + 1,
                depth_separator == std::string_view::npos
                    ? std::string_view::npos
                    : depth_separator - separator - 1),
            height) || width == 0 ||
        height == 0 || width > std::numeric_limits<std::uint16_t>::max() ||
        height > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    if (depth_separator != std::string_view::npos) {
        unsigned depth = 0;
        if (!parse_unsigned(text.substr(depth_separator + 1), depth) ||
            depth != 24) {
            return false;
        }
    }
    config.width = static_cast<std::uint16_t>(width);
    config.height = static_cast<std::uint16_t>(height);
    return true;
}

bool
parse_display(std::string_view text, unsigned &display)
{
    if (!text.empty() && text.front() == ':')
        text.remove_prefix(1);
    return parse_unsigned(text, display) && display <= 65535;
}

int
hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

bool
parse_cookie(std::string_view text, std::vector<std::uint8_t> &cookie)
{
    if (text.empty() || (text.size() & 1) != 0 || text.size() > 512)
        return false;
    std::vector<std::uint8_t> parsed;
    parsed.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = hex_digit(text[index]);
        const int low = hex_digit(text[index + 1]);
        if (high < 0 || low < 0)
            return false;
        parsed.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    cookie = std::move(parsed);
    return true;
}

int
fail(std::string_view message)
{
    std::cerr << "Xmin: " << message << '\n';
    return 2;
}

bool
valid_descriptor(int descriptor)
{
    return descriptor >= 0 && ::fcntl(descriptor, F_GETFD) >= 0;
}

enum class Authentication {
    unspecified,
    cookie,
    authority_file,
    disabled,
};

int
run(int argc, char **argv)
{
    int client_fd = -1;
    int display_fd = -1;
    std::optional<unsigned> requested_display;
    std::string authority_path;
    std::string runtime_root = "/tmp";
    std::size_t maximum_clients = 64;
    ServerConfig config;
    Authentication authentication = Authentication::unspecified;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            print_help();
            return 0;
        }
        if (argument == "--version") {
            std::cout << "Xmin " << XMIN_VERSION << '\n';
            return 0;
        }
        if (argument == "--client-fd" || argument == "--display-fd" ||
            argument == "-displayfd") {
            if (++index >= argc)
                return fail(std::string(argument) + " requires a value");
            unsigned parsed = 0;
            if (!parse_unsigned(std::string_view(argv[index]), parsed) ||
                parsed > static_cast<unsigned>(INT_MAX)) {
                return fail(std::string("invalid descriptor for ") +
                            std::string(argument));
            }
            if (argument == "--client-fd")
                client_fd = static_cast<int>(parsed);
            else
                display_fd = static_cast<int>(parsed);
            continue;
        }
        if (argument == "--cookie-hex") {
            if (++index >= argc)
                return fail("--cookie-hex requires a value");
            if (authentication != Authentication::unspecified ||
                !parse_cookie(argv[index], config.cookie)) {
                return fail("invalid or conflicting cookie");
            }
            authentication = Authentication::cookie;
            continue;
        }
        if (argument == "--auth" || argument == "-auth") {
            if (++index >= argc)
                return fail("--auth requires a file");
            if (authentication != Authentication::unspecified ||
                std::string_view(argv[index]).empty()) {
                return fail("invalid or conflicting authority file");
            }
            authority_path = argv[index];
            authentication = Authentication::authority_file;
            continue;
        }
        if (argument == "--no-auth") {
            if (authentication != Authentication::unspecified)
                return fail("authentication modes are mutually exclusive");
            config.allow_unauthenticated = true;
            authentication = Authentication::disabled;
            continue;
        }
        if (argument == "--screen" || argument == "-screen") {
            if (++index >= argc)
                return fail("--screen requires WIDTHxHEIGHT");
            if (argument == "-screen") {
                unsigned screen = 0;
                if (!parse_unsigned(std::string_view(argv[index]), screen) ||
                    screen != 0 || ++index >= argc) {
                    return fail("-screen requires 0 WIDTHxHEIGHTx24");
                }
            }
            if (!parse_screen(argv[index], config))
                return fail("--screen requires WIDTHxHEIGHT[x24]");
            continue;
        }
        if (argument == "-noreset")
            continue;
        if (argument == "-nolisten") {
            if (++index >= argc || std::string_view(argv[index]) != "tcp")
                return fail("only -nolisten tcp is supported");
            continue;
        }
        if (argument == "--max-clients") {
            if (++index >= argc)
                return fail("--max-clients requires a value");
            unsigned parsed = 0;
            if (!parse_unsigned(std::string_view(argv[index]), parsed) ||
                parsed == 0 || parsed > 127) {
                return fail("--max-clients must be between 1 and 127");
            }
            maximum_clients = parsed;
            continue;
        }
        if (argument == "--runtime-root") {
            if (++index >= argc || std::string_view(argv[index]).empty())
                return fail("--runtime-root requires a directory");
            runtime_root = argv[index];
            continue;
        }
        if (!argument.empty() && argument.front() != '-') {
            unsigned display = 0;
            if (requested_display || !parse_display(argument, display))
                return fail("invalid or duplicate DISPLAY");
            requested_display = display;
            continue;
        }
        return fail(std::string("unknown option: ") + std::string(argument));
    }

    if (authentication == Authentication::unspecified)
        return fail("select --auth, --cookie-hex, or explicit --no-auth");
    if (client_fd >= 0) {
        if (display_fd >= 0 || requested_display ||
            authentication == Authentication::authority_file) {
            return fail("--client-fd cannot be combined with display options "
                        "or --auth");
        }
        if (!valid_descriptor(client_fd)) {
            return fail(
                std::string("invalid client fd: ") + std::strerror(errno));
        }
        xmin::server::ServerState state(config.width, config.height);
        xmin::server::Connection connection(
            UniqueFd(client_fd), std::move(config), state);
        const auto result = connection.serve();
        if (!result) {
            std::cerr << "Xmin: " << result.error().message << '\n';
            return 1;
        }
        return 0;
    }

    if (display_fd >= 0 && !valid_descriptor(display_fd)) {
        return fail(
            std::string("invalid display fd: ") + std::strerror(errno));
    }
    auto listener = DisplaySocket::open(requested_display, runtime_root);
    if (!listener) {
        std::cerr << "Xmin: " << listener.error().message << '\n';
        return 1;
    }
    if (authentication == Authentication::authority_file) {
        auto cookie = xmin::server::load_xauthority_cookie(
            authority_path, listener.value().display());
        if (!cookie) {
            std::cerr << "Xmin: " << cookie.error().message << '\n';
            return 1;
        }
        config.cookie = std::move(cookie.value());
    }

    std::cerr << "Xmin: listening on :" << listener.value().display()
              << " at " << listener.value().socket_path() << '\n';
    xmin::server::Server server(
        std::move(listener.value()), std::move(config), maximum_clients,
        UniqueFd(display_fd));
    const auto result = server.run();
    if (!result) {
        std::cerr << "Xmin: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int
main(int argc, char **argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    try {
        return run(argc, argv);
    }
    catch (const std::bad_alloc &) {
        std::cerr << "Xmin: out of memory\n";
        return 1;
    }
    catch (const std::exception &error) {
        std::cerr << "Xmin: fatal error: " << error.what() << '\n';
        return 1;
    }
}
