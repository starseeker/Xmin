#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string executable_path(const char *argument)
{
    std::vector<char> resolved(PATH_MAX + 1U);
    if (argument != nullptr && std::strchr(argument, '/') != nullptr &&
        ::realpath(argument, resolved.data()) != nullptr) {
        return resolved.data();
    }
    const char *path = std::getenv("PATH");
    std::string_view remaining = path == nullptr ? "" : path;
    while (!remaining.empty()) {
        const std::size_t separator = remaining.find(':');
        const std::string directory(
            remaining.substr(0, separator).empty()
                ? std::string_view(".") : remaining.substr(0, separator));
        const std::string candidate = directory + "/" + argument;
        if (::access(candidate.c_str(), X_OK) == 0 &&
            ::realpath(candidate.c_str(), resolved.data()) != nullptr) {
            return resolved.data();
        }
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1U);
    }
    return argument == nullptr ? "xmin-session" : argument;
}

std::string directory_name(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

bool regular_file(const std::string &path)
{
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

std::string executable_or_build(
    const std::string &installed, const char *build_path)
{
    return ::access(installed.c_str(), X_OK) == 0
        ? installed : std::string(build_path);
}

std::string default_config(const std::string &binary_directory)
{
    const std::string installed =
        binary_directory + "/../" XMIN_INSTALL_DATADIR "/Xmin/desktop/jwmrc";
    return regular_file(installed) ? installed : XMIN_DESKTOP_BUILD_CONFIG;
}

std::string create_session_info()
{
    const char *candidates[]{
        std::getenv("XDG_RUNTIME_DIR"), std::getenv("TMPDIR"), "/tmp"};
    for (const char *directory : candidates) {
        if (directory == nullptr || *directory == '\0')
            continue;
        std::string pattern =
            std::string(directory) + "/xmin-session-XXXXXX";
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        const int descriptor = ::mkstemp(path.data());
        if (descriptor < 0)
            continue;
        if (::fchmod(descriptor, 0600) != 0) {
            const int chmod_error = errno;
            static_cast<void>(::close(descriptor));
            static_cast<void>(::unlink(path.data()));
            errno = chmod_error;
            continue;
        }
        static_cast<void>(::close(descriptor));
        return path.data();
    }
    return {};
}

void usage(std::ostream &stream, std::string_view program)
{
    stream << "usage: " << program
           << " [--server PATH] [--screen WxHxD] [--config FILE]"
              " [--session-info FILE]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string server;
    std::string screen;
    std::string config;
    std::string session_info;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            usage(std::cout, argv[0]);
            return 0;
        }
        if ((argument == "--server" || argument == "--screen" ||
             argument == "--config" || argument == "--session-info") &&
            index + 1 < argc) {
            const std::string value = argv[++index];
            if (argument == "--server") server = value;
            else if (argument == "--screen") screen = value;
            else if (argument == "--config") config = value;
            else session_info = value;
            continue;
        }
        std::cerr << "xmin-session: unknown or incomplete option: "
                  << argument << '\n';
        usage(std::cerr, argv[0]);
        return 2;
    }

    const std::string executable = executable_path(argv[0]);
    const std::string binary_directory = directory_name(executable);
    const std::string run = executable_or_build(
        binary_directory + "/xmin-run", XMIN_RUN_BUILD_PATH);
    const std::string jwm = binary_directory + "/xmin-jwm";
    if (config.empty())
        config = default_config(binary_directory);
    if (session_info.empty()) {
        session_info = create_session_info();
        if (session_info.empty()) {
            std::cerr << "xmin-session: cannot reserve session information: "
                      << std::strerror(errno) << '\n';
            return 125;
        }
    }

    const char *old_path = std::getenv("PATH");
    const std::string new_path = old_path == nullptr || *old_path == '\0'
        ? binary_directory : binary_directory + ":" + old_path;
    if (::setenv("PATH", new_path.c_str(), 1) != 0) {
        std::cerr << "xmin-session: cannot prepare PATH: "
                  << std::strerror(errno) << '\n';
        static_cast<void>(::unlink(session_info.c_str()));
        return 125;
    }
    const std::string bundled_shell =
        binary_directory + "/" XMIN_BUNDLED_SHELL_NAME;
    if (::access(bundled_shell.c_str(), X_OK) == 0 &&
        (::setenv("XMIN_TERMINAL_SHELL", bundled_shell.c_str(), 1) != 0 ||
         ::setenv("SHELL", bundled_shell.c_str(), 1) != 0)) {
        std::cerr << "xmin-session: cannot select bundled shell: "
                  << std::strerror(errno) << '\n';
        static_cast<void>(::unlink(session_info.c_str()));
        return 125;
    }

    std::vector<std::string> arguments{run};
    if (server.empty() && run == XMIN_RUN_BUILD_PATH)
        server = XMIN_SERVER_BUILD_PATH;
    if (!server.empty()) {
        arguments.emplace_back("--server");
        arguments.push_back(server);
    }
    if (!screen.empty()) {
        arguments.emplace_back("--screen");
        arguments.push_back(screen);
    }
    arguments.emplace_back("--session-info");
    arguments.push_back(session_info);
    arguments.emplace_back("--");
    arguments.push_back(jwm);
    arguments.emplace_back("-f");
    arguments.push_back(config);
    std::vector<char *> pointers;
    pointers.reserve(arguments.size() + 1U);
    for (std::string &argument : arguments)
        pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    ::execv(run.c_str(), pointers.data());
    std::cerr << "xmin-session: cannot execute " << run << ": "
              << std::strerror(errno) << '\n';
    static_cast<void>(::unlink(session_info.c_str()));
    return 127;
}
