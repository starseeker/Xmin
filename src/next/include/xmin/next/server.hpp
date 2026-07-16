#ifndef XMIN_NEXT_SERVER_HPP
#define XMIN_NEXT_SERVER_HPP

#include "xmin/next/connection.hpp"
#include "xmin/next/display_socket.hpp"
#include "xmin/next/result.hpp"
#include "xmin/next/server_state.hpp"
#include "xmin/next/unique_fd.hpp"

#include <cstddef>

namespace xmin::next {

class Server {
public:
    Server(DisplaySocket listener, ServerConfig config,
           std::size_t maximum_clients, UniqueFd display_notification = {});

    Result<void> run();

private:
    DisplaySocket listener_;
    ServerConfig config_;
    ServerState state_;
    std::size_t maximum_clients_ = 0;
    UniqueFd display_notification_;
};

} // namespace xmin::next

#endif
