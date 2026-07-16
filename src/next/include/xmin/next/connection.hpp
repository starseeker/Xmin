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
    std::uint32_t resource_base = 0x00200000;
    std::vector<std::uint8_t> cookie;
    bool allow_unauthenticated = false;
};

class Connection {
public:
    Connection(UniqueFd socket, ServerConfig config);

    Result<void> prepare();
    Result<void> serve();

    [[nodiscard]] int fd() const noexcept { return socket_.get(); }
    [[nodiscard]] short poll_events() const noexcept;
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    Result<void> on_readable();
    Result<void> on_writable();

private:
    enum class State {
        setup_prefix,
        setup_authentication,
        requests,
    };

    Result<void> process_input();
    Result<bool> process_setup_prefix();
    Result<bool> process_setup_authentication();
    Result<bool> process_request();
    void consume_input(std::size_t size);
    void close_after_output() noexcept;
    Result<void> queue(const std::vector<std::uint8_t> &bytes);
    Result<void> send_setup_failure(ByteOrder order, std::string reason);
    Result<void> send_setup_success(ByteOrder order);
    Result<void> dispatch(ByteOrder order, std::uint8_t opcode,
                          const std::vector<std::uint8_t> &request,
                          std::uint16_t sequence);
    Result<void> send_error(ByteOrder order, std::uint8_t code,
                            std::uint8_t opcode, std::uint16_t sequence,
                            std::uint32_t bad_value = 0);

    UniqueFd socket_;
    ServerConfig config_;
    ResourceRegistry resources_;
    State state_ = State::setup_prefix;
    std::optional<ByteOrder> order_;
    std::vector<std::uint8_t> input_;
    std::vector<std::uint8_t> output_;
    std::size_t output_offset_ = 0;
    std::size_t setup_payload_size_ = 0;
    std::size_t setup_name_size_ = 0;
    std::size_t setup_data_size_ = 0;
    std::size_t setup_padded_name_size_ = 0;
    std::uint16_t sequence_ = 0;
    bool prepared_ = false;
    bool close_after_output_ = false;
    bool finished_ = false;
};

} // namespace xmin::next

#endif
