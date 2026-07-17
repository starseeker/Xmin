#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/color.hpp"
#include "xmin/server/core_raster.hpp"
#include "xmin/server/extension_registry.hpp"
#include "xmin/server/generated/core_protocol.hpp"
#include "xmin/server/property_data.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace xmin::server {
namespace {

constexpr std::uint16_t protocol_major = 11;
constexpr std::uint16_t protocol_minor = 0;
constexpr std::size_t maximum_core_request_units = 65535U;
constexpr std::size_t maximum_extended_request_units = 262144U;
constexpr std::size_t maximum_core_request_bytes =
    maximum_core_request_units * 4U;
constexpr std::size_t maximum_extended_request_bytes =
    maximum_extended_request_units * 4U;
constexpr std::size_t maximum_buffered_input =
    maximum_extended_request_bytes + 16384U;
constexpr std::size_t maximum_buffered_output = 1024U * 1024U;
constexpr std::size_t maximum_pending_descriptors = 16;
// SCM_RIGHTS payloads use the socket ABI's word alignment.  In particular,
// FreeBSD's cmsghdr type itself has four-byte alignment on 64-bit systems,
// while CMSG_DATA is still aligned to an eight-byte machine word.
constexpr std::size_t ancillary_alignment = sizeof(std::size_t);
constexpr std::size_t
align_ancillary(std::size_t size) noexcept
{
    return (size + ancillary_alignment - 1) & ~(ancillary_alignment - 1);
}
constexpr std::size_t
ancillary_length(std::size_t payload) noexcept
{
    return align_ancillary(sizeof(cmsghdr)) + payload;
}
constexpr std::size_t
ancillary_space(std::size_t payload) noexcept
{
    return align_ancillary(sizeof(cmsghdr)) + align_ancillary(payload);
}
constexpr std::string_view cookie_protocol = "MIT-MAGIC-COOKIE-1";
constexpr std::uint32_t all_event_masks = 0x01ffffffU;
constexpr std::uint32_t propagate_event_masks = 0x00003f4fU;
constexpr std::uint32_t exclusive_event_masks =
    (1U << 2) | (1U << 18) | (1U << 20); // ButtonPress/redirect masks
constexpr std::uint32_t input_only_attribute_masks =
    (1U << 5) | (1U << 9) | (1U << 11) | (1U << 12) | (1U << 14);
constexpr std::uint32_t all_graphics_context_values = 0x007fffffU;
constexpr std::uint16_t pointer_grab_mask = 0x7ffcU;

bool
valid_passive_modifiers(std::uint16_t modifiers) noexcept
{
    return modifiers == any_modifier ||
        (modifiers & ~all_modifiers_mask) == 0;
}

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_pixmap = 4,
    bad_atom = 5,
    bad_cursor = 6,
    bad_font = 7,
    bad_match = 8,
    bad_drawable = 9,
    bad_access = 10,
    bad_alloc = 11,
    bad_colormap = 12,
    bad_graphics_context = 13,
    bad_id_choice = 14,
    bad_name = 15,
    bad_length = 16,
};

constexpr std::size_t
opcode_index(CoreOpcode opcode) noexcept
{
    return static_cast<std::uint8_t>(opcode);
}

std::int16_t
signed_word(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

std::int16_t
signed_byte(std::uint8_t value) noexcept
{
    const std::int16_t widened = value;
    return widened <= std::numeric_limits<std::int8_t>::max()
        ? widened
        : static_cast<std::int16_t>(widened - 256);
}

std::optional<Region>
bitmap_clip_region(const Surface &surface)
{
    std::vector<Rectangle> rectangles;
    try {
        for (std::uint16_t y = 0; y < surface.height(); ++y) {
            std::uint16_t x = 0;
            while (x < surface.width()) {
                while (x < surface.width() && surface.pixel(x, y) == 0)
                    ++x;
                const std::uint16_t start = x;
                while (x < surface.width() && surface.pixel(x, y) != 0)
                    ++x;
                if (x == start)
                    continue;
                if (rectangles.size() == maximum_shape_rectangles)
                    return std::nullopt;
                rectangles.push_back(
                    Rectangle{start, y, static_cast<std::uint32_t>(x - start),
                              1});
            }
        }
    }
    catch (const std::bad_alloc &) {
        return std::nullopt;
    }
    Region result;
    if (!Region::canonicalize(rectangles, result))
        return std::nullopt;
    return result;
}

bool
key_is_pressed(const InputState &input, std::uint8_t keycode) noexcept
{
    return (input.pressed_keys[keycode >> 3] &
            (1U << (keycode & 7U))) != 0;
}

std::int32_t
signed_dword(std::uint32_t value) noexcept
{
    const std::int64_t widened = value;
    return static_cast<std::int32_t>(
        widened <= std::numeric_limits<std::int32_t>::max()
            ? widened
            : widened - (std::int64_t{1} << 32));
}

std::int64_t
signed_qword(std::uint64_t value) noexcept
{
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (value <= signed_max)
        return static_cast<std::int64_t>(value);
    return -1 - static_cast<std::int64_t>(
        std::numeric_limits<std::uint64_t>::max() - value);
}

std::optional<std::int64_t>
read_sync_int64(WireReader &reader)
{
    const auto high = reader.u32();
    const auto low = reader.u32();
    if (!high || !low)
        return std::nullopt;
    return signed_qword(
        (static_cast<std::uint64_t>(*high) << 32) | *low);
}

void
write_sync_int64(WireWriter &writer, std::int64_t value)
{
    const auto bits = static_cast<std::uint64_t>(value);
    writer.u32(static_cast<std::uint32_t>(bits >> 32));
    writer.u32(static_cast<std::uint32_t>(bits));
}

std::int16_t
wire_coordinate(std::int32_t value) noexcept
{
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(
        value, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

std::uint16_t
wire_size(std::uint32_t value) noexcept
{
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(
        value, std::numeric_limits<std::uint16_t>::max()));
}

std::optional<RegionOperation>
region_operation(std::uint8_t operation) noexcept
{
    if (operation > static_cast<std::uint8_t>(RegionOperation::invert))
        return std::nullopt;
    return static_cast<RegionOperation>(operation);
}

bool
make_default_shape(const WindowRecord &window, std::uint8_t kind,
                   Region &result)
{
    try {
        const std::vector<Rectangle> rectangles{
            window.default_shape(kind)};
        return Region::canonicalize(rectangles, result);
    }
    catch (const std::bad_alloc &) {
        return false;
    }
}

bool
timestamp_later(std::uint32_t left, std::uint32_t right) noexcept
{
    const std::uint32_t difference = left - right;
    return difference != 0 && difference < 0x80000000U;
}

bool
timestamp_earlier(std::uint32_t left, std::uint32_t right) noexcept
{
    return timestamp_later(right, left);
}

bool
valid_clip_order(const std::vector<Rectangle> &rectangles,
                 std::uint8_t ordering) noexcept
{
    for (std::size_t index = 1; index < rectangles.size(); ++index) {
        const auto &previous = rectangles[index - 1];
        const auto &current = rectangles[index];
        if (ordering == 1 && current.y < previous.y)
            return false;
        if (ordering == 2 &&
            (current.y < previous.y ||
             (current.y == previous.y && current.x < previous.x))) {
            return false;
        }
        if (ordering == 3) {
            if (current.y != previous.y &&
                current.y < static_cast<std::int64_t>(previous.y) +
                        previous.height) {
                return false;
            }
            if (current.y == previous.y &&
                (current.height != previous.height ||
                 current.x < static_cast<std::int64_t>(previous.x) +
                         previous.width)) {
                return false;
            }
        }
    }
    return true;
}

bool
cookies_equal(const std::vector<std::uint8_t> &left,
              const std::vector<std::uint8_t> &right) noexcept
{
    if (left.size() != right.size())
        return false;
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
        difference |= left[index] ^ right[index];
    return difference == 0;
}

std::uint16_t
millimetres(std::uint16_t pixels) noexcept
{
    const auto tenths = static_cast<std::uint32_t>(pixels) * 254U;
    return static_cast<std::uint16_t>((tenths + 480U) / 960U);
}

Result<void>
malformed(std::string message)
{
    return Result<void>::failure(ErrorCode::malformed, std::move(message));
}

Result<void>
io_failure(std::string_view operation)
{
    return Result<void>::failure(
        ErrorCode::io,
        std::string(operation) + ": " + std::strerror(errno));
}

} // namespace

Connection::Connection(UniqueFd socket, ServerConfig config,
                       ServerState &server)
    : socket_(std::move(socket)), config_(std::move(config)), server_(server)
{}

Connection::~Connection()
{
    server_.disconnect_client(config_.resource_base);
}

Result<void>
Connection::prepare()
{
    if (prepared_)
        return Result<void>::success();
    if (!socket_)
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "invalid client socket");
    if (!server_.valid())
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "screen surface exceeds its size limit");

    const int status_flags = ::fcntl(socket_.get(), F_GETFL);
    if (status_flags < 0)
        return io_failure("fcntl(F_GETFL)");
    if (::fcntl(socket_.get(), F_SETFL, status_flags | O_NONBLOCK) < 0)
        return io_failure("fcntl(F_SETFL)");

    const int descriptor_flags = ::fcntl(socket_.get(), F_GETFD);
    if (descriptor_flags < 0)
        return io_failure("fcntl(F_GETFD)");
    if (::fcntl(socket_.get(), F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
        return io_failure("fcntl(F_SETFD)");

    prepared_ = true;
    return Result<void>::success();
}

short
Connection::poll_events() const noexcept
{
    if (finished_)
        return 0;
    short events = 0;
    const bool sync_waiting = state_ == State::requests &&
        server_.sync_waiting(config_.resource_base);
    if (!close_after_output_ && !sync_waiting)
        events |= POLLIN;
    if (output_offset_ < output_.size())
        events |= POLLOUT;
    if (!close_after_output_ && state_ == State::requests && order_ &&
        server_.has_pending_event(config_.resource_base)) {
        events |= POLLOUT;
    }
    if (!close_after_output_ && resume_sync_input_ && !sync_waiting &&
        !input_.empty()) {
        events |= POLLOUT;
    }
    return events;
}

void
Connection::consume_input(std::size_t size)
{
    input_.erase(
        input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(size));
}

void
Connection::close_after_output() noexcept
{
    close_after_output_ = true;
    if (output_offset_ == output_.size())
        finished_ = true;
}

Result<void>
Connection::queue(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.empty())
        return Result<void>::success();

    compact_output();

    const auto new_size = checked_add(output_.size(), bytes.size());
    if (!new_size || *new_size > maximum_buffered_output) {
        return Result<void>::failure(
            ErrorCode::io, "connection output buffer limit exceeded");
    }
    try {
        output_.reserve(*new_size);
        output_.insert(output_.end(), bytes.begin(), bytes.end());
    }
    catch (const std::bad_alloc &) {
        return Result<void>::failure(
            ErrorCode::io, "connection output buffer allocation failed");
    }
    return Result<void>::success();
}

void
Connection::compact_output()
{
    if (output_offset_ == 0)
        return;

    if (output_offset_ == output_.size()) {
        output_.clear();
        output_offset_ = 0;
        pending_output_fds_.clear();
    }
    else {
        const std::size_t consumed = output_offset_;
        output_.erase(
            output_.begin(),
            output_.begin() + static_cast<std::ptrdiff_t>(output_offset_));
        output_offset_ = 0;
        for (auto &pending : pending_output_fds_)
            pending.offset -= consumed;
    }
}

Result<void>
Connection::queue_with_fd(const std::vector<std::uint8_t> &bytes, UniqueFd fd)
{
    if (bytes.empty() || !fd) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            "descriptor output requires bytes and a valid descriptor");
    }
    compact_output();
    const auto new_size = checked_add(output_.size(), bytes.size());
    if (!new_size || *new_size > maximum_buffered_output) {
        return Result<void>::failure(
            ErrorCode::io, "connection output buffer limit exceeded");
    }
    const std::size_t offset = output_.size();
    try {
        output_.reserve(*new_size);
        pending_output_fds_.push_back(PendingOutputFd{offset, std::move(fd)});
    }
    catch (const std::bad_alloc &) {
        return Result<void>::failure(
            ErrorCode::io, "descriptor output queue allocation failed");
    }
    // reserve() above makes insertion of byte values non-throwing, preserving
    // the descriptor marker and byte stream as one transaction.
    output_.insert(output_.end(), bytes.begin(), bytes.end());
    return Result<void>::success();
}

UniqueFd
Connection::take_received_fd() noexcept
{
    if (received_fds_.empty())
        return {};
    UniqueFd result = std::move(received_fds_.front());
    received_fds_.pop_front();
    return result;
}

std::vector<std::uint8_t>
Connection::encode_event(const ClientEvent &event) const
{
    if (!order_)
        return {};
    WireWriter writer(*order_);
    if (const auto *clear = std::get_if<SelectionClearEvent>(&event)) {
        writer.u8(29); // SelectionClear
        writer.u8(0);
        writer.u16(clear->sequence);
        writer.u32(clear->time);
        writer.u32(clear->window);
        writer.u32(clear->selection);
        writer.pad(16);
        return writer.data();
    }

    if (const auto *request = std::get_if<SelectionRequestEvent>(&event)) {
        writer.u8(30); // SelectionRequest
        writer.u8(0);
        writer.u16(request->sequence);
        writer.u32(request->time);
        writer.u32(request->owner);
        writer.u32(request->requestor);
        writer.u32(request->selection);
        writer.u32(request->target);
        writer.u32(request->property);
        return writer.data();
    }

    if (const auto *notify = std::get_if<SelectionNotifyEvent>(&event)) {
        writer.u8(31); // SelectionNotify
        writer.u8(0);
        writer.u16(notify->sequence);
        writer.u32(notify->time);
        writer.u32(notify->requestor);
        writer.u32(notify->selection);
        writer.u32(notify->target);
        writer.u32(notify->property);
        writer.pad(4);
        return writer.data();
    }

    if (const auto *message = std::get_if<ClientMessageEvent>(&event)) {
        writer.u8(33 | 0x80U); // synthetic ClientMessage
        writer.u8(message->format);
        writer.u16(message->sequence);
        writer.u32(message->window);
        writer.u32(message->type);
        const std::size_t count = 160U / message->format;
        for (std::size_t index = 0; index < count; ++index) {
            if (message->format == 8)
                writer.u8(static_cast<std::uint8_t>(message->data[index]));
            else if (message->format == 16)
                writer.u16(static_cast<std::uint16_t>(message->data[index]));
            else
                writer.u32(message->data[index]);
        }
        return writer.data();
    }

    if (const auto *mapping = std::get_if<MappingNotifyEvent>(&event)) {
        writer.u8(34); // MappingNotify
        writer.u8(0);
        writer.u16(mapping->sequence);
        writer.u8(mapping->request);
        writer.u8(mapping->first_keycode);
        writer.u8(mapping->count);
        writer.pad(25);
        return writer.data();
    }

    if (const auto *shape = std::get_if<ShapeNotifyEvent>(&event)) {
        writer.u8(shape_extension.first_event);
        writer.u8(shape->kind);
        writer.u16(shape->sequence);
        writer.u32(shape->window);
        writer.i16(shape->x);
        writer.i16(shape->y);
        writer.u16(shape->width);
        writer.u16(shape->height);
        writer.u32(shape->time);
        writer.u8(shape->shaped ? 1 : 0);
        writer.pad(11);
        return writer.data();
    }

    if (const auto *counter = std::get_if<SyncCounterNotifyEvent>(&event)) {
        writer.u8(sync_extension.first_event);
        writer.u8(0); // CounterNotify
        writer.u16(counter->sequence);
        writer.u32(counter->counter);
        write_sync_int64(writer, counter->wait_value);
        write_sync_int64(writer, counter->counter_value);
        writer.u32(counter->time);
        writer.u16(counter->count);
        writer.u8(counter->destroyed ? 1 : 0);
        writer.u8(0);
        return writer.data();
    }

    if (const auto *alarm = std::get_if<SyncAlarmNotifyEvent>(&event)) {
        writer.u8(sync_extension.first_event + 1);
        writer.u8(1); // AlarmNotify
        writer.u16(alarm->sequence);
        writer.u32(alarm->alarm);
        write_sync_int64(writer, alarm->counter_value);
        write_sync_int64(writer, alarm->alarm_value);
        writer.u32(alarm->time);
        writer.u8(alarm->state);
        writer.pad(3);
        return writer.data();
    }

    if (const auto *selection =
            std::get_if<XFixesSelectionNotifyEvent>(&event)) {
        writer.u8(xfixes_extension.first_event);
        writer.u8(selection->subtype);
        writer.u16(selection->sequence);
        writer.u32(selection->window);
        writer.u32(selection->owner);
        writer.u32(selection->selection);
        writer.u32(selection->time);
        writer.u32(selection->selection_time);
        writer.pad(8);
        return writer.data();
    }

    if (const auto *cursor = std::get_if<XFixesCursorNotifyEvent>(&event)) {
        writer.u8(xfixes_extension.first_event + 1);
        writer.u8(cursor->subtype);
        writer.u16(cursor->sequence);
        writer.u32(cursor->window);
        writer.u32(cursor->cursor_serial);
        writer.u32(cursor->time);
        writer.u32(cursor->name);
        writer.pad(12);
        return writer.data();
    }

    if (const auto *screen =
            std::get_if<RandrScreenChangeNotifyEvent>(&event)) {
        writer.u8(randr_extension.first_event);
        writer.u8(static_cast<std::uint8_t>(screen->rotation));
        writer.u16(screen->sequence);
        writer.u32(screen->timestamp);
        writer.u32(screen->config_timestamp);
        writer.u32(root_window_id);
        writer.u32(screen->request_window);
        writer.u16(0); // the sole screen size
        writer.u16(0); // unknown subpixel order
        writer.u16(screen->width);
        writer.u16(screen->height);
        writer.u16(screen->millimetre_width);
        writer.u16(screen->millimetre_height);
        return writer.data();
    }

    if (const auto *notify = std::get_if<RandrNotifyEvent>(&event)) {
        writer.u8(randr_extension.first_event + 1);
        writer.u8(notify->subtype);
        writer.u16(notify->sequence);
        switch (notify->subtype) {
        case 0: // CrtcChange
            writer.u32(notify->timestamp);
            writer.u32(notify->window);
            writer.u32(notify->crtc);
            writer.u32(notify->mode);
            writer.u16(notify->rotation);
            writer.pad(2);
            writer.i16(notify->x);
            writer.i16(notify->y);
            writer.u16(notify->width);
            writer.u16(notify->height);
            break;
        case 1: // OutputChange
            writer.u32(notify->timestamp);
            writer.u32(notify->config_timestamp);
            writer.u32(notify->window);
            writer.u32(notify->output);
            writer.u32(notify->crtc);
            writer.u32(notify->mode);
            writer.u16(notify->rotation);
            writer.u8(notify->connection);
            writer.u8(notify->subpixel_order);
            break;
        case 2: // OutputProperty
            writer.u32(notify->window);
            writer.u32(notify->output);
            writer.u32(notify->atom);
            writer.u32(notify->timestamp);
            writer.u8(notify->property_status);
            writer.pad(11);
            break;
        case 5: // ResourceChange
            writer.u32(notify->timestamp);
            writer.u32(notify->window);
            writer.pad(20);
            break;
        default:
            return {};
        }
        return writer.data();
    }

    if (const auto *damage = std::get_if<DamageNotifyEvent>(&event)) {
        writer.u8(damage_extension.first_event);
        writer.u8(damage->level);
        writer.u16(damage->sequence);
        writer.u32(damage->drawable);
        writer.u32(damage->damage);
        writer.u32(damage->timestamp);
        writer.i16(damage->area_x);
        writer.i16(damage->area_y);
        writer.u16(damage->area_width);
        writer.u16(damage->area_height);
        writer.i16(damage->geometry_x);
        writer.i16(damage->geometry_y);
        writer.u16(damage->geometry_width);
        writer.u16(damage->geometry_height);
        return writer.data();
    }

    if (const auto *configure =
            std::get_if<PresentConfigureNotifyEvent>(&event)) {
        writer.u8(35); // GenericEvent
        writer.u8(present_extension.major_opcode);
        writer.u16(configure->sequence);
        writer.u32(2); // two words beyond the generic 32-byte event
        writer.u16(0); // ConfigureNotify
        writer.pad(2);
        writer.u32(configure->event);
        writer.u32(configure->window);
        writer.i16(configure->x);
        writer.i16(configure->y);
        writer.u16(configure->width);
        writer.u16(configure->height);
        writer.i16(configure->off_x);
        writer.i16(configure->off_y);
        writer.u16(configure->pixmap_width);
        writer.u16(configure->pixmap_height);
        writer.u32(configure->pixmap_flags);
        return writer.data();
    }

    if (const auto *complete =
            std::get_if<PresentCompleteNotifyEvent>(&event)) {
        writer.u8(35); // GenericEvent
        writer.u8(present_extension.major_opcode);
        writer.u16(complete->sequence);
        writer.u32(2); // two words beyond the generic 32-byte event
        writer.u16(1); // CompleteNotify
        writer.u8(complete->kind);
        writer.u8(complete->mode);
        writer.u32(complete->event);
        writer.u32(complete->window);
        writer.u32(complete->serial);
        writer.u64(complete->ust);
        writer.u64(complete->msc);
        return writer.data();
    }

    if (const auto *idle = std::get_if<PresentIdleNotifyEvent>(&event)) {
        writer.u8(35); // GenericEvent
        writer.u8(present_extension.major_opcode);
        writer.u16(idle->sequence);
        writer.u32(0);
        writer.u16(2); // IdleNotify
        writer.pad(2);
        writer.u32(idle->event);
        writer.u32(idle->window);
        writer.u32(idle->serial);
        writer.u32(idle->pixmap);
        writer.u32(idle->idle_fence);
        return writer.data();
    }

    if (const auto *map = std::get_if<XkbMapNotifyEvent>(&event)) {
        writer.u8(xkb_extension.first_event);
        writer.u8(1); // MapNotify
        writer.u16(map->sequence);
        writer.u32(map->time);
        writer.u8(map->device);
        writer.u8(0); // no pointer-button actions
        writer.u16(map->changed);
        writer.u8(map->min_keycode);
        writer.u8(map->max_keycode);
        writer.u8(map->first_type);
        writer.u8(map->type_count);
        writer.u8(map->first_keysym);
        writer.u8(map->keysym_count);
        writer.u8(0); // first action
        writer.u8(0); // action count
        writer.u8(0); // first behavior
        writer.u8(0); // behavior count
        writer.u8(0); // first explicit entry
        writer.u8(0); // explicit count
        writer.u8(map->first_modmap);
        writer.u8(map->modmap_count);
        writer.u8(0); // first virtual-modifier map
        writer.u8(0); // virtual-modifier map count
        writer.u16(0); // no virtual modifiers
        writer.pad(2);
        return writer.data();
    }

    if (const auto *state = std::get_if<XkbStateNotifyEvent>(&event)) {
        writer.u8(xkb_extension.first_event);
        writer.u8(2); // StateNotify
        writer.u16(state->sequence);
        writer.u32(state->time);
        writer.u8(state->device);
        writer.u8(state->mods);
        writer.u8(state->base_mods);
        writer.u8(state->latched_mods);
        writer.u8(state->locked_mods);
        writer.u8(state->group);
        writer.i16(state->base_group);
        writer.i16(state->latched_group);
        writer.u8(state->locked_group);
        writer.u8(state->mods); // compatibility state
        writer.u8(state->mods); // grab modifiers
        writer.u8(state->mods); // compatibility grab modifiers
        writer.u8(state->mods); // lookup modifiers
        writer.u8(state->mods); // compatibility lookup modifiers
        writer.u16(state->pointer_buttons);
        writer.u16(state->changed);
        writer.u8(state->keycode);
        writer.u8(state->event_type);
        writer.u8(state->request_major);
        writer.u8(state->request_minor);
        return writer.data();
    }

    if (const auto *controls =
            std::get_if<XkbControlsNotifyEvent>(&event)) {
        writer.u8(xkb_extension.first_event);
        writer.u8(3); // ControlsNotify
        writer.u16(controls->sequence);
        writer.u32(controls->time);
        writer.u8(controls->device);
        writer.u8(controls->groups);
        writer.pad(2);
        writer.u32(controls->changed);
        writer.u32(controls->enabled);
        writer.u32(controls->enabled_changes);
        writer.u8(controls->keycode);
        writer.u8(controls->event_type);
        writer.u8(controls->request_major);
        writer.u8(controls->request_minor);
        writer.pad(4);
        return writer.data();
    }

    if (const auto *input = std::get_if<Xi2DeviceEvent>(&event)) {
        const bool pointer = input->device == xi2_pointer_device_id;
        const bool valuators = pointer && input->event_type == 6;
        const std::uint16_t buttons_len = 1;
        const std::uint16_t valuators_len = valuators ? 1 : 0;
        const std::uint32_t extra_words = valuators ? 18 : 13;
        const auto fixed = [](std::int32_t value) {
            const auto bounded = std::clamp<std::int32_t>(
                value, std::numeric_limits<std::int16_t>::min(),
                std::numeric_limits<std::int16_t>::max());
            return static_cast<std::uint32_t>(bounded) << 16;
        };
        const auto long_fixed = [](std::int32_t value) {
            return static_cast<std::uint64_t>(
                static_cast<std::int64_t>(value) * (std::int64_t{1} << 32));
        };
        writer.u8(35); // GenericEvent
        writer.u8(xinput_extension.major_opcode);
        writer.u16(input->sequence);
        writer.u32(extra_words);
        writer.u16(input->event_type);
        writer.u16(input->device);
        writer.u32(input->time);
        writer.u32(input->detail);
        writer.u32(input->root);
        writer.u32(input->event);
        writer.u32(input->child);
        writer.u32(fixed(input->root_x));
        writer.u32(fixed(input->root_y));
        writer.u32(fixed(input->event_x));
        writer.u32(fixed(input->event_y));
        writer.u16(buttons_len);
        writer.u16(valuators_len);
        writer.u16(input->source);
        writer.pad(2);
        writer.u32(input->flags);
        writer.u32(input->base_mods);
        writer.u32(input->latched_mods);
        writer.u32(input->locked_mods);
        writer.u32(input->effective_mods);
        writer.u8(input->base_group);
        writer.u8(input->latched_group);
        writer.u8(input->locked_group);
        writer.u8(input->effective_group);
        writer.u32(input->buttons);
        if (valuators) {
            writer.u32(3); // X and Y valuators
            writer.u64(long_fixed(input->root_x));
            writer.u64(long_fixed(input->root_y));
        }
        return writer.data();
    }

    if (const auto *raw = std::get_if<Xi2RawEvent>(&event)) {
        const bool valuators = raw->event_type == 17;
        const auto long_fixed = [](std::int32_t value) {
            return static_cast<std::uint64_t>(
                static_cast<std::int64_t>(value) * (std::int64_t{1} << 32));
        };
        writer.u8(35); // GenericEvent
        writer.u8(xinput_extension.major_opcode);
        writer.u16(raw->sequence);
        writer.u32(valuators ? 9 : 0);
        writer.u16(raw->event_type);
        writer.u16(raw->device);
        writer.u32(raw->time);
        writer.u32(raw->detail);
        writer.u16(raw->source);
        writer.u16(valuators ? 1 : 0);
        writer.u32(raw->flags);
        writer.pad(4);
        if (valuators) {
            writer.u32(3); // X and Y valuators
            writer.u64(long_fixed(raw->root_x));
            writer.u64(long_fixed(raw->root_y));
            writer.u64(long_fixed(raw->root_x));
            writer.u64(long_fixed(raw->root_y));
        }
        return writer.data();
    }

    if (const auto *property = std::get_if<Xi2PropertyEvent>(&event)) {
        writer.u8(35); // GenericEvent
        writer.u8(xinput_extension.major_opcode);
        writer.u16(property->sequence);
        writer.u32(0);
        writer.u16(12); // PropertyEvent
        writer.u16(property->device);
        writer.u32(property->time);
        writer.u32(property->property);
        writer.u8(property->what);
        writer.pad(11);
        return writer.data();
    }

    if (const auto *crossing = std::get_if<Xi2CrossingEvent>(&event)) {
        const auto fixed = [](std::int32_t value) {
            const auto bounded = std::clamp<std::int32_t>(
                value, std::numeric_limits<std::int16_t>::min(),
                std::numeric_limits<std::int16_t>::max());
            return static_cast<std::uint32_t>(bounded) << 16;
        };
        writer.u8(35); // GenericEvent
        writer.u8(xinput_extension.major_opcode);
        writer.u16(crossing->sequence);
        writer.u32(11); // 44 bytes beyond the generic event header
        writer.u16(crossing->event_type);
        writer.u16(crossing->device);
        writer.u32(crossing->time);
        writer.u16(crossing->source);
        writer.u8(crossing->mode);
        writer.u8(crossing->detail);
        writer.u32(crossing->root);
        writer.u32(crossing->event);
        writer.u32(crossing->child);
        writer.u32(fixed(crossing->root_x));
        writer.u32(fixed(crossing->root_y));
        writer.u32(fixed(crossing->event_x));
        writer.u32(fixed(crossing->event_y));
        writer.u8(crossing->same_screen ? 1 : 0);
        writer.u8(crossing->focus ? 1 : 0);
        writer.u16(1);
        writer.u32(crossing->base_mods);
        writer.u32(crossing->latched_mods);
        writer.u32(crossing->locked_mods);
        writer.u32(crossing->effective_mods);
        writer.u8(crossing->base_group);
        writer.u8(crossing->latched_group);
        writer.u8(crossing->locked_group);
        writer.u8(crossing->effective_group);
        writer.u32(crossing->buttons);
        return writer.data();
    }

    if (const auto *input = std::get_if<CoreInputEvent>(&event)) {
        writer.u8(input->type);
        writer.u8(input->detail);
        writer.u16(input->sequence);
        writer.u32(input->time);
        writer.u32(input->root);
        writer.u32(input->event);
        writer.u32(input->child);
        writer.i16(input->root_x);
        writer.i16(input->root_y);
        writer.i16(input->event_x);
        writer.i16(input->event_y);
        writer.u16(input->state);
        writer.u8(input->same_screen ? 1 : 0);
        writer.u8(0);
        return writer.data();
    }

    if (const auto *crossing = std::get_if<CrossingEvent>(&event)) {
        writer.u8(crossing->type);
        writer.u8(crossing->detail);
        writer.u16(crossing->sequence);
        writer.u32(crossing->time);
        writer.u32(crossing->root);
        writer.u32(crossing->event);
        writer.u32(crossing->child);
        writer.i16(crossing->root_x);
        writer.i16(crossing->root_y);
        writer.i16(crossing->event_x);
        writer.i16(crossing->event_y);
        writer.u16(crossing->state);
        writer.u8(crossing->mode);
        writer.u8((crossing->same_screen ? 1U : 0U) |
                  (crossing->focus ? 2U : 0U));
        return writer.data();
    }

    const auto &focus = std::get<FocusEvent>(event);
    writer.u8(focus.type);
    writer.u8(focus.detail);
    writer.u16(focus.sequence);
    writer.u32(focus.event);
    writer.u8(focus.mode);
    writer.pad(23);
    return writer.data();
}

