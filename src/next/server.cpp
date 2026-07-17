#include "xmin/next/server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace xmin::next {
namespace {

volatile sig_atomic_t signal_descriptor = -1;

void
wake_for_signal(int signal_number)
{
    const int saved_errno = errno;
    const std::uint8_t byte = static_cast<std::uint8_t>(signal_number);
    if (signal_descriptor >= 0) {
        static_cast<void>(::write(
            static_cast<int>(signal_descriptor), &byte, sizeof(byte)));
    }
    errno = saved_errno;
}

Result<void>
io_failure(std::string operation)
{
    return Result<void>::failure(
        ErrorCode::io, std::move(operation) + ": " + std::strerror(errno));
}

Result<void>
set_pipe_flags(int descriptor)
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

Result<std::array<UniqueFd, 2>>
make_signal_pipe()
{
    int descriptors[2];
    if (::pipe(descriptors) != 0) {
        return Result<std::array<UniqueFd, 2>>::failure(
            ErrorCode::io, std::string("pipe: ") + std::strerror(errno));
    }
    std::array<UniqueFd, 2> pipe{
        UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
    for (const auto &descriptor : pipe) {
        auto flags = set_pipe_flags(descriptor.get());
        if (!flags) {
            return Result<std::array<UniqueFd, 2>>::failure(
                flags.error().code, flags.error().message);
        }
    }
    return Result<std::array<UniqueFd, 2>>::success(std::move(pipe));
}

class SignalHandlers {
public:
    static Result<SignalHandlers> install(int descriptor)
    {
        SignalHandlers handlers;
        struct sigaction action {};
        action.sa_handler = wake_for_signal;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (::sigaction(SIGTERM, &action, &handlers.old_term_) != 0) {
            return Result<SignalHandlers>::failure(
                ErrorCode::io,
                std::string("sigaction(SIGTERM): ") + std::strerror(errno));
        }
        handlers.term_installed_ = true;
        if (::sigaction(SIGINT, &action, &handlers.old_int_) != 0) {
            const int install_error = errno;
            static_cast<void>(
                ::sigaction(SIGTERM, &handlers.old_term_, nullptr));
            handlers.term_installed_ = false;
            errno = install_error;
            return Result<SignalHandlers>::failure(
                ErrorCode::io,
                std::string("sigaction(SIGINT): ") + std::strerror(errno));
        }
        handlers.int_installed_ = true;
        signal_descriptor = descriptor;
        return Result<SignalHandlers>::success(std::move(handlers));
    }

    SignalHandlers(SignalHandlers &&other) noexcept
        : old_term_(other.old_term_),
          old_int_(other.old_int_),
          term_installed_(std::exchange(other.term_installed_, false)),
          int_installed_(std::exchange(other.int_installed_, false))
    {}

    SignalHandlers &operator=(SignalHandlers &&) = delete;
    SignalHandlers(const SignalHandlers &) = delete;
    SignalHandlers &operator=(const SignalHandlers &) = delete;

    ~SignalHandlers()
    {
        if (!int_installed_ && !term_installed_)
            return;
        signal_descriptor = -1;
        if (int_installed_)
            static_cast<void>(::sigaction(SIGINT, &old_int_, nullptr));
        if (term_installed_)
            static_cast<void>(::sigaction(SIGTERM, &old_term_, nullptr));
    }

private:
    SignalHandlers() = default;

    struct sigaction old_term_ {};
    struct sigaction old_int_ {};
    bool term_installed_ = false;
    bool int_installed_ = false;
};

struct Client {
    std::size_t slot;
    std::unique_ptr<Connection> connection;
};

std::optional<std::size_t>
free_slot(const std::vector<bool> &slots)
{
    const auto found = std::find(slots.begin(), slots.end(), false);
    if (found == slots.end())
        return std::nullopt;
    return static_cast<std::size_t>(found - slots.begin());
}

void
drain_signal_pipe(int descriptor)
{
    std::array<std::uint8_t, 64> bytes{};
    for (;;) {
        const auto count = ::read(descriptor, bytes.data(), bytes.size());
        if (count > 0)
            continue;
        if (count < 0 && errno == EINTR)
            continue;
        return;
    }
}

} // namespace

Server::Server(DisplaySocket listener, ServerConfig config,
               std::size_t maximum_clients, UniqueFd display_notification)
    : listener_(std::move(listener)),
      config_(std::move(config)),
      state_(config_.width, config_.height),
      maximum_clients_(maximum_clients),
      display_notification_(std::move(display_notification))
{}

Result<void>
Server::run()
{
    if (!state_.valid()) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            "screen surface exceeds its size limit");
    }
    if (maximum_clients_ == 0 || maximum_clients_ > 127) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            "maximum clients must be between 1 and 127");
    }

    auto pipe = make_signal_pipe();
    if (!pipe) {
        return Result<void>::failure(pipe.error().code, pipe.error().message);
    }
    auto handlers = SignalHandlers::install(pipe.value()[1].get());
    if (!handlers) {
        return Result<void>::failure(
            handlers.error().code, handlers.error().message);
    }

    if (display_notification_) {
        auto notified = listener_.notify_display(display_notification_.get());
        display_notification_.reset();
        if (!notified)
            return notified;
    }

    std::vector<Client> clients;
    clients.reserve(maximum_clients_);
    std::vector<bool> slots(maximum_clients_, false);

    for (;;) {
        static_cast<void>(state_.process_timers());
        std::vector<pollfd> descriptors;
        descriptors.reserve(2 + clients.size());
        descriptors.push_back(pollfd{listener_.fd(), POLLIN, 0});
        descriptors.push_back(pollfd{pipe.value()[0].get(), POLLIN, 0});
        const std::uint32_t grab_owner = state_.server_grab_owner();
        for (const auto &client : clients) {
            short events = client.connection->poll_events();
            if (grab_owner != 0 &&
                client.connection->client_id() != grab_owner) {
                events &= static_cast<short>(~POLLIN);
            }
            descriptors.push_back(pollfd{
                client.connection->fd(), events, 0});
        }
        const std::size_t polled_client_count = clients.size();

        int ready;
        do {
            ready = ::poll(
                descriptors.data(),
                static_cast<nfds_t>(descriptors.size()),
                state_.timer_timeout_milliseconds());
        } while (ready < 0 && errno == EINTR);
        if (ready < 0)
            return io_failure("poll");

        if ((descriptors[1].revents & POLLIN) != 0) {
            drain_signal_pipe(pipe.value()[0].get());
            return Result<void>::success();
        }
        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return Result<void>::failure(ErrorCode::io,
                                         "signal pipe failed");
        }

        if ((descriptors[0].revents & POLLIN) != 0) {
            for (;;) {
                auto accepted = listener_.accept_client();
                if (!accepted) {
                    return Result<void>::failure(
                        accepted.error().code, accepted.error().message);
                }
                if (!accepted.value())
                    break;
                const auto slot = free_slot(slots);
                if (!slot)
                    continue;

                ServerConfig client_config = config_;
                client_config.resource_base = static_cast<std::uint32_t>(
                    (*slot + 1U) * 0x00200000U);
                auto connection = std::make_unique<Connection>(
                    std::move(*accepted.value()), std::move(client_config),
                    state_);
                auto prepared = connection->prepare();
                if (!prepared) {
                    std::cerr << "Xmin-next: rejected client: "
                              << prepared.error().message << '\n';
                    continue;
                }
                slots[*slot] = true;
                clients.push_back(Client{*slot, std::move(connection)});
            }
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return Result<void>::failure(ErrorCode::io,
                                         "display listener failed");
        }

        std::vector<bool> remove(clients.size(), false);
        std::vector<std::size_t> dispatch_order(polled_client_count);
        for (std::size_t index = 0; index < polled_client_count; ++index)
            dispatch_order[index] = index;
        std::stable_sort(
            dispatch_order.begin(), dispatch_order.end(),
            [this, &clients](std::size_t left, std::size_t right) {
                return state_.sync_priority(
                           clients[left].connection->client_id()) >
                    state_.sync_priority(
                           clients[right].connection->client_id());
            });
        for (const auto index : dispatch_order) {
            const short events = descriptors[index + 2].revents;
            const std::uint32_t current_grab = state_.server_grab_owner();
            const bool may_process = current_grab == 0 ||
                current_grab == clients[index].connection->client_id();
            Result<void> operation = Result<void>::success();
            if ((events & POLLNVAL) != 0) {
                operation = Result<void>::failure(
                    ErrorCode::io, "client descriptor became invalid");
            }
            else if (!may_process && (events & POLLHUP) != 0) {
                remove[index] = true;
                continue;
            }
            else {
                if (may_process && (events & POLLOUT) != 0) {
                    operation = clients[index].connection->on_writable();
                }
                if (operation && !clients[index].connection->finished() &&
                    may_process &&
                    (events & (POLLIN | POLLHUP)) != 0) {
                    operation = clients[index].connection->on_readable();
                }
                if (operation && !clients[index].connection->finished() &&
                    (events & POLLHUP) != 0 &&
                    (events & POLLOUT) == 0) {
                    remove[index] = true;
                    continue;
                }
                if (operation && (events & POLLERR) != 0 &&
                    (events & (POLLIN | POLLOUT | POLLHUP)) == 0) {
                    operation = Result<void>::failure(
                        ErrorCode::io, "client socket reported an error");
                }
            }
            if (!operation) {
                std::cerr << "Xmin-next: closing client: "
                          << operation.error().message << '\n';
                remove[index] = true;
            }
            else if (clients[index].connection->finished()) {
                remove[index] = true;
            }
            if (state_.client_termination_requested(
                    clients[index].connection->client_id())) {
                remove[index] = true;
            }
        }

        for (std::size_t index = clients.size(); index > 0; --index) {
            const std::size_t candidate = index - 1;
            if (!remove[candidate])
                continue;
            slots[clients[candidate].slot] = false;
            clients.erase(
                clients.begin() + static_cast<std::ptrdiff_t>(candidate));
        }
    }
}

} // namespace xmin::next
