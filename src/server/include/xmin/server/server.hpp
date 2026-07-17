#ifndef XMIN_SERVER_SERVER_HPP
#define XMIN_SERVER_SERVER_HPP

#include "xmin/server/connection.hpp"
#include "xmin/server/display_socket.hpp"
#include "xmin/server/result.hpp"
#include "xmin/server/server_state.hpp"
#include "xmin/server/unique_fd.hpp"

#include <cstddef>

namespace xmin::server {

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

} // namespace xmin::server

#endif