Result<void>
Connection::drain_pending_events()
{
    while (const auto *event = server_.next_event(config_.resource_base)) {
        const auto encoded = encode_event(*event);
        if (encoded.size() < 32 || (encoded.size() & 3U) != 0)
            return malformed("invalid queued client event");
        auto queued = queue(encoded);
        if (!queued)
            return queued;
        server_.pop_event(config_.resource_base);
    }
    return Result<void>::success();
}

Result<void>
Connection::send_setup_failure(ByteOrder order, std::string reason)
{
    if (reason.size() > std::numeric_limits<std::uint8_t>::max())
        reason.resize(std::numeric_limits<std::uint8_t>::max());
    const auto padded_reason = padded_to_four(reason.size());
    if (!padded_reason)
        return malformed("setup failure reason overflow");

    WireWriter reply(order);
    reply.u8(0);
    reply.u8(static_cast<std::uint8_t>(reason.size()));
    reply.u16(protocol_major);
    reply.u16(protocol_minor);
    reply.u16(static_cast<std::uint16_t>(*padded_reason / 4));
    reply.bytes(reason);
    reply.pad(*padded_reason - reason.size());
    return queue(reply.data());
}

Result<void>
Connection::send_setup_success(ByteOrder order)
{
    constexpr std::string_view vendor = "Xmin";
    WireWriter payload(order);

    payload.u32(1); // release
    payload.u32(config_.resource_base);
    payload.u32(client_resource_mask);
    payload.u32(0);          // motion buffer
    payload.u16(static_cast<std::uint16_t>(vendor.size()));
    // Core framing remains available before BIG-REQUESTS is enabled.
    payload.u16(static_cast<std::uint16_t>(maximum_core_request_units));
    payload.u8(1);      // roots
    payload.u8(4);      // pixmap formats
    const auto image_order = host_byte_order() == ByteOrder::little ? 0U : 1U;
    payload.u8(static_cast<std::uint8_t>(image_order));
    payload.u8(static_cast<std::uint8_t>(image_order));
    payload.u8(32);  // bitmap scanline unit
    payload.u8(32);  // bitmap scanline pad
    payload.u8(minimum_keycode);
    payload.u8(maximum_keycode);
    payload.pad(4);

    payload.bytes(vendor);
    payload.pad_to_four();
    for (const auto &format : {
             std::pair<std::uint8_t, std::uint8_t>{1, 1},
             {8, 8},
             {24, 32},
             {32, 32}}) {
        payload.u8(format.first);
        payload.u8(format.second);
        payload.u8(32);
        payload.pad(5);
    }

    payload.u32(root_window_id);
    payload.u32(default_colormap_id);
    payload.u32(0x00ffffff);
    payload.u32(0x00000000);
    payload.u32(0);
    payload.u16(server_.width());
    payload.u16(server_.height());
    payload.u16(millimetres(server_.width()));
    payload.u16(millimetres(server_.height()));
    payload.u16(1);
    payload.u16(1);
    payload.u32(root_visual_id);
    payload.u8(0);
    payload.u8(0);
    payload.u8(24);
    payload.u8(1);

    payload.u8(24);
    payload.u8(0);
    payload.u16(1);
    payload.pad(4);
    payload.u32(root_visual_id);
    payload.u8(4); // TrueColor
    payload.u8(8);
    payload.u16(256);
    payload.u32(0x00ff0000);
    payload.u32(0x0000ff00);
    payload.u32(0x000000ff);
    payload.pad(4);

    if ((payload.size() & 3) != 0 || payload.size() / 4 > 65535)
        return malformed("invalid generated setup length");

    WireWriter prefix(order);
    prefix.u8(1);
    prefix.u8(0);
    prefix.u16(protocol_major);
    prefix.u16(protocol_minor);
    prefix.u16(static_cast<std::uint16_t>(payload.size() / 4));
    prefix.bytes(payload.data());
    return queue(prefix.data());
}

Result<bool>
Connection::process_setup_prefix()
{
    constexpr std::size_t prefix_size = 12;
    if (input_.size() < prefix_size)
        return Result<bool>::success(false);

    ByteOrder order;
    if (input_[0] == static_cast<std::uint8_t>('l'))
        order = ByteOrder::little;
    else if (input_[0] == static_cast<std::uint8_t>('B'))
        order = ByteOrder::big;
    else {
        return Result<bool>::failure(
            ErrorCode::malformed, "invalid setup byte order");
    }

    WireReader reader(input_.data(), prefix_size, order);
    const auto byte_order = reader.u8();
    const auto unused = reader.u8();
    const auto major = reader.u16();
    const auto minor = reader.u16();
    const auto auth_name_size = reader.u16();
    const auto auth_data_size = reader.u16();
    if (!byte_order || !unused || !major || !minor || !auth_name_size ||
        !auth_data_size || !reader.skip(2)) {
        return Result<bool>::failure(
            ErrorCode::malformed, "truncated setup prefix");
    }

    consume_input(prefix_size);
    order_ = order;
    if (*major != protocol_major || *minor != protocol_minor) {
        auto sent = send_setup_failure(order, "X11 protocol 11.0 required");
        if (!sent)
            return Result<bool>::failure(
                sent.error().code, sent.error().message);
        input_.clear();
        close_after_output();
        return Result<bool>::success(true);
    }

    const auto padded_name = padded_to_four(*auth_name_size);
    const auto padded_data = padded_to_four(*auth_data_size);
    if (!padded_name || !padded_data || *padded_name > 1024 ||
        *padded_data > 1024) {
        return Result<bool>::failure(
            ErrorCode::malformed, "invalid setup authentication lengths");
    }
    const auto payload_size = checked_add(*padded_name, *padded_data);
    if (!payload_size) {
        return Result<bool>::failure(
            ErrorCode::malformed, "setup authentication length overflow");
    }

    setup_payload_size_ = *payload_size;
    setup_name_size_ = *auth_name_size;
    setup_data_size_ = *auth_data_size;
    setup_padded_name_size_ = *padded_name;
    state_ = State::setup_authentication;
    return Result<bool>::success(true);
}

Result<bool>
Connection::process_setup_authentication()
{
    if (input_.size() < setup_payload_size_)
        return Result<bool>::success(false);
    if (!order_)
        return Result<bool>::failure(ErrorCode::malformed,
                                     "setup byte order was not recorded");

    std::string auth_name;
    auth_name.reserve(setup_name_size_);
    for (std::size_t index = 0; index < setup_name_size_; ++index)
        auth_name.push_back(static_cast<char>(input_[index]));
    std::vector<std::uint8_t> auth_data(
        input_.begin() +
            static_cast<std::ptrdiff_t>(setup_padded_name_size_),
        input_.begin() + static_cast<std::ptrdiff_t>(
                             setup_padded_name_size_ + setup_data_size_));
    consume_input(setup_payload_size_);

    const bool authenticated = config_.allow_unauthenticated
        ? auth_name.empty() && auth_data.empty()
        : !config_.cookie.empty() && auth_name == cookie_protocol &&
            cookies_equal(auth_data, config_.cookie);
    if (!authenticated) {
        auto sent = send_setup_failure(*order_, "Authentication failed");
        if (!sent)
            return Result<bool>::failure(
                sent.error().code, sent.error().message);
        input_.clear();
        close_after_output();
        return Result<bool>::success(true);
    }

    if (!server_.register_client(config_.resource_base)) {
        return Result<bool>::failure(
            ErrorCode::busy, "unable to register authenticated client");
    }
    auto sent = send_setup_success(*order_);
    if (!sent)
        return Result<bool>::failure(sent.error().code, sent.error().message);
    state_ = State::requests;
    return Result<bool>::success(true);
}

Result<void>
Connection::send_error(ByteOrder order, std::uint8_t code,
                       std::uint8_t opcode, std::uint16_t sequence,
                       std::uint32_t bad_value,
                       std::uint16_t minor_opcode)
{
    WireWriter reply(order);
    reply.u8(0);
    reply.u8(code);
    reply.u16(sequence);
    reply.u32(bad_value);
    reply.u16(minor_opcode);
    reply.u8(opcode);
    reply.pad(21);
    return queue(reply.data());
}

