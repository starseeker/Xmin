#include "xmin/config.h"
#include "xmin/next/connection.hpp"
#include "xmin/next/unique_fd.hpp"

#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using xmin::next::ServerConfig;
using xmin::next::UniqueFd;

void
print_help()
{
    std::cout
        << "usage: Xmin-next --client-fd FD (--cookie-hex HEX | --no-auth) "
           "[--screen WIDTHxHEIGHT]\n"
        << "       Xmin-next --help | --version\n";
}

template <typename T>
bool
parse_unsigned(std::string_view text, T &value)
{
    static_assert(std::is_unsigned_v<T>);
    T parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    value = parsed;
    return true;
}

bool
parse_screen(std::string_view text, ServerConfig &config)
{
    const auto separator = text.find('x');
    if (separator == std::string_view::npos)
        return false;
    unsigned width = 0;
    unsigned height = 0;
    if (!parse_unsigned(text.substr(0, separator), width) ||
        !parse_unsigned(text.substr(separator + 1), height) || width == 0 ||
        height == 0 || width > std::numeric_limits<std::uint16_t>::max() ||
        height > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    config.width = static_cast<std::uint16_t>(width);
    config.height = static_cast<std::uint16_t>(height);
    return true;
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
    std::cerr << "Xmin-next: " << message << '\n';
    return 2;
}

} // namespace

int
main(int argc, char **argv)
{
    int client_fd = -1;
    ServerConfig config;
    bool authentication_selected = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            print_help();
            return 0;
        }
        if (argument == "--version") {
            std::cout << "Xmin-next " << XMIN_VERSION << '\n';
            return 0;
        }
        if (argument == "--client-fd") {
            if (++index >= argc)
                return fail("--client-fd requires a value");
            unsigned parsed = 0;
            if (!parse_unsigned(std::string_view(argv[index]), parsed) ||
                parsed > static_cast<unsigned>(INT_MAX)) {
                return fail("invalid client file descriptor");
            }
            client_fd = static_cast<int>(parsed);
            continue;
        }
        if (argument == "--cookie-hex") {
            if (++index >= argc)
                return fail("--cookie-hex requires a value");
            if (authentication_selected ||
                !parse_cookie(argv[index], config.cookie)) {
                return fail("invalid or conflicting cookie");
            }
            authentication_selected = true;
            continue;
        }
        if (argument == "--no-auth") {
            if (authentication_selected)
                return fail("authentication modes are mutually exclusive");
            config.allow_unauthenticated = true;
            authentication_selected = true;
            continue;
        }
        if (argument == "--screen") {
            if (++index >= argc || !parse_screen(argv[index], config))
                return fail("--screen requires WIDTHxHEIGHT");
            continue;
        }
        return fail(std::string("unknown option: ") + std::string(argument));
    }

    if (client_fd < 0)
        return fail("--client-fd is required in this migration milestone");
    if (!authentication_selected)
        return fail("select --cookie-hex or explicit --no-auth");
    if (::fcntl(client_fd, F_GETFD) < 0)
        return fail(std::string("invalid client fd: ") + std::strerror(errno));

    std::signal(SIGPIPE, SIG_IGN);
    xmin::next::Connection connection(UniqueFd(client_fd), std::move(config));
    const auto result = connection.serve();
    if (!result) {
        std::cerr << "Xmin-next: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
