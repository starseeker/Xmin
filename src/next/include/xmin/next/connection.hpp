#ifndef XMIN_NEXT_CONNECTION_HPP
#define XMIN_NEXT_CONNECTION_HPP

#include "xmin/next/resource_registry.hpp"
#include "xmin/next/result.hpp"
#include "xmin/next/unique_fd.hpp"
#include "xmin/next/wire.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xmin::next {

struct ServerConfig {
    std::uint16_t width = 1280;
    std::uint16_t height = 1024;
    std::vector<std::uint8_t> cookie;
    bool allow_unauthenticated = false;
};

class Connection {
public:
    Connection(UniqueFd socket, ServerConfig config);

    Result<void> serve();

private:
    Result<bool> read_exact(std::uint8_t *destination, std::size_t size);
    Result<void> write_all(const std::vector<std::uint8_t> &bytes);
    Result<std::optional<ByteOrder>> perform_setup();
    Result<void> send_setup_failure(ByteOrder order, std::string reason);
    Result<void> send_setup_success(ByteOrder order);
    Result<void> serve_requests(ByteOrder order);
    Result<void> dispatch(ByteOrder order, std::uint8_t opcode,
                          const std::vector<std::uint8_t> &request,
                          std::uint16_t sequence);
    Result<void> send_error(ByteOrder order, std::uint8_t code,
                            std::uint8_t opcode, std::uint16_t sequence,
                            std::uint32_t bad_value = 0);

    UniqueFd socket_;
    ServerConfig config_;
    ResourceRegistry resources_;
};

} // namespace xmin::next

#endif