Result<void>
Connection::handle_create_window(const RequestContext &context)
{
    if (context.request.size() < 32)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto parent_id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto border_width = reader.u16();
    const auto requested_class = reader.u16();
    const auto visual = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !parent_id || !x || !y || !width || !height ||
        !border_width || !requested_class || !visual || !value_mask) {
        return malformed("truncated CreateWindow request");
    }

    constexpr std::uint32_t supported_value_mask = 0x00007fffU;
    const auto value_count = std::bitset<32>(*value_mask).count();
    const auto value_bytes = checked_multiply(value_count, std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{32}, *value_bytes)
        : std::optional<std::size_t>{};
    if ((*value_mask & ~supported_value_mask) != 0) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    }
    if (!expected_size || context.request.size() != *expected_size) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    if (!server_.valid_client_resource(*id, config_.resource_base)) {
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    }
    if (server_.resource_limit_reached(config_.resource_base)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    const auto *parent = server_.window(*parent_id);
    if (parent == nullptr) {
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *parent_id);
    }
    if (*width == 0 || *height == 0) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *width == 0 ? *width : *height);
    }

    WindowClass window_class;
    if (*requested_class == 0)
        window_class = parent->window_class;
    else if (*requested_class == 1)
        window_class = WindowClass::input_output;
    else if (*requested_class == 2)
        window_class = WindowClass::input_only;
    else {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *requested_class);
    }

    WindowRecord window;
    window.id = *id;
    window.parent = *parent_id;
    window.x = signed_word(*x);
    window.y = signed_word(*y);
    window.width = *width;
    window.height = *height;
    window.border_width = *border_width;
    window.window_class = window_class;
    window.colormap = parent->colormap;

    if (window_class == WindowClass::input_only) {
        if (context.data != 0 || *visual != 0 || *border_width != 0) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        if ((*value_mask & ~input_only_attribute_masks) != 0) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        window.depth = 0;
        window.visual = 0;
        window.colormap = 0;
    }
    else {
        if (parent->window_class == WindowClass::input_only) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        window.depth = context.data == 0 ? parent->depth : context.data;
        window.visual = *visual == 0 ? parent->visual : *visual;
        if (window.depth != 24 || window.visual != root_visual_id) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
        auto surface = Surface::create(window.width, window.height,
                                       window.depth);
        if (!surface)
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        window.surface = server_.adopt_surface(std::move(*surface));
        if (!window.surface)
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
    }

    WireReader values(context.request.data() + 32,
                      context.request.size() - 32, context.order);
    for (unsigned bit = 0; bit < 15; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = values.u32();
        if (!value)
            return malformed("truncated CreateWindow value list");
        switch (bit) {
        case 0:
            if (*value > 1) {
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            }
            break;
        case 1:
            window.background_pixel = *value;
            break;
        case 2:
            if (*value != 0) {
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            }
            break;
        case 3:
            window.border_pixel = *value;
            break;
        case 4:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.bit_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 5:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.window_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 6:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.backing_store = static_cast<std::uint8_t>(*value);
            break;
        case 7:
            window.backing_planes = *value;
            break;
        case 8:
            window.backing_pixel = *value;
            break;
        case 9:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.override_redirect = *value != 0;
            break;
        case 10:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window.save_under = *value != 0;
            break;
        case 11:
            if ((*value & ~all_event_masks) != 0) {
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            }
            if (*value != 0)
                window.event_masks.emplace(config_.resource_base, *value);
            break;
        case 12:
            if ((*value & ~propagate_event_masks) != 0) {
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            }
            window.do_not_propagate_mask =
                static_cast<std::uint16_t>(*value);
            break;
        case 13:
            if (*value == 0)
                window.colormap = parent->colormap;
            else if (server_.colormap_exists(*value))
                window.colormap = *value;
            else
                return send_error(context.order, bad_colormap, context.opcode,
                                  context.sequence, *value);
            break;
        case 14:
            if (*value == 0) {
                window.cursor.reset();
            }
            else if (const auto *cursor = server_.cursor(*value)) {
                window.cursor = cursor->image;
            }
            else {
                return send_error(context.order, bad_cursor, context.opcode,
                                  context.sequence, *value);
            }
            break;
        }
    }

    if (window.surface) {
        window.surface->fill(
            Rectangle{0, 0, window.width, window.height},
            window.background_pixel, 3, 0xffffffffU);
    }
    if (!server_.add_window(std::move(window), config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_change_window_attributes(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !value_mask)
        return malformed("truncated ChangeWindowAttributes request");
    constexpr std::uint32_t supported_value_mask = 0x00007fffU;
    if ((*value_mask & ~supported_value_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto value_bytes = checked_multiply(
        std::bitset<32>(*value_mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{12}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    if (window->window_class == WindowClass::input_only &&
        (*value_mask & ~input_only_attribute_masks) != 0) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }

    auto bit_gravity = window->bit_gravity;
    auto window_gravity = window->window_gravity;
    auto backing_store = window->backing_store;
    auto backing_planes = window->backing_planes;
    auto backing_pixel = window->backing_pixel;
    auto override_redirect = window->override_redirect;
    auto save_under = window->save_under;
    auto do_not_propagate = window->do_not_propagate_mask;
    auto colormap = window->colormap;
    auto background_pixel = window->background_pixel;
    auto border_pixel = window->border_pixel;
    auto cursor_image = window->cursor;
    std::optional<std::uint32_t> event_mask;
    for (unsigned bit = 0; bit < 15; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = reader.u32();
        if (!value)
            return malformed("truncated ChangeWindowAttributes value list");
        switch (bit) {
        case 0:
            if (*value > 1)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            break;
        case 1:
            background_pixel = *value;
            break;
        case 2:
            if (*value != 0)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            break;
        case 3:
            border_pixel = *value;
            break;
        case 4:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            bit_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 5:
            if (*value > 10)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            window_gravity = static_cast<std::uint8_t>(*value);
            break;
        case 6:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            backing_store = static_cast<std::uint8_t>(*value);
            break;
        case 7:
            backing_planes = *value;
            break;
        case 8:
            backing_pixel = *value;
            break;
        case 9:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            override_redirect = *value != 0;
            break;
        case 10:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            save_under = *value != 0;
            break;
        case 11:
            if ((*value & ~all_event_masks) != 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            for (const auto &selection : window->event_masks) {
                if (selection.first != config_.resource_base &&
                    (selection.second & *value & exclusive_event_masks) != 0) {
                    return send_error(context.order, bad_access,
                                      context.opcode, context.sequence);
                }
            }
            event_mask = *value;
            break;
        case 12:
            if ((*value & ~propagate_event_masks) != 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            do_not_propagate = static_cast<std::uint16_t>(*value);
            break;
        case 13:
            if (*value == 0) {
                const auto *parent = server_.window(window->parent);
                colormap = parent == nullptr ? default_colormap_id
                                             : parent->colormap;
            }
            else if (server_.colormap_exists(*value))
                colormap = *value;
            else
                return send_error(context.order, bad_colormap,
                                  context.opcode, context.sequence, *value);
            break;
        case 14:
            if (*value == 0)
                cursor_image.reset();
            else if (const auto *cursor = server_.cursor(*value))
                cursor_image = cursor->image;
            else
                return send_error(context.order, bad_cursor, context.opcode,
                                  context.sequence, *value);
            break;
        }
    }

    std::optional<std::uint32_t> previous_event_mask;
    if (event_mask && *event_mask != 0) {
        const auto previous = window->event_masks.find(config_.resource_base);
        if (previous != window->event_masks.end())
            previous_event_mask = previous->second;
        try {
            window->event_masks.insert_or_assign(config_.resource_base,
                                                  *event_mask);
        }
        catch (const std::bad_alloc &) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
    }
    const auto cursor_update = server_.set_window_cursor(
        *window, std::move(cursor_image));
    if (cursor_update != XFixesUpdate::updated) {
        if (event_mask && *event_mask != 0) {
            if (previous_event_mask) {
                window->event_masks.find(config_.resource_base)->second =
                    *previous_event_mask;
            }
            else {
                window->event_masks.erase(config_.resource_base);
            }
        }
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    if (event_mask && *event_mask == 0)
        window->event_masks.erase(config_.resource_base);
    window->bit_gravity = bit_gravity;
    window->window_gravity = window_gravity;
    window->backing_store = backing_store;
    window->backing_planes = backing_planes;
    window->backing_pixel = backing_pixel;
    window->override_redirect = override_redirect;
    window->save_under = save_under;
    window->do_not_propagate_mask = do_not_propagate;
    window->colormap = colormap;
    window->background_pixel = background_pixel;
    window->border_pixel = border_pixel;
    server_.invalidate_scene();
    return drain_pending_events();
}

Result<void>
Connection::handle_get_window_attributes(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated GetWindowAttributes request");
    const auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);

    const auto own_mask = window->event_masks.find(config_.resource_base);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(window->backing_store);
    reply.u16(context.sequence);
    reply.u32(3);
    reply.u32(window->visual);
    reply.u16(static_cast<std::uint16_t>(window->window_class));
    reply.u8(window->bit_gravity);
    reply.u8(window->window_gravity);
    reply.u32(window->backing_planes);
    reply.u32(window->backing_pixel);
    reply.u8(window->save_under ? 1 : 0);
    reply.u8(window->colormap == server_.installed_colormap() ? 1 : 0);
    reply.u8(server_.map_state(*id));
    reply.u8(window->override_redirect ? 1 : 0);
    reply.u32(window->colormap);
    reply.u32(server_.all_event_masks(*window));
    reply.u32(own_mask == window->event_masks.end() ? 0 : own_mask->second);
    reply.u16(window->do_not_propagate_mask);
    reply.pad(2);
    return queue(reply.data());
}

Result<void>
Connection::handle_destroy_window(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated DestroyWindow request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto delivered = server_.destroy_window(*id);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_change_save_set(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated ChangeSaveSet request");
    const auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    if (window->owner == config_.resource_base)
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    const auto updated = server_.alter_save_set(
        config_.resource_base, *id, context.data == 0, false, true);
    if (updated != XFixesUpdate::updated)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_destroy_subwindows(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated DestroySubwindows request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto delivered = server_.destroy_subwindows(*id);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_reparent_window(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto id = reader.u32();
    const auto parent_id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    if (!id || !parent_id || !x || !y)
        return malformed("truncated ReparentWindow request");
    const auto *window = server_.window(*id);
    if (window == nullptr || *id == root_window_id)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto *parent = server_.window(*parent_id);
    if (parent == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *parent_id);
    if (parent->window_class == WindowClass::input_only &&
        window->window_class == WindowClass::input_output) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    const auto updated = server_.reparent_window(
        *id, *parent_id, signed_word(*x), signed_word(*y));
    if (updated == ReparentUpdate::invalid) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    if (updated == ReparentUpdate::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_map_window(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated MapWindow request");
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto delivered = server_.set_window_mapped(*window, true);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_map_subwindows(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated MapSubwindows request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto delivered = server_.set_subwindows_mapped(*id, true);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_unmap_window(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated UnmapWindow request");
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto delivered = server_.set_window_mapped(*window, false);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_unmap_subwindows(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated UnmapSubwindows request");
    if (server_.window(*id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    const auto delivered = server_.set_subwindows_mapped(*id, false);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_configure_window(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto mask = reader.u16();
    if (!id || !mask || !reader.skip(2))
        return malformed("truncated ConfigureWindow request");
    if ((*mask & ~std::uint16_t{0x007f}) != 0) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *mask);
    }
    const auto value_bytes = checked_multiply(
        std::bitset<16>(*mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{12}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);

    std::int16_t x = window->x;
    std::int16_t y = window->y;
    std::uint16_t width = window->width;
    std::uint16_t height = window->height;
    std::uint16_t border_width = window->border_width;
    std::optional<std::uint32_t> sibling;
    std::optional<std::uint8_t> stack_mode;
    WireReader values(context.request.data() + 12,
                      context.request.size() - 12, context.order);
    for (unsigned bit = 0; bit < 7; ++bit) {
        if ((*mask & (std::uint16_t{1} << bit)) == 0)
            continue;
        const auto value = values.u32();
        if (!value)
            return malformed("truncated ConfigureWindow value list");
        switch (bit) {
        case 0:
            x = signed_word(static_cast<std::uint16_t>(*value));
            break;
        case 1:
            y = signed_word(static_cast<std::uint16_t>(*value));
            break;
        case 2:
            width = static_cast<std::uint16_t>(*value);
            if (width == 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence);
            break;
        case 3:
            height = static_cast<std::uint16_t>(*value);
            if (height == 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence);
            break;
        case 4:
            if (window->window_class == WindowClass::input_only)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence);
            border_width = static_cast<std::uint16_t>(*value);
            break;
        case 5:
            sibling = *value;
            break;
        case 6:
            stack_mode = static_cast<std::uint8_t>(*value);
            if (*stack_mode > 4)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *stack_mode);
            break;
        }
    }
    if (sibling && !stack_mode)
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    if (sibling) {
        const auto *sibling_window = server_.window(*sibling);
        if (sibling_window == nullptr)
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *sibling);
        if (*sibling == *id || sibling_window->parent != window->parent)
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
    }
    if (window->parent == 0)
        return Result<void>::success();

    if (server_.configure_window(
            *window, x, y, width, height, border_width,
            sibling, stack_mode) == EventDelivery::queue_full) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_get_geometry(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto drawable = reader.u32();
    if (!drawable)
        return malformed("truncated GetGeometry request");
    const auto *window = server_.window(*drawable);
    const auto *pixmap = server_.pixmap(*drawable);
    if (window == nullptr && pixmap == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);

    const std::uint8_t depth = window == nullptr ? pixmap->surface->depth()
                                                 : window->depth;
    const std::int16_t x = window == nullptr ? 0 : window->x;
    const std::int16_t y = window == nullptr ? 0 : window->y;
    const std::uint16_t width = window == nullptr ? pixmap->surface->width()
                                                  : window->width;
    const std::uint16_t height = window == nullptr ? pixmap->surface->height()
                                                   : window->height;
    const std::uint16_t border = window == nullptr ? 0 : window->border_width;

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(depth);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(root_window_id);
    reply.i16(x);
    reply.i16(y);
    reply.u16(width);
    reply.u16(height);
    reply.u16(border);
    reply.pad(10);
    return queue(reply.data());
}

Result<void>
Connection::handle_query_tree(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated QueryTree request");
    const auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    if (window->children.size() > std::numeric_limits<std::uint16_t>::max()) {
        return Result<void>::failure(ErrorCode::io,
                                     "window child limit exceeded");
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(window->children.size()));
    reply.u32(root_window_id);
    reply.u32(window->parent);
    reply.u16(static_cast<std::uint16_t>(window->children.size()));
    reply.pad(14);
    for (const auto child : window->children)
        reply.u32(child);
    return queue(reply.data());
}

Result<void>
Connection::handle_intern_atom(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto name_size = reader.u16();
    if (!name_size || !reader.skip(2))
        return malformed("truncated InternAtom request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 8 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1 || *name_size == 0 || *name_size > 1024) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence,
                          context.data > 1 ? context.data : *name_size);
    }
    const std::string_view name(
        reinterpret_cast<const char *>(context.request.data() + 8),
        *name_size);
    const AtomId atom = server_.atoms().intern(name, context.data != 0);
    if (atom == 0 && context.data == 0) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(atom);
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_get_atom_name(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto atom = reader.u32();
    if (!atom)
        return malformed("truncated GetAtomName request");
    const auto name = server_.atoms().name(*atom);
    if (!name)
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *atom);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>((name->size() + 3) / 4));
    reply.u16(static_cast<std::uint16_t>(name->size()));
    reply.pad(22);
    reply.bytes(*name);
    reply.pad_to_four();
    return queue(reply.data());
}

Result<void>
Connection::handle_change_property(const RequestContext &context)
{
    if (context.request.size() < 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto window_id = reader.u32();
    const auto property = reader.u32();
    const auto type = reader.u32();
    const auto format = reader.u8();
    const bool fixed_fields = reader.skip(3);
    const auto item_count = reader.u32();
    if (!window_id || !property || !type || !format || !fixed_fields ||
        !item_count) {
        return malformed("truncated ChangeProperty request");
    }
    if (context.data > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    if (*format != 8 && *format != 16 && *format != 32)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *format);

    auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (!server_.atoms().name(*property))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *property);
    if (!server_.atoms().name(*type))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *type);

    const auto data_size = checked_multiply(
        static_cast<std::size_t>(*item_count),
        static_cast<std::size_t>(*format / 8));
    const auto padded_size = data_size ? padded_to_four(*data_size)
                                       : std::optional<std::size_t>{};
    const auto expected_size = padded_size
        ? checked_add(std::size_t{24}, *padded_size)
        : std::optional<std::size_t>{};
    if (!data_size || !padded_size || !expected_size ||
        context.request.size() != *expected_size) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    const auto canonical = canonical_property_data(
        context.request.data() + 24, *data_size, *format, context.order);
    if (!canonical)
        return malformed("misaligned ChangeProperty data");

    PropertyValue updated{*type, *format, {}};
    const auto existing = window->properties.find(*property);
    if (existing != window->properties.end() && context.data != 0 &&
        (existing->second.type != *type ||
         existing->second.format != *format)) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    const std::size_t previous_size = existing == window->properties.end()
        ? 0
        : existing->second.data.size();
    const auto combined_size = context.data == 0
        ? std::optional<std::size_t>(canonical->size())
        : checked_add(previous_size, canonical->size());
    if (!combined_size || *combined_size > maximum_property_bytes)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    updated.data.reserve(*combined_size);
    if (context.data == 1)
        updated.data.insert(updated.data.end(), canonical->begin(),
                            canonical->end());
    if (context.data != 0 && existing != window->properties.end()) {
        updated.data.insert(updated.data.end(), existing->second.data.begin(),
                            existing->second.data.end());
    }
    if (context.data != 1)
        updated.data.insert(updated.data.end(), canonical->begin(),
                            canonical->end());

    if (!server_.set_property(*window, *property, std::move(updated)))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_delete_property(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto window_id = reader.u32();
    const auto property = reader.u32();
    if (!window_id || !property)
        return malformed("truncated DeleteProperty request");
    auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (!server_.atoms().name(*property))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *property);
    server_.delete_property(*window, *property);
    return Result<void>::success();
}

Result<void>
Connection::handle_get_property(const RequestContext &context)
{
    if (context.request.size() != 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 20, context.order);
    const auto window_id = reader.u32();
    const auto property = reader.u32();
    const auto requested_type = reader.u32();
    const auto long_offset = reader.u32();
    const auto long_length = reader.u32();
    if (!window_id || !property || !requested_type || !long_offset ||
        !long_length) {
        return malformed("truncated GetProperty request");
    }
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (!server_.atoms().name(*property))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *property);
    if (*requested_type != 0 && !server_.atoms().name(*requested_type))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *requested_type);

    const auto found = window->properties.find(*property);
    const PropertyValue *value = found == window->properties.end()
        ? nullptr
        : &found->second;
    std::size_t offset = 0;
    std::size_t returned_size = 0;
    std::size_t bytes_after = 0;
    bool matching_type = value != nullptr &&
        (*requested_type == 0 || value->type == *requested_type);
    if (value != nullptr) {
        const auto byte_offset = checked_multiply(
            static_cast<std::size_t>(*long_offset), std::size_t{4});
        if (!byte_offset || *byte_offset > value->data.size())
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *long_offset);
        offset = *byte_offset;
        if (matching_type) {
            const auto maximum_bytes = checked_multiply(
                static_cast<std::size_t>(*long_length), std::size_t{4});
            if (!maximum_bytes)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *long_length);
            returned_size = std::min(*maximum_bytes,
                                     value->data.size() - offset);
            bytes_after = value->data.size() - offset - returned_size;
        }
        else {
            bytes_after = value->data.size();
        }
    }

    const std::uint8_t format = value == nullptr ? 0 : value->format;
    const AtomId actual_type = value == nullptr ? 0 : value->type;
    const std::size_t unit = format == 0 ? 1 : format / 8;
    const auto encoded = matching_type && returned_size != 0
        ? wire_property_data(value->data.data() + offset, returned_size,
                             format, context.order)
        : std::vector<std::uint8_t>{};
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(format);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>((encoded.size() + 3) / 4));
    reply.u32(actual_type);
    reply.u32(static_cast<std::uint32_t>(bytes_after));
    reply.u32(static_cast<std::uint32_t>(encoded.size() / unit));
    reply.pad(12);
    reply.bytes(encoded);
    reply.pad_to_four();
    auto queued = queue(reply.data());
    if (queued && context.data != 0 && matching_type && bytes_after == 0)
        server_.delete_property(*window, *property);
    return queued;
}

Result<void>
Connection::handle_list_properties(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto window_id = reader.u32();
    if (!window_id)
        return malformed("truncated ListProperties request");
    const auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (window->properties.size() > std::numeric_limits<std::uint16_t>::max())
        return Result<void>::failure(ErrorCode::io,
                                     "window property limit exceeded");

    std::vector<AtomId> properties;
    properties.reserve(window->properties.size());
    for (const auto &property : window->properties)
        properties.push_back(property.first);
    std::sort(properties.begin(), properties.end());

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(properties.size()));
    reply.u16(static_cast<std::uint16_t>(properties.size()));
    reply.pad(22);
    for (const auto property : properties)
        reply.u32(property);
    return queue(reply.data());
}

Result<void>
Connection::handle_rotate_properties(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto window_id = reader.u32();
    const auto atom_count = reader.u16();
    const auto encoded_delta = reader.u16();
    if (!window_id || !atom_count || !encoded_delta)
        return malformed("truncated RotateProperties request");
    const auto atom_bytes = checked_multiply(
        static_cast<std::size_t>(*atom_count), sizeof(AtomId));
    const auto expected_size = atom_bytes
        ? checked_add(std::size_t{12}, *atom_bytes)
        : std::optional<std::size_t>{};
    if (!atom_bytes || !expected_size ||
        context.request.size() != *expected_size) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }

    auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (*atom_count == 0)
        return Result<void>::success();

    try {
        std::vector<AtomId> atoms;
        atoms.reserve(*atom_count);
        for (std::size_t index = 0; index < *atom_count; ++index) {
            const auto atom = reader.u32();
            if (!atom)
                return malformed("truncated RotateProperties atom list");
            atoms.push_back(*atom);
        }

        auto sorted_atoms = atoms;
        std::sort(sorted_atoms.begin(), sorted_atoms.end());
        for (const auto atom : atoms) {
            if (!server_.atoms().name(atom))
                return send_error(context.order, bad_atom, context.opcode,
                                  context.sequence, atom);
            const auto duplicates = std::equal_range(
                sorted_atoms.begin(), sorted_atoms.end(), atom);
            if (duplicates.second - duplicates.first > 1) {
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence);
            }
            if (window->properties.find(atom) == window->properties.end()) {
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, atom);
            }
        }

        const auto signed_delta = static_cast<std::int32_t>(
            signed_word(*encoded_delta));
        const auto count = static_cast<std::int32_t>(*atom_count);
        std::int32_t delta = signed_delta % count;
        if (delta < 0)
            delta += count;
        if (delta == 0)
            return Result<void>::success();

        std::vector<PropertyValue> values;
        values.reserve(atoms.size());
        for (const auto atom : atoms)
            values.push_back(std::move(window->properties.find(atom)->second));
        for (std::size_t index = 0; index < atoms.size(); ++index) {
            const auto destination = (index + static_cast<std::size_t>(delta)) %
                atoms.size();
            window->properties.find(atoms[destination])->second =
                std::move(values[index]);
        }
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_set_selection_owner(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto owner = reader.u32();
    const auto selection = reader.u32();
    const auto time = reader.u32();
    if (!owner || !selection || !time)
        return malformed("truncated SetSelectionOwner request");
    if (*owner != 0 && server_.window(*owner) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *owner);
    if (!server_.atoms().name(*selection))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *selection);

    const auto updated = server_.set_selection_owner(
        *selection, *owner, config_.resource_base, *time);
    if (updated == SelectionUpdate::event_queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_get_selection_owner(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto selection = reader.u32();
    if (!selection)
        return malformed("truncated GetSelectionOwner request");
    if (!server_.atoms().name(*selection))
        return send_error(context.order, bad_atom, context.opcode,
                          context.sequence, *selection);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(server_.selection_owner(*selection));
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_send_event(const RequestContext &context)
{
    if (context.request.size() != 44)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto requested_destination = reader.u32();
    const auto event_mask = reader.u32();
    if (!requested_destination || !event_mask)
        return malformed("truncated SendEvent request");
    if ((*event_mask & ~all_event_masks) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *event_mask);

    const std::uint32_t destination = *requested_destination <= 1
        ? root_window_id
        : *requested_destination;
    if (server_.window(destination) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *requested_destination);
    const std::uint8_t event_type = context.request[12] & 0x7fU;
    const std::uint8_t format = context.request[13];
    if (event_type != 33)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, event_type);
    if (format != 8 && format != 16 && format != 32)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, format);

    WireReader event_reader(context.request.data() + 16, 28, context.order);
    const auto event_window = event_reader.u32();
    const auto event_type_atom = event_reader.u32();
    if (!event_window || !event_type_atom)
        return malformed("truncated ClientMessage event");
    ClientMessageEvent event;
    event.format = format;
    event.window = *event_window;
    event.type = *event_type_atom;
    const std::size_t count = 160U / format;
    for (std::size_t index = 0; index < count; ++index) {
        if (format == 8) {
            const auto value = event_reader.u8();
            if (!value)
                return malformed("truncated 8-bit ClientMessage data");
            event.data[index] = *value;
        }
        else if (format == 16) {
            const auto value = event_reader.u16();
            if (!value)
                return malformed("truncated 16-bit ClientMessage data");
            event.data[index] = *value;
        }
        else {
            const auto value = event_reader.u32();
            if (!value)
                return malformed("truncated 32-bit ClientMessage data");
            event.data[index] = *value;
        }
    }

    const auto delivered = server_.deliver_client_message(
        destination, *event_mask, context.data != 0, event);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_grab_pointer(const RequestContext &context)
{
    if (context.request.size() != 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 20, context.order);
    const auto window_id = reader.u32();
    const auto event_mask = reader.u16();
    const auto pointer_mode = reader.u8();
    const auto keyboard_mode = reader.u8();
    const auto confine_to = reader.u32();
    const auto cursor = reader.u32();
    const auto time = reader.u32();
    if (!window_id || !event_mask || !pointer_mode || !keyboard_mode ||
        !confine_to || !cursor || !time) {
        return malformed("truncated GrabPointer request");
    }
    if ((*event_mask & ~pointer_grab_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *event_mask);
    const auto *confine = *confine_to == 0
        ? nullptr
        : server_.window(*confine_to);
    if (*confine_to != 0 && confine == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *confine_to);
    if (*keyboard_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *keyboard_mode);
    if (*pointer_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *pointer_mode);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    const auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (*cursor != 0 && server_.cursor(*cursor) == nullptr)
        return send_error(context.order, bad_cursor, context.opcode,
                          context.sequence, *cursor);

    auto &input = server_.input();
    const std::uint32_t effective_time = *time == 0
        ? server_.current_time()
        : *time;
    std::uint8_t status = 0;
    if (input.pointer_grab &&
        input.pointer_grab->owner != config_.resource_base) {
        status = 1; // AlreadyGrabbed
    }
    else if (server_.map_state(window->id) != 2 ||
             (confine != nullptr && server_.map_state(confine->id) != 2)) {
        status = 3; // GrabNotViewable
    }
    else if (timestamp_later(effective_time, server_.current_time()) ||
             timestamp_earlier(effective_time, input.pointer_grab_time)) {
        status = 2; // GrabInvalidTime
    }
    else {
        const auto cursor_image = *cursor == 0
            ? std::shared_ptr<CursorImage>{}
            : server_.cursor(*cursor)->image;
        const auto activated = server_.activate_pointer_grab(ActiveGrab{
            config_.resource_base, *window_id, *confine_to, effective_time,
            *event_mask, *pointer_mode, *keyboard_mode, context.data != 0,
            false, 0, false, cursor_image});
        if (activated == EventDelivery::queue_full)
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
    }
    auto drained = drain_pending_events();
    if (!drained)
        return drained;

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(status);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.pad(24);
    return queue(reply.data());
}

Result<void>
Connection::handle_ungrab_pointer(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto time = reader.u32();
    if (!time)
        return malformed("truncated UngrabPointer request");
    auto &input = server_.input();
    const std::uint32_t effective_time = *time == 0
        ? server_.current_time()
        : *time;
    if (input.pointer_grab &&
        input.pointer_grab->owner == config_.resource_base &&
        !timestamp_later(effective_time, server_.current_time()) &&
        !timestamp_earlier(effective_time,
                           input.pointer_grab->activated_at)) {
        if (server_.deactivate_pointer_grab() ==
                EventDelivery::queue_full) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_grab_button(const RequestContext &context)
{
    if (context.request.size() != 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 20, context.order);
    const auto window_id = reader.u32();
    const auto event_mask = reader.u16();
    const auto pointer_mode = reader.u8();
    const auto keyboard_mode = reader.u8();
    const auto confine_to = reader.u32();
    const auto cursor = reader.u32();
    const auto button = reader.u8();
    const bool padding = reader.skip(1);
    const auto modifiers = reader.u16();
    if (!window_id || !event_mask || !pointer_mode || !keyboard_mode ||
        !confine_to || !cursor || !button || !padding || !modifiers) {
        return malformed("truncated GrabButton request");
    }
    if (*pointer_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *pointer_mode);
    if (*keyboard_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *keyboard_mode);
    if (!valid_passive_modifiers(*modifiers))
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *modifiers);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    if ((*event_mask & ~pointer_grab_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *event_mask);
    if (server_.window(*window_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (*confine_to != 0 && server_.window(*confine_to) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *confine_to);
    if (*cursor != 0 && server_.cursor(*cursor) == nullptr)
        return send_error(context.order, bad_cursor, context.opcode,
                          context.sequence, *cursor);

    PassiveGrab grab;
    grab.kind = PassiveGrabKind::button;
    grab.details = passive_grab_details(grab.kind, *button);
    grab.modifiers = passive_grab_modifiers(*modifiers);
    grab.owner = config_.resource_base;
    grab.window = *window_id;
    grab.confine_to = *confine_to;
    grab.event_mask = *event_mask;
    grab.pointer_mode = *pointer_mode;
    grab.keyboard_mode = *keyboard_mode;
    grab.owner_events = context.data != 0;
    if (*cursor != 0)
        grab.cursor = server_.cursor(*cursor)->image;
    const auto update = server_.add_passive_grab(std::move(grab));
    if (update == PassiveGrabUpdate::access_denied)
        return send_error(context.order, bad_access, context.opcode,
                          context.sequence);
    if (update == PassiveGrabUpdate::resource_exhausted)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_ungrab_button(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto window_id = reader.u32();
    const auto modifiers = reader.u16();
    if (!window_id || !modifiers || !reader.skip(2))
        return malformed("truncated UngrabButton request");
    if (!valid_passive_modifiers(*modifiers))
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *modifiers);
    if (server_.window(*window_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    const auto update = server_.remove_passive_grab(
        PassiveGrabKind::button, config_.resource_base, *window_id,
        passive_grab_details(PassiveGrabKind::button, context.data),
        passive_grab_modifiers(*modifiers));
    if (update == PassiveGrabUpdate::resource_exhausted)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_change_active_pointer_grab(
    const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto cursor = reader.u32();
    const auto time = reader.u32();
    const auto event_mask = reader.u16();
    if (!cursor || !time || !event_mask || !reader.skip(2))
        return malformed("truncated ChangeActivePointerGrab request");
    if ((*event_mask & ~pointer_grab_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *event_mask);
    if (*cursor != 0 && server_.cursor(*cursor) == nullptr)
        return send_error(context.order, bad_cursor, context.opcode,
                          context.sequence, *cursor);
    auto &input = server_.input();
    const std::uint32_t effective_time = *time == 0
        ? server_.current_time()
        : *time;
    if (input.pointer_grab &&
        input.pointer_grab->owner == config_.resource_base &&
        !timestamp_later(effective_time, server_.current_time()) &&
        !timestamp_earlier(effective_time,
                           input.pointer_grab->activated_at)) {
        auto cursor_image = *cursor == 0
            ? std::shared_ptr<CursorImage>{}
            : server_.cursor(*cursor)->image;
        if (server_.set_pointer_grab_cursor(
                *event_mask, std::move(cursor_image)) !=
            XFixesUpdate::updated) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_grab_keyboard(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto window_id = reader.u32();
    const auto time = reader.u32();
    const auto pointer_mode = reader.u8();
    const auto keyboard_mode = reader.u8();
    if (!window_id || !time || !pointer_mode || !keyboard_mode ||
        !reader.skip(2)) {
        return malformed("truncated GrabKeyboard request");
    }
    if (*keyboard_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *keyboard_mode);
    if (*pointer_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *pointer_mode);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    const auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);

    auto &input = server_.input();
    const std::uint32_t effective_time = *time == 0
        ? server_.current_time()
        : *time;
    std::uint8_t status = 0;
    if (input.keyboard_grab &&
        input.keyboard_grab->owner != config_.resource_base) {
        status = 1;
    }
    else if (server_.map_state(window->id) != 2) {
        status = 3;
    }
    else if (timestamp_later(effective_time, server_.current_time()) ||
             timestamp_earlier(effective_time, input.keyboard_grab_time)) {
        status = 2;
    }
    else {
        const auto activated = server_.activate_keyboard_grab(ActiveGrab{
            config_.resource_base, *window_id, 0, effective_time,
            (1U << 0) | (1U << 1), *pointer_mode, *keyboard_mode,
            context.data != 0, false, 0, false, {}});
        if (activated == EventDelivery::queue_full)
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
    }
    auto drained = drain_pending_events();
    if (!drained)
        return drained;

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(status);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.pad(24);
    return queue(reply.data());
}

Result<void>
Connection::handle_ungrab_keyboard(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto time = reader.u32();
    if (!time)
        return malformed("truncated UngrabKeyboard request");
    auto &input = server_.input();
    const std::uint32_t effective_time = *time == 0
        ? server_.current_time()
        : *time;
    if (input.keyboard_grab &&
        input.keyboard_grab->owner == config_.resource_base &&
        !timestamp_later(effective_time, server_.current_time()) &&
        !timestamp_earlier(effective_time,
                           input.keyboard_grab->activated_at)) {
        if (server_.deactivate_keyboard_grab() ==
                EventDelivery::queue_full) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_grab_key(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto window_id = reader.u32();
    const auto modifiers = reader.u16();
    const auto key = reader.u8();
    const auto pointer_mode = reader.u8();
    const auto keyboard_mode = reader.u8();
    if (!window_id || !modifiers || !key || !pointer_mode ||
        !keyboard_mode || !reader.skip(3)) {
        return malformed("truncated GrabKey request");
    }
    if (*keyboard_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *keyboard_mode);
    if (*pointer_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *pointer_mode);
    if (!valid_passive_modifiers(*modifiers))
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *modifiers);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    if (*key != 0 && *key < minimum_keycode)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *key);
    if (server_.window(*window_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);

    PassiveGrab grab;
    grab.kind = PassiveGrabKind::key;
    grab.details = passive_grab_details(grab.kind, *key);
    grab.modifiers = passive_grab_modifiers(*modifiers);
    grab.owner = config_.resource_base;
    grab.window = *window_id;
    grab.event_mask = (1U << 0) | (1U << 1);
    grab.pointer_mode = *pointer_mode;
    grab.keyboard_mode = *keyboard_mode;
    grab.owner_events = context.data != 0;
    const auto update = server_.add_passive_grab(std::move(grab));
    if (update == PassiveGrabUpdate::access_denied)
        return send_error(context.order, bad_access, context.opcode,
                          context.sequence);
    if (update == PassiveGrabUpdate::resource_exhausted)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_ungrab_key(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto window_id = reader.u32();
    const auto modifiers = reader.u16();
    if (!window_id || !modifiers || !reader.skip(2))
        return malformed("truncated UngrabKey request");
    if (server_.window(*window_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (context.data != 0 && context.data < minimum_keycode)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    if (!valid_passive_modifiers(*modifiers))
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *modifiers);
    const auto update = server_.remove_passive_grab(
        PassiveGrabKind::key, config_.resource_base, *window_id,
        passive_grab_details(PassiveGrabKind::key, context.data),
        passive_grab_modifiers(*modifiers));
    if (update == PassiveGrabUpdate::resource_exhausted)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_allow_events(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 7)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    return Result<void>::success();
}

Result<void>
Connection::handle_grab_server(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    server_.grab_server(config_.resource_base);
    return Result<void>::success();
}

Result<void>
Connection::handle_ungrab_server(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    server_.ungrab_server(config_.resource_base);
    return Result<void>::success();
}

Result<void>
Connection::handle_query_pointer(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto window_id = reader.u32();
    if (!window_id)
        return malformed("truncated QueryPointer request");
    const auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);

    const auto &input = server_.input();
    const auto origin = server_.absolute_position(*window_id);
    std::uint32_t child = 0;
    const std::int64_t window_right = origin.first + window->width;
    const std::int64_t window_bottom = origin.second + window->height;
    if (server_.map_state(window->id) == 2 &&
        input.pointer_x >= origin.first && input.pointer_x < window_right &&
        input.pointer_y >= origin.second && input.pointer_y < window_bottom) {
        child = server_.child_window_at(
            window->id, input.pointer_x, input.pointer_y);
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(1); // same screen
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(root_window_id);
    reply.u32(child);
    reply.i16(wire_coordinate(input.pointer_x));
    reply.i16(wire_coordinate(input.pointer_y));
    reply.i16(wire_coordinate(input.pointer_x - origin.first));
    reply.i16(wire_coordinate(input.pointer_y - origin.second));
    reply.u16(input.modifier_button_mask);
    reply.pad(6);
    return queue(reply.data());
}

Result<void>
Connection::handle_get_motion_events(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto window_id = reader.u32();
    const auto start = reader.u32();
    const auto stop = reader.u32();
    if (!window_id || !start || !stop)
        return malformed("truncated GetMotionEvents request");
    if (server_.window(*window_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(0); // no retained motion history
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_translate_coordinates(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto source_x = reader.u16();
    const auto source_y = reader.u16();
    if (!source_id || !destination_id || !source_x || !source_y)
        return malformed("truncated TranslateCoordinates request");
    if (server_.window(*source_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *source_id);
    const auto *destination = server_.window(*destination_id);
    if (destination == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *destination_id);

    const auto source_origin = server_.absolute_position(*source_id);
    const auto destination_origin = server_.absolute_position(*destination_id);
    const auto signed_source_x = signed_word(*source_x);
    const auto signed_source_y = signed_word(*source_y);
    const std::int32_t x = source_origin.first + signed_source_x -
        destination_origin.first;
    const std::int32_t y = source_origin.second + signed_source_y -
        destination_origin.second;
    std::uint32_t child = 0;
    for (auto iterator = destination->children.rbegin();
         iterator != destination->children.rend(); ++iterator) {
        const auto *candidate = server_.window(*iterator);
        if (candidate != nullptr && candidate->mapped &&
            x >= candidate->x && y >= candidate->y &&
            x < candidate->x + candidate->width &&
            y < candidate->y + candidate->height) {
            child = candidate->id;
            break;
        }
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(1); // same screen
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(child);
    reply.i16(wire_coordinate(x));
    reply.i16(wire_coordinate(y));
    reply.pad(16);
    return queue(reply.data());
}

Result<void>
Connection::handle_warp_pointer(const RequestContext &context)
{
    if (context.request.size() != 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 20, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto source_x = reader.u16();
    const auto source_y = reader.u16();
    const auto source_width = reader.u16();
    const auto source_height = reader.u16();
    const auto destination_x = reader.u16();
    const auto destination_y = reader.u16();
    if (!source_id || !destination_id || !source_x || !source_y ||
        !source_width || !source_height || !destination_x || !destination_y) {
        return malformed("truncated WarpPointer request");
    }

    const WindowRecord *destination = nullptr;
    if (*destination_id != 0) {
        destination = server_.window(*destination_id);
        if (destination == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *destination_id);
        }
    }
    const WindowRecord *source = nullptr;
    if (*source_id != 0) {
        source = server_.window(*source_id);
        if (source == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *source_id);
        }
    }

    auto &input = server_.input();
    if (source != nullptr) {
        const auto origin = server_.absolute_position(source->id);
        const std::int64_t left = origin.first + signed_word(*source_x);
        const std::int64_t top = origin.second + signed_word(*source_y);
        const std::int64_t right = left + *source_width;
        const std::int64_t bottom = top + *source_height;
        const std::int64_t border = source->border_width;
        const bool inside_visible_window = source->id == root_window_id ||
            (server_.map_state(source->id) == 2 &&
             input.pointer_x >= origin.first - border &&
             input.pointer_x < origin.first + source->width + border &&
             input.pointer_y >= origin.second - border &&
             input.pointer_y < origin.second + source->height + border);
        if (!inside_visible_window || input.pointer_x < left ||
            input.pointer_y < top ||
            (*source_width != 0 && input.pointer_x > right) ||
            (*source_height != 0 && input.pointer_y > bottom)) {
            return Result<void>::success();
        }
    }

    std::int64_t x = input.pointer_x;
    std::int64_t y = input.pointer_y;
    if (destination != nullptr) {
        const auto origin = server_.absolute_position(destination->id);
        x = origin.first;
        y = origin.second;
    }
    x += signed_word(*destination_x);
    y += signed_word(*destination_y);
    const auto pointer_x = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        x, 0, static_cast<std::int64_t>(server_.width()) - 1));
    const auto pointer_y = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        y, 0, static_cast<std::int64_t>(server_.height()) - 1));
    const auto delivered = server_.inject_input(
        6, 0, pointer_x, pointer_y);
    if (delivered == EventDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_set_input_focus(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto focus_id = reader.u32();
    const auto time = reader.u32();
    if (!focus_id || !time)
        return malformed("truncated SetInputFocus request");

    FocusKind kind = FocusKind::window;
    if (*focus_id == 0)
        kind = FocusKind::none;
    else if (*focus_id == pointer_root_id)
        kind = FocusKind::pointer_root;
    else {
        if (server_.window(*focus_id) == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *focus_id);
        }
        if (server_.map_state(*focus_id) != 2) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence);
        }
    }
    const auto updated = server_.set_input_focus(
        kind, *focus_id, context.data, *time);
    if (updated == FocusUpdate::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return drain_pending_events();
}

Result<void>
Connection::handle_create_pixmap(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto id = reader.u32();
    const auto drawable = reader.u32();
    const auto width = reader.u16();
    const auto height = reader.u16();
    if (!id || !drawable || !width || !height)
        return malformed("truncated CreatePixmap request");
    if (server_.drawable_surface(*drawable) == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    if (*width == 0 || *height == 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence);
    if (context.data != 1 && context.data != 8 && context.data != 24 &&
        context.data != 32) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    }
    auto surface = Surface::create(*width, *height, context.data);
    if (!surface)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    auto managed_surface = server_.adopt_surface(std::move(*surface));
    if (!managed_surface ||
        !server_.add_pixmap(PixmapRecord{*id, std::move(managed_surface)},
                            config_.resource_base)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_free_pixmap(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated FreePixmap request");
    if (!server_.erase_pixmap(*id))
        return send_error(context.order, bad_pixmap, context.opcode,
                          context.sequence, *id);
    return Result<void>::success();
}

Result<void>
Connection::handle_create_graphics_context(const RequestContext &context)
{
    if (context.request.size() < 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto drawable = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !drawable || !value_mask)
        return malformed("truncated CreateGC request");
    constexpr std::uint32_t supported_mask = all_graphics_context_values;
    if ((*value_mask & ~supported_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto value_bytes = checked_multiply(
        std::bitset<32>(*value_mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{16}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::uint8_t depth = server_.drawable_depth(*drawable);
    if (depth == 0)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);

    GraphicsContextRecord graphics;
    graphics.id = *id;
    graphics.depth = depth;
    WireReader values(context.request.data() + 16,
                      context.request.size() - 16, context.order);
    for (unsigned bit = 0; bit <= 22; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = values.u32();
        if (!value)
            return malformed("truncated CreateGC value list");
        switch (bit) {
        case 0:
            if (*value > 15)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.function = static_cast<std::uint8_t>(*value);
            break;
        case 1:
            graphics.plane_mask = *value;
            break;
        case 2:
            graphics.foreground = *value;
            break;
        case 3:
            graphics.background = *value;
            break;
        case 4:
            if (*value > std::numeric_limits<std::uint16_t>::max())
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.line_width = static_cast<std::uint16_t>(*value);
            break;
        case 5:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.line_style = static_cast<std::uint8_t>(*value);
            break;
        case 6:
            if (*value > 3)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.cap_style = static_cast<std::uint8_t>(*value);
            break;
        case 7:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.join_style = static_cast<std::uint8_t>(*value);
            break;
        case 8:
            if (*value > 3)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.fill_style = static_cast<std::uint8_t>(*value);
            break;
        case 9:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.fill_rule = static_cast<std::uint8_t>(*value);
            break;
        case 10: {
            const auto *tile = server_.pixmap(*value);
            if (tile == nullptr)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            if (tile->surface->depth() != depth)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, *value);
            graphics.tile = tile->surface;
            break;
        }
        case 11: {
            const auto *stipple = server_.pixmap(*value);
            if (stipple == nullptr)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            if (stipple->surface->depth() != 1)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, *value);
            graphics.stipple = stipple->surface;
            break;
        }
        case 12:
            graphics.tile_x_origin = signed_dword(*value);
            break;
        case 13:
            graphics.tile_y_origin = signed_dword(*value);
            break;
        case 14: {
            const auto *font = server_.font(*value);
            if (font == nullptr)
                return send_error(context.order, bad_font, context.opcode,
                                  context.sequence, *value);
            graphics.font = font->font;
            break;
        }
        case 15:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.subwindow_mode = static_cast<std::uint8_t>(*value);
            break;
        case 16:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.graphics_exposures = *value != 0;
            break;
        case 17:
            graphics.clip_x_origin = signed_dword(*value);
            break;
        case 18:
            graphics.clip_y_origin = signed_dword(*value);
            break;
        case 19:
            if (*value == 0) {
                graphics.clip_region.reset();
                break;
            }
            if (const auto *mask = server_.pixmap(*value)) {
                if (mask->surface->depth() != 1)
                    return send_error(context.order, bad_match,
                                      context.opcode, context.sequence,
                                      *value);
                auto region = bitmap_clip_region(*mask->surface);
                if (!region)
                    return send_error(context.order, bad_alloc,
                                      context.opcode, context.sequence);
                graphics.clip_region = std::move(*region);
            }
            else {
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            }
            break;
        case 20:
            if (*value > std::numeric_limits<std::uint16_t>::max())
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.dash_offset = static_cast<std::uint16_t>(*value);
            break;
        case 21:
            if (*value == 0 || *value > 255)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.dashes[0] = static_cast<std::uint8_t>(*value);
            graphics.dashes[1] = static_cast<std::uint8_t>(*value);
            graphics.dash_count = 2;
            break;
        case 22:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            graphics.arc_mode = static_cast<std::uint8_t>(*value);
            break;
        }
    }
    if (!server_.add_graphics_context(std::move(graphics),
                                      config_.resource_base)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_change_graphics_context(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto value_mask = reader.u32();
    if (!id || !value_mask)
        return malformed("truncated ChangeGC request");
    constexpr std::uint32_t supported_mask = all_graphics_context_values;
    if ((*value_mask & ~supported_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto value_bytes = checked_multiply(
        std::bitset<32>(*value_mask).count(), std::size_t{4});
    const auto expected_size = value_bytes
        ? checked_add(std::size_t{12}, *value_bytes)
        : std::optional<std::size_t>{};
    if (!expected_size || context.request.size() != *expected_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    auto *graphics = server_.graphics_context(*id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *id);

    std::optional<GraphicsContextRecord> updated;
    try {
        updated.emplace(*graphics);
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    for (unsigned bit = 0; bit <= 22; ++bit) {
        if ((*value_mask & (std::uint32_t{1} << bit)) == 0)
            continue;
        const auto value = reader.u32();
        if (!value)
            return malformed("truncated ChangeGC value list");
        switch (bit) {
        case 0:
            if (*value > 15)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->function = static_cast<std::uint8_t>(*value);
            break;
        case 1:
            updated->plane_mask = *value;
            break;
        case 2:
            updated->foreground = *value;
            break;
        case 3:
            updated->background = *value;
            break;
        case 4:
            if (*value > std::numeric_limits<std::uint16_t>::max())
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->line_width = static_cast<std::uint16_t>(*value);
            break;
        case 5:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->line_style = static_cast<std::uint8_t>(*value);
            break;
        case 6:
            if (*value > 3)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->cap_style = static_cast<std::uint8_t>(*value);
            break;
        case 7:
            if (*value > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->join_style = static_cast<std::uint8_t>(*value);
            break;
        case 8:
            if (*value > 3)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->fill_style = static_cast<std::uint8_t>(*value);
            break;
        case 9:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->fill_rule = static_cast<std::uint8_t>(*value);
            break;
        case 10: {
            const auto *tile = server_.pixmap(*value);
            if (tile == nullptr)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            if (tile->surface->depth() != updated->depth)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, *value);
            updated->tile = tile->surface;
            break;
        }
        case 11: {
            const auto *stipple = server_.pixmap(*value);
            if (stipple == nullptr)
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            if (stipple->surface->depth() != 1)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, *value);
            updated->stipple = stipple->surface;
            break;
        }
        case 12:
            updated->tile_x_origin = signed_dword(*value);
            break;
        case 13:
            updated->tile_y_origin = signed_dword(*value);
            break;
        case 14: {
            const auto *font = server_.font(*value);
            if (font == nullptr)
                return send_error(context.order, bad_font, context.opcode,
                                  context.sequence, *value);
            updated->font = font->font;
            break;
        }
        case 15:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->subwindow_mode = static_cast<std::uint8_t>(*value);
            break;
        case 16:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->graphics_exposures = *value != 0;
            break;
        case 17:
            updated->clip_x_origin = signed_dword(*value);
            break;
        case 18:
            updated->clip_y_origin = signed_dword(*value);
            break;
        case 19:
            if (*value == 0) {
                updated->clip_region.reset();
                break;
            }
            if (const auto *mask = server_.pixmap(*value)) {
                if (mask->surface->depth() != 1)
                    return send_error(context.order, bad_match,
                                      context.opcode, context.sequence,
                                      *value);
                auto region = bitmap_clip_region(*mask->surface);
                if (!region)
                    return send_error(context.order, bad_alloc,
                                      context.opcode, context.sequence);
                updated->clip_region = std::move(*region);
            }
            else {
                return send_error(context.order, bad_pixmap, context.opcode,
                                  context.sequence, *value);
            }
            break;
        case 20:
            if (*value > std::numeric_limits<std::uint16_t>::max())
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->dash_offset = static_cast<std::uint16_t>(*value);
            break;
        case 21:
            if (*value == 0 || *value > 255)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->dashes[0] = static_cast<std::uint8_t>(*value);
            updated->dashes[1] = static_cast<std::uint8_t>(*value);
            updated->dash_count = 2;
            break;
        case 22:
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value);
            updated->arc_mode = static_cast<std::uint8_t>(*value);
            break;
        }
    }
    *graphics = std::move(*updated);
    return Result<void>::success();
}

Result<void>
Connection::handle_copy_graphics_context(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto value_mask = reader.u32();
    if (!source_id || !destination_id || !value_mask)
        return malformed("truncated CopyGC request");
    constexpr std::uint32_t supported_mask = all_graphics_context_values;
    if ((*value_mask & ~supported_mask) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *value_mask);
    const auto *source = server_.graphics_context(*source_id);
    if (source == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *source_id);
    auto *destination = server_.graphics_context(*destination_id);
    if (destination == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *destination_id);
    std::optional<GraphicsContextRecord> updated;
    try {
        updated.emplace(*destination);
        if ((*value_mask & (1U << 0)) != 0)
            updated->function = source->function;
        if ((*value_mask & (1U << 1)) != 0)
            updated->plane_mask = source->plane_mask;
        if ((*value_mask & (1U << 2)) != 0)
            updated->foreground = source->foreground;
        if ((*value_mask & (1U << 3)) != 0)
            updated->background = source->background;
        if ((*value_mask & (1U << 4)) != 0)
            updated->line_width = source->line_width;
        if ((*value_mask & (1U << 5)) != 0)
            updated->line_style = source->line_style;
        if ((*value_mask & (1U << 6)) != 0)
            updated->cap_style = source->cap_style;
        if ((*value_mask & (1U << 7)) != 0)
            updated->join_style = source->join_style;
        if ((*value_mask & (1U << 8)) != 0)
            updated->fill_style = source->fill_style;
        if ((*value_mask & (1U << 9)) != 0)
            updated->fill_rule = source->fill_rule;
        if ((*value_mask & (1U << 10)) != 0)
            updated->tile = source->tile;
        if ((*value_mask & (1U << 11)) != 0)
            updated->stipple = source->stipple;
        if ((*value_mask & (1U << 12)) != 0)
            updated->tile_x_origin = source->tile_x_origin;
        if ((*value_mask & (1U << 13)) != 0)
            updated->tile_y_origin = source->tile_y_origin;
        if ((*value_mask & (1U << 14)) != 0)
            updated->font = source->font;
        if ((*value_mask & (1U << 15)) != 0)
            updated->subwindow_mode = source->subwindow_mode;
        if ((*value_mask & (1U << 16)) != 0)
            updated->graphics_exposures = source->graphics_exposures;
        if ((*value_mask & (1U << 17)) != 0)
            updated->clip_x_origin = source->clip_x_origin;
        if ((*value_mask & (1U << 18)) != 0)
            updated->clip_y_origin = source->clip_y_origin;
        if ((*value_mask & (1U << 19)) != 0)
            updated->clip_region = source->clip_region;
        if ((*value_mask & (1U << 20)) != 0)
            updated->dash_offset = source->dash_offset;
        if ((*value_mask & (1U << 21)) != 0) {
            updated->dashes = source->dashes;
            updated->dash_count = source->dash_count;
        }
        if ((*value_mask & (1U << 22)) != 0)
            updated->arc_mode = source->arc_mode;
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    *destination = std::move(*updated);
    return Result<void>::success();
}

Result<void>
Connection::handle_set_clip_rectangles(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 3)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);

    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto graphics_id = reader.u32();
    const auto encoded_origin_x = reader.u16();
    const auto encoded_origin_y = reader.u16();
    if (!graphics_id || !encoded_origin_x || !encoded_origin_y)
        return malformed("truncated SetClipRectangles request");
    auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr) {
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    }
    if (((context.request.size() - 12) & 7U) != 0)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    std::vector<Rectangle> rectangles;
    try {
        rectangles.reserve((context.request.size() - 12) / 8);
        while (reader.remaining() != 0) {
            const auto encoded_x = reader.u16();
            const auto encoded_y = reader.u16();
            const auto width = reader.u16();
            const auto height = reader.u16();
            if (!encoded_x || !encoded_y || !width || !height)
                return malformed("truncated SetClipRectangles list");
            rectangles.push_back(Rectangle{
                signed_word(*encoded_x), signed_word(*encoded_y), *width,
                *height});
        }
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    if (!valid_clip_order(rectangles, context.data)) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }

    Region canonical;
    if (!Region::canonicalize(rectangles, canonical)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    graphics->clip_x_origin = signed_word(*encoded_origin_x);
    graphics->clip_y_origin = signed_word(*encoded_origin_y);
    graphics->clip_region = std::move(canonical);
    return Result<void>::success();
}

Result<void>
Connection::handle_free_graphics_context(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated FreeGC request");
    if (!server_.erase_graphics_context(*id))
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, *id);
    return Result<void>::success();
}

Result<void>
Connection::handle_clear_area(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    if (!id || !x || !y || !width || !height)
        return malformed("truncated ClearArea request");
    auto *window = server_.window(*id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *id);
    if (!window->surface)
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    const std::int32_t area_x = signed_word(*x);
    const std::int32_t area_y = signed_word(*y);
    const auto remaining_width = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(window->width) - area_x);
    const auto remaining_height = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(window->height) - area_y);
    window->surface->fill(
        Rectangle{
            area_x, area_y,
            *width == 0 ? static_cast<std::uint32_t>(remaining_width) : *width,
            *height == 0 ? static_cast<std::uint32_t>(remaining_height)
                         : *height},
        window->background_pixel, 3, 0xffffffffU);
    return finish_draw(context, *id);
}

Result<void>
Connection::finish_draw(const RequestContext &context,
                        std::uint32_t drawable)
{
    const auto updated = server_.damage_drawable(drawable);
    if (updated == DamageUpdate::invalid)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, drawable);
    if (updated == DamageUpdate::resource_exhausted ||
        updated == DamageUpdate::queue_full) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_copy_area(const RequestContext &context)
{
    if (context.request.size() != 28)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 24, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto graphics_id = reader.u32();
    const auto source_x = reader.u16();
    const auto source_y = reader.u16();
    const auto destination_x = reader.u16();
    const auto destination_y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    if (!source_id || !destination_id || !graphics_id || !source_x ||
        !source_y || !destination_x || !destination_y || !width || !height) {
        return malformed("truncated CopyArea request");
    }
    const auto *source = server_.drawable_surface(*source_id);
    if (source == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *source_id);
    auto *destination = server_.drawable_surface(*destination_id);
    if (destination == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *destination_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, *graphics_id);
    if (source->depth() != destination->depth() ||
        graphics->depth != destination->depth()) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    destination->copy_from(*source, signed_word(*source_x),
                           signed_word(*source_y),
                           signed_word(*destination_x),
                           signed_word(*destination_y), *width, *height,
                           graphics->function, graphics->plane_mask,
                           graphics->clip());
    return finish_draw(context, *destination_id);
}

Result<void>
Connection::handle_copy_plane(const RequestContext &context)
{
    if (context.request.size() != 32)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 28, context.order);
    const auto source_id = reader.u32();
    const auto destination_id = reader.u32();
    const auto graphics_id = reader.u32();
    const auto source_x = reader.u16();
    const auto source_y = reader.u16();
    const auto destination_x = reader.u16();
    const auto destination_y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto bit_plane = reader.u32();
    if (!source_id || !destination_id || !graphics_id || !source_x ||
        !source_y || !destination_x || !destination_y || !width || !height ||
        !bit_plane) {
        return malformed("truncated CopyPlane request");
    }
    const auto *source = server_.drawable_surface(*source_id);
    if (source == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *source_id);
    auto *destination = server_.drawable_surface(*destination_id);
    if (destination == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *destination_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    const bool plane_in_depth = source->depth() == 32 ||
        *bit_plane < (std::uint32_t{1} << source->depth());
    if (*bit_plane == 0 || (*bit_plane & (*bit_plane - 1U)) != 0 ||
        !plane_in_depth) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *bit_plane);
    }
    if (graphics->depth != destination->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    destination->copy_plane_from(
        *source, signed_word(*source_x), signed_word(*source_y),
        signed_word(*destination_x), signed_word(*destination_y), *width,
        *height, *bit_plane, graphics->foreground, graphics->background,
        graphics->function, graphics->plane_mask, graphics->clip());
    return finish_draw(context, *destination_id);
}

Result<void>
Connection::handle_poly_points(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyPoint request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    std::int32_t current_x = 0;
    std::int32_t current_y = 0;
    bool first = true;
    while (reader.remaining() != 0) {
        const auto x = reader.u16();
        const auto y = reader.u16();
        if (!x || !y)
            return malformed("truncated PolyPoint list");
        const std::int32_t decoded_x = signed_word(*x);
        const std::int32_t decoded_y = signed_word(*y);
        if (context.data == 1 && !first) {
            current_x += decoded_x;
            current_y += decoded_y;
        }
        else {
            current_x = decoded_x;
            current_y = decoded_y;
        }
        first = false;
        surface->draw_pixel(current_x, current_y, graphics->foreground,
                            graphics->function, graphics->plane_mask,
                            graphics->clip());
    }
    return finish_draw(context, *drawable_id);
}

Result<void>
Connection::handle_poly_lines(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyLine request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    std::int32_t previous_x = 0;
    std::int32_t previous_y = 0;
    bool first = true;
    std::size_t dash_phase = 0;
    while (reader.remaining() != 0) {
        const auto x = reader.u16();
        const auto y = reader.u16();
        if (!x || !y)
            return malformed("truncated PolyLine list");
        std::int32_t current_x = signed_word(*x);
        std::int32_t current_y = signed_word(*y);
        if (context.data == 1 && !first) {
            current_x += previous_x;
            current_y += previous_y;
        }
        if (!first) {
            draw_gc_line(*surface, *graphics,
                         {previous_x, previous_y}, {current_x, current_y},
                         dash_phase);
        }
        previous_x = current_x;
        previous_y = current_y;
        first = false;
    }
    return finish_draw(context, *drawable_id);
}

Result<void>
Connection::handle_poly_segments(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 7U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolySegment request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    while (reader.remaining() != 0) {
        const auto start_x = reader.u16();
        const auto start_y = reader.u16();
        const auto end_x = reader.u16();
        const auto end_y = reader.u16();
        if (!start_x || !start_y || !end_x || !end_y)
            return malformed("truncated PolySegment list");
        std::size_t dash_phase = 0;
        draw_gc_line(
            *surface, *graphics,
            {signed_word(*start_x), signed_word(*start_y)},
            {signed_word(*end_x), signed_word(*end_y)}, dash_phase);
    }
    return finish_draw(context, *drawable_id);
}

Result<void>
Connection::handle_poly_rectangles(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 7U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyRectangle request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);

    while (reader.remaining() != 0) {
        const auto encoded_x = reader.u16();
        const auto encoded_y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        if (!encoded_x || !encoded_y || !width || !height)
            return malformed("truncated PolyRectangle list");
        const std::int32_t x = signed_word(*encoded_x);
        const std::int32_t y = signed_word(*encoded_y);
        const std::int32_t right = x + *width;
        const std::int32_t bottom = y + *height;
        std::size_t dash_phase = 0;
        draw_gc_line(*surface, *graphics, {x, y}, {right, y}, dash_phase);
        if (*height == 0)
            continue;
        draw_gc_line(*surface, *graphics, {right, bottom}, {x, bottom},
                     dash_phase);
        if (*height > 1) {
            draw_gc_line(*surface, *graphics, {x, bottom - 1}, {x, y + 1},
                         dash_phase);
            if (*width != 0) {
                draw_gc_line(*surface, *graphics, {right, y + 1},
                             {right, bottom - 1}, dash_phase);
            }
        }
    }
    return finish_draw(context, *drawable_id);
}

Result<void>
Connection::handle_fill_rectangles(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 7U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable_id || !graphics_id)
        return malformed("truncated PolyFillRectangle request");
    auto *surface = server_.drawable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    while (reader.remaining() != 0) {
        const auto x = reader.u16();
        const auto y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        if (!x || !y || !width || !height)
            return malformed("truncated PolyFillRectangle list");
        fill_gc_rectangle(
            *surface, *graphics,
            Rectangle{signed_word(*x), signed_word(*y), *width, *height});
    }
    return finish_draw(context, *drawable_id);
}

Result<void>
Connection::handle_put_image(const RequestContext &context)
{
    if (context.request.size() < 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable_id = reader.u32();
    const auto graphics_id = reader.u32();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto destination_x = reader.u16();
    const auto destination_y = reader.u16();
    const auto left_pad = reader.u8();
    const auto depth = reader.u8();
    if (!drawable_id || !graphics_id || !width || !height ||
        !destination_x || !destination_y || !left_pad || !depth ||
        !reader.skip(2)) {
        return malformed("truncated PutImage request");
    }
    if (context.data != 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    if (*left_pad != 0)
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    return draw_zpixmap(
        context, *drawable_id, *graphics_id, *width, *height, 0, 0,
        *width, *height, signed_word(*destination_x),
        signed_word(*destination_y), *depth, context.request.data() + 24,
        context.request.size() - 24, true);
}

Result<void>
Connection::draw_zpixmap(
    const RequestContext &context, std::uint32_t drawable,
    std::uint32_t graphics_id, std::uint16_t total_width,
    std::uint16_t total_height, std::uint16_t source_x,
    std::uint16_t source_y, std::uint16_t width, std::uint16_t height,
    std::int16_t destination_x, std::int16_t destination_y,
    std::uint8_t depth, const std::uint8_t *image, std::size_t image_size,
    bool exact_image_size)
{
    const std::uint16_t minor_opcode =
        extension_by_opcode(context.opcode) == nullptr ? 0 : context.data;
    auto *surface = server_.drawable_surface(drawable);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, drawable, minor_opcode);
    const auto *graphics = server_.graphics_context(graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context, context.opcode,
                          context.sequence, graphics_id, minor_opcode);
    if (total_width == 0 || total_height == 0 || width == 0 || height == 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, 0, minor_opcode);
    const auto source_right = checked_add(
        static_cast<std::size_t>(source_x), static_cast<std::size_t>(width));
    const auto source_bottom = checked_add(
        static_cast<std::size_t>(source_y), static_cast<std::size_t>(height));
    if (!source_right || !source_bottom || *source_right > total_width ||
        *source_bottom > total_height) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, 0, minor_opcode);
    }
    if (depth != surface->depth() || graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence, 0, minor_opcode);

    const std::size_t bits_per_pixel = depth == 1
        ? 1
        : (depth == 8 ? 8 : 32);
    const auto row_bits = checked_multiply(
        static_cast<std::size_t>(total_width), bits_per_pixel);
    const auto rounded_bits = row_bits
        ? checked_add(*row_bits, std::size_t{7})
        : std::optional<std::size_t>{};
    const auto stride = rounded_bits
        ? padded_to_four(*rounded_bits / 8)
        : std::optional<std::size_t>{};
    const auto expected_size = stride
        ? checked_multiply(*stride, static_cast<std::size_t>(total_height))
        : std::optional<std::size_t>{};
    if (!expected_size || image == nullptr || image_size < *expected_size ||
        (exact_image_size && image_size != *expected_size)) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence, 0, minor_opcode);
    }

    const bool least_significant_bit_first =
        host_byte_order() == ByteOrder::little;
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto *row_data = image +
            static_cast<std::size_t>(source_y + row) * *stride;
        WireReader pixels(row_data, *stride, host_byte_order());
        if (!pixels.skip(static_cast<std::size_t>(source_x) *
                         (depth == 1 ? 0 : bits_per_pixel / 8))) {
            return malformed("truncated ZPixmap source offset");
        }
        for (std::uint32_t column = 0; column < width; ++column) {
            std::uint32_t pixel = 0;
            const std::uint32_t source_column = source_x + column;
            if (depth == 1) {
                const auto byte = row_data[source_column / 8];
                const unsigned bit = least_significant_bit_first
                    ? source_column & 7U
                    : 7U - (source_column & 7U);
                pixel = (byte >> bit) & 1U;
            }
            else if (depth == 8) {
                pixel = row_data[source_column];
            }
            else {
                const auto value = pixels.u32();
                if (!value)
                    return malformed("truncated ZPixmap scanline");
                pixel = *value;
            }
            surface->draw_pixel(
                                destination_x + static_cast<std::int32_t>(column),
                                destination_y + static_cast<std::int32_t>(row),
                                pixel, graphics->function,
                                graphics->plane_mask, graphics->clip());
        }
    }
    return finish_draw(context, drawable);
}

Result<void>
Connection::handle_get_image(const RequestContext &context)
{
    if (context.request.size() != 20)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 16, context.order);
    const auto drawable_id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto plane_mask = reader.u32();
    if (!drawable_id || !x || !y || !width || !height || !plane_mask)
        return malformed("truncated GetImage request");
    const auto *surface = server_.readable_surface(*drawable_id);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable_id);
    if (context.data != 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    const std::int32_t image_x = signed_word(*x);
    const std::int32_t image_y = signed_word(*y);
    if (*width == 0 || *height == 0 || image_x < 0 || image_y < 0 ||
        image_x + *width > surface->width() ||
        image_y + *height > surface->height()) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }

    const std::size_t bits_per_pixel = surface->depth() == 1
        ? 1
        : (surface->depth() == 8 ? 8 : 32);
    const auto row_bits = checked_multiply(
        static_cast<std::size_t>(*width), bits_per_pixel);
    const auto rounded_bits = row_bits
        ? checked_add(*row_bits, std::size_t{7})
        : std::optional<std::size_t>{};
    const auto stride = rounded_bits
        ? padded_to_four(*rounded_bits / 8)
        : std::optional<std::size_t>{};
    const auto image_size = stride
        ? checked_multiply(*stride, static_cast<std::size_t>(*height))
        : std::optional<std::size_t>{};
    if (!image_size || *image_size > maximum_buffered_output - 32)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    std::vector<std::uint8_t> image;
    try {
        image.assign(*image_size, 0);
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    const bool least_significant_byte_first =
        host_byte_order() == ByteOrder::little;
    for (std::uint32_t row = 0; row < *height; ++row) {
        for (std::uint32_t column = 0; column < *width; ++column) {
            const std::uint32_t pixel = surface->pixel(
                static_cast<std::uint16_t>(image_x + column),
                static_cast<std::uint16_t>(image_y + row)) & *plane_mask;
            const std::size_t row_offset =
                static_cast<std::size_t>(row) * *stride;
            if (surface->depth() == 1) {
                const unsigned bit = least_significant_byte_first
                    ? column & 7U
                    : 7U - (column & 7U);
                image[row_offset + column / 8] |=
                    static_cast<std::uint8_t>((pixel & 1U) << bit);
            }
            else if (surface->depth() == 8) {
                image[row_offset + column] =
                    static_cast<std::uint8_t>(pixel);
            }
            else {
                const std::size_t pixel_offset = row_offset + column * 4;
                for (unsigned index = 0; index < 4; ++index) {
                    const unsigned shift = least_significant_byte_first
                        ? index * 8
                        : (3 - index) * 8;
                    image[pixel_offset + index] =
                        static_cast<std::uint8_t>(pixel >> shift);
                }
            }
        }
    }
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(surface->depth());
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(image.size() / 4));
    reply.u32(server_.window(*drawable_id) == nullptr ? 0 : root_visual_id);
    reply.pad(20);
    reply.bytes(image);
    return queue(reply.data());
}

Result<void>
Connection::handle_create_colormap(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto id = reader.u32();
    const auto window_id = reader.u32();
    const auto visual = reader.u32();
    if (!id || !window_id || !visual)
        return malformed("truncated CreateColormap request");
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    const auto *window = server_.window(*window_id);
    if (window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    if (window->window_class != WindowClass::input_output ||
        *visual != root_visual_id || context.data != 0) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    if (!server_.add_colormap(*id, config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_free_colormap(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated FreeColormap request");
    if (!server_.colormap_exists(*id))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *id);
    if (*id != default_colormap_id)
        static_cast<void>(server_.erase_colormap(*id));
    return Result<void>::success();
}

Result<void>
Connection::handle_copy_colormap(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto id = reader.u32();
    const auto source = reader.u32();
    if (!id || !source)
        return malformed("truncated CopyColormapAndFree request");
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (!server_.colormap_exists(*source))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *source);
    if (server_.resource_limit_reached(config_.resource_base) ||
        !server_.add_colormap(*id, config_.resource_base)) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_install_colormap(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated InstallColormap request");
    if (!server_.colormap_exists(*id))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *id);
    server_.install_colormap(*id);
    return Result<void>::success();
}

Result<void>
Connection::handle_uninstall_colormap(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed("truncated UninstallColormap request");
    if (!server_.colormap_exists(*id))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *id);
    if (server_.installed_colormap() == *id)
        server_.install_colormap(default_colormap_id);
    return Result<void>::success();
}

Result<void>
Connection::handle_list_installed_colormaps(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto window_id = reader.u32();
    if (!window_id)
        return malformed("truncated ListInstalledColormaps request");
    if (server_.window(*window_id) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *window_id);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(1);
    reply.u16(1);
    reply.pad(22);
    reply.u32(server_.installed_colormap());
    return queue(reply.data());
}

Result<void>
Connection::handle_alloc_color(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto colormap = reader.u32();
    const auto red = reader.u16();
    const auto green = reader.u16();
    const auto blue = reader.u16();
    if (!colormap || !red || !green || !blue || !reader.skip(2))
        return malformed("truncated AllocColor request");
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);
    const std::uint32_t pixel = true_color_pixel({*red, *green, *blue});
    const RgbColor visual = true_color_rgb(pixel);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u16(visual.red);
    reply.u16(visual.green);
    reply.u16(visual.blue);
    reply.pad(2);
    reply.u32(pixel);
    reply.pad(12);
    return queue(reply.data());
}

Result<void>
Connection::handle_alloc_named_color(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    const auto name_size = reader.u16();
    if (!colormap || !name_size || !reader.skip(2))
        return malformed("truncated AllocNamedColor request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 12 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);

    const auto *name_data = reinterpret_cast<const char *>(
        context.request.data() + 12);
    const auto color = parse_color(std::string_view(name_data, *name_size));
    if (!color)
        return send_error(context.order, bad_name, context.opcode,
                          context.sequence);
    const std::uint32_t pixel = true_color_pixel(*color);
    const RgbColor visual = true_color_rgb(pixel);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(pixel);
    reply.u16(color->red);
    reply.u16(color->green);
    reply.u16(color->blue);
    reply.u16(visual.red);
    reply.u16(visual.green);
    reply.u16(visual.blue);
    reply.pad(8);
    return queue(reply.data());
}

Result<void>
Connection::handle_alloc_color_cells(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto colormap = reader.u32();
    const auto colors = reader.u16();
    const auto planes = reader.u16();
    if (!colormap || !colors || !planes)
        return malformed("truncated AllocColorCells request");
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);
    return send_error(context.order, bad_match, context.opcode,
                      context.sequence);
}

Result<void>
Connection::handle_alloc_color_planes(const RequestContext &context)
{
    if (context.request.size() != 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto colormap = reader.u32();
    if (!colormap || !reader.skip(8))
        return malformed("truncated AllocColorPlanes request");
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);
    return send_error(context.order, bad_match, context.opcode,
                      context.sequence);
}

Result<void>
Connection::handle_free_colors(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        ((context.request.size() - 12) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    if (!colormap || !reader.u32())
        return malformed("truncated FreeColors request");
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);
    while (reader.remaining() != 0) {
        const auto pixel = reader.u32();
        if (!pixel)
            return malformed("truncated FreeColors pixel list");
        if ((*pixel & 0xff000000U) != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *pixel);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_store_colors(const RequestContext &context)
{
    if (context.request.size() < 8 ||
        ((context.request.size() - 8) % 12U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    if (!colormap)
        return malformed("truncated StoreColors request");
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);
    while (reader.remaining() != 0) {
        const auto pixel = reader.u32();
        if (!pixel || !reader.skip(6))
            return malformed("truncated StoreColors item list");
        const auto flags = reader.u8();
        if (!flags || !reader.skip(1))
            return malformed("truncated StoreColors flags");
        if ((*flags & ~std::uint8_t{7}) != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *flags);
    }
    return context.request.size() == 8
        ? Result<void>::success()
        : send_error(context.order, bad_access, context.opcode,
                     context.sequence);
}

Result<void>
Connection::handle_store_named_color(const RequestContext &context)
{
    if (context.request.size() < 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if ((context.data & ~std::uint8_t{7}) != 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    const auto pixel = reader.u32();
    const auto name_size = reader.u16();
    if (!colormap || !pixel || !name_size || !reader.skip(2))
        return malformed("truncated StoreNamedColor request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 16 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);
    const auto *name_data = reinterpret_cast<const char *>(
        context.request.data() + 16);
    if (!parse_color(std::string_view(name_data, *name_size)))
        return send_error(context.order, bad_name, context.opcode,
                          context.sequence);
    return send_error(context.order, bad_access, context.opcode,
                      context.sequence);
}

Result<void>
Connection::handle_query_colors(const RequestContext &context)
{
    if (context.request.size() < 8 ||
        ((context.request.size() - 8) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    if (!colormap)
        return malformed("truncated QueryColors request");
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);

    std::vector<RgbColor> colors;
    colors.reserve(reader.remaining() / 4);
    while (reader.remaining() != 0) {
        const auto pixel = reader.u32();
        if (!pixel)
            return malformed("truncated QueryColors pixel list");
        if ((*pixel & 0xff000000U) != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *pixel);
        colors.push_back(true_color_rgb(*pixel));
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(colors.size() * 2));
    reply.u16(static_cast<std::uint16_t>(colors.size()));
    reply.pad(22);
    for (const auto color : colors) {
        reply.u16(color.red);
        reply.u16(color.green);
        reply.u16(color.blue);
        reply.pad(2);
    }
    return queue(reply.data());
}

Result<void>
Connection::handle_lookup_color(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto colormap = reader.u32();
    const auto name_size = reader.u16();
    if (!colormap || !name_size || !reader.skip(2))
        return malformed("truncated LookupColor request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 12 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (!server_.colormap_exists(*colormap))
        return send_error(context.order, bad_colormap, context.opcode,
                          context.sequence, *colormap);
    const auto *name_data = reinterpret_cast<const char *>(
        context.request.data() + 12);
    const auto color = parse_color(std::string_view(name_data, *name_size));
    if (!color)
        return send_error(context.order, bad_name, context.opcode,
                          context.sequence);
    const RgbColor visual = true_color_rgb(true_color_pixel(*color));
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u16(color->red);
    reply.u16(color->green);
    reply.u16(color->blue);
    reply.u16(visual.red);
    reply.u16(visual.green);
    reply.u16(visual.blue);
    reply.pad(12);
    return queue(reply.data());
}

Result<void>
Connection::handle_query_best_size(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);

    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto drawable = reader.u32();
    const auto requested_width = reader.u16();
    const auto requested_height = reader.u16();
    if (!drawable || !requested_width || !requested_height)
        return malformed("truncated QueryBestSize request");

    const auto *window = server_.window(*drawable);
    if (window == nullptr && server_.pixmap(*drawable) == nullptr) {
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    }
    if (context.data != 0 && window != nullptr &&
        window->window_class == WindowClass::input_only) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }

    std::uint16_t width = *requested_width;
    std::uint16_t height = *requested_height;
    if (context.data == 0) {
        width = std::min(width, server_.width());
        height = std::min(height, server_.height());
    }
    else if (width < 32 && (width & (width - 1)) != 0) {
        std::uint16_t rounded = 1;
        while (rounded < width)
            rounded = static_cast<std::uint16_t>(rounded << 1);
        width = rounded;
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u16(width);
    reply.u16(height);
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_change_keyboard_mapping(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader header(context.request.data() + 4, 4, context.order);
    const auto first_keycode = header.u8();
    const auto requested_width = header.u8();
    if (!first_keycode || !requested_width || !header.skip(2))
        return malformed("truncated ChangeKeyboardMapping request");

    const std::size_t symbol_count =
        static_cast<std::size_t>(context.data) * *requested_width;
    if (context.request.size() != 8 + symbol_count * 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (*first_keycode < minimum_keycode ||
        *first_keycode > maximum_keycode) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *first_keycode);
    }
    const std::uint16_t end =
        static_cast<std::uint16_t>(*first_keycode) + context.data;
    if (*requested_width == 0 ||
        end > static_cast<std::uint16_t>(maximum_keycode) + 1U) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *requested_width);
    }
    if (context.data == 0)
        return Result<void>::success();

    try {
        const auto &input = server_.input();
        auto row_widths = input.keymap_row_widths;
        const std::uint8_t stored_width = *requested_width <= 2
            ? 4
            : *requested_width;
        for (std::uint16_t keycode = *first_keycode; keycode < end;
             ++keycode) {
            row_widths[keycode] = stored_width;
        }
        const std::uint8_t map_width = *std::max_element(
            row_widths.begin(), row_widths.end());
        std::vector<std::uint32_t> keymap(
            static_cast<std::size_t>(map_width) * 256, 0);
        const std::size_t copied_width = std::min<std::size_t>(
            input.keymap_width, map_width);
        for (std::size_t keycode = 0; keycode < 256; ++keycode) {
            std::copy_n(
                input.keymap.begin() + keycode * input.keymap_width,
                copied_width,
                keymap.begin() + keycode * map_width);
        }

        WireReader symbols(context.request.data() + 8,
                           context.request.size() - 8, context.order);
        for (std::uint16_t keycode = *first_keycode; keycode < end;
             ++keycode) {
            auto row = keymap.begin() + keycode * map_width;
            std::fill_n(row, map_width, 0);
            for (std::size_t index = 0; index < *requested_width; ++index) {
                const auto keysym = symbols.u32();
                if (!keysym)
                    return malformed(
                        "truncated ChangeKeyboardMapping keysyms");
                row[index] = *keysym;
            }
            if (*requested_width <= 2) {
                row[2] = row[0];
                row[3] = *requested_width == 2 ? row[1] : 0;
            }
        }

        if (!server_.broadcast_mapping_notify(
                1, *first_keycode, context.data)) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
        auto &updated = server_.input();
        updated.keymap_width = map_width;
        updated.keymap_row_widths = row_widths;
        updated.keymap = std::move(keymap);
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_get_keyboard_mapping(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto first_keycode = reader.u8();
    const auto count = reader.u8();
    if (!first_keycode || !count || !reader.skip(2))
        return malformed("truncated GetKeyboardMapping request");
    if (*first_keycode < minimum_keycode ||
        *first_keycode > maximum_keycode) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *first_keycode);
    }
    const std::uint16_t end =
        static_cast<std::uint16_t>(*first_keycode) + *count;
    if (end > static_cast<std::uint16_t>(maximum_keycode) + 1U)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *count);

    WireWriter reply(context.order);
    reply.u8(1);
    const auto &input = server_.input();
    reply.u8(input.keymap_width);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(input.keymap_width * *count));
    reply.pad(24);
    for (std::uint16_t keycode = *first_keycode; keycode < end; ++keycode) {
        const auto row = input.keymap.begin() +
            static_cast<std::size_t>(keycode) * input.keymap_width;
        for (std::size_t index = 0; index < input.keymap_width; ++index)
            reply.u32(row[index]);
    }
    return queue(reply.data());
}

Result<void>
Connection::handle_change_keyboard_control(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto value_mask = reader.u32();
    if (!value_mask)
        return malformed("truncated ChangeKeyboardControl request");
    const std::size_t value_count = std::bitset<32>(*value_mask).count();
    if (context.request.size() != 8 + value_count * 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);

    const auto &input = server_.input();
    auto auto_repeats = input.auto_repeats;
    std::uint32_t led_mask = input.led_mask;
    std::uint16_t bell_pitch = input.bell_pitch;
    std::uint16_t bell_duration = input.bell_duration;
    std::uint8_t key_click_percent = input.key_click_percent;
    std::uint8_t bell_percent = input.bell_percent;
    bool global_auto_repeat = input.global_auto_repeat;
    std::optional<std::uint8_t> selected_led;
    std::optional<std::uint8_t> selected_key;

    for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((*value_mask & bit) == 0)
            continue;
        const auto value = reader.u32();
        if (!value)
            return malformed("truncated ChangeKeyboardControl values");
        switch (bit) {
        case 1U << 0: {
            std::int16_t percent = signed_byte(
                static_cast<std::uint8_t>(*value));
            if (percent == -1)
                percent = default_key_click_percent;
            else if (percent < 0 || percent > 100)
                return send_error(
                    context.order, bad_value, context.opcode,
                    context.sequence, static_cast<std::uint32_t>(percent));
            key_click_percent = static_cast<std::uint8_t>(percent);
            break;
        }
        case 1U << 1: {
            std::int16_t percent = signed_byte(
                static_cast<std::uint8_t>(*value));
            if (percent == -1)
                percent = default_bell_percent;
            else if (percent < 0 || percent > 100)
                return send_error(
                    context.order, bad_value, context.opcode,
                    context.sequence, static_cast<std::uint32_t>(percent));
            bell_percent = static_cast<std::uint8_t>(percent);
            break;
        }
        case 1U << 2: {
            std::int32_t pitch = signed_word(
                static_cast<std::uint16_t>(*value));
            if (pitch == -1)
                pitch = default_bell_pitch;
            else if (pitch < 0)
                return send_error(
                    context.order, bad_value, context.opcode,
                    context.sequence, static_cast<std::uint32_t>(pitch));
            bell_pitch = static_cast<std::uint16_t>(pitch);
            break;
        }
        case 1U << 3: {
            std::int32_t duration = signed_word(
                static_cast<std::uint16_t>(*value));
            if (duration == -1)
                duration = default_bell_duration;
            else if (duration < 0)
                return send_error(
                    context.order, bad_value, context.opcode,
                    context.sequence, static_cast<std::uint32_t>(duration));
            bell_duration = static_cast<std::uint16_t>(duration);
            break;
        }
        case 1U << 4: {
            const auto led = static_cast<std::uint8_t>(*value);
            if (led < 1 || led > 32)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, led);
            if ((*value_mask & (1U << 5)) == 0)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence);
            selected_led = led;
            break;
        }
        case 1U << 5: {
            const auto mode = static_cast<std::uint8_t>(*value);
            if (mode > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, mode);
            const std::uint32_t affected = selected_led
                ? 1U << (*selected_led - 1U)
                : 0xffffffffU;
            if (mode == 0)
                led_mask &= ~affected;
            else
                led_mask |= affected;
            break;
        }
        case 1U << 6: {
            const auto key = static_cast<std::uint8_t>(*value);
            if (key < minimum_keycode || key > maximum_keycode)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, key);
            if ((*value_mask & (1U << 7)) == 0)
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence);
            selected_key = key;
            break;
        }
        case 1U << 7: {
            const auto mode = static_cast<std::uint8_t>(*value);
            if (mode > 2)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, mode);
            if (!selected_key) {
                global_auto_repeat = mode == 2
                    ? default_global_auto_repeat
                    : mode == 1;
                break;
            }
            const std::uint8_t key = *selected_key;
            const std::uint8_t mask =
                static_cast<std::uint8_t>(1U << (key & 7U));
            auto &repeats = auto_repeats[key >> 3];
            if (mode == 0)
                repeats &= static_cast<std::uint8_t>(~mask);
            else if (mode == 1)
                repeats |= mask;
            else {
                repeats = static_cast<std::uint8_t>(
                    (repeats & ~mask) |
                    (default_auto_repeats[key >> 3] & mask));
            }
            break;
        }
        default:
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *value_mask);
        }
    }

    auto &updated = server_.input();
    updated.auto_repeats = auto_repeats;
    updated.led_mask = led_mask;
    updated.bell_pitch = bell_pitch;
    updated.bell_duration = bell_duration;
    updated.key_click_percent = key_click_percent;
    updated.bell_percent = bell_percent;
    updated.global_auto_repeat = global_auto_repeat;
    updated.xkb.controls.per_key_repeat = auto_repeats;
    if (global_auto_repeat)
        updated.xkb.controls.enabled |= 1U;
    else
        updated.xkb.controls.enabled &= ~1U;
    server_.update_repeat_controls();
    return Result<void>::success();
}

Result<void>
Connection::handle_get_keyboard_control(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const auto &input = server_.input();
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(input.global_auto_repeat ? 1 : 0);
    reply.u16(context.sequence);
    reply.u32(5);
    reply.u32(input.led_mask);
    reply.u8(input.key_click_percent);
    reply.u8(input.bell_percent);
    reply.u16(input.bell_pitch);
    reply.u16(input.bell_duration);
    reply.pad(2);
    for (const auto repeats : input.auto_repeats)
        reply.u8(repeats);
    return queue(reply.data());
}

Result<void>
Connection::handle_bell(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::int16_t percent = signed_byte(context.data);
    if (percent < -100 || percent > 100)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence,
                          static_cast<std::uint32_t>(percent));
    return Result<void>::success();
}

Result<void>
Connection::handle_change_pointer_control(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto numerator_wire = reader.u16();
    const auto denominator_wire = reader.u16();
    const auto threshold_wire = reader.u16();
    const auto change_acceleration = reader.u8();
    const auto change_threshold = reader.u8();
    if (!numerator_wire || !denominator_wire || !threshold_wire ||
        !change_acceleration || !change_threshold) {
        return malformed("truncated ChangePointerControl request");
    }
    if (*change_acceleration > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *change_acceleration);
    if (*change_threshold > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *change_threshold);

    auto &input = server_.input();
    std::int16_t numerator = input.pointer_acceleration_numerator;
    std::int16_t denominator = input.pointer_acceleration_denominator;
    std::int16_t threshold = input.pointer_threshold;
    if (*change_acceleration != 0) {
        numerator = signed_word(*numerator_wire);
        denominator = signed_word(*denominator_wire);
        if (numerator == -1)
            numerator = default_pointer_acceleration_numerator;
        else if (numerator < 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence,
                              static_cast<std::uint32_t>(numerator));
        if (denominator == -1)
            denominator = default_pointer_acceleration_denominator;
        else if (denominator <= 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence,
                              static_cast<std::uint32_t>(denominator));
    }
    if (*change_threshold != 0) {
        threshold = signed_word(*threshold_wire);
        if (threshold == -1)
            threshold = default_pointer_threshold;
        else if (threshold < 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence,
                              static_cast<std::uint32_t>(threshold));
    }
    input.pointer_acceleration_numerator = numerator;
    input.pointer_acceleration_denominator = denominator;
    input.pointer_threshold = threshold;
    return Result<void>::success();
}

Result<void>
Connection::handle_get_pointer_control(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const auto &input = server_.input();
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u16(static_cast<std::uint16_t>(
        input.pointer_acceleration_numerator));
    reply.u16(static_cast<std::uint16_t>(
        input.pointer_acceleration_denominator));
    reply.u16(static_cast<std::uint16_t>(input.pointer_threshold));
    reply.pad(18);
    return queue(reply.data());
}

Result<void>
Connection::handle_set_pointer_mapping(const RequestContext &context)
{
    const auto padded = padded_to_four(context.data);
    if (!padded || context.request.size() != 4 + *padded)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    auto &input = server_.input();
    if (context.data != input.pointer_map.size())
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    std::array<std::uint8_t, 256> seen{};
    for (std::size_t index = 0; index < input.pointer_map.size(); ++index) {
        const std::uint8_t mapped = context.request[4 + index];
        if (mapped != 0 && seen[mapped] != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, mapped);
        seen[mapped] = 1;
    }

    std::uint8_t status = 0;
    for (std::size_t index = 0; index < input.pointer_map.size(); ++index) {
        if (input.pointer_map[index] != context.request[4 + index] &&
            input.pressed_buttons.test(index + 1)) {
            status = 1;
            break;
        }
    }
    if (status == 0) {
        if (!server_.broadcast_mapping_notify(2, 0, 0))
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        std::copy_n(context.request.begin() + 4, input.pointer_map.size(),
                    input.pointer_map.begin());
        auto drained = drain_pending_events();
        if (!drained)
            return drained;
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(status);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.pad(24);
    return queue(reply.data());
}

Result<void>
Connection::handle_get_pointer_mapping(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const auto &mapping = server_.input().pointer_map;
    const auto padded = padded_to_four(mapping.size());
    if (!padded)
        return malformed("pointer mapping size overflow");
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(static_cast<std::uint8_t>(mapping.size()));
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(*padded / 4));
    reply.pad(24);
    for (const auto button : mapping)
        reply.u8(button);
    reply.pad(*padded - mapping.size());
    return queue(reply.data());
}

Result<void>
Connection::handle_set_modifier_mapping(const RequestContext &context)
{
    const std::size_t map_size = static_cast<std::size_t>(context.data) * 8;
    if (context.request.size() != 4 + map_size)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    std::array<std::uint8_t, 256> seen{};
    std::array<std::uint8_t, 256> assigned_group{};
    std::array<std::size_t, 8> group_sizes{};
    const auto &input = server_.input();
    std::uint8_t status = 0;
    for (std::size_t index = 0; index < map_size; ++index) {
        const std::uint8_t keycode = context.request[4 + index];
        if (keycode == 0)
            continue;
        if (seen[keycode] != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence);
        seen[keycode] = 1;
        const std::size_t group = index / context.data;
        assigned_group[keycode] = static_cast<std::uint8_t>(group + 1);
        ++group_sizes[group];
        if (keycode < minimum_keycode || keycode > maximum_keycode)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, keycode);
        if (key_is_pressed(input, keycode))
            status = 1;
    }
    if (status == 0) {
        for (const auto keycode : input.modifier_map) {
            if (keycode != 0 && key_is_pressed(input, keycode)) {
                status = 1;
                break;
            }
        }
    }
    std::vector<std::uint8_t> canonical;
    std::size_t canonical_width = 0;
    if (status == 0) {
        try {
            canonical_width = *std::max_element(
                group_sizes.begin(), group_sizes.end());
            canonical.assign(canonical_width * 8, 0);
            std::array<std::size_t, 8> offsets{};
            for (std::size_t keycode = minimum_keycode;
                 keycode <= maximum_keycode; ++keycode) {
                if (assigned_group[keycode] == 0)
                    continue;
                const std::size_t group = assigned_group[keycode] - 1;
                canonical[group * canonical_width + offsets[group]++] =
                    static_cast<std::uint8_t>(keycode);
            }
        }
        catch (const std::bad_alloc &) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
    }
    if (status == 0) {
        if (!server_.broadcast_mapping_notify(0, 0, 0))
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        auto &updated = server_.input();
        updated.modifier_map = std::move(canonical);
        updated.modifier_keys_per_group =
            static_cast<std::uint8_t>(canonical_width);
        auto drained = drain_pending_events();
        if (!drained)
            return drained;
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(status);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.pad(24);
    return queue(reply.data());
}

Result<void>
Connection::handle_get_modifier_mapping(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const auto &mapping = server_.input().modifier_map;
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(server_.input().modifier_keys_per_group);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(mapping.size() / 4));
    reply.pad(24);
    for (const auto keycode : mapping)
        reply.u8(keycode);
    return queue(reply.data());
}

Result<void>
Connection::handle_get_input_focus(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(server_.input().focus.revert_to);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(server_.input().focus.wire_id());
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_query_keymap(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(2);
    for (const auto byte : server_.input().pressed_keys)
        reply.u8(byte);
    return queue(reply.data());
}

Result<void>
Connection::handle_query_extension(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto name_size = reader.u16();
    if (!name_size || !reader.skip(2))
        return malformed("truncated QueryExtension request");
    const auto padded_name = padded_to_four(*name_size);
    if (!padded_name || context.request.size() != 8 + *padded_name)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::string_view name{
        reinterpret_cast<const char *>(context.request.data() + 8),
        *name_size};
    const auto *extension = extension_by_name(name);

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u8(extension == nullptr ? 0 : 1);
    reply.u8(extension == nullptr ? 0 : extension->major_opcode);
    reply.u8(extension == nullptr ? 0 : extension->first_event);
    reply.u8(extension == nullptr ? 0 : extension->first_error);
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::handle_list_extensions(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(static_cast<std::uint8_t>(extension_registry.size()));
    reply.u16(context.sequence);
    std::size_t payload_size = 0;
    for (const auto &extension : extension_registry)
        payload_size += 1 + extension.name.size();
    const auto padded_size = padded_to_four(payload_size);
    if (!padded_size)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    reply.u32(static_cast<std::uint32_t>(*padded_size / 4));
    reply.pad(24);
    for (const auto &extension : extension_registry) {
        reply.u8(static_cast<std::uint8_t>(extension.name.size()));
        reply.bytes(extension.name);
    }
    reply.pad(*padded_size - payload_size);
    return queue(reply.data());
}

Result<void>
Connection::handle_big_requests(const RequestContext &context)
{
    if (context.data != 0) {
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
    if (context.request.size() != 4) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence, 0, context.data);
    }

    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u32(static_cast<std::uint32_t>(maximum_extended_request_units));
    reply.pad(20);
    auto queued = queue(reply.data());
    if (queued)
        big_requests_enabled_ = true;
    return queued;
}

Result<void>
Connection::handle_xc_misc(const RequestContext &context)
{
    switch (context.data) {
    case 0: { // GetVersion
        if (context.request.size() != 8) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 4, context.order);
        if (!reader.u16() || !reader.u16())
            return malformed("truncated XC-MISC GetVersion request");
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(xc_misc_extension.major_version);
        reply.u16(xc_misc_extension.minor_version);
        reply.pad(20);
        return queue(reply.data());
    }
    case 1: { // GetXIDRange
        if (context.request.size() != 4) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(0); // setup already exposes the complete client XID range
        reply.u32(0);
        reply.pad(16);
        return queue(reply.data());
    }
    case 2: { // GetXIDList
        if (context.request.size() != 8) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 4, context.order);
        if (!reader.u32())
            return malformed("truncated XC-MISC GetXIDList request");
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(0); // no server-side supplemental XID pool
        reply.pad(20);
        return queue(reply.data());
    }
    default:
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
}

Result<void>
Connection::handle_generic_event(const RequestContext &context)
{
    if (context.data != 0) {
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
    if (context.request.size() != 8) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence, 0, context.data);
    }
    WireReader reader(context.request.data() + 4, 4, context.order);
    if (!reader.u16() || !reader.u16())
        return malformed("truncated Generic Event QueryVersion request");
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u16(generic_event_extension.major_version);
    reply.u16(generic_event_extension.minor_version);
    reply.pad(20);
    return queue(reply.data());
}

Result<void>
Connection::update_shape(const RequestContext &context, WindowRecord &window,
                         std::uint8_t operation, std::uint8_t kind,
                         std::optional<Region> source)
{
    const auto selected_operation = region_operation(operation);
    if (!selected_operation) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, operation, context.data);
    }
    if (kind >= window.shapes.size()) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, kind, context.data);
    }

    std::optional<Region> revised;
    if (!source) {
        revised.reset();
    }
    else {
        Region combined;
        switch (*selected_operation) {
        case RegionOperation::set:
            revised = std::move(source);
            break;
        case RegionOperation::unite:
            if (window.shapes[kind]) {
                if (!Region::combine(
                        *selected_operation, *window.shapes[kind], *source,
                        combined)) {
                    return send_error(
                        context.order, bad_alloc, context.opcode,
                        context.sequence, 0, context.data);
                }
                revised = std::move(combined);
            }
            break;
        case RegionOperation::intersect:
            if (!window.shapes[kind]) {
                revised = std::move(source);
                break;
            }
            if (!Region::combine(
                    *selected_operation, *window.shapes[kind], *source,
                    combined)) {
                return send_error(
                    context.order, bad_alloc, context.opcode,
                    context.sequence, 0, context.data);
            }
            revised = std::move(combined);
            break;
        case RegionOperation::subtract: {
            Region default_region;
            const Region *destination = window.shapes[kind]
                ? &*window.shapes[kind]
                : &default_region;
            if (!window.shapes[kind] &&
                !make_default_shape(window, kind, default_region)) {
                return send_error(
                    context.order, bad_alloc, context.opcode,
                    context.sequence, 0, context.data);
            }
            if (!Region::combine(
                    *selected_operation, *destination, *source, combined)) {
                return send_error(
                    context.order, bad_alloc, context.opcode,
                    context.sequence, 0, context.data);
            }
            revised = std::move(combined);
            break;
        }
        case RegionOperation::invert:
            if (!window.shapes[kind]) {
                const std::vector<Rectangle> empty;
                if (!Region::canonicalize(empty, combined)) {
                    return send_error(
                        context.order, bad_alloc, context.opcode,
                        context.sequence, 0, context.data);
                }
            }
            else if (!Region::combine(
                         *selected_operation, *window.shapes[kind], *source,
                         combined)) {
                return send_error(
                    context.order, bad_alloc, context.opcode,
                    context.sequence, 0, context.data);
            }
            revised = std::move(combined);
            break;
        }
    }

    const auto updated = server_.set_window_shape(
        window, kind, std::move(revised));
    if (updated == ShapeUpdate::invalid) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, kind, context.data);
    }
    if (updated == ShapeUpdate::resource_exhausted ||
        updated == ShapeUpdate::queue_full) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence, 0, context.data);
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_shape(const RequestContext &context)
{
    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 4) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(shape_extension.major_version);
        reply.u16(shape_extension.minor_version);
        reply.pad(20);
        return queue(reply.data());
    }
    case 1: { // Rectangles
        if (context.request.size() < 16 ||
            ((context.request.size() - 16) & 7U) != 0) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto operation = reader.u8();
        const auto kind = reader.u8();
        const auto ordering = reader.u8();
        const auto destination = [&]() -> std::optional<std::uint32_t> {
            if (!reader.skip(1))
                return std::nullopt;
            return reader.u32();
        }();
        const auto x_offset = reader.u16();
        const auto y_offset = reader.u16();
        if (!operation || !kind || !ordering || !destination || !x_offset ||
            !y_offset) {
            return malformed("truncated SHAPE Rectangles request");
        }
        if (!region_operation(*operation)) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *operation, context.data);
        }
        if (*kind >= 3) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *kind, context.data);
        }
        if (*ordering > 3) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *ordering, context.data);
        }
        auto *window = server_.window(*destination);
        if (window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *destination, context.data);
        }

        WireReader rectangles_reader(
            context.request.data() + 16, context.request.size() - 16,
            context.order);
        std::vector<Rectangle> rectangles;
        try {
            rectangles.reserve((context.request.size() - 16) / 8);
            while (rectangles_reader.remaining() != 0) {
                const auto x = rectangles_reader.u16();
                const auto y = rectangles_reader.u16();
                const auto width = rectangles_reader.u16();
                const auto height = rectangles_reader.u16();
                if (!x || !y || !width || !height)
                    return malformed("truncated SHAPE rectangle list");
                rectangles.push_back(Rectangle{
                    signed_word(*x), signed_word(*y), *width, *height});
            }
        }
        catch (const std::bad_alloc &) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        if (!valid_clip_order(rectangles, *ordering)) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence, 0, context.data);
        }
        Region source;
        if (!Region::canonicalize(rectangles, source) ||
            !source.translate(
                signed_word(*x_offset), signed_word(*y_offset))) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        return update_shape(
            context, *window, *operation, *kind, std::move(source));
    }
    case 2: { // Mask
        if (context.request.size() != 20) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto operation = reader.u8();
        const auto kind = reader.u8();
        if (!operation || !kind || !reader.skip(2))
            return malformed("truncated SHAPE Mask header");
        const auto destination = reader.u32();
        const auto x_offset = reader.u16();
        const auto y_offset = reader.u16();
        const auto source_id = reader.u32();
        if (!destination || !x_offset || !y_offset || !source_id)
            return malformed("truncated SHAPE Mask request");
        if (!region_operation(*operation)) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *operation, context.data);
        }
        if (*kind >= 3) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *kind, context.data);
        }
        auto *window = server_.window(*destination);
        if (window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *destination, context.data);
        }
        if (*source_id == 0)
            return update_shape(context, *window, *operation, *kind, {});
        const auto *pixmap = server_.pixmap(*source_id);
        if (pixmap == nullptr) {
            return send_error(context.order, bad_pixmap, context.opcode,
                              context.sequence, *source_id, context.data);
        }
        if (pixmap->surface->depth() != 1) {
            return send_error(context.order, bad_match, context.opcode,
                              context.sequence, 0, context.data);
        }

        std::vector<Rectangle> rectangles;
        try {
            for (std::uint16_t y = 0; y < pixmap->surface->height(); ++y) {
                std::uint16_t x = 0;
                while (x < pixmap->surface->width()) {
                    while (x < pixmap->surface->width() &&
                           (pixmap->surface->pixel(x, y) & 1U) == 0) {
                        ++x;
                    }
                    const std::uint16_t start = x;
                    while (x < pixmap->surface->width() &&
                           (pixmap->surface->pixel(x, y) & 1U) != 0) {
                        ++x;
                    }
                    if (x != start) {
                        if (rectangles.size() == maximum_shape_rectangles) {
                            return send_error(
                                context.order, bad_alloc, context.opcode,
                                context.sequence, 0, context.data);
                        }
                        rectangles.push_back(Rectangle{
                            start, y,
                            static_cast<std::uint32_t>(x - start), 1});
                    }
                }
            }
        }
        catch (const std::bad_alloc &) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        Region source;
        if (!Region::canonicalize(rectangles, source) ||
            !source.translate(
                signed_word(*x_offset), signed_word(*y_offset))) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        return update_shape(
            context, *window, *operation, *kind, std::move(source));
    }
    case 3: { // Combine
        if (context.request.size() != 20) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto operation = reader.u8();
        const auto destination_kind = reader.u8();
        const auto source_kind = reader.u8();
        if (!operation || !destination_kind || !source_kind ||
            !reader.skip(1)) {
            return malformed("truncated SHAPE Combine header");
        }
        const auto destination_id = reader.u32();
        const auto x_offset = reader.u16();
        const auto y_offset = reader.u16();
        const auto source_id = reader.u32();
        if (!destination_id || !x_offset || !y_offset || !source_id)
            return malformed("truncated SHAPE Combine request");
        if (!region_operation(*operation)) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *operation, context.data);
        }
        if (*destination_kind >= 3 || *source_kind >= 3) {
            const std::uint8_t bad_kind = *destination_kind >= 3
                ? *destination_kind
                : *source_kind;
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, bad_kind, context.data);
        }
        auto *destination = server_.window(*destination_id);
        if (destination == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *destination_id, context.data);
        }
        const auto *source_window = server_.window(*source_id);
        if (source_window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *source_id, context.data);
        }
        Region source;
        if (source_window->shapes[*source_kind]) {
            if (!Region::combine(
                    RegionOperation::set,
                    *source_window->shapes[*source_kind],
                    *source_window->shapes[*source_kind], source)) {
                return send_error(context.order, bad_alloc, context.opcode,
                                  context.sequence, 0, context.data);
            }
        }
        else if (!make_default_shape(*source_window, *source_kind, source)) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        if (!source.translate(
                signed_word(*x_offset), signed_word(*y_offset))) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        return update_shape(
            context, *destination, *operation, *destination_kind,
            std::move(source));
    }
    case 4: { // Offset
        if (context.request.size() != 16) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto kind = reader.u8();
        if (!kind || !reader.skip(3))
            return malformed("truncated SHAPE Offset header");
        const auto destination_id = reader.u32();
        const auto x_offset = reader.u16();
        const auto y_offset = reader.u16();
        if (!destination_id || !x_offset || !y_offset)
            return malformed("truncated SHAPE Offset request");
        if (*kind >= 3) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *kind, context.data);
        }
        auto *window = server_.window(*destination_id);
        if (window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *destination_id, context.data);
        }
        std::optional<Region> revised;
        if (window->shapes[*kind]) {
            Region translated;
            if (!Region::combine(
                    RegionOperation::set, *window->shapes[*kind],
                    *window->shapes[*kind], translated) ||
                !translated.translate(
                    signed_word(*x_offset), signed_word(*y_offset))) {
                return send_error(context.order, bad_alloc, context.opcode,
                                  context.sequence, 0, context.data);
            }
            revised = std::move(translated);
        }
        const auto updated = server_.set_window_shape(
            *window, *kind, std::move(revised));
        if (updated == ShapeUpdate::resource_exhausted ||
            updated == ShapeUpdate::queue_full) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        return drain_pending_events();
    }
    case 5: { // QueryExtents
        if (context.request.size() != 8) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window_id = reader.u32();
        if (!window_id)
            return malformed("truncated SHAPE QueryExtents request");
        const auto *window = server_.window(*window_id);
        if (window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window_id, context.data);
        }
        const Rectangle bounding = window->shapes[0]
            ? window->shapes[0]->extents()
            : window->default_shape(0);
        const Rectangle clip = window->shapes[1]
            ? window->shapes[1]->extents()
            : window->default_shape(1);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u8(window->shapes[0] ? 1 : 0);
        reply.u8(window->shapes[1] ? 1 : 0);
        reply.pad(2);
        reply.i16(wire_coordinate(bounding.x));
        reply.i16(wire_coordinate(bounding.y));
        reply.u16(wire_size(bounding.width));
        reply.u16(wire_size(bounding.height));
        reply.i16(wire_coordinate(clip.x));
        reply.i16(wire_coordinate(clip.y));
        reply.u16(wire_size(clip.width));
        reply.u16(wire_size(clip.height));
        reply.pad(4);
        return queue(reply.data());
    }
    case 6: { // SelectInput
        if (context.request.size() != 12) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window_id = reader.u32();
        const auto enabled = reader.u8();
        if (!window_id || !enabled || !reader.skip(3))
            return malformed("truncated SHAPE SelectInput request");
        if (*enabled > 1) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *enabled, context.data);
        }
        auto *window = server_.window(*window_id);
        if (window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window_id, context.data);
        }
        if (!server_.select_shape_events(
                *window, config_.resource_base, *enabled != 0)) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        return Result<void>::success();
    }
    case 7: { // InputSelected
        if (context.request.size() != 8) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window_id = reader.u32();
        if (!window_id)
            return malformed("truncated SHAPE InputSelected request");
        const auto *window = server_.window(*window_id);
        if (window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window_id, context.data);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(server_.shape_events_selected(
                     *window, config_.resource_base)
                     ? 1
                     : 0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.pad(24);
        return queue(reply.data());
    }
    case 8: { // GetRectangles
        if (context.request.size() != 12) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window_id = reader.u32();
        const auto kind = reader.u8();
        if (!window_id || !kind || !reader.skip(3))
            return malformed("truncated SHAPE GetRectangles request");
        if (*kind >= 3) {
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *kind, context.data);
        }
        const auto *window = server_.window(*window_id);
        if (window == nullptr) {
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window_id, context.data);
        }
        Region default_region;
        const Region *shape = window->shapes[*kind]
            ? &*window->shapes[*kind]
            : &default_region;
        if (!window->shapes[*kind] &&
            !make_default_shape(*window, *kind, default_region)) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        const auto &rectangles = shape->rectangles();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(3); // canonical regions are YXBanded
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(rectangles.size() * 2U));
        reply.u32(static_cast<std::uint32_t>(rectangles.size()));
        reply.pad(20);
        for (const auto &rectangle : rectangles) {
            reply.i16(wire_coordinate(rectangle.x));
            reply.i16(wire_coordinate(rectangle.y));
            reply.u16(wire_size(rectangle.width));
            reply.u16(wire_size(rectangle.height));
        }
        return queue(reply.data());
    }
    default:
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
}

