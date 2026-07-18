#ifndef XMIN_CLIENT_X11_CONNECTION_HPP
#define XMIN_CLIENT_X11_CONNECTION_HPP

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include "xmin/server/unique_fd.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xmin::client::x11 {

struct Packet {
    std::vector<std::uint8_t> bytes;
    std::vector<int> descriptors;
    std::uint64_t sequence = 0;
};

class Connection {
public:
    static std::unique_ptr<Connection>
    connect(const char *display_name, int *screen_number);
    static std::unique_ptr<Connection>
    connect_to_fd(int descriptor, const xcb_auth_info_t *authorization);

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection();

    [[nodiscard]] int error() const noexcept;
    [[nodiscard]] int descriptor() const noexcept;
    [[nodiscard]] const xcb_setup_t *setup() const noexcept;
    [[nodiscard]] std::uint32_t maximum_request_length();
    [[nodiscard]] std::uint32_t generate_id() noexcept;

    std::uint64_t send(
        int flags, const struct iovec *vectors,
        const xcb_protocol_request_t &request,
        const int *descriptors = nullptr, std::size_t descriptor_count = 0);

    void *wait_for_reply(std::uint64_t sequence, xcb_generic_error_t **error);
    bool poll_for_reply(
        std::uint64_t sequence, void **reply, xcb_generic_error_t **error);
    xcb_generic_event_t *wait_for_event();
    xcb_generic_event_t *poll_for_event();
    xcb_generic_error_t *request_check(std::uint64_t sequence);
    void discard_reply(std::uint64_t sequence);

    const xcb_query_extension_reply_t *extension(xcb_extension_t *extension);

private:
    explicit Connection(int error_code = XCB_CONN_ERROR);

    bool open_display(const char *display_name, int *screen_number);
    bool perform_setup(const xcb_auth_info_t *authorization);
    void start_reader();
    void reader_loop();
    bool read_packet(Packet &packet);
    void queue_packet(Packet packet);
    std::uint64_t widen_sequence(std::uint16_t sequence) const noexcept;

    std::uint64_t send_bytes(
        std::vector<std::uint8_t> bytes, int flags, bool expects_reply,
        const int *descriptors, std::size_t descriptor_count);
    std::uint64_t send_core_reply_request(
        std::uint8_t opcode, std::uint8_t data,
        std::vector<std::uint8_t> body);

    static void *allocate_reply(Packet packet);
    static xcb_generic_event_t *allocate_event(Packet packet);
    static xcb_generic_error_t *allocate_error(Packet packet);

    server::UniqueFd socket_;
    std::vector<std::uint8_t> setup_bytes_;
    std::atomic<int> error_code_{XCB_CONN_ERROR};
    std::atomic<bool> stopping_{false};
    std::thread reader_;

    mutable std::mutex send_mutex_;
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint32_t> next_resource_{1};
    std::atomic<std::uint32_t> maximum_request_length_{0};

    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::unordered_map<std::uint64_t, Packet> replies_;
    std::unordered_map<std::uint64_t, Packet> errors_;
    std::deque<Packet> events_;
    std::unordered_set<std::uint64_t> checked_requests_;
    std::unordered_set<std::uint64_t> reply_requests_;
    std::unordered_set<std::uint64_t> discarded_replies_;
    std::map<std::string, xcb_query_extension_reply_t> extensions_;
};

} // namespace xmin::client::x11

struct xcb_connection_t {
    std::unique_ptr<xmin::client::x11::Connection> implementation;
};

#endif
