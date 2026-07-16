#ifndef XMIN_NEXT_CONNECTION_HPP
#define XMIN_NEXT_CONNECTION_HPP

#include "xmin/next/result.hpp"
#include "xmin/next/server_state.hpp"
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
    Connection(UniqueFd socket, ServerConfig config, ServerState &server);
    ~Connection();

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;

    Result<void> prepare();
    Result<void> serve();

    [[nodiscard]] int fd() const noexcept { return socket_.get(); }
    [[nodiscard]] short poll_events() const noexcept;
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    Result<void> on_readable();
    Result<void> on_writable();

private:
    struct RequestContext {
        ByteOrder order;
        std::uint8_t opcode;
        std::uint8_t data;
        std::uint16_t sequence;
        const std::vector<std::uint8_t> &request;
    };

    using RequestHandler = Result<void> (Connection::*)(const RequestContext &);

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
    Result<void> drain_pending_events();
    [[nodiscard]] std::vector<std::uint8_t>
    encode_event(const ClientEvent &event) const;
    Result<void> send_setup_failure(ByteOrder order, std::string reason);
    Result<void> send_setup_success(ByteOrder order);
    Result<void> dispatch(const RequestContext &context);
    Result<void> handle_create_window(const RequestContext &context);
    Result<void> handle_get_window_attributes(const RequestContext &context);
    Result<void> handle_destroy_window(const RequestContext &context);
    Result<void> handle_map_window(const RequestContext &context);
    Result<void> handle_unmap_window(const RequestContext &context);
    Result<void> handle_configure_window(const RequestContext &context);
    Result<void> handle_get_geometry(const RequestContext &context);
    Result<void> handle_query_tree(const RequestContext &context);
    Result<void> handle_intern_atom(const RequestContext &context);
    Result<void> handle_get_atom_name(const RequestContext &context);
    Result<void> handle_change_property(const RequestContext &context);
    Result<void> handle_delete_property(const RequestContext &context);
    Result<void> handle_get_property(const RequestContext &context);
    Result<void> handle_list_properties(const RequestContext &context);
    Result<void> handle_set_selection_owner(const RequestContext &context);
    Result<void> handle_get_selection_owner(const RequestContext &context);
    Result<void> handle_send_event(const RequestContext &context);
    Result<void> handle_translate_coordinates(const RequestContext &context);
    Result<void> handle_create_pixmap(const RequestContext &context);
    Result<void> handle_free_pixmap(const RequestContext &context);
    Result<void> handle_create_graphics_context(
        const RequestContext &context);
    Result<void> handle_free_graphics_context(const RequestContext &context);
    Result<void> handle_copy_area(const RequestContext &context);
    Result<void> handle_fill_rectangles(const RequestContext &context);
    Result<void> handle_get_image(const RequestContext &context);
    Result<void> handle_alloc_named_color(const RequestContext &context);
    Result<void> handle_query_colors(const RequestContext &context);
    Result<void> handle_get_input_focus(const RequestContext &context);
    Result<void> handle_query_extension(const RequestContext &context);
    Result<void> handle_list_extensions(const RequestContext &context);
    Result<void> handle_no_operation(const RequestContext &context);
    Result<void> send_error(ByteOrder order, std::uint8_t code,
                            std::uint8_t opcode, std::uint16_t sequence,
                            std::uint32_t bad_value = 0);

    UniqueFd socket_;
    ServerConfig config_;
    ServerState &server_;
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