Result<void>
Connection::handle_xtest(const RequestContext &context)
{
    switch (context.data) {
    case 0: { // GetVersion
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        if (!reader.u8() || !reader.skip(1) || !reader.u16())
            return malformed("truncated XTEST GetVersion request");
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xtest_extension.major_version);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(xtest_extension.minor_version);
        reply.pad(22);
        return queue(reply.data());
    }
    case 1: { // CompareCursor
        if (context.request.size() != 12)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window = reader.u32();
        const auto cursor = reader.u32();
        if (!window || !cursor)
            return malformed("truncated XTEST CompareCursor request");
        if (server_.window(*window) == nullptr)
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window, context.data);
        if (*cursor > 1 && server_.cursor(*cursor) == nullptr)
            return send_error(context.order, bad_cursor, context.opcode,
                              context.sequence, *cursor, context.data);
        const auto window_cursor = server_.effective_cursor(*window);
        std::shared_ptr<CursorImage> requested_cursor;
        if (*cursor == 1)
            requested_cursor = server_.current_cursor();
        else if (*cursor > 1)
            requested_cursor = server_.cursor(*cursor)->image;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(requested_cursor == window_cursor ? 1 : 0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.pad(24);
        return queue(reply.data());
    }
    case 2: { // FakeInput
        if (context.request.size() != 36)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 32, context.order);
        const auto type = reader.u8();
        const auto detail = reader.u8();
        if (!type || !detail || !reader.skip(2) || !reader.u32())
            return malformed("truncated XTEST FakeInput request");
        const auto root = reader.u32();
        if (!root || !reader.skip(8))
            return malformed("truncated XTEST FakeInput root");
        const auto root_x_wire = reader.u16();
        const auto root_y_wire = reader.u16();
        if (!root_x_wire || !root_y_wire || !reader.skip(7))
            return malformed("truncated XTEST FakeInput coordinates");
        const auto device = reader.u8();
        if (!device)
            return malformed("truncated XTEST FakeInput device");
        if (*root != 0 && *root != root_window_id)
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *root, context.data);
        if (*device != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *device, context.data);

        auto &input = server_.input();
        if (*type == 2 || *type == 3) {
            if (*detail < minimum_keycode || *detail > maximum_keycode)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *detail, context.data);
            const auto delivered = server_.inject_input(
                *type, *detail, input.pointer_x, input.pointer_y);
            if (delivered == EventDelivery::queue_full)
                return send_error(context.order, bad_alloc, context.opcode,
                                  context.sequence, 0, context.data);
            return drain_pending_events();
        }
        if (*type == 4 || *type == 5) {
            if (*detail < 1 || *detail > input.pointer_map.size())
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *detail, context.data);
            const auto delivered = server_.inject_input(
                *type, *detail, input.pointer_x, input.pointer_y);
            if (delivered == EventDelivery::queue_full)
                return send_error(context.order, bad_alloc, context.opcode,
                                  context.sequence, 0, context.data);
            return drain_pending_events();
        }
        if (*type == 6) {
            if (*detail > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *detail, context.data);
            std::int32_t x = signed_word(*root_x_wire);
            std::int32_t y = signed_word(*root_y_wire);
            if (*detail == 1) {
                x += input.pointer_x;
                y += input.pointer_y;
            }
            x = std::clamp<std::int32_t>(
                x, 0, static_cast<std::int32_t>(server_.width()) - 1);
            y = std::clamp<std::int32_t>(
                y, 0, static_cast<std::int32_t>(server_.height()) - 1);
            const auto delivered = server_.inject_input(*type, *detail, x, y);
            if (delivered == EventDelivery::queue_full)
                return send_error(context.order, bad_alloc, context.opcode,
                                  context.sequence, 0, context.data);
            return drain_pending_events();
        }
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *type, context.data);
    }
    case 3: { // GrabControl
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        const std::uint8_t impervious = context.request[4];
        if (impervious > 1)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, impervious, context.data);
        return Result<void>::success();
    }
    default:
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
}

Result<void>
Connection::handle_sync(const RequestContext &context)
{
    constexpr std::uint8_t bad_counter = sync_extension.first_error;
    constexpr std::uint8_t bad_alarm = sync_extension.first_error + 1;
    constexpr std::uint8_t bad_fence = sync_extension.first_error + 2;
    const auto sync_error = [&](std::uint8_t code, std::uint32_t value) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto allocation_error = [&]() {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence, 0, context.data);
    };
    const auto finish_update = [&](SyncUpdate update) -> Result<void> {
        if (update == SyncUpdate::updated)
            return drain_pending_events();
        if (update == SyncUpdate::resource_exhausted ||
            update == SyncUpdate::queue_full) {
            return allocation_error();
        }
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, 0, context.data);
    };

    switch (context.data) {
    case 0: { // Initialize
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        if (!reader.u8() || !reader.u8() || !reader.skip(2))
            return malformed("truncated SYNC Initialize request");
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u8(sync_extension.major_version);
        reply.u8(static_cast<std::uint8_t>(sync_extension.minor_version));
        reply.pad(22);
        return queue(reply.data());
    }
    case 1: { // ListSystemCounters
        if (context.request.size() != 4)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(0); // Xmin has no server-maintained system counters
        reply.pad(20);
        return queue(reply.data());
    }
    case 2: { // CreateCounter
        if (context.request.size() != 16)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto value = read_sync_int64(reader);
        if (!id || !value)
            return malformed("truncated SYNC CreateCounter request");
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return sync_error(bad_id_choice, *id);
        if (server_.resource_limit_reached(config_.resource_base))
            return allocation_error();
        if (!server_.add_sync_counter({*id, *value}, config_.resource_base))
            return allocation_error();
        return Result<void>::success();
    }
    case 3: // SetCounter
    case 4: { // ChangeCounter
        if (context.request.size() != 16)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto operand = read_sync_int64(reader);
        if (!id || !operand)
            return malformed("truncated SYNC counter update request");
        auto *counter = server_.sync_counter(*id);
        if (counter == nullptr)
            return sync_error(bad_counter, *id);
        std::int64_t value = *operand;
        if (context.data == 4) {
            const auto changed = checked_add(counter->value, *operand);
            if (!changed) {
                const auto high = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(*operand) >> 32);
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, high, context.data);
            }
            value = *changed;
        }
        return finish_update(server_.set_sync_counter(*counter, value));
    }
    case 5: { // QueryCounter
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed("truncated SYNC QueryCounter request");
        const auto *counter = server_.sync_counter(*id);
        if (counter == nullptr)
            return sync_error(bad_counter, *id);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        write_sync_int64(reply, counter->value);
        reply.pad(16);
        return queue(reply.data());
    }
    case 6: { // DestroyCounter
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed("truncated SYNC DestroyCounter request");
        if (server_.sync_counter(*id) == nullptr)
            return sync_error(bad_counter, *id);
        return finish_update(server_.erase_sync_counter(*id));
    }
    case 7: { // Await
        if ((context.request.size() - 4) % 28 != 0) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        const std::size_t count = (context.request.size() - 4) / 28;
        if (count == 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, 0, context.data);
        if (count > maximum_sync_wait_conditions)
            return allocation_error();
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        std::vector<SyncWaitCondition> conditions;
        try {
            conditions.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                const auto id = reader.u32();
                const auto value_type = reader.u32();
                const auto wait_value = read_sync_int64(reader);
                const auto test_type = reader.u32();
                const auto threshold = read_sync_int64(reader);
                if (!id || !value_type || !wait_value || !test_type ||
                    !threshold) {
                    return malformed("truncated SYNC Await condition");
                }
                const auto *counter = server_.sync_counter(*id);
                if (counter == nullptr)
                    return sync_error(bad_counter, *id);
                if (*value_type > 1)
                    return send_error(context.order, bad_value,
                                      context.opcode, context.sequence,
                                      *value_type, context.data);
                if (*test_type > 3)
                    return send_error(context.order, bad_value,
                                      context.opcode, context.sequence,
                                      *test_type, context.data);
                std::int64_t test_value = *wait_value;
                if (*value_type == 1) {
                    const auto relative = checked_add(
                        counter->value, *wait_value);
                    if (!relative) {
                        const auto high = static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(*wait_value) >> 32);
                        return send_error(
                            context.order, bad_value, context.opcode,
                            context.sequence, high, context.data);
                    }
                    test_value = *relative;
                }
                SyncTrigger trigger;
                trigger.counter = *id;
                trigger.wait_value = *wait_value;
                trigger.test_value = test_value;
                trigger.value_type = static_cast<std::uint8_t>(*value_type);
                trigger.test_type = static_cast<SyncTestType>(*test_type);
                conditions.push_back(SyncWaitCondition{trigger, *threshold});
            }
        }
        catch (const std::bad_alloc &) {
            return allocation_error();
        }
        return finish_update(server_.begin_sync_counter_await(
            config_.resource_base, std::move(conditions)));
    }
    case 8: // CreateAlarm
    case 9: { // ChangeAlarm
        if (context.request.size() < 12)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader header(context.request.data() + 4, 8, context.order);
        const auto id = header.u32();
        const auto mask = header.u32();
        if (!id || !mask)
            return malformed("truncated SYNC alarm request");
        if ((*mask & ~std::uint32_t{0x3f}) != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *mask, context.data);
        std::size_t words = 0;
        for (std::uint32_t bit = 1; bit <= 32; bit <<= 1) {
            if ((*mask & bit) != 0)
                words += (bit == 4 || bit == 16) ? 2 : 1;
        }
        if (context.request.size() != 12 + words * 4)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);

        const bool creating = context.data == 8;
        SyncAlarmRecord alarm;
        if (creating) {
            if (!server_.valid_client_resource(*id, config_.resource_base))
                return sync_error(bad_id_choice, *id);
            if (server_.resource_limit_reached(config_.resource_base))
                return allocation_error();
            alarm.id = *id;
            alarm.owner = config_.resource_base;
            try {
                alarm.event_clients.push_back(config_.resource_base);
            }
            catch (const std::bad_alloc &) {
                return allocation_error();
            }
        }
        else {
            const auto *existing = server_.sync_alarm(*id);
            if (existing == nullptr)
                return sync_error(bad_alarm, *id);
            try {
                alarm = *existing;
            }
            catch (const std::bad_alloc &) {
                return allocation_error();
            }
        }

        WireReader values(context.request.data() + 12,
                          context.request.size() - 12, context.order);
        std::uint32_t selected_counter = alarm.trigger.counter;
        bool value_type_changed = false;
        bool wait_value_changed = false;
        bool test_type_changed = false;
        bool delta_changed = false;
        if ((*mask & 1) != 0) {
            const auto value = values.u32();
            if (!value)
                return malformed("truncated SYNC alarm counter");
            if (*value != 0 && server_.sync_counter(*value) == nullptr)
                return sync_error(bad_counter, *value);
            selected_counter = *value;
        }
        if ((*mask & 2) != 0) {
            const auto value = values.u32();
            if (!value)
                return malformed("truncated SYNC alarm value type");
            if (*value > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value, context.data);
            alarm.trigger.value_type = static_cast<std::uint8_t>(*value);
            value_type_changed = true;
        }
        if ((*mask & 4) != 0) {
            const auto value = read_sync_int64(values);
            if (!value)
                return malformed("truncated SYNC alarm value");
            alarm.trigger.wait_value = *value;
            wait_value_changed = true;
        }
        if ((*mask & 8) != 0) {
            const auto value = values.u32();
            if (!value)
                return malformed("truncated SYNC alarm test type");
            if (*value > 3)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *value, context.data);
            alarm.trigger.test_type = static_cast<SyncTestType>(*value);
            test_type_changed = true;
        }
        if ((*mask & 16) != 0) {
            const auto value = read_sync_int64(values);
            if (!value)
                return malformed("truncated SYNC alarm delta");
            alarm.delta = *value;
            delta_changed = true;
        }
        if ((*mask & 32) != 0) {
            const auto enabled = values.u32();
            if (!enabled)
                return malformed("truncated SYNC alarm events value");
            if (*enabled > 1)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *enabled, context.data);
            const auto selected = std::find(
                alarm.event_clients.begin(), alarm.event_clients.end(),
                config_.resource_base);
            if (*enabled == 0) {
                if (selected != alarm.event_clients.end())
                    alarm.event_clients.erase(selected);
            }
            else if (selected == alarm.event_clients.end()) {
                try {
                    alarm.event_clients.push_back(config_.resource_base);
                }
                catch (const std::bad_alloc &) {
                    return allocation_error();
                }
            }
        }
        alarm.trigger.counter = selected_counter;
        if (value_type_changed || wait_value_changed) {
            if (alarm.trigger.value_type == 0) {
                alarm.trigger.test_value = alarm.trigger.wait_value;
            }
            else {
                const auto *counter = server_.sync_counter(selected_counter);
                if (counter == nullptr)
                    return send_error(context.order, bad_match,
                                      context.opcode, context.sequence, 0,
                                      context.data);
                const auto relative = checked_add(
                    counter->value, alarm.trigger.wait_value);
                if (!relative) {
                    const auto high = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(
                            alarm.trigger.wait_value) >> 32);
                    return send_error(context.order, bad_value,
                                      context.opcode, context.sequence, high,
                                      context.data);
                }
                alarm.trigger.test_value = *relative;
            }
        }
        if (test_type_changed || delta_changed) {
            const bool positive =
                alarm.trigger.test_type ==
                    SyncTestType::positive_transition ||
                alarm.trigger.test_type ==
                    SyncTestType::positive_comparison;
            if ((positive && alarm.delta < 0) ||
                (!positive && alarm.delta > 0)) {
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, 0, context.data);
            }
        }
        const SyncUpdate update = creating
            ? server_.add_sync_alarm(std::move(alarm), config_.resource_base)
            : server_.change_sync_alarm(std::move(alarm));
        return finish_update(update);
    }
    case 10: { // QueryAlarm
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed("truncated SYNC QueryAlarm request");
        const auto *alarm = server_.sync_alarm(*id);
        if (alarm == nullptr)
            return sync_error(bad_alarm, *id);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(2);
        reply.u32(alarm->trigger.counter);
        reply.u32(0); // relative values are consumed when configured
        write_sync_int64(reply, alarm->trigger.test_value);
        reply.u32(static_cast<std::uint32_t>(alarm->trigger.test_type));
        write_sync_int64(reply, alarm->delta);
        reply.u8(std::find(
                     alarm->event_clients.begin(),
                     alarm->event_clients.end(), alarm->owner) !=
                         alarm->event_clients.end()
                     ? 1
                     : 0);
        reply.u8(alarm->state);
        reply.pad(2);
        return queue(reply.data());
    }
    case 11: { // DestroyAlarm
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed("truncated SYNC DestroyAlarm request");
        if (server_.sync_alarm(*id) == nullptr)
            return sync_error(bad_alarm, *id);
        return finish_update(server_.erase_sync_alarm(*id));
    }
    case 12: { // SetPriority
        if (context.request.size() != 12)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto id = reader.u32();
        const auto priority = reader.u32();
        if (!id || !priority)
            return malformed("truncated SYNC SetPriority request");
        std::uint32_t client = config_.resource_base;
        if (*id != 0) {
            const auto owner = server_.resource_owner(*id);
            if (!owner || *owner == 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *id, context.data);
            client = *owner;
        }
        server_.set_sync_priority(client, signed_dword(*priority));
        return Result<void>::success();
    }
    case 13: { // GetPriority
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed("truncated SYNC GetPriority request");
        std::uint32_t client = config_.resource_base;
        if (*id != 0) {
            const auto owner = server_.resource_owner(*id);
            if (!owner || *owner == 0)
                return send_error(context.order, bad_value, context.opcode,
                                  context.sequence, *id, context.data);
            client = *owner;
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(static_cast<std::uint32_t>(
            server_.sync_priority(client)));
        reply.pad(20);
        return queue(reply.data());
    }
    case 14: { // CreateFence
        if (context.request.size() != 16)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto drawable = reader.u32();
        const auto id = reader.u32();
        const auto triggered = reader.u8();
        if (!drawable || !id || !triggered || !reader.skip(3))
            return malformed("truncated SYNC CreateFence request");
        if (server_.drawable_surface(*drawable) == nullptr)
            return send_error(context.order, bad_drawable, context.opcode,
                              context.sequence, *drawable, context.data);
        if (*triggered > 1)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *triggered, context.data);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return sync_error(bad_id_choice, *id);
        if (server_.resource_limit_reached(config_.resource_base))
            return allocation_error();
        if (!server_.add_sync_fence(
                {*id, *triggered != 0}, config_.resource_base)) {
            return allocation_error();
        }
        return Result<void>::success();
    }
    case 15: // TriggerFence
    case 16: // ResetFence
    case 17: // DestroyFence
    case 18: { // QueryFence
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed("truncated SYNC fence request");
        const auto *fence = server_.sync_fence(*id);
        if (fence == nullptr)
            return sync_error(bad_fence, *id);
        if (context.data == 15)
            return finish_update(server_.trigger_sync_fence(*id));
        if (context.data == 16) {
            if (!server_.reset_sync_fence(*id))
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, 0, context.data);
            return Result<void>::success();
        }
        if (context.data == 17)
            return finish_update(server_.erase_sync_fence(*id));
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u8(fence->triggered ? 1 : 0);
        reply.pad(23);
        return queue(reply.data());
    }
    case 19: { // AwaitFence
        if ((context.request.size() - 4) % 4 != 0) {
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        }
        const std::size_t count = (context.request.size() - 4) / 4;
        if (count == 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, 0, context.data);
        if (count > maximum_sync_wait_conditions)
            return allocation_error();
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        std::vector<std::uint32_t> fences;
        try {
            fences.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                const auto id = reader.u32();
                if (!id)
                    return malformed("truncated SYNC AwaitFence request");
                if (server_.sync_fence(*id) == nullptr)
                    return sync_error(bad_fence, *id);
                fences.push_back(*id);
            }
        }
        catch (const std::bad_alloc &) {
            return allocation_error();
        }
        return finish_update(server_.begin_sync_fence_await(
            config_.resource_base, std::move(fences)));
    }
    default:
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
}

Result<void>
Connection::handle_no_operation(const RequestContext &)
{
    return Result<void>::success();
}

Result<void>
Connection::dispatch(const RequestContext &context)
{
    if (const auto *extension = extension_by_opcode(context.opcode)) {
        switch (extension->kind) {
        case ExtensionKind::big_requests:
            return handle_big_requests(context);
        case ExtensionKind::xc_misc:
            return handle_xc_misc(context);
        case ExtensionKind::generic_event:
            return handle_generic_event(context);
        case ExtensionKind::xtest:
            return handle_xtest(context);
        case ExtensionKind::shape:
            return handle_shape(context);
        case ExtensionKind::sync:
            return handle_sync(context);
        case ExtensionKind::render:
            return handle_render(context);
        case ExtensionKind::xfixes:
            return handle_xfixes(context);
        case ExtensionKind::randr:
            return handle_randr(context);
        case ExtensionKind::damage:
            return handle_damage(context);
        case ExtensionKind::composite:
            return handle_composite(context);
        case ExtensionKind::present:
            return handle_present(context);
        case ExtensionKind::xkb:
            return handle_xkb(context);
        case ExtensionKind::xinput:
            return handle_xinput(context);
        case ExtensionKind::shm:
            return handle_shm(context);
        case ExtensionKind::xinerama:
            return handle_xinerama(context);
        case ExtensionKind::screensaver:
            return handle_screensaver(context);
        case ExtensionKind::dbe:
            return handle_dbe(context);
        }
    }
    static const std::array<RequestHandler, 128> handlers = [] {
        std::array<RequestHandler, 128> table{};
        table[opcode_index(CoreOpcode::CreateWindow)] =
            &Connection::handle_create_window;
        table[opcode_index(CoreOpcode::ChangeWindowAttributes)] =
            &Connection::handle_change_window_attributes;
        table[opcode_index(CoreOpcode::GetWindowAttributes)] =
            &Connection::handle_get_window_attributes;
        table[opcode_index(CoreOpcode::DestroyWindow)] =
            &Connection::handle_destroy_window;
        table[opcode_index(CoreOpcode::DestroySubwindows)] =
            &Connection::handle_destroy_subwindows;
        table[opcode_index(CoreOpcode::ReparentWindow)] =
            &Connection::handle_reparent_window;
        table[opcode_index(CoreOpcode::ChangeSaveSet)] =
            &Connection::handle_change_save_set;
        table[opcode_index(CoreOpcode::MapWindow)] =
            &Connection::handle_map_window;
        table[opcode_index(CoreOpcode::MapSubwindows)] =
            &Connection::handle_map_subwindows;
        table[opcode_index(CoreOpcode::UnmapWindow)] =
            &Connection::handle_unmap_window;
        table[opcode_index(CoreOpcode::UnmapSubwindows)] =
            &Connection::handle_unmap_subwindows;
        table[opcode_index(CoreOpcode::ConfigureWindow)] =
            &Connection::handle_configure_window;
        table[opcode_index(CoreOpcode::CirculateWindow)] =
            &Connection::handle_circulate_window;
        table[opcode_index(CoreOpcode::GetGeometry)] =
            &Connection::handle_get_geometry;
        table[opcode_index(CoreOpcode::QueryTree)] =
            &Connection::handle_query_tree;
        table[opcode_index(CoreOpcode::InternAtom)] =
            &Connection::handle_intern_atom;
        table[opcode_index(CoreOpcode::GetAtomName)] =
            &Connection::handle_get_atom_name;
        table[opcode_index(CoreOpcode::ChangeProperty)] =
            &Connection::handle_change_property;
        table[opcode_index(CoreOpcode::DeleteProperty)] =
            &Connection::handle_delete_property;
        table[opcode_index(CoreOpcode::GetProperty)] =
            &Connection::handle_get_property;
        table[opcode_index(CoreOpcode::ListProperties)] =
            &Connection::handle_list_properties;
        table[opcode_index(CoreOpcode::RotateProperties)] =
            &Connection::handle_rotate_properties;
        table[opcode_index(CoreOpcode::SetSelectionOwner)] =
            &Connection::handle_set_selection_owner;
        table[opcode_index(CoreOpcode::GetSelectionOwner)] =
            &Connection::handle_get_selection_owner;
        table[opcode_index(CoreOpcode::ConvertSelection)] =
            &Connection::handle_convert_selection;
        table[opcode_index(CoreOpcode::SendEvent)] =
            &Connection::handle_send_event;
        table[opcode_index(CoreOpcode::GrabPointer)] =
            &Connection::handle_grab_pointer;
        table[opcode_index(CoreOpcode::UngrabPointer)] =
            &Connection::handle_ungrab_pointer;
        table[opcode_index(CoreOpcode::GrabButton)] =
            &Connection::handle_grab_button;
        table[opcode_index(CoreOpcode::UngrabButton)] =
            &Connection::handle_ungrab_button;
        table[opcode_index(CoreOpcode::ChangeActivePointerGrab)] =
            &Connection::handle_change_active_pointer_grab;
        table[opcode_index(CoreOpcode::GrabKeyboard)] =
            &Connection::handle_grab_keyboard;
        table[opcode_index(CoreOpcode::UngrabKeyboard)] =
            &Connection::handle_ungrab_keyboard;
        table[opcode_index(CoreOpcode::GrabKey)] =
            &Connection::handle_grab_key;
        table[opcode_index(CoreOpcode::UngrabKey)] =
            &Connection::handle_ungrab_key;
        table[opcode_index(CoreOpcode::AllowEvents)] =
            &Connection::handle_allow_events;
        table[opcode_index(CoreOpcode::GrabServer)] =
            &Connection::handle_grab_server;
        table[opcode_index(CoreOpcode::UngrabServer)] =
            &Connection::handle_ungrab_server;
        table[opcode_index(CoreOpcode::QueryPointer)] =
            &Connection::handle_query_pointer;
        table[opcode_index(CoreOpcode::GetMotionEvents)] =
            &Connection::handle_get_motion_events;
        table[opcode_index(CoreOpcode::TranslateCoordinates)] =
            &Connection::handle_translate_coordinates;
        table[opcode_index(CoreOpcode::WarpPointer)] =
            &Connection::handle_warp_pointer;
        table[opcode_index(CoreOpcode::SetInputFocus)] =
            &Connection::handle_set_input_focus;
        table[opcode_index(CoreOpcode::OpenFont)] =
            &Connection::handle_open_font;
        table[opcode_index(CoreOpcode::CloseFont)] =
            &Connection::handle_close_font;
        table[opcode_index(CoreOpcode::QueryFont)] =
            &Connection::handle_query_font;
        table[opcode_index(CoreOpcode::QueryTextExtents)] =
            &Connection::handle_query_text_extents;
        table[opcode_index(CoreOpcode::ListFonts)] =
            &Connection::handle_list_fonts;
        table[opcode_index(CoreOpcode::ListFontsWithInfo)] =
            &Connection::handle_list_fonts_with_info;
        table[opcode_index(CoreOpcode::SetFontPath)] =
            &Connection::handle_set_font_path;
        table[opcode_index(CoreOpcode::GetFontPath)] =
            &Connection::handle_get_font_path;
        table[opcode_index(CoreOpcode::CreatePixmap)] =
            &Connection::handle_create_pixmap;
        table[opcode_index(CoreOpcode::FreePixmap)] =
            &Connection::handle_free_pixmap;
        table[opcode_index(CoreOpcode::CreateGC)] =
            &Connection::handle_create_graphics_context;
        table[opcode_index(CoreOpcode::ChangeGC)] =
            &Connection::handle_change_graphics_context;
        table[opcode_index(CoreOpcode::CopyGC)] =
            &Connection::handle_copy_graphics_context;
        table[opcode_index(CoreOpcode::SetDashes)] =
            &Connection::handle_set_dashes;
        table[opcode_index(CoreOpcode::SetClipRectangles)] =
            &Connection::handle_set_clip_rectangles;
        table[opcode_index(CoreOpcode::FreeGC)] =
            &Connection::handle_free_graphics_context;
        table[opcode_index(CoreOpcode::ClearArea)] =
            &Connection::handle_clear_area;
        table[opcode_index(CoreOpcode::CopyArea)] =
            &Connection::handle_copy_area;
        table[opcode_index(CoreOpcode::CopyPlane)] =
            &Connection::handle_copy_plane;
        table[opcode_index(CoreOpcode::PolyPoint)] =
            &Connection::handle_poly_points;
        table[opcode_index(CoreOpcode::PolyLine)] =
            &Connection::handle_poly_lines;
        table[opcode_index(CoreOpcode::PolySegment)] =
            &Connection::handle_poly_segments;
        table[opcode_index(CoreOpcode::PolyRectangle)] =
            &Connection::handle_poly_rectangles;
        table[opcode_index(CoreOpcode::PolyArc)] =
            &Connection::handle_poly_arcs;
        table[opcode_index(CoreOpcode::FillPoly)] =
            &Connection::handle_fill_polygon;
        table[opcode_index(CoreOpcode::PolyFillRectangle)] =
            &Connection::handle_fill_rectangles;
        table[opcode_index(CoreOpcode::PolyFillArc)] =
            &Connection::handle_fill_arcs;
        table[opcode_index(CoreOpcode::PolyText8)] =
            &Connection::handle_poly_text;
        table[opcode_index(CoreOpcode::PolyText16)] =
            &Connection::handle_poly_text;
        table[opcode_index(CoreOpcode::ImageText8)] =
            &Connection::handle_image_text;
        table[opcode_index(CoreOpcode::ImageText16)] =
            &Connection::handle_image_text;
        table[opcode_index(CoreOpcode::PutImage)] =
            &Connection::handle_put_image;
        table[opcode_index(CoreOpcode::GetImage)] =
            &Connection::handle_get_image;
        table[opcode_index(CoreOpcode::CreateCursor)] =
            &Connection::handle_create_cursor;
        table[opcode_index(CoreOpcode::CreateGlyphCursor)] =
            &Connection::handle_create_glyph_cursor;
        table[opcode_index(CoreOpcode::FreeCursor)] =
            &Connection::handle_free_cursor;
        table[opcode_index(CoreOpcode::RecolorCursor)] =
            &Connection::handle_recolor_cursor;
        table[opcode_index(CoreOpcode::CreateColormap)] =
            &Connection::handle_create_colormap;
        table[opcode_index(CoreOpcode::FreeColormap)] =
            &Connection::handle_free_colormap;
        table[opcode_index(CoreOpcode::CopyColormapAndFree)] =
            &Connection::handle_copy_colormap;
        table[opcode_index(CoreOpcode::InstallColormap)] =
            &Connection::handle_install_colormap;
        table[opcode_index(CoreOpcode::UninstallColormap)] =
            &Connection::handle_uninstall_colormap;
        table[opcode_index(CoreOpcode::ListInstalledColormaps)] =
            &Connection::handle_list_installed_colormaps;
        table[opcode_index(CoreOpcode::AllocColor)] =
            &Connection::handle_alloc_color;
        table[opcode_index(CoreOpcode::AllocNamedColor)] =
            &Connection::handle_alloc_named_color;
        table[opcode_index(CoreOpcode::AllocColorCells)] =
            &Connection::handle_alloc_color_cells;
        table[opcode_index(CoreOpcode::AllocColorPlanes)] =
            &Connection::handle_alloc_color_planes;
        table[opcode_index(CoreOpcode::FreeColors)] =
            &Connection::handle_free_colors;
        table[opcode_index(CoreOpcode::StoreColors)] =
            &Connection::handle_store_colors;
        table[opcode_index(CoreOpcode::StoreNamedColor)] =
            &Connection::handle_store_named_color;
        table[opcode_index(CoreOpcode::QueryColors)] =
            &Connection::handle_query_colors;
        table[opcode_index(CoreOpcode::LookupColor)] =
            &Connection::handle_lookup_color;
        table[opcode_index(CoreOpcode::QueryBestSize)] =
            &Connection::handle_query_best_size;
        table[opcode_index(CoreOpcode::ChangeKeyboardMapping)] =
            &Connection::handle_change_keyboard_mapping;
        table[opcode_index(CoreOpcode::GetKeyboardMapping)] =
            &Connection::handle_get_keyboard_mapping;
        table[opcode_index(CoreOpcode::ChangeKeyboardControl)] =
            &Connection::handle_change_keyboard_control;
        table[opcode_index(CoreOpcode::GetKeyboardControl)] =
            &Connection::handle_get_keyboard_control;
        table[opcode_index(CoreOpcode::Bell)] =
            &Connection::handle_bell;
        table[opcode_index(CoreOpcode::ChangePointerControl)] =
            &Connection::handle_change_pointer_control;
        table[opcode_index(CoreOpcode::GetPointerControl)] =
            &Connection::handle_get_pointer_control;
        table[opcode_index(CoreOpcode::SetPointerMapping)] =
            &Connection::handle_set_pointer_mapping;
        table[opcode_index(CoreOpcode::GetPointerMapping)] =
            &Connection::handle_get_pointer_mapping;
        table[opcode_index(CoreOpcode::SetModifierMapping)] =
            &Connection::handle_set_modifier_mapping;
        table[opcode_index(CoreOpcode::GetModifierMapping)] =
            &Connection::handle_get_modifier_mapping;
        table[opcode_index(CoreOpcode::SetScreenSaver)] =
            &Connection::handle_set_screen_saver;
        table[opcode_index(CoreOpcode::GetScreenSaver)] =
            &Connection::handle_get_screen_saver;
        table[opcode_index(CoreOpcode::ChangeHosts)] =
            &Connection::handle_change_hosts;
        table[opcode_index(CoreOpcode::ListHosts)] =
            &Connection::handle_list_hosts;
        table[opcode_index(CoreOpcode::SetAccessControl)] =
            &Connection::handle_set_access_control;
        table[opcode_index(CoreOpcode::SetCloseDownMode)] =
            &Connection::handle_set_close_down_mode;
        table[opcode_index(CoreOpcode::KillClient)] =
            &Connection::handle_kill_client;
        table[opcode_index(CoreOpcode::GetInputFocus)] =
            &Connection::handle_get_input_focus;
        table[opcode_index(CoreOpcode::QueryKeymap)] =
            &Connection::handle_query_keymap;
        table[opcode_index(CoreOpcode::ForceScreenSaver)] =
            &Connection::handle_force_screen_saver;
        table[opcode_index(CoreOpcode::QueryExtension)] =
            &Connection::handle_query_extension;
        table[opcode_index(CoreOpcode::ListExtensions)] =
            &Connection::handle_list_extensions;
        table[opcode_index(CoreOpcode::NoOperation)] =
            &Connection::handle_no_operation;
        return table;
    }();
    if (context.opcode >= handlers.size()) {
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence);
    }
    const auto handler = handlers[context.opcode];
    if (handler == nullptr) {
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence);
    }
    return (this->*handler)(context);
}

Result<bool>
Connection::process_request()
{
    constexpr std::size_t header_size = 4;
    if (input_.size() < header_size)
        return Result<bool>::success(false);
    if (!order_)
        return Result<bool>::failure(ErrorCode::malformed,
                                     "request byte order was not recorded");

    WireReader header(input_.data(), header_size, *order_);
    const auto opcode = header.u8();
    const auto data = header.u8();
    const auto length_units = header.u16();
    if (!opcode || !data || !length_units)
        return Result<bool>::failure(ErrorCode::malformed,
                                     "truncated request header");

    ++sequence_;
    server_.note_client_sequence(config_.resource_base, sequence_);
    std::size_t wire_header_size = header_size;
    std::size_t request_size = 0;
    if (*length_units == 0) {
        if (!big_requests_enabled_) {
            consume_input(header_size);
            auto sent = send_error(*order_, bad_length, *opcode, sequence_);
            if (!sent) {
                return Result<bool>::failure(
                    sent.error().code, sent.error().message);
            }
            return Result<bool>::success(true);
        }
        constexpr std::size_t extended_header_size = 8;
        if (input_.size() < extended_header_size) {
            --sequence_;
            server_.note_client_sequence(config_.resource_base, sequence_);
            return Result<bool>::success(false);
        }
        WireReader extended_header(
            input_.data() + header_size, header_size, *order_);
        const auto extended_length_units = extended_header.u32();
        if (!extended_length_units) {
            return Result<bool>::failure(
                ErrorCode::malformed, "truncated extended request header");
        }
        const auto extended_size = checked_multiply(
            static_cast<std::size_t>(*extended_length_units),
            std::size_t{4});
        if (extended_size)
            request_size = *extended_size;
        wire_header_size = extended_header_size;
    }
    else {
        const auto core_size = checked_multiply(
            static_cast<std::size_t>(*length_units), std::size_t{4});
        if (core_size)
            request_size = *core_size;
    }
    const std::size_t maximum_size = *length_units == 0
        ? maximum_extended_request_bytes
        : maximum_core_request_bytes;
    if (request_size < wire_header_size || request_size > maximum_size) {
        auto sent = send_error(*order_, bad_length, *opcode, sequence_);
        if (!sent)
            return Result<bool>::failure(
                sent.error().code, sent.error().message);
        input_.clear();
        close_after_output();
        return Result<bool>::success(true);
    }
    if (input_.size() < request_size) {
        --sequence_;
        server_.note_client_sequence(config_.resource_base, sequence_);
        return Result<bool>::success(false);
    }

    std::vector<std::uint8_t> request;
    if (wire_header_size == header_size) {
        request.assign(
            input_.begin(),
            input_.begin() + static_cast<std::ptrdiff_t>(request_size));
    }
    else {
        request.reserve(request_size - header_size);
        request.insert(request.end(), input_.begin(),
                       input_.begin() + static_cast<std::ptrdiff_t>(header_size));
        request.insert(
            request.end(),
            input_.begin() + static_cast<std::ptrdiff_t>(wire_header_size),
            input_.begin() + static_cast<std::ptrdiff_t>(request_size));
    }
    consume_input(request_size);
    const RequestContext context{
        *order_, *opcode, *data, sequence_, request};
    server_.advance_time();
    auto dispatched = dispatch(context);
    if (!dispatched) {
        return Result<bool>::failure(
            dispatched.error().code, dispatched.error().message);
    }
    return Result<bool>::success(true);
}

Result<void>
Connection::process_input()
{
    if (state_ == State::requests &&
        !server_.sync_waiting(config_.resource_base)) {
        resume_sync_input_ = false;
    }
    while (!finished_ && !close_after_output_ &&
           !(state_ == State::requests &&
             server_.sync_waiting(config_.resource_base))) {
        Result<bool> processed = [&]() {
            switch (state_) {
            case State::setup_prefix:
                return process_setup_prefix();
            case State::setup_authentication:
                return process_setup_authentication();
            case State::requests:
                return process_request();
            }
            return Result<bool>::failure(ErrorCode::malformed,
                                         "invalid connection state");
        }();
        if (!processed) {
            return Result<void>::failure(
                processed.error().code, processed.error().message);
        }
        if (!processed.value())
            break;
        if (state_ == State::requests &&
            server_.sync_waiting(config_.resource_base)) {
            resume_sync_input_ = true;
        }
    }
    return Result<void>::success();
}

Result<void>
Connection::on_readable()
{
    if (!prepared_)
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "connection was not prepared");
    if (finished_ || close_after_output_)
        return Result<void>::success();
    if (state_ == State::requests &&
        server_.sync_waiting(config_.resource_base)) {
        return Result<void>::success();
    }

    std::array<std::uint8_t, 16384> bytes{};
    for (;;) {
        ssize_t count = -1;
#if XMIN_HAVE_SCM_RIGHTS
        std::array<unsigned char,
                   ancillary_space(sizeof(int) * 4)> control{};
        iovec vector{bytes.data(), bytes.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        count = ::recvmsg(socket_.get(), &message, 0);
        if (count >= 0) {
            if ((message.msg_flags & MSG_CTRUNC) != 0) {
                return Result<void>::failure(
                    ErrorCode::malformed,
                    "client sent too many ancillary descriptors");
            }
            for (auto *header = CMSG_FIRSTHDR(&message); header != nullptr;
                 header = CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level != SOL_SOCKET ||
                    header->cmsg_type != SCM_RIGHTS ||
                    header->cmsg_len < ancillary_length(sizeof(int))) {
                    continue;
                }
                const std::size_t payload =
                    header->cmsg_len - ancillary_length(0);
                const std::size_t descriptor_count = payload / sizeof(int);
                const auto *descriptors =
                    reinterpret_cast<const int *>(CMSG_DATA(header));
                for (std::size_t index = 0; index < descriptor_count;
                     ++index) {
                    UniqueFd descriptor(descriptors[index]);
                    const int flags = ::fcntl(descriptor.get(), F_GETFD);
                    if (flags >= 0) {
                        static_cast<void>(::fcntl(
                            descriptor.get(), F_SETFD, flags | FD_CLOEXEC));
                    }
                    if (received_fds_.size() >=
                        maximum_pending_descriptors) {
                        return Result<void>::failure(
                            ErrorCode::malformed,
                            "client descriptor queue limit exceeded");
                    }
                    try {
                        received_fds_.push_back(std::move(descriptor));
                    }
                    catch (const std::bad_alloc &) {
                        return Result<void>::failure(
                            ErrorCode::io,
                            "client descriptor queue allocation failed");
                    }
                }
            }
        }
#else
        count = ::read(socket_.get(), bytes.data(), bytes.size());
#endif
        if (count > 0) {
            const auto new_size = checked_add(
                input_.size(), static_cast<std::size_t>(count));
            if (!new_size || *new_size > maximum_buffered_input) {
                return Result<void>::failure(
                    ErrorCode::malformed,
                    "connection input buffer limit exceeded");
            }
            input_.insert(
                input_.end(), bytes.begin(),
                bytes.begin() + static_cast<std::ptrdiff_t>(count));
            auto processed = process_input();
            if (!processed)
                return processed;
            if (close_after_output_)
                return Result<void>::success();
            if (state_ == State::requests &&
                server_.sync_waiting(config_.resource_base)) {
                return Result<void>::success();
            }
            continue;
        }
        if (count == 0) {
            if (!input_.empty() || state_ != State::requests) {
                return Result<void>::failure(
                    ErrorCode::unexpected_eof,
                    "connection closed within a protocol packet");
            }
            close_after_output();
            return Result<void>::success();
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return Result<void>::success();
        return io_failure("read");
    }
}

Result<void>
Connection::on_writable()
{
    if (!prepared_)
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     "connection was not prepared");
    auto drained = drain_pending_events();
    if (!drained)
        return drained;
    if (resume_sync_input_ &&
        !server_.sync_waiting(config_.resource_base) && !input_.empty()) {
        auto processed = process_input();
        if (!processed)
            return processed;
    }
    drained = drain_pending_events();
    if (!drained)
        return drained;
    while (output_offset_ < output_.size()) {
        if (!pending_output_fds_.empty() &&
            pending_output_fds_.front().offset < output_offset_) {
            return Result<void>::failure(
                ErrorCode::malformed,
                "descriptor output marker was passed without being sent");
        }
        const bool send_descriptor = !pending_output_fds_.empty() &&
            pending_output_fds_.front().offset == output_offset_;
        std::size_t end = output_.size();
        if (send_descriptor) {
            if (pending_output_fds_.size() > 1)
                end = pending_output_fds_[1].offset;
        }
        else if (!pending_output_fds_.empty()) {
            end = pending_output_fds_.front().offset;
        }

        ssize_t count = -1;
        if (send_descriptor) {
#if XMIN_HAVE_SCM_RIGHTS
            std::array<unsigned char,
                       ancillary_space(sizeof(int))> control{};
            iovec vector{
                output_.data() + output_offset_, end - output_offset_};
            msghdr message{};
            message.msg_iov = &vector;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            auto *header = CMSG_FIRSTHDR(&message);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = ancillary_length(sizeof(int));
            const int descriptor = pending_output_fds_.front().fd.get();
            std::memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
            count = ::sendmsg(socket_.get(), &message, 0);
#else
            return Result<void>::failure(
                ErrorCode::invalid_argument,
                "descriptor passing is unavailable on this platform");
#endif
        }
        else {
            count = ::write(
                socket_.get(), output_.data() + output_offset_,
                end - output_offset_);
        }
        if (count > 0) {
            output_offset_ += static_cast<std::size_t>(count);
            if (send_descriptor)
                pending_output_fds_.pop_front();
            continue;
        }
        if (count == 0)
            return Result<void>::failure(ErrorCode::io,
                                         "write made no progress");
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return Result<void>::success();
        return io_failure("write");
    }

    output_.clear();
    output_offset_ = 0;
    if (!pending_output_fds_.empty()) {
        return Result<void>::failure(
            ErrorCode::malformed, "unsent descriptor output marker");
    }
    if (close_after_output_)
        finished_ = true;
    return Result<void>::success();
}

Result<void>
Connection::serve()
{
    auto prepared = prepare();
    if (!prepared)
        return prepared;

    while (!finished_) {
        static_cast<void>(server_.process_timers());
        pollfd descriptor{socket_.get(), poll_events(), 0};
        int result;
        do {
            result = ::poll(
                &descriptor, 1, server_.timer_timeout_milliseconds());
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return io_failure("poll");
        if ((descriptor.revents & POLLNVAL) != 0) {
            return Result<void>::failure(ErrorCode::io,
                                         "client descriptor became invalid");
        }
        if ((descriptor.revents & POLLOUT) != 0) {
            auto written = on_writable();
            if (!written)
                return written;
        }
        if (!finished_ &&
            (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            auto read = on_readable();
            if (!read)
                return read;
        }
        if (!finished_ && (descriptor.revents & POLLHUP) != 0 &&
            (descriptor.revents & POLLOUT) == 0) {
            return Result<void>::success();
        }
        if (!finished_ && (descriptor.revents & POLLERR) != 0 &&
            (descriptor.revents & (POLLIN | POLLOUT | POLLHUP)) == 0) {
            return Result<void>::failure(ErrorCode::io,
                                         "client socket reported an error");
        }
    }
    return Result<void>::success();
}

} // namespace xmin::server
